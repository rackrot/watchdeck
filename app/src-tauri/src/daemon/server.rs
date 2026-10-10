//! Plain `ws://` listener: one task per connection, one live session per device id.

use std::collections::HashMap;
use std::io;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::stream::{SplitSink, SplitStream};
use futures_util::{SinkExt, StreamExt};
use tokio::net::{TcpListener, TcpStream, ToSocketAddrs};
use tokio::sync::{broadcast, oneshot};
use tokio::task::JoinSet;
use tokio::time::{timeout, timeout_at, Instant};
use tokio_tungstenite::tungstenite::protocol::WebSocketConfig;
use tokio_tungstenite::tungstenite::{Error as WsError, Message as WsMessage};
use tokio_tungstenite::{accept_async_with_config, WebSocketStream};

use super::actions::{self, ActionResult, Runner};
use super::protocol::{ErrorCode, Message, MAX_MESSAGE_BYTES, VERSION};

pub const PORT: u16 = 47820;
const HELLO_TIMEOUT: Duration = Duration::from_secs(5);
const IDLE_TIMEOUT: Duration = Duration::from_secs(30);

type Ws = WebSocketStream<TcpStream>;
type WsSink = SplitSink<Ws, WsMessage>;
type WsRead = SplitStream<Ws>;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DaemonEvent {
    DeviceConnected(String),
    DeviceDisconnected(String),
}

pub struct Server {
    listener: TcpListener,
    shared: Arc<Shared>,
}

struct Shared {
    hostname: String,
    runner: Runner,
    events: broadcast::Sender<DaemonEvent>,
    next_id: AtomicU64,
    /// device id -> (session id, kick that closes the session)
    sessions: Mutex<HashMap<String, (u64, oneshot::Sender<()>)>>,
}

impl Server {
    pub async fn bind(addr: impl ToSocketAddrs, runner: Runner) -> io::Result<Self> {
        let hostname = gethostname::gethostname().to_string_lossy().into_owned();
        let (events, _) = broadcast::channel(64);
        Ok(Self {
            listener: TcpListener::bind(addr).await?,
            shared: Arc::new(Shared {
                hostname,
                runner,
                events,
                next_id: AtomicU64::new(0),
                sessions: Mutex::new(HashMap::new()),
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

    /// Accepts connections until the future is dropped, which also drops every session.
    pub async fn run(self) {
        let mut sessions = JoinSet::new();
        loop {
            tokio::select! {
                accepted = self.listener.accept() => match accepted {
                    Ok((stream, peer)) => {
                        sessions.spawn(handle(stream, peer, self.shared.clone()));
                    }
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

impl Shared {
    /// Registers a session for `device`, kicking the one it replaces.
    fn register(&self, device: &str) -> (u64, oneshot::Receiver<()>) {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let (kick_tx, kick_rx) = oneshot::channel();
        let old = self
            .sessions
            .lock()
            .expect("session registry poisoned")
            .insert(device.to_string(), (id, kick_tx));
        if let Some((_, old_kick)) = old {
            log::info!("{device} reconnected, replacing its old session");
            // An error only means the old session is already ending.
            let _ = old_kick.send(());
            self.emit(DaemonEvent::DeviceDisconnected(device.to_string()));
        }
        self.emit(DaemonEvent::DeviceConnected(device.to_string()));
        (id, kick_rx)
    }

    fn unregister(&self, device: &str, id: u64) {
        let mut sessions = self.sessions.lock().expect("session registry poisoned");
        // A replaced session finds its slot taken and stays silent.
        if sessions
            .get(device)
            .is_some_and(|(current, _)| *current == id)
        {
            sessions.remove(device);
            drop(sessions);
            self.emit(DaemonEvent::DeviceDisconnected(device.to_string()));
        }
    }

    fn emit(&self, event: DaemonEvent) {
        // An error only means nobody is subscribed.
        let _ = self.events.send(event);
    }
}

async fn handle(stream: TcpStream, peer: SocketAddr, shared: Arc<Shared>) {
    if let Err(e) = session(stream, &shared).await {
        log::warn!("{peer}: {e}");
    }
}

enum Incoming {
    Msg(Message),
    BadRequest(String),
    Closed,
    Idle,
    Failed(WsError),
}

/// Next text message. Control frames are skipped (tungstenite answers pings itself).
/// `IDLE_TIMEOUT` applies per frame, so pings keep a session alive.
async fn next_message(rx: &mut WsRead) -> Incoming {
    loop {
        let frame = match timeout(IDLE_TIMEOUT, rx.next()).await {
            Err(_) => return Incoming::Idle,
            Ok(None) => return Incoming::Closed,
            Ok(Some(Err(e))) => return Incoming::Failed(e),
            Ok(Some(Ok(frame))) => frame,
        };
        match frame {
            WsMessage::Text(text) => {
                return match serde_json::from_str(text.as_str()) {
                    Ok(msg) => Incoming::Msg(msg),
                    Err(e) => Incoming::BadRequest(e.to_string()),
                }
            }
            WsMessage::Close(_) => return Incoming::Closed,
            WsMessage::Binary(_) => {
                return Incoming::BadRequest("binary frames are not supported".into())
            }
            _ => {}
        }
    }
}

async fn send(tx: &mut WsSink, msg: &Message) -> Result<(), WsError> {
    let json = serde_json::to_string(msg).expect("protocol messages serialize");
    tx.send(WsMessage::text(json)).await
}

async fn send_error(tx: &mut WsSink, code: ErrorCode, msg: String) -> Result<(), WsError> {
    send(tx, &Message::Error { code, msg }).await
}

async fn close(tx: &mut WsSink) {
    if let Err(e) = tx.close().await {
        log::debug!("close: {e}");
    }
}

fn valid_device_id(device: &str) -> bool {
    (1..=64).contains(&device.len()) && device.bytes().all(|b| b.is_ascii_graphic())
}

async fn session(stream: TcpStream, shared: &Shared) -> Result<(), WsError> {
    let config = WebSocketConfig::default()
        .max_message_size(Some(MAX_MESSAGE_BYTES))
        .max_frame_size(Some(MAX_MESSAGE_BYTES));
    let deadline = Instant::now() + HELLO_TIMEOUT;

    let Ok(ws) = timeout_at(deadline, accept_async_with_config(stream, Some(config))).await else {
        log::warn!("handshake did not finish in {HELLO_TIMEOUT:?}");
        return Ok(());
    };
    let (mut tx, mut rx) = ws?.split();

    let hello = match timeout_at(deadline, next_message(&mut rx)).await {
        Ok(incoming) => incoming,
        Err(_) => {
            log::warn!("no hello within {HELLO_TIMEOUT:?}");
            close(&mut tx).await;
            return Ok(());
        }
    };
    let device = match hello {
        Incoming::Msg(Message::Hello {
            v,
            device,
            fw,
            screen,
        }) => {
            if v != VERSION {
                send_error(
                    &mut tx,
                    ErrorCode::Version,
                    format!("unsupported protocol version {v}"),
                )
                .await?;
                close(&mut tx).await;
                return Ok(());
            }
            if !valid_device_id(&device) {
                send_error(&mut tx, ErrorCode::BadRequest, "invalid device id".into()).await?;
                close(&mut tx).await;
                return Ok(());
            }
            log::info!(
                "hello from {device} (fw {fw}, screen {}x{})",
                screen[0],
                screen[1]
            );
            device
        }
        Incoming::BadRequest(msg) => {
            send_error(&mut tx, ErrorCode::BadRequest, msg).await?;
            close(&mut tx).await;
            return Ok(());
        }
        Incoming::Msg(_) => {
            send_error(&mut tx, ErrorCode::BadRequest, "expected hello".into()).await?;
            close(&mut tx).await;
            return Ok(());
        }
        Incoming::Idle | Incoming::Closed => return Ok(()),
        Incoming::Failed(e) => return Err(e),
    };

    let (id, kick) = shared.register(&device);
    let outcome = serve(&mut tx, &mut rx, kick, shared, &device).await;
    shared.unregister(&device, id);
    close(&mut tx).await;
    outcome
}

async fn serve(
    tx: &mut WsSink,
    rx: &mut WsRead,
    mut kick: oneshot::Receiver<()>,
    shared: &Shared,
    device: &str,
) -> Result<(), WsError> {
    let buttons = actions::buttons();
    send(
        tx,
        &Message::Welcome {
            v: VERSION,
            name: shared.hostname.clone(),
        },
    )
    .await?;
    send(
        tx,
        &Message::Layout {
            rev: 1,
            cols: actions::COLS,
            rows: actions::ROWS,
            buttons: buttons.clone(),
        },
    )
    .await?;

    loop {
        let incoming = tokio::select! {
            _ = &mut kick => return Ok(()),
            incoming = next_message(rx) => incoming,
        };
        match incoming {
            Incoming::Msg(Message::Press { btn, seq }) => {
                let result = if buttons.iter().any(|b| b.id == btn) {
                    (shared.runner)(btn.clone(), device.to_string()).await
                } else {
                    ActionResult {
                        ok: false,
                        code: -1,
                        text: "unknown button".into(),
                        ms: 0,
                    }
                };
                send(
                    tx,
                    &Message::Result {
                        btn,
                        seq,
                        ok: result.ok,
                        code: result.code,
                        text: result.text,
                        ms: result.ms,
                    },
                )
                .await?;
            }
            Incoming::Msg(Message::Unknown) => {
                log::warn!("{device}: ignoring unknown message type")
            }
            Incoming::Msg(other) => log::warn!("{device}: ignoring unexpected message {other:?}"),
            Incoming::BadRequest(msg) => {
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
