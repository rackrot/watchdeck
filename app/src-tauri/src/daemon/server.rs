//! `wss://` listener: one task per connection, one live session per paired device id.

use std::collections::HashMap;
use std::io;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::stream::{SplitSink, SplitStream};
use futures_util::{SinkExt, StreamExt};
use serde::Serialize;
use tokio::io::{AsyncRead, AsyncWrite};
use tokio::net::{TcpListener, TcpStream, ToSocketAddrs};
use tokio::sync::{broadcast, mpsc, oneshot, watch, Semaphore};
use tokio::task::JoinSet;
use tokio::time::{sleep, sleep_until, timeout, timeout_at, Instant};
use tokio_rustls::rustls::ServerConfig;
use tokio_rustls::TlsAcceptor;
use tokio_tungstenite::tungstenite::protocol::WebSocketConfig;
use tokio_tungstenite::tungstenite::Bytes;
use tokio_tungstenite::tungstenite::{Error as WsError, Message as WsMessage};
use tokio_tungstenite::{accept_async_with_config, WebSocketStream};

use time::OffsetDateTime;

use super::actions::{ActionResult, Actions, Run, MAX_RUNNING};
use super::config::{Action, IntegrationConfig, Layout, Live, TileConfig, MAX_COLS, MAX_ROWS};
use super::hoststats::{self, HostSources, HostStats};
use super::integrations::{self, FeedHandle, FeedSlots, Integrations};
use super::protocol::TileState;
use super::protocol::{
    decode_hex, encode_asset_frame, encode_hex, valid_device_id, Colors, ErrorCode, Feed, Message,
    Report, Token, ASSET_ID_BYTES, CAPS, CAP_FEED, CAP_FONTS, CAP_PAGES, CAP_SLEEP, CAP_STATUS_BAR,
    ICON_ID_BYTES, MAX_MESSAGE_BYTES, MAX_REPORTS_PER_SESSION, TOKEN_BYTES, VERSION,
};

use super::screens::IMAGE_BYTES;
use super::store::Store;
use super::tiles::{self, Sent, Tiles, Values};

pub const PORT: u16 = 47820;
/// From the TCP accept to the hello, covering the TLS handshake and the WebSocket upgrade.
const HELLO_TIMEOUT: Duration = Duration::from_secs(5);
const IDLE_TIMEOUT: Duration = Duration::from_secs(30);
const WRITE_TIMEOUT: Duration = Duration::from_secs(5);
/// How long a closing session waits for the device to close too.
const DRAIN_TIMEOUT: Duration = Duration::from_secs(2);
const MAX_BUTTON_ID_BYTES: usize = 64;
/// A session sends the feed at most this often, so a busy source costs the board two frames a
/// second at most.
const FEED_GAP: Duration = Duration::from_millis(500);
/// Ids in one `icon_get`: one page never names more icons than it has cells, and a device asks
/// for the icons of a set of pages in several requests.
const MAX_ICON_IDS: usize = (MAX_COLS * MAX_ROWS) as usize;
/// Concurrent connections, handshaking or live. Beyond this the listener drops new sockets.
const MAX_CONNECTIONS: usize = 64;
/// The one reply to every failed authentication, so a client can't tell an unknown device from
/// a wrong token.
const UNAUTHORIZED: &str = "unauthorized";

pub(super) type WsSink<S> = SplitSink<WebSocketStream<S>, WsMessage>;
pub(super) type WsRead<S> = SplitStream<WebSocketStream<S>>;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DaemonEvent {
    DeviceConnected(String),
    DeviceDisconnected(String),
    DevicePaired(String),
    DeviceForgotten(String),
    /// A config change reached a connected device as layout `rev`. The first layout of a session
    /// is not announced.
    LayoutPushed(String, u32),
    /// The device built the grid of layout `rev`. Only a newer board sends the ack, and the first
    /// layout's is announced too.
    LayoutApplied(String, u32),
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct DeviceInfo {
    pub id: String,
    pub connected: bool,
    /// The capabilities of the live session (see [`CAPS`]), empty while disconnected.
    pub caps: Vec<String>,
}

/// Why a live session is told to end.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Kick {
    /// The same device connected again.
    Replaced,
    /// The device's pairing was forgotten or replaced, so its token no longer opens a session.
    Revoked,
}

pub struct Server {
    listener: TcpListener,
    shared: Arc<Shared>,
}

/// The paired devices and their live sessions, for the desktop UI and for pairing.
#[derive(Clone)]
pub struct Devices(Arc<Shared>);

struct Shared {
    hostname: String,
    tls: TlsAcceptor,
    config: watch::Receiver<Arc<Live>>,
    actions: Actions,
    events: broadcast::Sender<DaemonEvent>,
    next_id: AtomicU64,
    /// Polls the tiles while a session is open, and holds their latest values.
    tiles: Tiles,
    /// The feed's sources, polled while a session is open, and the cards and notes they fill.
    integrations: Integrations,
    /// The host's stats, sampled while a session is open. `None` between periods.
    stats: watch::Receiver<Option<HostStats>>,
    /// One lock, so a session is only ever registered under a pairing that still exists.
    registry: Mutex<Registry>,
}

/// A device's live session, as the registry knows it.
struct SessionEntry {
    id: u64,
    /// Closes the session.
    kick: oneshot::Sender<Kick>,
    caps: Vec<String>,
}

struct Registry {
    store: Store,
    sessions: HashMap<String, SessionEntry>,
    /// How many sessions are live. Tile polling follows it.
    connected: watch::Sender<usize>,
}

impl Server {
    /// Sessions show the layout of the live config in `config`, and push it again whenever it
    /// changes. Presses run through `actions`, which also poll the config's tiles for as long as
    /// a session is open. The status bar's host stats are read from `sources`. Must run inside a
    /// tokio runtime.
    pub async fn bind(
        addr: impl ToSocketAddrs,
        tls: Arc<ServerConfig>,
        store: Store,
        config: watch::Receiver<Arc<Live>>,
        actions: Actions,
        sources: HostSources,
    ) -> io::Result<Self> {
        let hostname = gethostname::gethostname().to_string_lossy().into_owned();
        let (events, _) = broadcast::channel(64);
        let listener = TcpListener::bind(addr).await?;
        let (connected, polling) = watch::channel(0);
        let stats = hoststats::spawn(polling.clone(), config.clone(), sources);
        let tiles = tiles::spawn(actions.clone(), config.clone(), polling.clone());
        let integrations = integrations::spawn(FeedSlots(actions.clone()), config.clone(), polling);
        Ok(Self {
            listener,
            shared: Arc::new(Shared {
                hostname,
                tls: TlsAcceptor::from(tls),
                config,
                actions,
                events,
                next_id: AtomicU64::new(0),
                tiles,
                integrations,
                stats,
                registry: Mutex::new(Registry {
                    store,
                    sessions: HashMap::new(),
                    connected,
                }),
            }),
        })
    }

    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        self.listener.local_addr()
    }

    pub fn hostname(&self) -> &str {
        &self.shared.hostname
    }

    pub fn subscribe(&self) -> broadcast::Receiver<DaemonEvent> {
        self.shared.events.subscribe()
    }

    pub fn devices(&self) -> Devices {
        Devices(self.shared.clone())
    }

    /// Accepts connections until the future is dropped, which also drops every session.
    pub async fn run(self) {
        let mut sessions = JoinSet::new();
        let slots = Arc::new(Semaphore::new(MAX_CONNECTIONS));
        loop {
            tokio::select! {
                accepted = self.listener.accept() => match accepted {
                    Ok((stream, peer)) => match slots.clone().try_acquire_owned() {
                        Ok(permit) => {
                            let shared = self.shared.clone();
                            sessions.spawn(async move {
                                handle(stream, peer, &shared).await;
                                drop(permit);
                            });
                        }
                        Err(_) => log::debug!("{peer}: dropped, {MAX_CONNECTIONS} connections open"),
                    },
                    Err(e) => {
                        log::error!("accept failed: {e}");
                        tokio::time::sleep(Duration::from_millis(100)).await;
                    }
                },
                Some(joined) = sessions.join_next() => {
                    if let Err(e) = joined {
                        log::error!("session task failed: {e}");
                    }
                }
            }
        }
    }
}

impl Devices {
    /// The latest tile readings, which change as devices connect, poll and leave.
    pub fn tile_values(&self) -> watch::Receiver<Values> {
        self.0.tiles.values()
    }

    /// The feed: its snapshot, and the in-process API that posts and dismisses notes.
    pub fn feed(&self) -> FeedHandle {
        self.0.integrations.handle()
    }

    pub fn list(&self) -> Vec<DeviceInfo> {
        let registry = self.0.registry();
        registry
            .store
            .ids()
            .map(|id| {
                let session = registry.sessions.get(id);
                DeviceInfo {
                    id: id.to_string(),
                    connected: session.is_some(),
                    caps: session.map(|s| s.caps.clone()).unwrap_or_default(),
                }
            })
            .collect()
    }

    /// Stores `device`'s pairing, replacing any earlier one, and ends the session that the
    /// earlier token opened.
    pub fn pair(&self, device: &str, token_sha256: [u8; TOKEN_BYTES]) -> io::Result<()> {
        let mut registry = self.0.registry();
        registry.store.insert(device, token_sha256)?;
        let revoked = registry.revoke(device);
        drop(registry);
        log::info!("paired {device}");
        self.0.emit(DaemonEvent::DevicePaired(device.to_string()));
        if revoked {
            self.0
                .emit(DaemonEvent::DeviceDisconnected(device.to_string()));
        }
        Ok(())
    }

    /// Forgets `device` and closes its live session, which is told `unauthorized` first so the
    /// device drops its pairing too. Returns whether it was paired.
    pub fn forget(&self, device: &str) -> io::Result<bool> {
        let mut registry = self.0.registry();
        if !registry.store.remove(device)? {
            return Ok(false);
        }
        let revoked = registry.revoke(device);
        drop(registry);
        log::info!("forgot {device}");
        self.0
            .emit(DaemonEvent::DeviceForgotten(device.to_string()));
        if revoked {
            self.0
                .emit(DaemonEvent::DeviceDisconnected(device.to_string()));
        }
        Ok(true)
    }

    /// Runs button `btn` as a device press would, from the live config and through the same
    /// slots, and returns the result the device would get. No press result is sent to any
    /// device; a button with a tile polls it again, as a press does. An
    /// unknown button is an error, and a refused run ("already running", "busy") is a result.
    pub async fn test_press(&self, btn: &str) -> Result<ActionResult, String> {
        if self.0.config.borrow().button(btn).is_none() {
            return Err(format!("There is no saved button {btn:?}."));
        }
        let (run, action) = match self.0.begin_press(btn) {
            Ok(started) => started,
            Err(refused) => {
                log::info!("test: {btn:?}: {}", refused.summary);
                return Ok(refused);
            }
        };
        let result = run.run(&action).await;
        log::info!("test: {btn:?}: {} ({} ms)", result.summary, result.ms);
        // The press may have changed what the button's tile shows.
        self.0.tiles.poker().poke(btn);
        Ok(result)
    }

    /// Runs the source of stat integration `id` once, from the live config and in one of the
    /// feed's slots, and returns the card it would show: `summary` is the value, `ok` its state.
    /// Nothing reaches the store or a device. An unknown or non-stat id is an error, and a refused
    /// run ("already running", "busy") is a result.
    pub async fn test_integration(&self, id: &str) -> Result<ActionResult, String> {
        let tile = match self
            .0
            .config
            .borrow()
            .integrations
            .iter()
            .find(|i| i.id() == id)
        {
            Some(IntegrationConfig::Stat {
                every_s,
                json_pointer,
                source,
                ..
            }) => TileConfig {
                every_s: *every_s,
                json_pointer: json_pointer.clone(),
                font: None,
                source: source.clone(),
            },
            Some(_) => return Err(format!("Integration {id:?} is not a stat source.")),
            None => return Err(format!("There is no saved integration {id:?}.")),
        };
        let run = match self.0.actions.start_feed(id) {
            Ok(run) => run,
            Err(refused) => {
                log::info!("test: integration {id:?}: {}", refused.summary);
                return Ok(refused);
            }
        };
        let ran = run.poll(&tile.source).await;
        let value = tiles::reading(&tile, &ran);
        log::info!(
            "test: integration {id:?}: {:?} ({} ms)",
            value.state,
            ran.result.ms
        );
        Ok(ActionResult {
            ok: value.state == TileState::Ok,
            code: ran.result.code,
            summary: value.text,
            text: String::new(),
            ms: ran.result.ms,
        })
    }
}

impl Registry {
    /// Tells `device`'s live session, if any, that its token is void. Returns whether there was one.
    fn revoke(&mut self, device: &str) -> bool {
        match self.sessions.remove(device) {
            Some(session) => {
                // An error only means the session is already ending.
                let _ = session.kick.send(Kick::Revoked);
                self.publish_count();
                true
            }
            None => false,
        }
    }

    fn publish_count(&self) {
        self.connected.send_replace(self.sessions.len());
    }
}

impl Shared {
    fn registry(&self) -> std::sync::MutexGuard<'_, Registry> {
        self.registry.lock().expect("device registry poisoned")
    }

    /// Checks `token` for `device` and, only if it is valid, registers a session for it, kicking
    /// the one it replaces. An impostor using a paired device's id never gets this far, so it
    /// can't kick the real session. `caps` are the device's, shown in [`DeviceInfo`].
    fn authenticate(
        &self,
        device: &str,
        token: &[u8; TOKEN_BYTES],
        caps: Vec<String>,
    ) -> Option<(u64, oneshot::Receiver<Kick>)> {
        let mut registry = self.registry();
        if !registry.store.verify(device, token) {
            return None;
        }
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let (kick_tx, kick_rx) = oneshot::channel();
        let entry = SessionEntry {
            id,
            kick: kick_tx,
            caps,
        };
        let old = registry.sessions.insert(device.to_string(), entry);
        registry.publish_count();
        drop(registry);
        if let Some(old) = old {
            log::info!("{device} reconnected, replacing its old session");
            // An error only means the old session is already ending.
            let _ = old.kick.send(Kick::Replaced);
            self.emit(DaemonEvent::DeviceDisconnected(device.to_string()));
        }
        self.emit(DaemonEvent::DeviceConnected(device.to_string()));
        Some((id, kick_rx))
    }

    fn unregister(&self, device: &str, id: u64) {
        let mut registry = self.registry();
        // A replaced or revoked session finds its slot taken or gone and stays silent.
        if registry
            .sessions
            .get(device)
            .is_some_and(|current| current.id == id)
        {
            registry.sessions.remove(device);
            registry.publish_count();
            drop(registry);
            self.emit(DaemonEvent::DeviceDisconnected(device.to_string()));
        }
    }

    fn emit(&self, event: DaemonEvent) {
        // An error only means nobody is subscribed.
        let _ = self.events.send(event);
    }

    /// Claims the run for a press of `btn`, with its action from the live config, or says why
    /// there is none: no such button, already running, or too many runs.
    fn begin_press(&self, btn: &str) -> Result<(Run, Action), ActionResult> {
        let action = self.config.borrow().button(btn).map(|b| b.action.clone());
        match action {
            Some(action) => self.actions.start(btn).map(|run| (run, action)),
            None => Err(ActionResult::failed("unknown button")),
        }
    }
}

async fn handle(stream: TcpStream, peer: SocketAddr, shared: &Shared) {
    if let Err(e) = session(stream, peer, shared).await {
        log::warn!("{peer}: {e}");
    }
}

pub(super) enum Incoming {
    Msg(Message),
    BadRequest(String),
    /// A hello with a `v` other than 1 that may not parse as a full [`Message::Hello`].
    BadVersion(String),
    Closed,
    Idle,
    Failed(WsError),
}

/// Next text message. Control frames are skipped (tungstenite answers pings itself).
/// `IDLE_TIMEOUT` applies per frame, so pings keep a session alive.
pub(super) async fn next_message<S>(rx: &mut WsRead<S>) -> Incoming
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    loop {
        match timeout(IDLE_TIMEOUT, rx.next()).await {
            Err(_) => return Incoming::Idle,
            Ok(None) => return Incoming::Closed,
            Ok(Some(Err(e))) => return Incoming::Failed(e),
            Ok(Some(Ok(frame))) => {
                if let Some(incoming) = incoming(frame) {
                    return incoming;
                }
            }
        }
    }
}

/// What a frame means to a session, or `None` for a control frame.
fn incoming(frame: WsMessage) -> Option<Incoming> {
    match frame {
        WsMessage::Text(text) => Some(match serde_json::from_str(text.as_str()) {
            Ok(msg) => Incoming::Msg(msg),
            Err(e) => match hello_version_mismatch(text.as_str()) {
                Some(reason) => Incoming::BadVersion(reason),
                None => match unparsable_report_id(text.as_str()) {
                    // A report with mistyped fields is rejected by `check` like any other bad
                    // one (its kind is empty), instead of ending the session.
                    Some(id) => Incoming::Msg(Message::Report(Report {
                        id,
                        ..Default::default()
                    })),
                    None => Incoming::BadRequest(e.to_string()),
                },
            },
        }),
        WsMessage::Close(_) => Some(Incoming::Closed),
        WsMessage::Binary(_) => Some(Incoming::BadRequest(
            "binary frames are not supported".into(),
        )),
        _ => None,
    }
}

/// Sends one message. A peer that stops reading ends the session instead of parking it.
pub(super) async fn send<S>(tx: &mut WsSink<S>, msg: &Message) -> Result<(), WsError>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    let json = serde_json::to_string(msg).expect("protocol messages serialize");
    send_frame(tx, WsMessage::text(json)).await
}

async fn send_frame<S>(tx: &mut WsSink<S>, frame: WsMessage) -> Result<(), WsError>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    match timeout(WRITE_TIMEOUT, tx.send(frame)).await {
        Ok(sent) => sent,
        Err(_) => Err(WsError::Io(io::Error::new(
            io::ErrorKind::TimedOut,
            format!("send timed out after {WRITE_TIMEOUT:?}"),
        ))),
    }
}

pub(super) async fn send_error<S>(
    tx: &mut WsSink<S>,
    code: ErrorCode,
    msg: String,
) -> Result<(), WsError>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    send(tx, &Message::Error { code, msg }).await
}

pub(super) async fn close<S>(tx: &mut WsSink<S>)
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    match timeout(WRITE_TIMEOUT, tx.close()).await {
        Ok(Ok(())) => {}
        Ok(Err(e)) => log::debug!("close: {e}"),
        Err(_) => log::warn!("close timed out after {WRITE_TIMEOUT:?}"),
    }
}

/// For a `report` that failed to parse, its `id` if that is a valid one and empty if not.
fn unparsable_report_id(text: &str) -> Option<String> {
    let value: serde_json::Value = serde_json::from_str(text).ok()?;
    if value.get("t")? != "report" {
        return None;
    }
    let id = value.get("id").and_then(|id| id.as_str()).unwrap_or("");
    Some(
        if decode_hex::<4>(id).is_some() {
            id
        } else {
            ""
        }
        .to_string(),
    )
}

/// `Some(reason)` when `text` is a hello whose `v` is not exactly 1, whatever else is wrong with it.
fn hello_version_mismatch(text: &str) -> Option<String> {
    let value: serde_json::Value = serde_json::from_str(text).ok()?;
    if value.get("t")? != "hello" {
        return None;
    }
    let v = value.get("v")?;
    (*v != VERSION).then(|| format!("unsupported protocol version {v}"))
}

async fn session(stream: TcpStream, peer: SocketAddr, shared: &Shared) -> Result<(), WsError> {
    let config = WebSocketConfig::default()
        .max_message_size(Some(MAX_MESSAGE_BYTES))
        .max_frame_size(Some(MAX_MESSAGE_BYTES));
    let deadline = Instant::now() + HELLO_TIMEOUT;

    let tls = match timeout_at(deadline, shared.tls.accept(stream)).await {
        Ok(Ok(tls)) => tls,
        Ok(Err(e)) => {
            log::debug!("{peer}: TLS handshake failed: {e}");
            return Ok(());
        }
        Err(_) => {
            log::debug!("{peer}: TLS handshake did not finish in {HELLO_TIMEOUT:?}");
            return Ok(());
        }
    };
    let (_, conn) = tls.get_ref();
    if let (Some(suite), Some(kx)) = (
        conn.negotiated_cipher_suite(),
        conn.negotiated_key_exchange_group(),
    ) {
        log::debug!(
            "{peer}: {:?} {:?} {:?}",
            suite.version().version,
            suite.suite(),
            kx.name()
        );
    }

    let Ok(ws) = timeout_at(deadline, accept_async_with_config(tls, Some(config))).await else {
        log::debug!("{peer}: WebSocket upgrade did not finish in {HELLO_TIMEOUT:?}");
        return Ok(());
    };
    let (mut tx, mut rx) = ws?.split();
    let outcome = converse(&mut tx, &mut rx, peer, deadline, shared).await;
    close(&mut tx).await;
    drain(&mut rx).await;
    outcome
}

/// Reads and drops whatever the device still sends until it closes too, for at most
/// `DRAIN_TIMEOUT`. Closing a socket that holds unread input resets the connection, and the reset
/// discards frames still queued to send, such as the error that says why the session ended.
async fn drain<S>(rx: &mut WsRead<S>)
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    let until_closed = async { while let Some(Ok(_)) = rx.next().await {} };
    if timeout(DRAIN_TIMEOUT, until_closed).await.is_err() {
        log::debug!("the device did not close within {DRAIN_TIMEOUT:?}");
    }
}

/// The hello, then the session it opens. The caller closes the connection however this ends.
async fn converse<S>(
    tx: &mut WsSink<S>,
    rx: &mut WsRead<S>,
    peer: SocketAddr,
    deadline: Instant,
    shared: &Shared,
) -> Result<(), WsError>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    let hello = match timeout_at(deadline, next_message(rx)).await {
        Ok(incoming) => incoming,
        Err(_) => {
            log::warn!("no hello within {HELLO_TIMEOUT:?}");
            return Ok(());
        }
    };
    let (device, token, fw, screen, caps) = match hello {
        Incoming::Msg(Message::Hello {
            v,
            device,
            token,
            fw,
            screen,
            caps,
        }) => {
            if v != VERSION {
                let why = format!("unsupported protocol version {v}");
                return send_error(tx, ErrorCode::Version, why).await;
            }
            if !valid_device_id(&device) {
                return send_error(tx, ErrorCode::BadRequest, "invalid device id".into()).await;
            }
            // Names this daemon doesn't know are a newer device's, and mean nothing here. Built
            // from CAPS, so the list is at most CAPS long whatever the device repeats.
            let caps: Vec<String> = CAPS
                .iter()
                .filter(|known| caps.iter().any(|cap| cap == *known))
                .map(|known| (*known).to_owned())
                .collect();
            (device, token, fw, screen, caps)
        }
        Incoming::BadRequest(msg) => return send_error(tx, ErrorCode::BadRequest, msg).await,
        Incoming::BadVersion(msg) => return send_error(tx, ErrorCode::Version, msg).await,
        Incoming::Msg(_) => {
            return send_error(tx, ErrorCode::BadRequest, "expected hello".into()).await
        }
        Incoming::Idle | Incoming::Closed => return Ok(()),
        Incoming::Failed(e) => return Err(e),
    };

    let token = token.and_then(|Token(hex)| decode_hex::<TOKEN_BYTES>(&hex));
    // The registry keeps the caps for the Devices tab, and the session its own copy.
    let session_caps = caps.clone();
    let Some((id, kick)) = token.and_then(|token| shared.authenticate(&device, &token, caps))
    else {
        log::warn!("{peer}: refused {device}: not paired, or a missing or wrong token");
        return send_error(tx, ErrorCode::Unauthorized, UNAUTHORIZED.into()).await;
    };
    log::info!(
        "hello from {device} (fw {fw:?}, screen {}x{})",
        screen[0],
        screen[1]
    );
    let outcome = serve(tx, rx, kick, shared, &device, &session_caps).await;
    shared.unregister(&device, id);
    outcome
}

/// A finished run, on its way back to the session whose press started it.
struct Done {
    btn: String,
    seq: u64,
    result: ActionResult,
}

fn result_message(btn: String, seq: u64, result: ActionResult) -> Message {
    Message::Result {
        btn,
        seq,
        ok: result.ok,
        code: result.code,
        summary: result.summary,
        text: result.text,
        ms: result.ms,
    }
}

/// What one session has told its device about tiles. The poller never waits on a session: it
/// publishes values to a watch channel, and each session reads them at its own pace.
struct TileFeed {
    values: watch::Receiver<Values>,
    /// What the device last got, so only changes are sent.
    sent: Sent,
}

impl TileFeed {
    /// Forget what the device has, as after a new layout, which blanks every tile.
    fn reset(&mut self) {
        self.sent.clear();
    }

    /// Sends each tile of `layout` whose value the device doesn't have, leaving out any reading
    /// made under a tile definition other than the one in `live`. A kick stops it between
    /// messages, and the result says how the session ends: `Some(kick)`, where `kick` is `None`
    /// if the registry just dropped the session.
    async fn push<S>(
        &mut self,
        tx: &mut WsSink<S>,
        kick: &mut oneshot::Receiver<Kick>,
        layout: &Layout,
        live: &Live,
    ) -> Result<Option<Option<Kick>>, WsError>
    where
        S: AsyncRead + AsyncWrite + Unpin,
    {
        // A copy, because a watch guard held across a send would stall the poller's next update.
        let values = self.values.borrow_and_update().clone();
        for button in layout.buttons().filter(|b| b.tile) {
            let Some(reading) = values.get(&button.id) else {
                continue;
            };
            // The poller drops a changed tile's value, but not before a session may look.
            if live.button(&button.id).and_then(|b| b.tile.as_ref()) != Some(&reading.tile) {
                continue;
            }
            let value = &reading.value;
            if self.sent.get(&button.id) == Some(value) {
                continue;
            }
            match kick.try_recv() {
                Err(oneshot::error::TryRecvError::Empty) => {}
                kicked => return Ok(Some(kicked.ok())),
            }
            let msg = Message::Tile {
                btn: button.id.clone(),
                text: value.text.clone(),
                state: value.state,
            };
            send(tx, &msg).await?;
            self.sent.insert(button.id.clone(), value.clone());
        }
        Ok(None)
    }
}

/// The session after a good hello. Presses run as tasks of their own, so the session keeps
/// reading (and answering pings) and pushing layouts and tiles while actions run. A run whose
/// session has ended still finishes, and its result is dropped. A session without the `fonts`
/// capability sees the layout with its font fields stripped, so a font-only edit changes nothing
/// it sees and pushes nothing. One without `pages` sees the home page alone, so an edit to the
/// other pages pushes it nothing either. Tiles and icons come from what the session sees.
async fn serve<S>(
    tx: &mut WsSink<S>,
    rx: &mut WsRead<S>,
    mut kick: oneshot::Receiver<Kick>,
    shared: &Shared,
    device: &str,
    caps: &[String],
) -> Result<(), WsError>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    let mut config = shared.config.clone();
    send(
        tx,
        &Message::Welcome {
            v: VERSION,
            name: shared.hostname.clone(),
        },
    )
    .await?;
    let fonts = caps.iter().any(|cap| cap == CAP_FONTS);
    let pages = caps.iter().any(|cap| cap == CAP_PAGES);
    let mut told_home_only = false;
    let mut view = |live: &Live| {
        let layout = live.layout();
        let layout = if fonts {
            layout
        } else {
            layout.without_fonts()
        };
        if pages {
            return layout;
        }
        let count = live.pages.len();
        if count >= 2 && !told_home_only {
            log::info!("{device}: firmware without pages, showing page 1 of {count}");
            told_home_only = true;
        }
        layout.home_only()
    };
    let mut layout = view(&config.borrow_and_update());
    let mut rev = 1;
    for message in layout.messages(rev) {
        send(tx, &message).await?;
    }
    let mut tiles = TileFeed {
        values: shared.tiles.values(),
        sent: Sent::new(),
    };
    let live = config.borrow().clone();
    if let Some(kicked) = tiles.push(tx, &mut kick, &layout, &live).await? {
        return end_kicked(tx, kicked, device).await;
    }
    // What the device knows of the sleep screen, and the picture its `asset_get`s are answered
    // from. Nothing is kept for a session that did not name `sleep`.
    let sleep_cap = caps.iter().any(|cap| cap == CAP_SLEEP);
    let mut sleeping = sleep_cap.then(|| live.sleep());
    let mut screen = sleep_cap.then(|| live.screen.clone()).flatten();
    if let Some(frame) = &sleeping {
        send(tx, &Message::Sleep(frame.clone())).await?;
    }

    let feed = shared.integrations.handle();
    let feed_on = caps.iter().any(|cap| cap == CAP_FEED);
    let mut feed_rx = feed.subscribe();
    // The first snapshot goes out with the session, even an empty one, which tells the board
    // there is no -1 page. Later ones wait for the gap.
    let mut feed_ready = Instant::now();
    if feed_on {
        send_feed(tx, &mut feed_rx).await?;
        feed_ready = Instant::now() + FEED_GAP;
    }

    let mut reports = 0;
    let mut acked = 0;
    let (done_tx, mut done_rx) = mpsc::channel::<Done>(MAX_RUNNING);
    let idle = sleep(IDLE_TIMEOUT);
    tokio::pin!(idle);
    let status_bar = caps.iter().any(|cap| cap == CAP_STATUS_BAR);
    // A clone starts out unseen, so the latest reading from before this session is marked seen
    // here; only readings from now on are sent.
    let mut stats = shared.stats.clone();
    stats.borrow_and_update();
    loop {
        let incoming = tokio::select! {
            // The kick goes first, so presses queued behind a revocation never run.
            biased;
            kicked = &mut kick => return end_kicked(tx, kicked.ok(), device).await,
            Some(done) = done_rx.recv() => {
                send(tx, &result_message(done.btn, done.seq, done.result)).await?;
                continue;
            }
            Ok(()) = config.changed() => {
                // An edit to actions alone leaves the layout as it was, and sends nothing.
                let live = config.borrow_and_update().clone();
                let next = view(&live);
                if next != layout {
                    layout = next;
                    rev += 1;
                    log::info!("{device}: pushing layout rev {rev}");
                    for message in layout.messages(rev) {
                        send(tx, &message).await?;
                    }
                    shared.emit(DaemonEvent::LayoutPushed(device.to_string(), rev));
                    tiles.reset();
                    if let Some(kicked) = tiles.push(tx, &mut kick, &layout, &live).await? {
                        return end_kicked(tx, kicked, device).await;
                    }
                }
                // An edit to `[sleep]` alone changes this and not the layout.
                if let Some(known) = &mut sleeping {
                    let next = live.sleep();
                    if next != *known {
                        log::info!("{device}: pushing sleep");
                        send(tx, &Message::Sleep(next.clone())).await?;
                        *known = next;
                    }
                    screen = live.screen.clone();
                }
                continue;
            }
            Ok(()) = tiles.values.changed() => {
                let live = config.borrow().clone();
                if let Some(kicked) = tiles.push(tx, &mut kick, &layout, &live).await? {
                    return end_kicked(tx, kicked, device).await;
                }
                continue;
            }
            // The gap is waited here, not in the arm, so presses and pings go on meanwhile.
            Ok(()) = async {
                sleep_until(feed_ready).await;
                feed_rx.changed().await
            }, if feed_on => {
                send_feed(tx, &mut feed_rx).await?;
                feed_ready = Instant::now() + FEED_GAP;
                continue;
            }
            Ok(()) = stats.changed(), if status_bar => {
                // Copied out, so no watch guard is held across the send.
                let reading = *stats.borrow_and_update();
                if let Some(reading) = reading {
                    let now = OffsetDateTime::now_utc();
                    let time = u64::try_from(now.unix_timestamp()).unwrap_or_default();
                    let offset = hoststats::utc_offset_seconds(now);
                    send(tx, &stats_frame(reading, time, offset)).await?;
                }
                continue;
            }
            frame = rx.next() => {
                idle.as_mut().reset(Instant::now() + IDLE_TIMEOUT);
                match frame {
                    None => Incoming::Closed,
                    Some(Err(e)) => Incoming::Failed(e),
                    Some(Ok(frame)) => match incoming(frame) {
                        Some(incoming) => incoming,
                        None => continue,
                    },
                }
            }
            () = &mut idle => Incoming::Idle,
        };
        match incoming {
            Incoming::Msg(Message::Press { btn, .. }) if btn.len() > MAX_BUTTON_ID_BYTES => {
                // The result echoes the id, which must keep it under the device's 16 KB cap.
                send_error(tx, ErrorCode::BadRequest, "button id too long".into()).await?;
                return Ok(());
            }
            Incoming::Msg(Message::Press { btn, seq }) => {
                // The kick may have landed while this press was being read.
                match kick.try_recv() {
                    Err(oneshot::error::TryRecvError::Empty) => {}
                    kicked => return end_kicked(tx, kicked.ok(), device).await,
                }
                match shared.begin_press(&btn) {
                    Ok((run, action)) => {
                        let done = done_tx.clone();
                        let poker = shared.tiles.poker();
                        let device = device.to_string();
                        tokio::spawn(async move {
                            let result = run.run(&action).await;
                            log::info!("{device}: {btn:?}: {} ({} ms)", result.summary, result.ms);
                            // The press may have changed what the button's tile shows.
                            poker.poke(&btn);
                            // An error only means the session has ended.
                            let _ = done.send(Done { btn, seq, result }).await;
                        });
                    }
                    Err(refused) => {
                        // Debug escapes what an unknown id may carry, such as newlines or ANSI.
                        log::info!("{device}: {btn:?}: {}", refused.summary);
                        send(tx, &result_message(btn, seq, refused)).await?;
                    }
                }
            }
            Incoming::Msg(Message::LayoutAck { rev: applied }) => {
                // A rev never sent, or no newer than the last ack, tells the window nothing.
                if applied == 0 || applied > rev || applied <= acked {
                    log::warn!("{device}: ignoring layout_ack for rev {applied} (sent {rev}, acked {acked})");
                } else {
                    acked = applied;
                    shared.emit(DaemonEvent::LayoutApplied(device.to_string(), applied));
                }
            }
            Incoming::Msg(Message::IconGet { ids }) => {
                let Some(wanted) = icon_ids(&ids) else {
                    let why = format!(
                        "icon_get takes at most {MAX_ICON_IDS} ids of {} lowercase hex digits",
                        2 * ICON_ID_BYTES
                    );
                    send_error(tx, ErrorCode::BadRequest, why).await?;
                    return Ok(());
                };
                let mut sent = Vec::new();
                for id in wanted {
                    if sent.contains(&id) {
                        continue;
                    }
                    sent.push(id);
                    // A revoke ends the session before the next frame, not after the last.
                    match kick.try_recv() {
                        Err(oneshot::error::TryRecvError::Empty) => {}
                        kicked => return end_kicked(tx, kicked.ok(), device).await,
                    }
                    // An icon the layout dropped since the device asked is no longer wanted.
                    match layout.icons.iter().find(|icon| icon.id == id) {
                        Some(icon) => {
                            let frame = WsMessage::Binary(Bytes::from_owner(icon.wire()));
                            send_frame(tx, frame).await?
                        }
                        None => {
                            log::info!("{device}: icon {} is not in the layout", encode_hex(&id))
                        }
                    }
                }
            }
            Incoming::Msg(Message::FeedDismiss(dismiss)) if feed_on => {
                if let Err(why) = dismiss.check() {
                    send_error(tx, ErrorCode::BadRequest, why).await?;
                    return Ok(());
                }
                // An unknown id is ignored: another board may have dismissed it first.
                if feed.dismiss(&dismiss.id) {
                    log::info!("{device}: dismissed note {:?}", dismiss.id);
                }
            }
            Incoming::Msg(Message::AssetGet(get)) if sleep_cap => {
                // The size of an image is fixed, so a request past its end is malformed whichever
                // image it names. Only an id that is not the current image goes unanswered.
                let refused = get.check().err().or_else(|| {
                    (get.offset >= IMAGE_BYTES as u64)
                        .then(|| format!("offset is not below the image's {IMAGE_BYTES} bytes"))
                });
                if let Some(why) = refused {
                    send_error(tx, ErrorCode::BadRequest, why).await?;
                    return Ok(());
                }
                let id = decode_hex::<ASSET_ID_BYTES>(&get.id).expect("checked with the request");
                match kick.try_recv() {
                    Err(oneshot::error::TryRecvError::Empty) => {}
                    kicked => return end_kicked(tx, kicked.ok(), device).await,
                }
                let frame = screen.as_ref().filter(|image| image.id == id).map(|image| {
                    image
                        .chunk(get.offset)
                        .and_then(|chunk| encode_asset_frame(&id, get.offset, chunk))
                });
                match frame {
                    Some(Some(frame)) => {
                        send_frame(tx, WsMessage::Binary(Bytes::from(frame))).await?
                    }
                    // Unreachable for a checked offset below the size. Dropped, the device's
                    // timeout ends the download.
                    Some(None) => log::error!("{device}: no chunk at {}", get.offset),
                    None => log::info!("{device}: asset {} is not the current image", get.id),
                }
            }
            Incoming::Msg(Message::Report(report)) => {
                reports += 1;
                // The device resends an unacked report on every session, so a bad or surplus one
                // is dropped without ending this one. Its values stay out of the log.
                if reports > MAX_REPORTS_PER_SESSION {
                    log::warn!(
                        "{device}: dropping a report; a session takes {MAX_REPORTS_PER_SESSION}"
                    );
                } else if let Err(why) = report.check() {
                    // With a usable id the device is told to drop it. Otherwise it would send
                    // the same bad report on every session.
                    if decode_hex::<4>(&report.id).is_some() {
                        log::warn!("{device}: rejecting report {}: {why}", report.id);
                        send(tx, &Message::ReportAck { id: report.id }).await?;
                    } else {
                        log::warn!("{device}: dropping a report: {why}");
                    }
                } else {
                    log::warn!("{device}: {report}");
                    send(tx, &Message::ReportAck { id: report.id }).await?;
                }
            }
            Incoming::Msg(Message::Unknown) => {
                log::warn!("{device}: ignoring unknown message type")
            }
            Incoming::Msg(other) => log::warn!("{device}: ignoring unexpected message {other:?}"),
            Incoming::BadRequest(msg) | Incoming::BadVersion(msg) => {
                send_error(tx, ErrorCode::BadRequest, msg).await?;
                return Ok(());
            }
            Incoming::Idle => {
                log::info!("{device}: silent for {IDLE_TIMEOUT:?}, dropping");
                return Ok(());
            }
            Incoming::Closed => {
                log::info!("{device}: disconnected");
                return Ok(());
            }
            Incoming::Failed(e) => return Err(e),
        }
    }
}

/// Sends the feed's latest snapshot, which `rx` then counts as seen. One that breaks the wire's
/// rules is logged and skipped, since the board would refuse it.
async fn send_feed<S>(
    tx: &mut WsSink<S>,
    rx: &mut watch::Receiver<Arc<Feed>>,
) -> Result<(), WsError>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    // Cloned out, so no watch guard is held across the send.
    let feed = rx.borrow_and_update().clone();
    if let Err(why) = feed.check() {
        log::error!("not sending the feed: {why}");
        return Ok(());
    }
    send(tx, &Message::Feed((*feed).clone())).await
}

/// The ids of an `icon_get`, or `None` if it has too many or one is malformed.
fn icon_ids(ids: &[String]) -> Option<Vec<[u8; ICON_ID_BYTES]>> {
    if ids.len() > MAX_ICON_IDS {
        return None;
    }
    ids.iter().map(|id| decode_hex(id)).collect()
}

/// Largest integer the wire carries exactly, 2^53 - 1 (`site/src/protocol.md`).
const MAX_WIRE_INT: u64 = (1 << 53) - 1;
/// The widest UTC offset the wire carries, in seconds either way (`site/src/protocol.md`).
const MAX_UTC_OFFSET: i32 = 64_800;
/// The wire's range for a temperature in whole °C.
const TEMP_RANGE: std::ops::RangeInclusive<i32> = -40..=150;
/// The largest `0xRRGGBB`.
const MAX_COLOR: u32 = 0xFF_FFFF;

/// A `stats` frame for one reading. A field outside its wire range is left out, and so is each
/// pair that is half missing or breaks its rule, so the device never refuses the frame.
fn stats_frame(stats: HostStats, time: u64, utc_offset: Option<i32>) -> Message {
    let cpu = stats.cpu.filter(|cpu| *cpu <= 100).map(u32::from);
    let mem = stats
        .mem
        .filter(|m| m.total >= 1 && m.used <= m.total && m.total <= MAX_WIRE_INT);
    let net = stats
        .net
        .filter(|n| n.rx <= MAX_WIRE_INT && n.tx <= MAX_WIRE_INT);
    // A sensor reading 511 °C or -273 °C costs its value, never the frame.
    let gpu = stats.gpu.unwrap_or_default();
    let gpu_busy = gpu.busy.filter(|busy| *busy <= 100);
    let cpu_temp = stats.cpu_temp.filter(|t| TEMP_RANGE.contains(t));
    let gpu_temp = gpu.temp.filter(|t| TEMP_RANGE.contains(t));
    // A color goes with its value, and only if it is a color.
    let color = |c: Option<u32>, shown: bool| c.filter(|c| shown && *c <= MAX_COLOR);
    let colors = Colors {
        cpu: color(stats.colors.cpu, cpu.is_some()),
        mem: color(stats.colors.mem, mem.is_some()),
        gpu: color(stats.colors.gpu, gpu_busy.is_some()),
        cpu_temp: color(stats.colors.cpu_temp, cpu_temp.is_some()),
        gpu_temp: color(stats.colors.gpu_temp, gpu_temp.is_some()),
        net_rx: color(stats.colors.net_rx, net.is_some()),
        net_tx: color(stats.colors.net_tx, net.is_some()),
    };
    Message::Stats {
        time,
        utc_offset: utc_offset.filter(|o| (-MAX_UTC_OFFSET..=MAX_UTC_OFFSET).contains(o)),
        cpu,
        mem_used: mem.map(|m| m.used),
        mem_total: mem.map(|m| m.total),
        net_rx: net.map(|n| n.rx),
        net_tx: net.map(|n| n.tx),
        gpu: gpu_busy,
        cpu_temp,
        gpu_temp,
        temp_unit: Some(stats.units.temp),
        net_unit: Some(stats.units.net),
        colors: (colors != Colors::default()).then_some(colors),
    }
}

/// Ends a session whose kick fired. A revoked device is told `unauthorized` first, so it drops
/// its pairing instead of retrying a dead token.
async fn end_kicked<S>(tx: &mut WsSink<S>, kick: Option<Kick>, device: &str) -> Result<(), WsError>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    if kick == Some(Kick::Revoked) {
        log::info!("{device}: pairing revoked, closing its session");
        send_error(tx, ErrorCode::Unauthorized, UNAUTHORIZED.into()).await?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::super::gpu::GpuReading;
    use super::super::hoststats::{Mem, Net};
    use super::super::protocol::{NetUnit, TempUnit};
    use super::super::units::Units;
    use super::*;

    fn cpu_of(msg: Message) -> Option<u32> {
        let Message::Stats { cpu, .. } = msg else {
            panic!("expected a stats frame")
        };
        cpu
    }

    fn mem_of(msg: Message) -> Option<(u64, u64)> {
        let Message::Stats {
            mem_used,
            mem_total,
            ..
        } = msg
        else {
            panic!("expected a stats frame")
        };
        mem_used.zip(mem_total)
    }

    fn net_of(msg: Message) -> Option<(u64, u64)> {
        let Message::Stats { net_rx, net_tx, .. } = msg else {
            panic!("expected a stats frame")
        };
        net_rx.zip(net_tx)
    }

    fn full() -> HostStats {
        HostStats {
            cpu: Some(12),
            mem: Some(Mem {
                used: 10,
                total: 20,
            }),
            net: Some(Net { rx: 5, tx: 6 }),
            ..HostStats::default()
        }
    }

    #[test]
    fn a_full_reading_maps_every_field() {
        let msg = stats_frame(full(), 1_700_000_000, Some(-14_400));
        assert_eq!(
            msg,
            Message::Stats {
                time: 1_700_000_000,
                utc_offset: Some(-14_400),
                cpu: Some(12),
                mem_used: Some(10),
                mem_total: Some(20),
                net_rx: Some(5),
                net_tx: Some(6),
                gpu: None,
                cpu_temp: None,
                gpu_temp: None,
                temp_unit: Some(TempUnit::C),
                net_unit: Some(NetUnit::Si),
                colors: None,
            }
        );
    }

    #[test]
    fn an_empty_reading_sends_only_the_time() {
        let msg = stats_frame(HostStats::default(), 7, None);
        assert_eq!(
            msg,
            Message::Stats {
                time: 7,
                utc_offset: None,
                cpu: None,
                mem_used: None,
                mem_total: None,
                net_rx: None,
                net_tx: None,
                gpu: None,
                cpu_temp: None,
                gpu_temp: None,
                temp_unit: Some(TempUnit::C),
                net_unit: Some(NetUnit::Si),
                colors: None,
            }
        );
    }

    #[test]
    fn the_wire_bounds_are_inclusive() {
        let edge = HostStats {
            cpu: Some(100),
            mem: Some(Mem {
                used: MAX_WIRE_INT,
                total: MAX_WIRE_INT,
            }),
            net: Some(Net {
                rx: MAX_WIRE_INT,
                tx: 0,
            }),
            ..HostStats::default()
        };
        let msg = stats_frame(edge, 1, Some(MAX_UTC_OFFSET));
        assert_eq!(cpu_of(msg.clone()), Some(100));
        assert_eq!(mem_of(msg.clone()), Some((MAX_WIRE_INT, MAX_WIRE_INT)));
        assert_eq!(net_of(msg), Some((MAX_WIRE_INT, 0)));
        let Message::Stats { utc_offset, .. } = stats_frame(edge, 1, Some(-MAX_UTC_OFFSET)) else {
            unreachable!()
        };
        assert_eq!(utc_offset, Some(-MAX_UTC_OFFSET));
    }

    #[test]
    fn out_of_range_values_are_left_out() {
        let bad = HostStats {
            cpu: Some(101),
            mem: Some(Mem {
                used: 1,
                total: MAX_WIRE_INT + 1,
            }),
            net: Some(Net {
                rx: 1,
                tx: MAX_WIRE_INT + 1,
            }),
            ..HostStats::default()
        };
        let msg = stats_frame(bad, 1, Some(MAX_UTC_OFFSET + 1));
        let Message::Stats { utc_offset, .. } = msg.clone() else {
            unreachable!()
        };
        assert_eq!(utc_offset, None);
        assert_eq!(cpu_of(msg.clone()), None);
        assert_eq!(mem_of(msg.clone()), None);
        assert_eq!(net_of(msg), None);
    }

    #[test]
    fn memory_needs_a_total_of_at_least_one_and_used_within_it() {
        let no_total = HostStats {
            mem: Some(Mem { used: 0, total: 0 }),
            ..HostStats::default()
        };
        assert_eq!(mem_of(stats_frame(no_total, 1, None)), None);
        let over = HostStats {
            mem: Some(Mem { used: 3, total: 2 }),
            ..HostStats::default()
        };
        assert_eq!(mem_of(stats_frame(over, 1, None)), None);
        let fine = HostStats {
            mem: Some(Mem { used: 0, total: 1 }),
            ..HostStats::default()
        };
        assert_eq!(mem_of(stats_frame(fine, 1, None)), Some((0, 1)));
    }

    #[test]
    fn a_network_pair_is_all_or_nothing() {
        let one_over = HostStats {
            net: Some(Net {
                rx: MAX_WIRE_INT + 1,
                tx: 9,
            }),
            ..HostStats::default()
        };
        assert_eq!(net_of(stats_frame(one_over, 1, None)), None);
    }

    /// The GPU, temperature and color fields of a frame.
    fn bar_of(msg: Message) -> (Option<u32>, Option<i32>, Option<i32>, Option<Colors>) {
        let Message::Stats {
            gpu,
            cpu_temp,
            gpu_temp,
            colors,
            ..
        } = msg
        else {
            panic!("expected a stats frame")
        };
        (gpu, cpu_temp, gpu_temp, colors)
    }

    fn sensors(cpu_temp: i32, busy: u32, gpu_temp: i32) -> HostStats {
        HostStats {
            cpu_temp: Some(cpu_temp),
            gpu: Some(GpuReading {
                busy: Some(busy),
                temp: Some(gpu_temp),
            }),
            ..HostStats::default()
        }
    }

    #[test]
    fn temperatures_and_gpu_pass_through_inside_their_ranges() {
        for edge in [-40, 150] {
            let got = bar_of(stats_frame(sensors(edge, 100, edge), 1, None));
            assert_eq!(got, (Some(100), Some(edge), Some(edge), None));
        }
    }

    #[test]
    fn a_bad_sensor_costs_its_value_and_its_color_and_never_the_frame() {
        let colors = Colors {
            cpu_temp: Some(1),
            gpu_temp: Some(2),
            gpu: Some(3),
            ..Colors::default()
        };
        for (cpu_temp, busy, gpu_temp, left) in [
            (511, 50, 40, (Some(50), None, Some(40))),
            (151, 50, 40, (Some(50), None, Some(40))),
            (-41, 50, 40, (Some(50), None, Some(40))),
            (60, 50, 511, (Some(50), Some(60), None)),
            (60, 101, -273, (None, Some(60), None)),
        ] {
            let stats = HostStats {
                colors,
                ..sensors(cpu_temp, busy, gpu_temp)
            };
            let (gpu, cpu_t, gpu_t, got) = bar_of(stats_frame(stats, 1, None));
            assert_eq!((gpu, cpu_t, gpu_t), left, "{cpu_temp} {busy} {gpu_temp}");
            let got = got.unwrap_or_default();
            assert_eq!(got.cpu_temp.is_some(), cpu_t.is_some());
            assert_eq!(got.gpu_temp.is_some(), gpu_t.is_some());
            assert_eq!(got.gpu.is_some(), gpu.is_some());
        }
    }

    #[test]
    fn a_color_over_ffffff_is_left_out_and_an_empty_colors_object_is_not_sent() {
        let stats = |cpu_color| HostStats {
            cpu: Some(50),
            colors: Colors {
                cpu: Some(cpu_color),
                ..Colors::default()
            },
            ..HostStats::default()
        };
        let (_, _, _, colors) = bar_of(stats_frame(stats(0x1000000), 1, None));
        assert_eq!(colors, None);
        let (_, _, _, colors) = bar_of(stats_frame(stats(0xFFFFFF), 1, None));
        assert_eq!(colors.unwrap().cpu, Some(0xFFFFFF));
        // A color for a value that is not there.
        let orphan = HostStats {
            colors: Colors {
                mem: Some(5),
                ..Colors::default()
            },
            ..HostStats::default()
        };
        assert_eq!(bar_of(stats_frame(orphan, 1, None)).3, None);
    }

    #[test]
    fn both_units_are_always_sent() {
        let stats = HostStats {
            units: Units {
                temp: TempUnit::F,
                net: NetUnit::Bits,
            },
            ..HostStats::default()
        };
        let Message::Stats {
            temp_unit,
            net_unit,
            ..
        } = stats_frame(stats, 1, None)
        else {
            panic!("expected a stats frame")
        };
        assert_eq!(
            (temp_unit, net_unit),
            (Some(TempUnit::F), Some(NetUnit::Bits))
        );
    }
}
