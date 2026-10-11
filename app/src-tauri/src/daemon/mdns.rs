//! Advertises `_watchdeck._tcp` through avahi-daemon's D-Bus API, so the host has one mDNS responder.
//!
//! A supervisor keeps the registration alive. It re-registers when avahi restarts, picks a new
//! instance name on a collision, and reports its state on a watch channel. TXT `pair` appears
//! only while a pairing window is open, so unpaired devices list only daemons ready to pair.

use std::time::Duration;

use futures_util::StreamExt;
use tokio::sync::watch;
use tokio::time::timeout;
use zbus::fdo::DBusProxy;
use zbus::names::BusName;
use zbus::zvariant::OwnedObjectPath;
use zbus::{proxy, Connection};

use super::protocol::VERSION;

const AVAHI: &str = "org.freedesktop.Avahi";
const SERVICE_TYPE: &str = "_watchdeck._tcp";
/// How long to wait for avahi before looking again.
const RETRY: Duration = Duration::from_secs(30);

const IF_UNSPEC: i32 = -1;
const PROTO_INET: i32 = 0;
const STATE_ESTABLISHED: i32 = 2;
const STATE_COLLISION: i32 = 3;
const STATE_FAILURE: i32 = 4;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MdnsStatus {
    Starting,
    /// Registered under this instance name.
    Registered(String),
    Unavailable(String),
}

#[proxy(
    interface = "org.freedesktop.Avahi.Server",
    default_service = "org.freedesktop.Avahi",
    default_path = "/"
)]
trait Server {
    fn entry_group_new(&self) -> zbus::Result<OwnedObjectPath>;
    fn get_alternative_service_name(&self, name: &str) -> zbus::Result<String>;
}

#[proxy(
    interface = "org.freedesktop.Avahi.EntryGroup",
    default_service = "org.freedesktop.Avahi"
)]
trait EntryGroup {
    #[allow(clippy::too_many_arguments)]
    fn add_service(
        &self,
        interface: i32,
        protocol: i32,
        flags: u32,
        name: &str,
        service_type: &str,
        domain: &str,
        host: &str,
        port: u16,
        txt: &[&[u8]],
    ) -> zbus::Result<()>;
    #[allow(clippy::too_many_arguments)]
    fn update_service_txt(
        &self,
        interface: i32,
        protocol: i32,
        flags: u32,
        name: &str,
        service_type: &str,
        domain: &str,
        txt: &[&[u8]],
    ) -> zbus::Result<()>;
    fn commit(&self) -> zbus::Result<()>;
    fn reset(&self) -> zbus::Result<()>;
    fn free(&self) -> zbus::Result<()>;

    #[zbus(signal)]
    fn state_changed(&self, state: i32, error: &str) -> zbus::Result<()>;
}

/// Keeps `name` advertised on `port` for as long as the future runs. `fp` is the TLS identity's
/// fingerprint, which devices use to pick their daemon. `pairing` holds the pairing port while
/// a pairing window is open.
pub async fn advertise(
    name: String,
    port: u16,
    fp: String,
    mut pairing: watch::Receiver<Option<u16>>,
    status: watch::Sender<MdnsStatus>,
) {
    loop {
        match Connection::system().await {
            Ok(conn) => {
                if let Err(e) = supervise(&conn, &name, port, &fp, &mut pairing, &status).await {
                    set_unavailable(&status, format!("D-Bus error: {e}"));
                }
            }
            Err(e) => set_unavailable(&status, format!("system bus unavailable: {e}")),
        }
        tokio::time::sleep(RETRY).await;
    }
}

/// Logs on the first report of a state, not on every retry.
fn set_unavailable(status: &watch::Sender<MdnsStatus>, reason: String) {
    let next = MdnsStatus::Unavailable(reason);
    let changed = status.send_if_modified(|current| {
        let changed = *current != next;
        if changed {
            *current = next.clone();
        }
        changed
    });
    if changed {
        log::error!("mDNS unavailable: {next:?}");
    }
}

async fn supervise(
    conn: &Connection,
    name: &str,
    port: u16,
    fp: &str,
    pairing: &mut watch::Receiver<Option<u16>>,
    status: &watch::Sender<MdnsStatus>,
) -> zbus::Result<()> {
    let dbus = DBusProxy::new(conn).await?;
    let mut owners = dbus
        .receive_name_owner_changed_with_args(&[(0, AVAHI)])
        .await?;
    loop {
        if dbus.name_has_owner(BusName::try_from(AVAHI)?).await? {
            match register(conn, name, port, fp, pairing, status, &mut owners).await {
                Ok(()) => set_unavailable(status, "avahi-daemon stopped".into()),
                Err(e) => set_unavailable(status, format!("avahi registration failed: {e}")),
            }
        } else {
            set_unavailable(status, "avahi-daemon is not running".into());
        }
        // Wake on avahi gaining an owner, or after RETRY to try again.
        let _ = timeout(RETRY, async {
            while let Some(sig) = owners.next().await {
                if sig.args().is_ok_and(|a| a.new_owner().is_some()) {
                    return;
                }
            }
        })
        .await;
    }
}

/// Registers the service and watches it. Returns `Ok` when avahi goes away.
async fn register(
    conn: &Connection,
    name: &str,
    port: u16,
    fp: &str,
    pairing: &mut watch::Receiver<Option<u16>>,
    status: &watch::Sender<MdnsStatus>,
    owners: &mut zbus::fdo::NameOwnerChangedStream,
) -> zbus::Result<()> {
    let server = ServerProxy::new(conn).await?;
    let group = EntryGroupProxy::builder(conn)
        .path(server.entry_group_new().await?)?
        .build()
        .await?;
    let mut states = group.receive_state_changed().await?;
    let mut name = name.to_string();
    let mut txt = txt(fp, *pairing.borrow_and_update());
    // Stops watching if the pairing side goes away, instead of waking on every poll.
    let mut watching = true;
    add(&group, &name, port, &txt).await?;

    let result = loop {
        tokio::select! {
            Some(sig) = states.next() => {
                let args = sig.args()?;
                match args.state {
                    STATE_ESTABLISHED => {
                        log::info!("advertising {SERVICE_TYPE} as {name:?} on port {port}");
                        let _ = status.send(MdnsStatus::Registered(name.clone()));
                    }
                    STATE_COLLISION => {
                        name = server.get_alternative_service_name(&name).await?;
                        log::warn!("mDNS name collision, retrying as {name:?}");
                        group.reset().await?;
                        add(&group, &name, port, &txt).await?;
                    }
                    STATE_FAILURE => break Err(zbus::Error::Failure(args.error.to_string())),
                    _ => {}
                }
            }
            Some(sig) = owners.next() => {
                if sig.args()?.new_owner().is_none() {
                    break Ok(());
                }
            }
            changed = pairing.changed(), if watching => {
                if changed.is_err() {
                    watching = false;
                    continue;
                }
                txt = self::txt(fp, *pairing.borrow_and_update());
                let fields: Vec<&[u8]> = txt.iter().map(|f| f.as_bytes()).collect();
                group
                    .update_service_txt(IF_UNSPEC, PROTO_INET, 0, &name, SERVICE_TYPE, "", &fields)
                    .await?;
                log::info!("mDNS TXT now {txt:?}");
            }
            else => break Err(zbus::Error::Failure("D-Bus signal stream closed".into())),
        }
    };
    if let Err(e) = group.free().await {
        log::debug!("freeing the avahi entry group: {e}");
    }
    result
}

/// TXT `v` and `fp`, plus `pair` while a pairing window is open.
fn txt(fp: &str, pair: Option<u16>) -> Vec<String> {
    let mut txt = vec![format!("v={VERSION}"), format!("fp={fp}")];
    txt.extend(pair.map(|port| format!("pair={port}")));
    txt
}

async fn add(
    group: &EntryGroupProxy<'_>,
    name: &str,
    port: u16,
    txt: &[String],
) -> zbus::Result<()> {
    let fields: Vec<&[u8]> = txt.iter().map(|f| f.as_bytes()).collect();
    group
        .add_service(
            IF_UNSPEC,
            PROTO_INET,
            0,
            name,
            SERVICE_TYPE,
            "",
            "",
            port,
            &fields,
        )
        .await?;
    group.commit().await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn txt_carries_pair_only_while_pairing_is_open() {
        assert_eq!(
            txt("0123456789abcdef", None),
            ["v=1", "fp=0123456789abcdef"]
        );
        assert_eq!(
            txt("0123456789abcdef", Some(47821)),
            ["v=1", "fp=0123456789abcdef", "pair=47821"]
        );
    }
}
