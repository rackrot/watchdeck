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
use tokio::time::{sleep, timeout, timeout_at, Instant};
use tokio_rustls::rustls::ServerConfig;
use tokio_rustls::TlsAcceptor;
use tokio_tungstenite::tungstenite::protocol::WebSocketConfig;
use tokio_tungstenite::tungstenite::Bytes;
use tokio_tungstenite::tungstenite::{Error as WsError, Message as WsMessage};
use tokio_tungstenite::{accept_async_with_config, WebSocketStream};

use super::actions::{ActionResult, Actions, MAX_RUNNING};
use super::config::{Live, MAX_COLS, MAX_ROWS};
use super::protocol::{
    decode_hex, encode_hex, valid_device_id, ErrorCode, Message, Token, ICON_ID_BYTES,
    MAX_MESSAGE_BYTES, TOKEN_BYTES, VERSION,
};
use super::store::Store;

pub const PORT: u16 = 47820;
/// From the TCP accept to the hello, covering the TLS handshake and the WebSocket upgrade.
const HELLO_TIMEOUT: Duration = Duration::from_secs(5);
const IDLE_TIMEOUT: Duration = Duration::from_secs(30);
const WRITE_TIMEOUT: Duration = Duration::from_secs(5);
/// How long a closing session waits for the device to close too.
const DRAIN_TIMEOUT: Duration = Duration::from_secs(2);
const MAX_BUTTON_ID_BYTES: usize = 64;
/// Ids in one `icon_get`: a layout never names more icons than it has cells.
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
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct DeviceInfo {
    pub id: String,
    pub connected: bool,
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
    /// One lock, so a session is only ever registered under a pairing that still exists.
    registry: Mutex<Registry>,
}

struct Registry {
    store: Store,
    /// device id -> (session id, kick that closes the session)
    sessions: HashMap<String, (u64, oneshot::Sender<Kick>)>,
}

impl Server {
    /// Sessions show the layout of the live config in `config`, and push it again whenever it
    /// changes. Presses run through `actions`.
    pub async fn bind(
        addr: impl ToSocketAddrs,
        tls: Arc<ServerConfig>,
        store: Store,
        config: watch::Receiver<Arc<Live>>,
        actions: Actions,
    ) -> io::Result<Self> {
        let hostname = gethostname::gethostname().to_string_lossy().into_owned();
        let (events, _) = broadcast::channel(64);
        Ok(Self {
            listener: TcpListener::bind(addr).await?,
            shared: Arc::new(Shared {
                hostname,
                tls: TlsAcceptor::from(tls),
                config,
                actions,
                events,
                next_id: AtomicU64::new(0),
                registry: Mutex::new(Registry {
                    store,
                    sessions: HashMap::new(),
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
    pub fn list(&self) -> Vec<DeviceInfo> {
        let registry = self.0.registry();
        registry
            .store
            .ids()
            .map(|id| DeviceInfo {
                id: id.to_string(),
                connected: registry.sessions.contains_key(id),
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
}

impl Registry {
    /// Tells `device`'s live session, if any, that its token is void. Returns whether there was one.
    fn revoke(&mut self, device: &str) -> bool {
        match self.sessions.remove(device) {
            Some((_, kick)) => {
                // An error only means the session is already ending.
                let _ = kick.send(Kick::Revoked);
                true
            }
            None => false,
        }
    }
}

impl Shared {
    fn registry(&self) -> std::sync::MutexGuard<'_, Registry> {
        self.registry.lock().expect("device registry poisoned")
    }

    /// Checks `token` for `device` and, only if it is valid, registers a session for it, kicking
    /// the one it replaces. An impostor using a paired device's id never gets this far, so it
    /// can't kick the real session.
    fn authenticate(
        &self,
        device: &str,
        token: &[u8; TOKEN_BYTES],
    ) -> Option<(u64, oneshot::Receiver<Kick>)> {
        let mut registry = self.registry();
        if !registry.store.verify(device, token) {
            return None;
        }
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let (kick_tx, kick_rx) = oneshot::channel();
        let old = registry.sessions.insert(device.to_string(), (id, kick_tx));
        drop(registry);
        if let Some((_, old_kick)) = old {
            log::info!("{device} reconnected, replacing its old session");
            // An error only means the old session is already ending.
            let _ = old_kick.send(Kick::Replaced);
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
            .is_some_and(|(current, _)| *current == id)
        {
            registry.sessions.remove(device);
            drop(registry);
            self.emit(DaemonEvent::DeviceDisconnected(device.to_string()));
        }
    }

    fn emit(&self, event: DaemonEvent) {
        // An error only means nobody is subscribed.
        let _ = self.events.send(event);
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
                None => Incoming::BadRequest(e.to_string()),
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
    let (device, token, fw, screen) = match hello {
        Incoming::Msg(Message::Hello {
            v,
            device,
            token,
            fw,
            screen,
        }) => {
            if v != VERSION {
                let why = format!("unsupported protocol version {v}");
                return send_error(tx, ErrorCode::Version, why).await;
            }
            if !valid_device_id(&device) {
                return send_error(tx, ErrorCode::BadRequest, "invalid device id".into()).await;
            }
            (device, token, fw, screen)
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
    let Some((id, kick)) = token.and_then(|token| shared.authenticate(&device, &token)) else {
        log::warn!("{peer}: refused {device}: not paired, or a missing or wrong token");
        return send_error(tx, ErrorCode::Unauthorized, UNAUTHORIZED.into()).await;
    };
    log::info!(
        "hello from {device} (fw {fw:?}, screen {}x{})",
        screen[0],
        screen[1]
    );
    let outcome = serve(tx, rx, kick, shared, &device).await;
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

/// The session after a good hello. Presses run as tasks of their own, so the session keeps
/// reading (and answering pings) and pushing layouts while actions run. A run whose session has
/// ended still finishes, and its result is dropped.
async fn serve<S>(
    tx: &mut WsSink<S>,
    rx: &mut WsRead<S>,
    mut kick: oneshot::Receiver<Kick>,
    shared: &Shared,
    device: &str,
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
    let mut layout = config.borrow_and_update().layout();
    let mut rev = 1;
    send(tx, &layout.message(rev)).await?;

    let (done_tx, mut done_rx) = mpsc::channel::<Done>(MAX_RUNNING);
    let idle = sleep(IDLE_TIMEOUT);
    tokio::pin!(idle);
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
                let next = config.borrow_and_update().layout();
                if next != layout {
                    layout = next;
                    rev += 1;
                    log::info!("{device}: pushing layout rev {rev}");
                    send(tx, &layout.message(rev)).await?;
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
                let action = config.borrow().button(&btn).map(|b| b.action.clone());
                let started = match action {
                    Some(action) => shared.actions.start(&btn).map(|run| (run, action)),
                    None => Err(ActionResult::failed("unknown button")),
                };
                match started {
                    Ok((run, action)) => {
                        let done = done_tx.clone();
                        let device = device.to_string();
                        tokio::spawn(async move {
                            let result = run.run(&action).await;
                            log::info!("{device}: {btn:?}: {} ({} ms)", result.summary, result.ms);
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

/// The ids of an `icon_get`, or `None` if it has too many or one is malformed.
fn icon_ids(ids: &[String]) -> Option<Vec<[u8; ICON_ID_BYTES]>> {
    if ids.len() > MAX_ICON_IDS {
        return None;
    }
    ids.iter().map(|id| decode_hex(id)).collect()
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
