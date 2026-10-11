//! The pairing listener on port 47821 (plain `ws://`). It is open only while the desktop's
//! pairing dialog is, for at most [`PAIR_WINDOW`], and runs the commit/reveal exchange in
//! `site/src/protocol.md`. Nothing secret crosses it: the device keeps its token and sends only
//! SHA-256(token), which the daemon stores once the user types in the code the device shows.

use std::io;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use aws_lc_rs::constant_time::verify_slices_are_equal;
use futures_util::StreamExt;
use serde::Serialize;
use sha2::{Digest, Sha256};
use tokio::io::{AsyncRead, AsyncWrite};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{oneshot, watch};
use tokio::task::JoinHandle;
use tokio::time::{sleep_until, timeout_at, Instant};
use tokio_tungstenite::accept_async_with_config;
use tokio_tungstenite::tungstenite::protocol::WebSocketConfig;
use tokio_tungstenite::tungstenite::Message as WsMessage;

use super::protocol::{
    decode_hex, encode_hex, pairing_code, valid_device_id, ErrorCode, Message, MAX_MESSAGE_BYTES,
    NONCE_BYTES, TOKEN_BYTES, VERSION,
};
use super::server::{close, next_message, send, send_error, Devices, Incoming, WsRead, WsSink};

pub const PAIR_PORT: u16 = 47821;
/// How long the listener stays open once the dialog opens it.
pub const PAIR_WINDOW: Duration = Duration::from_secs(120);
/// Each step before the code: the WebSocket upgrade, `pair_req` and `pair_nonce`.
const STEP_TIMEOUT: Duration = Duration::from_secs(10);
const CODE_DIGITS: usize = 6;

/// What the pairing dialog shows.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(tag = "state", rename_all = "snake_case")]
pub enum PairingState {
    /// No listener.
    Closed,
    /// Listening. No device has reached the code yet.
    Waiting,
    /// `device`, at IP address `peer`, shows a code, and the user must type it in.
    Confirm {
        device: String,
        peer: String,
    },
    Paired {
        device: String,
    },
    Failed {
        reason: String,
    },
}

/// How a pairing window ended.
enum End {
    Paired(String),
    Failed(String),
    TimedOut,
    Cancelled,
}

pub struct Pairing {
    addr: SocketAddr,
    window: Duration,
    name: String,
    cert_pem: String,
    devices: Devices,
    state: watch::Sender<PairingState>,
    /// The port for mDNS TXT `pair` while the listener is open.
    advertised: watch::Sender<Option<u16>>,
    /// Serializes `start` and `cancel`, so at most one window is ever open.
    control: tokio::sync::Mutex<()>,
    open: Mutex<Option<Window>>,
    next_id: AtomicU64,
}

struct Window {
    id: u64,
    cancel: watch::Sender<bool>,
    /// Takes the code the user typed, once a device has reached that step.
    code: Option<oneshot::Sender<String>>,
    task: Option<JoinHandle<()>>,
}

impl Pairing {
    /// `name` and `cert_pem` go to the device in `pair_offer`. Nothing listens until `start`.
    pub fn new(
        addr: SocketAddr,
        window: Duration,
        name: String,
        cert_pem: String,
        devices: Devices,
    ) -> Arc<Self> {
        Arc::new(Self {
            addr,
            window,
            name,
            cert_pem,
            devices,
            state: watch::channel(PairingState::Closed).0,
            advertised: watch::channel(None).0,
            control: tokio::sync::Mutex::new(()),
            open: Mutex::new(None),
            next_id: AtomicU64::new(0),
        })
    }

    pub fn subscribe(&self) -> watch::Receiver<PairingState> {
        self.state.subscribe()
    }

    pub fn advertised(&self) -> watch::Receiver<Option<u16>> {
        self.advertised.subscribe()
    }

    /// Opens the listener for one pairing, closing any open one first. Returns its address.
    pub async fn start(self: &Arc<Self>) -> io::Result<SocketAddr> {
        let _control = self.control.lock().await;
        self.close_window().await;
        let listener = TcpListener::bind(self.addr).await?;
        let addr = listener.local_addr()?;
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let (cancel, cancelled) = watch::channel(false);
        *self.windows() = Some(Window {
            id,
            cancel,
            code: None,
            task: None,
        });
        self.state.send_replace(PairingState::Waiting);
        self.advertised.send_replace(Some(addr.port()));
        log::info!("pairing open on {addr} for {:?}", self.window);
        let task = tokio::spawn(self.clone().run(id, listener, cancelled));
        if let Some(window) = self.windows().as_mut().filter(|w| w.id == id) {
            window.task = Some(task);
        }
        Ok(addr)
    }

    /// Closes the listener. A device waiting on the code is told the pairing failed.
    pub async fn cancel(&self) {
        let _control = self.control.lock().await;
        self.close_window().await;
    }

    /// Hands the code the user typed to the device waiting on it. The outcome arrives as the
    /// next state, `Paired` or `Failed`, and either way the listener closes: one guess per window.
    pub fn confirm(&self, code: &str) -> Result<(), String> {
        if code.len() != CODE_DIGITS || !code.bytes().all(|b| b.is_ascii_digit()) {
            return Err(format!("The code is {CODE_DIGITS} digits."));
        }
        let sender = self.windows().as_mut().and_then(|w| w.code.take());
        let sender = sender.ok_or("No device is waiting for a code.")?;
        sender
            .send(code.to_string())
            .map_err(|_| "The pairing has already ended.".to_string())
    }

    fn windows(&self) -> MutexGuard<'_, Option<Window>> {
        self.open.lock().expect("pairing window poisoned")
    }

    async fn close_window(&self) {
        let Some(mut window) = self.windows().take() else {
            return;
        };
        window.cancel.send_replace(true);
        if let Some(task) = window.task.take() {
            if let Err(e) = task.await {
                log::error!("pairing task failed: {e}");
            }
        }
    }

    async fn run(
        self: Arc<Self>,
        id: u64,
        listener: TcpListener,
        mut cancelled: watch::Receiver<bool>,
    ) {
        let deadline = Instant::now() + self.window;
        let end = loop {
            let accepted = tokio::select! {
                _ = cancelled.wait_for(|&c| c) => break End::Cancelled,
                () = sleep_until(deadline) => break End::TimedOut,
                accepted = listener.accept() => accepted,
            };
            match accepted {
                Ok((stream, peer)) => {
                    match self
                        .exchange(id, stream, peer, deadline, &mut cancelled)
                        .await
                    {
                        Ok(end) => break end,
                        // A device that never reached the code doesn't end the window.
                        Err(why) => log::info!("{peer}: pairing attempt dropped: {why}"),
                    }
                }
                Err(e) => {
                    log::warn!("pairing accept failed: {e}");
                    tokio::time::sleep(Duration::from_millis(100)).await;
                }
            }
        };
        drop(listener);
        let state = match end {
            End::Paired(device) => PairingState::Paired { device },
            End::Failed(reason) => PairingState::Failed { reason },
            End::TimedOut => PairingState::Failed {
                reason: "No code was entered in time.".into(),
            },
            End::Cancelled => PairingState::Closed,
        };
        log::info!("pairing closed: {state:?}");
        // Publish before releasing the window. A start() that finds the window still there
        // waits for this task, and one that doesn't comes after the release, so either way a
        // new window's Waiting and port land after this window's end, never before.
        self.advertised.send_replace(None);
        self.state.send_replace(state);
        self.windows().take_if(|w| w.id == id);
    }

    /// One device's attempt. `Err` means it never reached the code, and the listener stays open.
    async fn exchange(
        &self,
        id: u64,
        stream: TcpStream,
        peer: SocketAddr,
        deadline: Instant,
        cancelled: &mut watch::Receiver<bool>,
    ) -> Result<End, String> {
        let config = WebSocketConfig::default()
            .max_message_size(Some(MAX_MESSAGE_BYTES))
            .max_frame_size(Some(MAX_MESSAGE_BYTES));
        let ws = match timeout_at(
            step_deadline(deadline),
            accept_async_with_config(stream, Some(config)),
        )
        .await
        {
            Ok(Ok(ws)) => ws,
            Ok(Err(e)) => return Err(format!("WebSocket upgrade failed: {e}")),
            Err(_) => return Err("WebSocket upgrade timed out".into()),
        };
        let (mut tx, mut rx) = ws.split();

        let (device, commit) = match step(&mut tx, &mut rx, deadline, cancelled).await? {
            None => return Ok(End::Cancelled),
            Some(Message::PairReq { v, .. }) if v != VERSION => {
                let why = format!("unsupported protocol version {v}");
                return Err(refuse(&mut tx, ErrorCode::Version, why).await);
            }
            Some(Message::PairReq { device, commit, .. }) => {
                match decode_hex::<TOKEN_BYTES>(&commit) {
                    Some(commit) if valid_device_id(&device) => (device, commit),
                    _ => {
                        let why = "invalid device id or commit".to_string();
                        return Err(refuse(&mut tx, ErrorCode::BadRequest, why).await);
                    }
                }
            }
            Some(_) => {
                let why = "expected pair_req".to_string();
                return Err(refuse(&mut tx, ErrorCode::BadRequest, why).await);
            }
        };
        log::info!("{peer}: pairing request from {device}");

        let mut n_host = [0u8; NONCE_BYTES];
        aws_lc_rs::rand::fill(&mut n_host).map_err(|_| "no system randomness".to_string())?;
        let offer = Message::PairOffer {
            name: self.name.clone(),
            cert: self.cert_pem.clone(),
            c_host: encode_hex(&Sha256::digest(n_host)),
        };
        send(&mut tx, &offer).await.map_err(|e| e.to_string())?;

        let n_dev = match step(&mut tx, &mut rx, deadline, cancelled).await? {
            None => return Ok(End::Cancelled),
            Some(Message::PairNonce { n_dev }) => match decode_hex::<NONCE_BYTES>(&n_dev) {
                Some(n_dev) => n_dev,
                None => {
                    let why = "invalid n_dev".to_string();
                    return Err(refuse(&mut tx, ErrorCode::BadRequest, why).await);
                }
            },
            Some(_) => {
                let why = "expected pair_nonce".to_string();
                return Err(refuse(&mut tx, ErrorCode::BadRequest, why).await);
            }
        };
        let reveal = Message::PairReveal {
            n_host: encode_hex(&n_host),
        };
        send(&mut tx, &reveal).await.map_err(|e| e.to_string())?;
        let code = pairing_code(&device, &commit, &self.cert_pem, &n_dev, &n_host);

        let (code_tx, code_rx) = oneshot::channel();
        match self.windows().as_mut().filter(|w| w.id == id) {
            Some(window) => window.code = Some(code_tx),
            None => return Ok(End::Cancelled),
        }
        log::info!("pairing {device}: waiting for the code shown on the device");
        self.state.send_replace(PairingState::Confirm {
            device: device.clone(),
            peer: peer.ip().to_string(),
        });

        let end = tokio::select! {
            typed = code_rx => match typed {
                Ok(typed) if verify_slices_are_equal(typed.as_bytes(), code.as_bytes()).is_ok() => {
                    match self.devices.pair(&device, commit) {
                        Ok(()) => End::Paired(device),
                        Err(e) => End::Failed(format!("Could not save the pairing: {e}")),
                    }
                }
                Ok(_) => End::Failed("Wrong code. Nothing was stored.".into()),
                Err(_) => End::Cancelled,
            },
            _ = cancelled.wait_for(|&c| c) => End::Cancelled,
            () = sleep_until(deadline) => End::TimedOut,
            why = device_gone(&mut rx) => End::Failed(why.into()),
        };
        let ok = matches!(end, End::Paired(_));
        if let Err(e) = send(&mut tx, &Message::PairResult { ok }).await {
            log::debug!("pair_result: {e}");
        }
        close(&mut tx).await;
        Ok(end)
    }
}

fn step_deadline(window_deadline: Instant) -> Instant {
    (Instant::now() + STEP_TIMEOUT).min(window_deadline)
}

/// The device's next message, skipping unknown types, or `None` if the window was cancelled.
/// `Err` ends the attempt: a timeout, a close, or a malformed message (answered `bad_request`).
async fn step<S>(
    tx: &mut WsSink<S>,
    rx: &mut WsRead<S>,
    deadline: Instant,
    cancelled: &mut watch::Receiver<bool>,
) -> Result<Option<Message>, String>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    let next = async {
        loop {
            match next_message(rx).await {
                Incoming::Msg(Message::Unknown) => log::info!("pairing: ignoring an unknown type"),
                other => return other,
            }
        }
    };
    let incoming = tokio::select! {
        _ = cancelled.wait_for(|&c| c) => return Ok(None),
        read = timeout_at(step_deadline(deadline), next) => match read {
            Ok(incoming) => incoming,
            Err(_) => return Err("timed out".into()),
        },
    };
    match incoming {
        Incoming::Msg(msg) => Ok(Some(msg)),
        Incoming::BadRequest(why) | Incoming::BadVersion(why) => {
            Err(refuse(tx, ErrorCode::BadRequest, why).await)
        }
        Incoming::Closed => Err("closed".into()),
        Incoming::Idle => Err("silent".into()),
        Incoming::Failed(e) => Err(e.to_string()),
    }
}

/// Sends `error` and closes. Returns `why`, for the log.
async fn refuse<S>(tx: &mut WsSink<S>, code: ErrorCode, why: String) -> String
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    if let Err(e) = send_error(tx, code, why.clone()).await {
        log::debug!("pairing error reply: {e}");
    }
    close(tx).await;
    why
}

/// Resolves once the device closes, or breaks the protocol by speaking out of turn.
async fn device_gone<S>(rx: &mut WsRead<S>) -> &'static str
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    loop {
        match rx.next().await {
            None | Some(Err(_)) | Some(Ok(WsMessage::Close(_))) => {
                return "The device disconnected before the code was entered."
            }
            Some(Ok(WsMessage::Text(_) | WsMessage::Binary(_))) => {
                return "The device sent an unexpected message."
            }
            Some(Ok(_)) => {}
        }
    }
}
