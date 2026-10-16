//! The board's connection and the harness around it: handshake, hello check, and the sends and
//! waits the cases are built from.

use std::collections::HashMap;
use std::error::Error;
use std::fmt;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use futures_util::{SinkExt, StreamExt};
use tokio::io::{AsyncRead, AsyncWrite, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::time::{sleep, timeout, timeout_at, Instant};
use tokio_rustls::server::TlsStream;
use tokio_rustls::TlsAcceptor;
use tokio_tungstenite::tungstenite::protocol::frame::coding::{Data as OpData, OpCode};
use tokio_tungstenite::tungstenite::protocol::frame::Frame;
use tokio_tungstenite::tungstenite::protocol::WebSocketConfig;
use tokio_tungstenite::tungstenite::Message as Ws;
use tokio_tungstenite::{accept_async_with_config, WebSocketStream};
use watchdeck_lib::daemon::protocol::{
    decode_hex, encode_hex, Button, IconFrame, Message, Token, MAX_MESSAGE_BYTES, TOKEN_BYTES,
    VERSION,
};
use watchdeck_lib::daemon::store::Store;

pub type Res<T> = Result<T, Box<dyn Error>>;
pub type Tls = TlsStream<TcpStream>;

const STEP: Duration = Duration::from_secs(5);

/// The connection ended, or a send failed.
#[derive(Debug)]
pub struct Lost(pub String);

impl fmt::Display for Lost {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "link lost: {}", self.0)
    }
}

impl Error for Lost {}

pub enum Event {
    IconGet(Vec<String>),
    Press,
}

/// Prints `msg` and waits for Enter. A closed stdin is an error, so an unattended run stops
/// instead of walking past a step that needs the operator.
pub async fn prompt(msg: &str) -> Res<()> {
    println!("\n>>> {msg}\n    [Enter to continue]");
    let read = tokio::task::spawn_blocking(|| std::io::stdin().read_line(&mut String::new()));
    if read.await?? == 0 {
        return Err("stdin closed while waiting for the operator".into());
    }
    Ok(())
}

/// Stops the run without telling the board anything. An `error` message would make it erase its
/// pairing, so every refusal is a silent close.
pub fn die(msg: &str) -> ! {
    eprintln!("ABORT: {msg}");
    std::process::exit(2);
}

/// X is the icon every base layout names. It is the same on every run, so it stays cached.
pub fn icon_x() -> IconFrame {
    IconFrame::new(1, 1, vec![0, 0, 255, 255])
}

/// An icon no earlier run or call produced, 2x2 so its id depends on 16 bytes of time and count.
pub fn fresh_icon() -> IconFrame {
    static COUNT: AtomicU32 = AtomicU32::new(0);
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.subsec_nanos());
    let mut pixels = Vec::with_capacity(16);
    pixels.extend_from_slice(&COUNT.fetch_add(1, Ordering::Relaxed).to_le_bytes());
    pixels.extend_from_slice(&nanos.to_le_bytes());
    pixels.extend_from_slice(&std::process::id().to_le_bytes());
    pixels.extend_from_slice(&[0, 0, 0, 255]);
    IconFrame::new(2, 2, pixels)
}

fn button(id: &str, col: u32, label: &str, icon: Option<String>, tile: bool) -> Button {
    Button {
        id: id.into(),
        col,
        row: 0,
        label: label.into(),
        bg: 0x1e_3a5f,
        fg: 0xff_ffff,
        icon,
        tile,
    }
}

/// L0: a tile button `t1`, an icon button `ic` naming X, a plain button, and optionally a fourth
/// `(id, icon)` button for the case under test.
pub fn buttons(t1_label: &str, extra: Option<(&str, &str)>) -> Vec<Button> {
    let mut all = vec![
        button("t1", 0, t1_label, None, true),
        button("ic", 1, "Icon", Some(encode_hex(&icon_x().id)), false),
        button("plain", 2, "Plain", None, false),
    ];
    if let Some((id, icon)) = extra {
        all.push(button(id, 3, id, Some(icon.into()), false));
    }
    all
}

pub struct Session<S> {
    ws: WebSocketStream<S>,
    pub peer: SocketAddr,
    rev: u32,
    /// Encoded icon frames by id (32 hex digits), for the `icon_get`s the board sends.
    icons: HashMap<String, Vec<u8>>,
}

impl<S: AsyncRead + AsyncWrite + Unpin> Session<S> {
    pub fn new(ws: WebSocketStream<S>, peer: SocketAddr) -> Self {
        let mut session = Self {
            ws,
            peer,
            rev: 0,
            icons: HashMap::new(),
        };
        session.register(&icon_x());
        session
    }

    /// Remembers `icon` so `reply` can serve it, and returns its id as 32 hex digits.
    pub fn register(&mut self, icon: &IconFrame) -> String {
        let id = encode_hex(&icon.id);
        self.icons.insert(id.clone(), icon.encode());
        id
    }

    async fn send(&mut self, msg: Ws) -> Result<(), Lost> {
        match timeout(STEP, self.ws.send(msg)).await {
            Ok(Ok(())) => Ok(()),
            Ok(Err(e)) => Err(Lost(e.to_string())),
            Err(_) => Err(Lost("send timed out".into())),
        }
    }

    pub async fn send_json(&mut self, msg: &Message) -> Result<(), Lost> {
        let json = serde_json::to_string(msg).expect("protocol messages serialize");
        self.send(Ws::text(json)).await
    }

    pub async fn send_text(&mut self, text: &str) -> Result<(), Lost> {
        self.send(Ws::text(text)).await
    }

    /// A text frame whose payload need not be UTF-8.
    pub async fn send_text_bytes(&mut self, bytes: Vec<u8>) -> Result<(), Lost> {
        self.send_frame(true, OpData::Text, bytes).await
    }

    pub async fn send_binary(&mut self, bytes: Vec<u8>) -> Result<(), Lost> {
        self.send(Ws::binary(bytes)).await
    }

    /// One raw data frame, for fragments (`fin` false, or `OpData::Continue`).
    pub async fn send_frame(
        &mut self,
        fin: bool,
        op: OpData,
        payload: Vec<u8>,
    ) -> Result<(), Lost> {
        let frame = Frame::message(payload, OpCode::Data(op), fin);
        self.send(Ws::Frame(frame)).await
    }

    /// Writes a frame header that claims `claimed` payload bytes and then only `sent` of them, straight
    /// to the stream. The connection is unusable for further frames afterwards.
    pub async fn stall(&mut self, first_byte: u8, claimed: u64, sent: usize) -> Res<()> {
        let mut raw = vec![first_byte];
        if claimed <= u64::from(u16::MAX) {
            raw.push(126);
            raw.extend_from_slice(&u16::try_from(claimed)?.to_be_bytes());
        } else {
            raw.push(127);
            raw.extend_from_slice(&claimed.to_be_bytes());
        }
        raw.resize(raw.len() + sent, 0);
        let stream = self.ws.get_mut();
        stream.write_all(&raw).await?;
        stream.flush().await?;
        Ok(())
    }

    /// The marker the board logs on its `rx:` line, which ties the log to the case.
    pub async fn mark(&mut self, case: &str) -> Result<(), Lost> {
        self.send_text(&format!(r#"{{"t":"mark","case":"{case}"}}"#))
            .await
    }

    pub async fn welcome(&mut self, name: &str) -> Result<(), Lost> {
        let welcome = Message::Welcome {
            v: VERSION,
            name: name.into(),
        };
        self.send_json(&welcome).await
    }

    pub async fn layout(&mut self, buttons: Vec<Button>) -> Result<(), Lost> {
        self.rev += 1;
        let layout = Message::Layout {
            rev: self.rev,
            cols: 4,
            rows: 1,
            buttons,
        };
        self.send_json(&layout).await
    }

    /// The next `icon_get` or press before `until`, or `None` at the deadline.
    async fn event(&mut self, until: Instant) -> Result<Option<Event>, Lost> {
        loop {
            let frame = match timeout_at(until, self.ws.next()).await {
                Err(_) => return Ok(None),
                Ok(None) => return Err(Lost("the board closed the connection".into())),
                Ok(Some(Err(e))) => return Err(Lost(e.to_string())),
                Ok(Some(Ok(frame))) => frame,
            };
            match frame {
                Ws::Text(text) => match serde_json::from_str(text.as_str()) {
                    Ok(Message::IconGet { ids }) => return Ok(Some(Event::IconGet(ids))),
                    Ok(Message::Press { .. }) => return Ok(Some(Event::Press)),
                    _ => {}
                },
                Ws::Close(_) => return Err(Lost("the board sent close".into())),
                _ => {}
            }
        }
    }

    /// The ids of the next `icon_get`. The board sends none unless it lacks an icon.
    pub async fn next_get(&mut self, wait: Duration) -> Res<Vec<String>> {
        let until = Instant::now() + wait;
        loop {
            match self.event(until).await? {
                Some(Event::IconGet(ids)) => return Ok(ids),
                Some(Event::Press) => {}
                None => return Err(format!("no icon_get within {wait:?}").into()),
            }
        }
    }

    /// Sends the frame for each id in `ids` that this session knows.
    pub async fn reply(&mut self, ids: &[String]) -> Result<(), Lost> {
        for id in ids {
            if let Some(frame) = self.icons.get(id).cloned() {
                self.send_binary(frame).await?;
            }
        }
        Ok(())
    }

    /// Answers `icon_get`s until the board is quiet for `quiet`.
    pub async fn serve_until_quiet(&mut self, quiet: Duration) -> Result<(), Lost> {
        while let Some(event) = self.event(Instant::now() + quiet).await? {
            if let Event::IconGet(ids) = event {
                self.reply(&ids).await?;
            }
        }
        Ok(())
    }

    /// Reads for `wait` and fails on any `icon_get`, which also answers pings.
    pub async fn quiet(&mut self, wait: Duration) -> Res<()> {
        let until = Instant::now() + wait;
        while let Some(event) = self.event(until).await? {
            if let Event::IconGet(ids) = event {
                return Err(format!("unexpected icon_get {ids:?}").into());
            }
        }
        Ok(())
    }

    /// Reads until the board closes the link. True if it did within `wait`.
    pub async fn lost_within(&mut self, wait: Duration) -> bool {
        let until = Instant::now() + wait;
        loop {
            match self.event(until).await {
                Err(_) => return true,
                Ok(None) => return false,
                Ok(Some(_)) => {}
            }
        }
    }

    /// The next text message as a protocol message, or `None` if it does not parse. Control
    /// frames are skipped.
    pub async fn first_message(&mut self, until: Instant) -> Result<Option<Message>, Lost> {
        loop {
            match timeout_at(until, self.ws.next()).await {
                Err(_) => return Err(Lost("no message in time".into())),
                Ok(None) => return Err(Lost("closed before a message".into())),
                Ok(Some(Err(e))) => return Err(Lost(e.to_string())),
                Ok(Some(Ok(Ws::Text(text)))) => {
                    return Ok(serde_json::from_str(text.as_str()).ok());
                }
                Ok(Some(Ok(_))) => {}
            }
        }
    }

    /// Closes politely. The board may already be gone, which is only worth a line.
    pub async fn close(&mut self) {
        match timeout(Duration::from_secs(2), self.ws.close(None)).await {
            Ok(Ok(())) => {}
            Ok(Err(e)) => println!("    (close: {e})"),
            Err(_) => println!("    (close timed out)"),
        }
    }
}

/// Fresh-id liveness: the board must still apply a layout and ask for exactly the new icon.
pub async fn liveness<S: AsyncRead + AsyncWrite + Unpin>(sess: &mut Session<S>) -> Res<()> {
    let id = sess.register(&fresh_icon());
    sess.layout(buttons("T1", Some(("y", &id)))).await?;
    let ids = sess.next_get(STEP).await?;
    if ids != [id] {
        return Err(format!("icon_get named {ids:?}, expected only the fresh id").into());
    }
    sess.reply(&ids).await?;
    sess.quiet(Duration::from_millis(200)).await
}

/// Decodes a hello and checks it against the paired device. Anything else is a silent abort.
fn authenticate(hello: Option<Message>, store: &Store, device: &str) {
    let ok = match hello {
        Some(Message::Hello {
            v,
            device: id,
            token: Some(Token(hex)),
            ..
        }) => {
            id == device
                && v == VERSION
                && decode_hex::<TOKEN_BYTES>(&hex).is_some_and(|t| store.verify(&id, &t))
        }
        _ => false,
    };
    if !ok {
        die("a hello did not match --device and its stored token; closed without replying");
    }
}

/// The listener and the checks a connection must pass, shared by every reconnect.
pub struct Harness {
    pub listener: TcpListener,
    pub acceptor: TlsAcceptor,
    pub store: Store,
    pub device: String,
}

impl Harness {
    /// The next authenticated connection within `wait`. A peer that fails the handshake is
    /// skipped, and one that fails authentication ends the run.
    pub async fn accept(&self, wait: Duration) -> Res<Session<Tls>> {
        let until = Instant::now() + wait;
        loop {
            let (tcp, peer) = match timeout_at(until, self.listener.accept()).await {
                Err(_) => return Err(format!("no connection within {wait:?}").into()),
                Ok(accepted) => accepted?,
            };
            let deadline = Instant::now() + STEP;
            let Ok(Ok(tls)) = timeout_at(deadline, self.acceptor.accept(tcp)).await else {
                println!("    {peer}: TLS handshake failed, skipped");
                continue;
            };
            let config = WebSocketConfig::default()
                .max_message_size(Some(MAX_MESSAGE_BYTES))
                .max_frame_size(Some(MAX_MESSAGE_BYTES));
            let Ok(Ok(ws)) =
                timeout_at(deadline, accept_async_with_config(tls, Some(config))).await
            else {
                println!("    {peer}: WebSocket upgrade failed, skipped");
                continue;
            };
            let mut session = Session::new(ws, peer);
            let hello = match session.first_message(deadline).await {
                Ok(hello) => hello,
                Err(e) => {
                    println!("    {peer}: no hello ({e}), skipped");
                    continue;
                }
            };
            authenticate(hello, &self.store, &self.device);
            return Ok(session);
        }
    }
}

/// The harness and the live session, which cases replace when the link closes.
pub struct Rig {
    pub h: Harness,
    pub sess: Session<Tls>,
}

impl Rig {
    /// Welcome, L0, and whatever icons the board asks for.
    pub async fn bring_up(&mut self) -> Res<()> {
        self.sess.welcome("fault-harness").await?;
        self.sess.layout(buttons("T1", None)).await?;
        self.sess
            .serve_until_quiet(Duration::from_millis(1500))
            .await?;
        Ok(())
    }

    /// Closes the session and waits for the board to connect again. After `first_wait` it asks the
    /// operator to reboot the board and keeps waiting. Returns the seconds until the hello.
    pub async fn reconnect(&mut self, first_wait: Duration) -> Res<f64> {
        self.sess.close().await;
        self.next_session(first_wait).await
    }

    /// As `reconnect`, but for a session that is already gone.
    pub async fn next_session(&mut self, first_wait: Duration) -> Res<f64> {
        let secs = self.replace(first_wait).await?;
        self.bring_up().await?;
        Ok(secs)
    }

    /// Waits for the next hello and makes its session the live one, sending nothing.
    pub async fn replace(&mut self, first_wait: Duration) -> Res<f64> {
        let start = Instant::now();
        let mut wait = first_wait;
        let session = loop {
            match self.h.accept(wait).await {
                Ok(session) => break session,
                Err(e) => {
                    println!("    {e}");
                    prompt(
                        "The board has not come back. Reboot it with an RTS pulse (type r + Enter in the \
                         capture terminal), then press Enter here.",
                    )
                    .await?;
                    wait = Duration::from_secs(120);
                }
            }
        };
        self.sess = session;
        Ok(start.elapsed().as_secs_f64())
    }

    /// Pauses without reading, so the board's pings go unanswered.
    pub async fn sleep_deaf(&self, secs: u64) {
        println!("    not reading for {secs} s");
        sleep(Duration::from_secs(secs)).await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio_tungstenite::client_async;

    /// A server session and a plain client over loopback.
    async fn pair() -> (
        Session<TcpStream>,
        WebSocketStream<tokio_tungstenite::MaybeTlsStream<TcpStream>>,
    ) {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        let client = tokio::spawn(async move {
            let tcp = TcpStream::connect(addr).await.unwrap();
            client_async(
                "ws://localhost/",
                tokio_tungstenite::MaybeTlsStream::Plain(tcp),
            )
            .await
            .unwrap()
            .0
        });
        let (tcp, peer) = listener.accept().await.unwrap();
        let ws = accept_async_with_config(tcp, None).await.unwrap();
        (Session::new(ws, peer), client.await.unwrap())
    }

    #[tokio::test]
    async fn a_fragment_arrives_as_one_message() {
        let (mut server, mut client) = pair().await;
        server
            .send_frame(false, OpData::Binary, vec![1, 2, 3])
            .await
            .unwrap();
        server
            .send_frame(true, OpData::Continue, Vec::new())
            .await
            .unwrap();
        assert_eq!(
            client.next().await.unwrap().unwrap(),
            Ws::binary(vec![1, 2, 3])
        );
    }

    #[tokio::test]
    async fn invalid_utf8_goes_out_as_a_text_frame() {
        let (mut server, mut client) = pair().await;
        server.send_text_bytes(vec![b'a', 0xf0]).await.unwrap();
        // A conforming client refuses it, which proves the bytes left as text.
        assert!(client.next().await.unwrap().is_err());
    }

    #[tokio::test]
    async fn a_stall_header_encodes_both_length_forms() {
        for len in [300_usize, 70_000] {
            let (mut server, mut client) = pair().await;
            server.stall(0x82, len as u64, len).await.unwrap();
            let got = client.next().await.unwrap().unwrap();
            assert_eq!(got.len(), len);
        }
    }

    #[tokio::test]
    async fn a_short_body_leaves_the_client_waiting() {
        let (mut server, mut client) = pair().await;
        server.stall(0x82, 262_168, 1000).await.unwrap();
        let waited = timeout(Duration::from_millis(300), client.next()).await;
        assert!(
            waited.is_err(),
            "the client got a message from half a frame"
        );
    }
}
