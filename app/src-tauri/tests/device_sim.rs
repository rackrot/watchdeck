//! A fake device driving the daemon over a real socket, without mDNS. It pins the daemon's cert
//! the way the device does: the exact cert bytes, with no name or date checks.

use std::fs;
use std::io;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, Once, OnceLock};
use std::time::{Duration, Instant};

use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

use futures_util::{SinkExt, StreamExt};
use sha2::{Digest, Sha256};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::broadcast;
use tokio_rustls::client::TlsStream;
use tokio_rustls::rustls::client::danger::{
    HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier,
};
use tokio_rustls::rustls::crypto::{
    aws_lc_rs, verify_tls12_signature, verify_tls13_signature, WebPkiSupportedAlgorithms,
};
use tokio_rustls::rustls::pki_types::pem::PemObject;
use tokio_rustls::rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use tokio_rustls::rustls::{
    self, CertificateError, ClientConfig, DigitallySignedStruct, SignatureScheme,
};
use tokio_rustls::TlsConnector;
use tokio_tungstenite::tungstenite::Message as Ws;
use tokio_tungstenite::{client_async, connect_async, WebSocketStream};
use watchdeck_lib::daemon::actions::{ActionResult, Actions};
use watchdeck_lib::daemon::config::{self, Configs};
use watchdeck_lib::daemon::pairing::{Pairing, PairingState};
use watchdeck_lib::daemon::protocol::{
    decode_hex, encode_hex, pairing_code, ErrorCode, IconFrame, Message, Report, TileState, Token,
};
use watchdeck_lib::daemon::reporting::{self, Reporting};
use watchdeck_lib::daemon::secrets::Secrets;
use watchdeck_lib::daemon::server::{DaemonEvent, DeviceInfo, Devices, Server};
use watchdeck_lib::daemon::store::Store;
use watchdeck_lib::daemon::tls::Identity;

type Client = WebSocketStream<TlsStream<TcpStream>>;

/// Paired in every test daemon before it starts.
const DEVICE: &str = "wd-000000000001";
const TOKEN: [u8; 32] = [7; 32];
const OTHER: &str = "wd-000000000002";
const OTHER_TOKEN: [u8; 32] = [8; 32];

/// Accepts one exact end-entity cert. Handshake signatures are still verified.
#[derive(Debug)]
struct Pinned {
    cert: CertificateDer<'static>,
    algs: WebPkiSupportedAlgorithms,
}

impl ServerCertVerifier for Pinned {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        if end_entity.as_ref() == self.cert.as_ref() {
            Ok(ServerCertVerified::assertion())
        } else {
            Err(rustls::Error::InvalidCertificate(
                CertificateError::UnknownIssuer,
            ))
        }
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(message, cert, dss, &self.algs)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(message, cert, dss, &self.algs)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.algs.supported_schemes()
    }
}

/// The identity every test daemon serves, made once through the real load path.
fn identity() -> &'static Identity {
    static IDENTITY: OnceLock<Identity> = OnceLock::new();
    IDENTITY.get_or_init(|| {
        let dir = tempfile::tempdir().unwrap();
        Identity::load_or_create(dir.path()).unwrap()
    })
}

/// One button, `hello` at the top left in the default colors, that prints "ran hello".
const CONFIG: &str = r#"
version = 1
[layout]
cols = 5
rows = 3

[[button]]
id = "hello"
pos = [0, 0]
label = "Hello"
action = { kind = "shell", argv = ["echo", "ran hello"] }
"#;

struct Daemon {
    addr: SocketAddr,
    events: broadcast::Receiver<DaemonEvent>,
    devices: Devices,
    pairing: Arc<Pairing>,
    configs: Arc<Configs>,
    secrets: Arc<Secrets>,
    /// The config dir, which is also where shell actions run.
    dir: tempfile::TempDir,
}

impl Daemon {
    /// Replaces `config.toml` and reloads it, returning what the reload said.
    fn reconfigure(&self, text: &str) -> Result<bool, Vec<String>> {
        fs::write(self.dir.path().join(config::FILE), text).unwrap();
        self.configs.reload()
    }
}

/// A daemon on an ephemeral port with `DEVICE` paired and `config` as its `config.toml`, whose
/// pairing listener binds `pair_addr` for at most `window` once started.
async fn start_with(pair_addr: SocketAddr, window: Duration, config: &str) -> Daemon {
    start_reporting(pair_addr, window, config, None).await
}

/// [`start_with`] with device reports going to `reporting`.
async fn start_reporting(
    pair_addr: SocketAddr,
    window: Duration,
    config: &str,
    reporting: Option<Reporting>,
) -> Daemon {
    let dir = tempfile::tempdir().unwrap();
    fs::write(dir.path().join(config::FILE), config).unwrap();
    let secrets = Arc::new(Secrets::new(dir.path()));
    let configs = Configs::open(dir.path(), secrets.clone()).unwrap();
    assert!(
        configs.status().borrow().errors.is_empty(),
        "the test config must be valid"
    );
    let actions = Actions::new(secrets.clone(), dir.path().to_path_buf()).unwrap();
    let tls = identity().server_config().unwrap();
    let store = Store::load(dir.path()).unwrap();
    let server = Server::bind(
        "127.0.0.1:0",
        tls,
        store,
        configs.subscribe(),
        actions,
        reporting,
    )
    .await
    .unwrap();
    let devices = server.devices();
    devices.pair(DEVICE, Sha256::digest(TOKEN).into()).unwrap();
    let pairing = Pairing::new(
        pair_addr,
        window,
        "test-daemon".into(),
        identity().cert_pem().into(),
        devices.clone(),
    );
    let daemon = Daemon {
        addr: server.local_addr().unwrap(),
        events: server.subscribe(),
        devices,
        pairing,
        configs,
        secrets,
        dir,
    };
    tokio::spawn(server.run());
    daemon
}

async fn start() -> Daemon {
    start_with(
        "127.0.0.1:0".parse().unwrap(),
        Duration::from_secs(30),
        CONFIG,
    )
    .await
}

async fn start_configured(config: &str) -> Daemon {
    start_with(
        "127.0.0.1:0".parse().unwrap(),
        Duration::from_secs(30),
        config,
    )
    .await
}

/// Opens wss to `addr` pinned to `pin`'s cert, over TLS 1.2 like the device's mbedTLS.
async fn connect_pinned(addr: SocketAddr, pin: &Identity) -> io::Result<Client> {
    let provider = Arc::new(aws_lc_rs::default_provider());
    let verifier = Pinned {
        cert: CertificateDer::from_pem_slice(pin.cert_pem().as_bytes()).unwrap(),
        algs: provider.signature_verification_algorithms,
    };
    let config = ClientConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS12])
        .unwrap()
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(verifier))
        .with_no_client_auth();
    let tcp = TcpStream::connect(addr).await?;
    // An IP address as the server name sends no SNI, like the device.
    let tls = TlsConnector::from(Arc::new(config))
        .connect(ServerName::IpAddress(addr.ip().into()), tcp)
        .await?;
    let (ws, _) = client_async(format!("wss://{addr}/"), tls)
        .await
        .map_err(io::Error::other)?;
    Ok(ws)
}

async fn connect(addr: SocketAddr) -> Client {
    connect_pinned(addr, identity()).await.unwrap()
}

fn hello(v: u32, device: &str, token: Option<&[u8; 32]>) -> Ws {
    Ws::text(
        serde_json::to_string(&Message::Hello {
            v,
            device: device.into(),
            token: token.map(|t| Token(encode_hex(t))),
            fw: "0.2.0".into(),
            screen: [1024, 600],
        })
        .unwrap(),
    )
}

fn press(btn: &str, seq: u64) -> Ws {
    Ws::text(
        serde_json::to_string(&Message::Press {
            btn: btn.into(),
            seq,
        })
        .unwrap(),
    )
}

async fn recv<S: AsyncRead + AsyncWrite + Unpin>(c: &mut WebSocketStream<S>) -> Message {
    loop {
        match c.next().await.expect("stream ended").unwrap() {
            Ws::Text(t) => return serde_json::from_str(t.as_str()).unwrap(),
            Ws::Ping(_) | Ws::Pong(_) => {}
            other => panic!("unexpected frame {other:?}"),
        }
    }
}

/// The next binary frame, decoded the way the device checks it.
async fn recv_icon<S: AsyncRead + AsyncWrite + Unpin>(c: &mut WebSocketStream<S>) -> IconFrame {
    loop {
        match c.next().await.expect("stream ended").unwrap() {
            Ws::Binary(frame) => return IconFrame::decode(&frame).unwrap(),
            Ws::Ping(_) | Ws::Pong(_) => {}
            other => panic!("expected an icon frame, got {other:?}"),
        }
    }
}

fn icon_get(ids: &[&str]) -> Ws {
    Ws::text(serde_json::to_string(&serde_json::json!({"t": "icon_get", "ids": ids})).unwrap())
}

/// Reads until the server ends the connection, however it chooses to (close frame, EOF or reset).
async fn expect_closed<S: AsyncRead + AsyncWrite + Unpin>(c: &mut WebSocketStream<S>) {
    loop {
        match c.next().await {
            None | Some(Err(_)) | Some(Ok(Ws::Close(_))) => return,
            Some(Ok(_)) => {}
        }
    }
}

/// Expects the server to end the connection without sending anything first.
async fn expect_closed_unanswered<S: AsyncRead + AsyncWrite + Unpin>(c: &mut WebSocketStream<S>) {
    match c.next().await {
        None | Some(Err(_)) | Some(Ok(Ws::Close(_))) => {}
        Some(Ok(frame)) => panic!("expected the connection to end, got {frame:?}"),
    }
}

async fn handshake(addr: SocketAddr, device: &str, token: &[u8; 32]) -> Client {
    let mut c = connect(addr).await;
    c.send(hello(1, device, Some(token))).await.unwrap();
    assert!(matches!(recv(&mut c).await, Message::Welcome { .. }));
    assert!(matches!(recv(&mut c).await, Message::Layout { .. }));
    c
}

#[tokio::test]
async fn a_client_pinning_another_cert_fails_the_handshake() {
    let daemon = start().await;
    let addr = daemon.addr;
    let dir = tempfile::tempdir().unwrap();
    let other = Identity::load_or_create(dir.path()).unwrap();
    assert_ne!(other.fp(), identity().fp());

    let err = connect_pinned(addr, &other)
        .await
        .expect_err("the handshake must fail");
    let tls_err = err
        .get_ref()
        .and_then(|e| e.downcast_ref::<rustls::Error>());
    assert_eq!(
        tls_err,
        Some(&rustls::Error::InvalidCertificate(
            CertificateError::UnknownIssuer
        )),
        "{err}"
    );
}

#[tokio::test]
async fn plain_ws_is_refused() {
    let daemon = start().await;
    let addr = daemon.addr;
    assert!(connect_async(format!("ws://{addr}/")).await.is_err());
    // The refused attempt must not have taken the listener down.
    handshake(addr, DEVICE, &TOKEN).await;
}

/// Reads a raw socket until the server closes it, returning how long that took.
async fn time_to_close(mut tcp: TcpStream, started: Instant) -> Duration {
    let mut buf = [0u8; 64];
    loop {
        match tcp.read(&mut buf).await {
            Ok(0) | Err(_) => return started.elapsed(),
            Ok(_) => {}
        }
    }
}

// Real clock, like the other timeout tests.
#[tokio::test]
async fn a_socket_that_sends_nothing_is_closed_after_five_seconds() {
    let daemon = start().await;
    let addr = daemon.addr;
    let started = Instant::now();
    let tcp = TcpStream::connect(addr).await.unwrap();
    let waited = time_to_close(tcp, started).await;
    assert!(waited >= Duration::from_secs(5), "{waited:?}");
    assert!(waited < Duration::from_secs(6), "{waited:?}");
}

#[tokio::test]
async fn a_client_hello_that_stalls_midway_is_closed_after_five_seconds() {
    let daemon = start().await;
    let addr = daemon.addr;
    let started = Instant::now();
    let mut tcp = TcpStream::connect(addr).await.unwrap();
    // A TLS record header announcing 240 bytes of handshake, then 4 of them.
    tcp.write_all(&[0x16, 0x03, 0x01, 0x00, 0xf0, 0x01, 0x00, 0x00, 0xec])
        .await
        .unwrap();
    let waited = time_to_close(tcp, started).await;
    assert!(waited >= Duration::from_secs(5), "{waited:?}");
    assert!(waited < Duration::from_secs(6), "{waited:?}");
}

#[tokio::test]
async fn connections_beyond_64_are_dropped_until_one_frees_up() {
    let daemon = start().await;
    let addr = daemon.addr;
    let mut held = Vec::new();
    for _ in 0..64 {
        held.push(TcpStream::connect(addr).await.unwrap());
    }
    // Let the accept loop take all 64 before the next one arrives.
    tokio::time::sleep(Duration::from_millis(200)).await;
    let started = Instant::now();
    let extra = TcpStream::connect(addr).await.unwrap();
    let waited = time_to_close(extra, started).await;
    assert!(waited < Duration::from_secs(1), "{waited:?}");

    // The permit frees once the server sees the EOF, so retry briefly instead of guessing a delay.
    held.pop();
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut c = loop {
        match connect_pinned(addr, identity()).await {
            Ok(c) => break c,
            Err(e) => assert!(Instant::now() < deadline, "no permit freed within 2 s: {e}"),
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    };
    c.send(hello(1, DEVICE, Some(&TOKEN))).await.unwrap();
    assert!(matches!(recv(&mut c).await, Message::Welcome { .. }));
}

#[tokio::test]
async fn hello_gets_welcome_then_the_layout() {
    let daemon = start().await;
    let addr = daemon.addr;
    let mut c = connect(addr).await;
    c.send(hello(1, DEVICE, Some(&TOKEN))).await.unwrap();

    let Message::Welcome { v, name } = recv(&mut c).await else {
        panic!("expected welcome")
    };
    assert_eq!(v, 1);
    assert!(!name.is_empty());

    let Message::Layout {
        cols,
        rows,
        buttons,
        ..
    } = recv(&mut c).await
    else {
        panic!("expected layout")
    };
    assert_eq!((cols, rows), (5, 3));
    assert_eq!(buttons.len(), 1);
    let b = &buttons[0];
    assert_eq!((b.id.as_str(), b.col, b.row), ("hello", 0, 0));
    assert_eq!(
        (b.label.as_str(), b.bg, b.fg),
        ("Hello", 0x1e88e5, 0xffffff)
    );
}

#[tokio::test]
async fn press_hello_runs_its_command_and_gets_one_ok_result() {
    let daemon = start().await;
    let addr = daemon.addr;
    let mut c = handshake(addr, DEVICE, &TOKEN).await;
    c.send(press("hello", 17)).await.unwrap();
    let Message::Result {
        btn,
        seq,
        ok,
        code,
        summary,
        text,
        ..
    } = recv(&mut c).await
    else {
        panic!("expected result")
    };
    assert_eq!((btn.as_str(), seq, ok, code), ("hello", 17, true, 0));
    assert_eq!((summary.as_str(), text.as_str()), ("exit 0", "ran hello"));
}

#[tokio::test]
async fn press_unknown_button_fails_without_running_anything() {
    let daemon = start().await;
    let addr = daemon.addr;
    let mut c = handshake(addr, DEVICE, &TOKEN).await;
    c.send(press("nope", 3)).await.unwrap();
    assert_eq!(
        recv(&mut c).await,
        Message::Result {
            btn: "nope".into(),
            seq: 3,
            ok: false,
            code: -1,
            summary: "unknown button".into(),
            text: String::new(),
            ms: 0,
        }
    );
}

#[tokio::test]
async fn unknown_message_type_does_not_end_the_session() {
    let daemon = start().await;
    let addr = daemon.addr;
    let mut c = handshake(addr, DEVICE, &TOKEN).await;
    c.send(Ws::text(r#"{"t":"future","x":1}"#)).await.unwrap();
    c.send(press("hello", 1)).await.unwrap();
    assert!(matches!(
        recv(&mut c).await,
        Message::Result { ok: true, .. }
    ));
}

#[tokio::test]
async fn wrong_version_gets_an_error_then_close() {
    let daemon = start().await;
    let addr = daemon.addr;
    let mut c = connect(addr).await;
    c.send(hello(2, DEVICE, Some(&TOKEN))).await.unwrap();
    let Message::Error { code, .. } = recv(&mut c).await else {
        panic!("expected error")
    };
    assert_eq!(code, ErrorCode::Version);
    expect_closed(&mut c).await;
}

#[tokio::test]
async fn any_hello_with_a_wrong_version_gets_a_version_error() {
    for hello in [
        r#"{"t":"hello","v":2,"foo":1}"#,
        r#"{"t":"hello","v":-1}"#,
        r#"{"t":"hello","v":"1"}"#,
    ] {
        let daemon = start().await;
        let addr = daemon.addr;
        let mut c = connect(addr).await;
        c.send(Ws::text(hello)).await.unwrap();
        let Message::Error { code, .. } = recv(&mut c).await else {
            panic!("expected error for {hello}")
        };
        assert_eq!(code, ErrorCode::Version, "{hello}");
        expect_closed(&mut c).await;
    }
}

#[tokio::test]
async fn a_button_id_over_64_bytes_gets_bad_request_then_close() {
    let daemon = start().await;
    let addr = daemon.addr;
    let mut c = handshake(addr, DEVICE, &TOKEN).await;
    c.send(press(&"a".repeat(65), 1)).await.unwrap();
    let Message::Error { code, .. } = recv(&mut c).await else {
        panic!("expected error")
    };
    assert_eq!(code, ErrorCode::BadRequest);
    expect_closed(&mut c).await;
}

#[tokio::test]
async fn malformed_hello_gets_bad_request_then_close() {
    let daemon = start().await;
    let addr = daemon.addr;
    let mut c = connect(addr).await;
    c.send(Ws::text("not json")).await.unwrap();
    let Message::Error { code, .. } = recv(&mut c).await else {
        panic!("expected error")
    };
    assert_eq!(code, ErrorCode::BadRequest);
    expect_closed(&mut c).await;
}

// The two timeout tests run on the real clock. A paused clock jumps ahead whenever the runtime
// waits on socket I/O, which fires the daemon's timers before the traffic under test arrives.
#[tokio::test]
async fn silence_after_connecting_closes_after_five_seconds() {
    let daemon = start().await;
    let addr = daemon.addr;
    // The deadline starts at the TCP accept, so the TLS and WebSocket handshakes count too.
    let started = Instant::now();
    let mut c = connect(addr).await;
    expect_closed(&mut c).await;
    let waited = started.elapsed();
    assert!(waited >= Duration::from_secs(5), "{waited:?}");
    assert!(waited < Duration::from_secs(6), "{waited:?}");
}

#[tokio::test]
async fn a_session_silent_for_thirty_seconds_is_dropped() {
    let mut daemon = start().await;
    let addr = daemon.addr;
    let mut c = handshake(addr, DEVICE, &TOKEN).await;
    assert_eq!(
        daemon.events.recv().await.unwrap(),
        DaemonEvent::DeviceConnected(DEVICE.into())
    );
    let started = Instant::now();
    expect_closed(&mut c).await;
    let waited = started.elapsed();
    assert!(waited >= Duration::from_secs(29), "{waited:?}");
    assert!(waited < Duration::from_secs(31), "{waited:?}");
    assert_eq!(
        daemon.events.recv().await.unwrap(),
        DaemonEvent::DeviceDisconnected(DEVICE.into())
    );
}

#[tokio::test]
async fn pings_are_answered_with_pongs() {
    let daemon = start().await;
    let addr = daemon.addr;
    let mut c = handshake(addr, DEVICE, &TOKEN).await;
    c.send(Ws::Ping(vec![7].into())).await.unwrap();
    let frame = c.next().await.unwrap().unwrap();
    assert!(matches!(frame, Ws::Pong(p) if p.as_ref() == [7]));
}

#[tokio::test]
async fn a_valid_message_over_16_kb_closes_the_session_unanswered() {
    let daemon = start().await;
    let addr = daemon.addr;
    let mut c = handshake(addr, DEVICE, &TOKEN).await;
    // A press the daemon would run but for its size: the padding is a field it ignores.
    let big = format!(
        r#"{{"t":"press","btn":"hello","seq":1,"pad":"{}"}}"#,
        "a".repeat(16 * 1024)
    );
    c.send(Ws::text(big)).await.unwrap();
    expect_closed_unanswered(&mut c).await;
}

#[tokio::test]
async fn reconnecting_device_replaces_its_old_session() {
    let mut daemon = start().await;
    let addr = daemon.addr;
    let mut old = handshake(addr, DEVICE, &TOKEN).await;
    let mut new = handshake(addr, DEVICE, &TOKEN).await;

    expect_closed(&mut old).await;

    // The new session still works.
    new.send(press("hello", 1)).await.unwrap();
    assert!(matches!(
        recv(&mut new).await,
        Message::Result { ok: true, .. }
    ));

    assert_eq!(
        daemon.events.recv().await.unwrap(),
        DaemonEvent::DeviceConnected(DEVICE.into())
    );
    assert_eq!(
        daemon.events.recv().await.unwrap(),
        DaemonEvent::DeviceDisconnected(DEVICE.into())
    );
    assert_eq!(
        daemon.events.recv().await.unwrap(),
        DaemonEvent::DeviceConnected(DEVICE.into())
    );
}

// ---- Authentication on 47820 ----

/// Sends `hello`, expects `error`, then a close. Returns the error for comparison.
async fn refused_hello(addr: SocketAddr, hello: Ws) -> Message {
    let mut c = connect(addr).await;
    c.send(hello).await.unwrap();
    let error = recv(&mut c).await;
    expect_closed(&mut c).await;
    error
}

fn unauthorized() -> Message {
    Message::Error {
        code: ErrorCode::Unauthorized,
        msg: "unauthorized".into(),
    }
}

#[tokio::test]
async fn a_missing_wrong_or_malformed_token_or_an_unknown_device_gets_the_same_unauthorized() {
    let daemon = start().await;
    let hellos = [
        hello(1, DEVICE, None),
        hello(1, DEVICE, Some(&[0; 32])),
        Ws::text(
            r#"{"t":"hello","v":1,"device":"wd-000000000001","token":"zz","fw":"0","screen":[1,1]}"#,
        ),
        hello(1, "wd-0000000000ff", Some(&TOKEN)),
    ];
    for hello in hellos {
        assert_eq!(refused_hello(daemon.addr, hello).await, unauthorized());
    }
    // The real device still gets in.
    handshake(daemon.addr, DEVICE, &TOKEN).await;
}

#[tokio::test]
async fn a_token_for_one_device_does_not_open_another() {
    let daemon = start().await;
    daemon
        .devices
        .pair(OTHER, Sha256::digest(OTHER_TOKEN).into())
        .unwrap();
    assert_eq!(
        refused_hello(daemon.addr, hello(1, OTHER, Some(&TOKEN))).await,
        unauthorized()
    );
    assert_eq!(
        refused_hello(daemon.addr, hello(1, DEVICE, Some(&OTHER_TOKEN))).await,
        unauthorized()
    );
    handshake(daemon.addr, OTHER, &OTHER_TOKEN).await;
}

#[tokio::test]
async fn an_impostor_with_a_stolen_device_id_cannot_kick_the_paired_session() {
    let mut daemon = start().await;
    let mut real = handshake(daemon.addr, DEVICE, &TOKEN).await;
    assert_eq!(
        daemon.events.recv().await.unwrap(),
        DaemonEvent::DeviceConnected(DEVICE.into())
    );

    for token in [None, Some(&[0u8; 32])] {
        assert_eq!(
            refused_hello(daemon.addr, hello(1, DEVICE, token)).await,
            unauthorized()
        );
    }

    real.send(press("hello", 5)).await.unwrap();
    assert!(matches!(
        recv(&mut real).await,
        Message::Result {
            seq: 5,
            ok: true,
            ..
        }
    ));
    assert!(
        daemon.events.try_recv().is_err(),
        "the impostor must not have disconnected the real session"
    );
    assert_eq!(
        daemon.devices.list(),
        [DeviceInfo {
            id: DEVICE.into(),
            connected: true
        }]
    );
}

#[tokio::test]
async fn forget_closes_the_live_session_with_unauthorized_and_voids_the_token() {
    let mut daemon = start().await;
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    assert_eq!(
        daemon.events.recv().await.unwrap(),
        DaemonEvent::DeviceConnected(DEVICE.into())
    );

    assert!(daemon.devices.forget(DEVICE).unwrap());
    assert_eq!(recv(&mut c).await, unauthorized());
    expect_closed(&mut c).await;
    assert_eq!(
        daemon.events.recv().await.unwrap(),
        DaemonEvent::DeviceForgotten(DEVICE.into())
    );
    assert_eq!(
        daemon.events.recv().await.unwrap(),
        DaemonEvent::DeviceDisconnected(DEVICE.into())
    );

    assert!(daemon.devices.list().is_empty());
    assert!(!daemon.devices.forget(DEVICE).unwrap());
    assert_eq!(
        refused_hello(daemon.addr, hello(1, DEVICE, Some(&TOKEN))).await,
        unauthorized()
    );
}

/// Two buttons whose commands each add a line to `runs` in the config dir. `slow` then takes 300 ms.
const COUNTING: &str = r#"
version = 1
[layout]
cols = 5
rows = 3

[[button]]
id = "slow"
pos = [0, 0]
label = "Slow"
action = { kind = "shell", argv = ["sh", "-c", "echo slow >> runs; sleep 0.3"] }

[[button]]
id = "count"
pos = [1, 0]
label = "Count"
action = { kind = "shell", argv = ["sh", "-c", "echo count >> runs"] }
"#;

fn runs(daemon: &Daemon) -> Vec<String> {
    match fs::read_to_string(daemon.dir.path().join("runs")) {
        Ok(text) => text.lines().map(String::from).collect(),
        Err(_) => Vec::new(),
    }
}

// Presses run concurrently, so none waits behind another. What a revocation must stop is any
// press that arrives after it, even while an earlier run is still going.
#[tokio::test]
async fn no_press_runs_after_a_forget_even_with_a_run_in_flight() {
    let daemon = start_configured(COUNTING).await;
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    c.send(press("slow", 1)).await.unwrap();
    let deadline = Instant::now() + Duration::from_secs(2);
    while runs(&daemon).is_empty() {
        assert!(Instant::now() < deadline, "the first press never ran");
        tokio::time::sleep(Duration::from_millis(5)).await;
    }

    assert!(daemon.devices.forget(DEVICE).unwrap());
    // The session may not have seen the kick yet, but this press must not run either way.
    c.send(press("count", 2)).await.unwrap();
    assert_eq!(recv(&mut c).await, unauthorized());
    expect_closed(&mut c).await;

    // Long enough for both commands to finish, had both started.
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert_eq!(
        runs(&daemon),
        ["slow"],
        "a press started after forget returned"
    );
}

#[tokio::test]
async fn a_test_press_runs_the_live_action_and_sends_the_device_nothing() {
    let daemon = start_configured(COUNTING).await;
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;

    // The button is busy with a device press, so a test press is refused like a second press.
    c.send(press("slow", 1)).await.unwrap();
    let deadline = Instant::now() + Duration::from_secs(2);
    while runs(&daemon).is_empty() {
        assert!(Instant::now() < deadline, "the device press never ran");
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
    assert_eq!(
        daemon.devices.test_press("slow").await.unwrap(),
        ActionResult::failed("already running")
    );
    assert!(matches!(recv(&mut c).await, Message::Result { seq: 1, .. }));

    // It runs what the live config says, and returns what the device would have been sent.
    let ran = daemon.devices.test_press("count").await.unwrap();
    assert_eq!(
        (ran.ok, ran.code, ran.summary.as_str()),
        (true, 0, "exit 0")
    );
    daemon
        .reconfigure(&COUNTING.replace("echo count >>", "echo edited >>"))
        .unwrap();
    daemon.devices.test_press("count").await.unwrap();
    assert_eq!(runs(&daemon), ["slow", "count", "edited"]);

    let err = daemon.devices.test_press("nope").await.unwrap_err();
    assert!(err.contains("\"nope\""), "{err}");

    // Neither test press sent a result: the next frame answers this ping.
    c.send(Ws::Ping(vec![9].into())).await.unwrap();
    let frame = c.next().await.unwrap().unwrap();
    assert!(
        matches!(&frame, Ws::Pong(p) if p.as_ref() == [9]),
        "{frame:?}"
    );
}

// ---- Pairing on the pairing port ----

type PairClient = WebSocketStream<tokio_tungstenite::MaybeTlsStream<TcpStream>>;

/// Runs the device's side of the exchange up to the code, checking the daemon's commitment.
async fn pair_until_code(
    pair_addr: SocketAddr,
    device: &str,
    token: &[u8; 32],
) -> (PairClient, String) {
    let (mut ws, _) = connect_async(format!("ws://{pair_addr}/")).await.unwrap();
    let commit: [u8; 32] = Sha256::digest(token).into();
    let req = Message::PairReq {
        v: 1,
        device: device.into(),
        commit: encode_hex(&commit),
    };
    ws.send(Ws::text(serde_json::to_string(&req).unwrap()))
        .await
        .unwrap();
    let Message::PairOffer { name, cert, c_host } = recv(&mut ws).await else {
        panic!("expected pair_offer")
    };
    assert_eq!(name, "test-daemon");
    assert_eq!(cert, identity().cert_pem());

    let n_dev = [0x5a; 16];
    let nonce = Message::PairNonce {
        n_dev: encode_hex(&n_dev),
    };
    ws.send(Ws::text(serde_json::to_string(&nonce).unwrap()))
        .await
        .unwrap();
    let Message::PairReveal { n_host } = recv(&mut ws).await else {
        panic!("expected pair_reveal")
    };
    let n_host = decode_hex::<16>(&n_host).expect("n_host is 32 hex digits");
    assert_eq!(
        encode_hex(&Sha256::digest(n_host)),
        c_host,
        "the reveal must match c_host"
    );
    (ws, pairing_code(device, &commit, &cert, &n_dev, &n_host))
}

async fn wait_for_state(pairing: &Pairing, want: impl Fn(&PairingState) -> bool) -> PairingState {
    let mut states = pairing.subscribe();
    let state = tokio::time::timeout(Duration::from_secs(5), states.wait_for(|s| want(s)))
        .await
        .expect("the pairing state did not arrive in 5 s")
        .unwrap()
        .clone();
    state
}

/// `pairing` has no listener: it advertises no port, and nothing at `addr` offers to pair.
///
/// This doesn't insist that connecting is refused. The listener's port was ephemeral and is free
/// once the listener drops, so a test running in parallel can bind it before this looks, and its
/// connection would be accepted. Whatever holds the port then has to stay silent to a valid
/// `pair_req`, which no pairing listener does.
async fn assert_port_closed(pairing: &Pairing, addr: SocketAddr) {
    assert_eq!(
        *pairing.advertised().borrow(),
        None,
        "the pairing window still advertises a port"
    );
    let short = Duration::from_millis(500);
    let Ok(Ok(tcp)) = tokio::time::timeout(short, TcpStream::connect(addr)).await else {
        return; // refused, or nobody there
    };
    let Ok(Ok((mut ws, _))) =
        tokio::time::timeout(short, client_async(format!("ws://{addr}/"), tcp)).await
    else {
        return; // not a WebSocket server
    };
    let commit: [u8; 32] = Sha256::digest([0x42u8; 32]).into();
    let req = Message::PairReq {
        v: 1,
        device: OTHER.into(),
        commit: encode_hex(&commit),
    };
    if ws
        .send(Ws::text(serde_json::to_string(&req).unwrap()))
        .await
        .is_ok()
    {
        if let Ok(Some(Ok(Ws::Text(text)))) = tokio::time::timeout(short, ws.next()).await {
            let offer = serde_json::from_str::<Message>(text.as_str());
            assert!(
                !matches!(offer, Ok(Message::PairOffer { .. })),
                "a pairing listener is still answering at {addr}"
            );
        }
    }
}

fn wrong(code: &str) -> String {
    let n: u32 = code.parse().unwrap();
    format!("{:06}", (n + 1) % 1_000_000)
}

#[tokio::test]
async fn pairing_stores_the_device_and_its_token_then_opens_a_session() {
    let daemon = start().await;
    let pair_addr = daemon.pairing.start().await.unwrap();
    let token = [0x42; 32];
    let (mut ws, code) = pair_until_code(pair_addr, OTHER, &token).await;
    let state = wait_for_state(&daemon.pairing, |s| {
        matches!(s, PairingState::Confirm { .. })
    })
    .await;
    assert_eq!(
        state,
        PairingState::Confirm {
            device: OTHER.into(),
            peer: "127.0.0.1".into(),
        }
    );

    daemon.pairing.confirm(&code).unwrap();
    assert_eq!(recv(&mut ws).await, Message::PairResult { ok: true });
    expect_closed(&mut ws).await;
    let state = wait_for_state(&daemon.pairing, |s| {
        !matches!(s, PairingState::Confirm { .. })
    })
    .await;
    assert_eq!(
        state,
        PairingState::Paired {
            device: OTHER.into()
        }
    );
    assert_port_closed(&daemon.pairing, pair_addr).await;

    let ids: Vec<String> = daemon.devices.list().into_iter().map(|d| d.id).collect();
    assert_eq!(ids, [DEVICE, OTHER]);
    handshake(daemon.addr, OTHER, &token).await;
}

#[tokio::test]
async fn a_wrong_code_stores_nothing_and_ends_the_pairing() {
    let daemon = start().await;
    let pair_addr = daemon.pairing.start().await.unwrap();
    let token = [0x42; 32];
    let (mut ws, code) = pair_until_code(pair_addr, OTHER, &token).await;
    wait_for_state(&daemon.pairing, |s| {
        matches!(s, PairingState::Confirm { .. })
    })
    .await;

    assert!(
        daemon.pairing.confirm("12345").is_err(),
        "a malformed code is not a guess"
    );
    daemon.pairing.confirm(&wrong(&code)).unwrap();
    assert_eq!(recv(&mut ws).await, Message::PairResult { ok: false });
    let state = wait_for_state(&daemon.pairing, |s| {
        matches!(s, PairingState::Failed { .. })
    })
    .await;
    assert!(matches!(state, PairingState::Failed { reason } if reason.contains("Wrong code")));
    assert_port_closed(&daemon.pairing, pair_addr).await;
    assert!(
        daemon.pairing.confirm(&code).is_err(),
        "one guess per window"
    );

    assert_eq!(daemon.devices.list().len(), 1);
    assert_eq!(
        refused_hello(daemon.addr, hello(1, OTHER, Some(&token))).await,
        unauthorized()
    );
}

#[tokio::test]
async fn cancel_tells_the_waiting_device_and_closes_the_port() {
    let daemon = start().await;
    let pair_addr = daemon.pairing.start().await.unwrap();
    let (mut ws, _code) = pair_until_code(pair_addr, OTHER, &[0x42; 32]).await;
    wait_for_state(&daemon.pairing, |s| {
        matches!(s, PairingState::Confirm { .. })
    })
    .await;

    daemon.pairing.cancel().await;
    assert_eq!(recv(&mut ws).await, Message::PairResult { ok: false });
    assert_eq!(*daemon.pairing.subscribe().borrow(), PairingState::Closed);
    assert_port_closed(&daemon.pairing, pair_addr).await;
    assert_eq!(daemon.devices.list().len(), 1);
}

#[tokio::test]
async fn re_pairing_a_device_ends_its_live_session_with_unauthorized() {
    let daemon = start().await;
    let mut live = handshake(daemon.addr, DEVICE, &TOKEN).await;
    let pair_addr = daemon.pairing.start().await.unwrap();
    let token = [0x43; 32];
    let (mut ws, code) = pair_until_code(pair_addr, DEVICE, &token).await;
    wait_for_state(&daemon.pairing, |s| {
        matches!(s, PairingState::Confirm { .. })
    })
    .await;

    daemon.pairing.confirm(&code).unwrap();
    assert_eq!(recv(&mut ws).await, Message::PairResult { ok: true });
    assert_eq!(recv(&mut live).await, unauthorized());
    expect_closed(&mut live).await;
    assert_eq!(
        refused_hello(daemon.addr, hello(1, DEVICE, Some(&TOKEN))).await,
        unauthorized()
    );
    handshake(daemon.addr, DEVICE, &token).await;
}

#[tokio::test]
async fn a_device_speaking_out_of_turn_at_the_code_ends_the_window() {
    let daemon = start().await;
    let pair_addr = daemon.pairing.start().await.unwrap();
    let (mut ws, code) = pair_until_code(pair_addr, OTHER, &[0x42; 32]).await;
    wait_for_state(&daemon.pairing, |s| {
        matches!(s, PairingState::Confirm { .. })
    })
    .await;

    let nonce = Message::PairNonce {
        n_dev: encode_hex(&[0x5a; 16]),
    };
    ws.send(Ws::text(serde_json::to_string(&nonce).unwrap()))
        .await
        .unwrap();
    assert_eq!(recv(&mut ws).await, Message::PairResult { ok: false });
    let state = wait_for_state(&daemon.pairing, |s| {
        matches!(s, PairingState::Failed { .. })
    })
    .await;
    assert!(
        matches!(&state, PairingState::Failed { reason } if reason.contains("unexpected message")),
        "{state:?}"
    );
    assert_port_closed(&daemon.pairing, pair_addr).await;
    assert!(daemon.pairing.confirm(&code).is_err());
    assert_eq!(daemon.devices.list().len(), 1);
}

#[tokio::test]
async fn the_pairing_port_is_open_only_while_the_dialog_is() {
    let free = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let pair_addr = free.local_addr().unwrap();
    drop(free);
    let daemon = start_with(pair_addr, Duration::from_secs(30), CONFIG).await;
    let mut advertised = daemon.pairing.advertised();

    assert_port_closed(&daemon.pairing, pair_addr).await;
    assert_eq!(*advertised.borrow_and_update(), None);
    assert!(
        daemon.pairing.confirm("123456").is_err(),
        "no window is open"
    );

    assert_eq!(daemon.pairing.start().await.unwrap(), pair_addr);
    TcpStream::connect(pair_addr).await.unwrap();
    assert_eq!(*advertised.borrow_and_update(), Some(pair_addr.port()));

    daemon.pairing.cancel().await;
    assert_port_closed(&daemon.pairing, pair_addr).await;
    assert_eq!(*advertised.borrow_and_update(), None);
}

// Real clock: the window must close on its own.
#[tokio::test]
async fn an_unused_pairing_window_closes_when_it_times_out() {
    let daemon = start_with(
        "127.0.0.1:0".parse().unwrap(),
        Duration::from_secs(1),
        CONFIG,
    )
    .await;
    let started = Instant::now();
    let pair_addr = daemon.pairing.start().await.unwrap();
    let state = wait_for_state(&daemon.pairing, |s| {
        matches!(s, PairingState::Failed { .. })
    })
    .await;
    let waited = started.elapsed();
    assert!(waited >= Duration::from_secs(1), "{waited:?}");
    assert!(waited < Duration::from_secs(2), "{waited:?}");
    assert!(matches!(state, PairingState::Failed { reason } if reason.contains("in time")));
    assert_port_closed(&daemon.pairing, pair_addr).await;
}

#[tokio::test]
async fn a_device_waiting_on_the_code_is_told_when_the_window_times_out() {
    let daemon = start_with(
        "127.0.0.1:0".parse().unwrap(),
        Duration::from_secs(1),
        CONFIG,
    )
    .await;
    let pair_addr = daemon.pairing.start().await.unwrap();
    let (mut ws, code) = pair_until_code(pair_addr, OTHER, &[0x42; 32]).await;

    assert_eq!(recv(&mut ws).await, Message::PairResult { ok: false });
    wait_for_state(&daemon.pairing, |s| {
        matches!(s, PairingState::Failed { .. })
    })
    .await;
    assert!(daemon.pairing.confirm(&code).is_err());
    assert_eq!(daemon.devices.list().len(), 1);
}

#[tokio::test]
async fn a_bad_or_oversize_attempt_is_refused_and_the_window_stays_open() {
    let daemon = start().await;
    let pair_addr = daemon.pairing.start().await.unwrap();

    for bad in [
        "not json".to_string(),
        r#"{"t":"pair_req","v":1,"device":"wd-1","commit":"00"}"#.to_string(),
        r#"{"t":"pair_req","v":1,"device":"","commit":"630dcd2966c4336691125448bbb25b4ff412a49c732db2c8abc1b8581bd710dd"}"#.to_string(),
        r#"{"t":"hello","v":1,"device":"wd-1","fw":"0","screen":[1,1]}"#.to_string(),
    ] {
        let (mut ws, _) = connect_async(format!("ws://{pair_addr}/")).await.unwrap();
        ws.send(Ws::text(bad.clone())).await.unwrap();
        let Message::Error { code, .. } = recv(&mut ws).await else {
            panic!("expected error for {bad}")
        };
        assert_eq!(code, ErrorCode::BadRequest, "{bad}");
        expect_closed(&mut ws).await;
    }
    let (mut ws, _) = connect_async(format!("ws://{pair_addr}/")).await.unwrap();
    ws.send(Ws::text(
        r#"{"t":"pair_req","v":2,"device":"wd-1","commit":"00"}"#,
    ))
    .await
    .unwrap();
    assert!(matches!(
        recv(&mut ws).await,
        Message::Error {
            code: ErrorCode::Version,
            ..
        }
    ));
    // A pair_req the daemon would answer but for its size: the padding is a field it ignores.
    let (mut ws, _) = connect_async(format!("ws://{pair_addr}/")).await.unwrap();
    let commit = encode_hex(&Sha256::digest([0x42u8; 32]));
    let big = format!(
        r#"{{"t":"pair_req","v":1,"device":"{OTHER}","commit":"{commit}","pad":"{}"}}"#,
        "a".repeat(16 * 1024)
    );
    ws.send(Ws::text(big)).await.unwrap();
    expect_closed_unanswered(&mut ws).await;

    assert_eq!(*daemon.pairing.subscribe().borrow(), PairingState::Waiting);
    let (mut ws, code) = pair_until_code(pair_addr, OTHER, &[0x42; 32]).await;
    wait_for_state(&daemon.pairing, |s| {
        matches!(s, PairingState::Confirm { .. })
    })
    .await;
    daemon.pairing.confirm(&code).unwrap();
    assert_eq!(recv(&mut ws).await, Message::PairResult { ok: true });
}

// ---- Config and actions ----

/// `CONFIG` with `from` replaced by `to`.
fn config_with(from: &str, to: &str) -> String {
    assert!(CONFIG.contains(from), "{from}");
    CONFIG.replacen(from, to, 1)
}

const SECOND: &str = r##"
[[button]]
id = "second"
pos = [4, 2]
label = "Second"
bg = "#000000"
action = { kind = "shell", argv = ["true"] }
"##;

#[tokio::test]
async fn a_config_edit_reaches_a_connected_device_without_a_reconnect() {
    let mut daemon = start().await;
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    assert_eq!(
        daemon.events.recv().await.unwrap(),
        DaemonEvent::DeviceConnected(DEVICE.into())
    );

    let edited = config_with("label = \"Hello\"", "label = \"Hi\"") + SECOND;
    assert_eq!(daemon.reconfigure(&edited), Ok(true));
    let Message::Layout { rev, buttons, .. } = recv(&mut c).await else {
        panic!("expected the new layout")
    };
    assert_eq!(rev, 2);
    let shown: Vec<_> = buttons
        .iter()
        .map(|b| (b.id.as_str(), b.col, b.row, b.label.as_str(), b.bg))
        .collect();
    assert_eq!(
        shown,
        [
            ("hello", 0, 0, "Hi", 0x1e88e5),
            ("second", 4, 2, "Second", 0)
        ]
    );

    // The same session runs the new button, and nothing reconnected.
    c.send(press("second", 1)).await.unwrap();
    assert!(matches!(
        recv(&mut c).await,
        Message::Result {
            seq: 1,
            ok: true,
            ..
        }
    ));
    assert_eq!(
        daemon.events.recv().await.unwrap(),
        DaemonEvent::LayoutPushed(DEVICE.into()),
        "the window hears of the push"
    );
    assert!(
        daemon.events.try_recv().is_err(),
        "the session was replaced"
    );
}

#[tokio::test]
async fn an_invalid_config_is_never_pushed_and_the_last_good_one_keeps_working() {
    let daemon = start().await;
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    for bad in [
        config_with("cols = 5", "cols = 9"),
        config_with("label = \"Hello\"", "label = \"H\u{e9}llo\""),
        config_with("pos = [0, 0]", "pos = [5, 0]"),
        "not toml at all".to_string(),
    ] {
        let errors = daemon.reconfigure(&bad).unwrap_err();
        assert!(!errors.is_empty());
        assert_eq!(daemon.configs.status().borrow().errors, errors);
    }

    // A pushed layout would come before this press's result.
    c.send(press("hello", 1)).await.unwrap();
    let Message::Result { seq, text, .. } = recv(&mut c).await else {
        panic!("expected the press's result, not a layout")
    };
    assert_eq!((seq, text.as_str()), (1, "ran hello"));

    // A fresh session gets the last good layout too.
    let mut fresh = connect(daemon.addr).await;
    fresh.send(hello(1, DEVICE, Some(&TOKEN))).await.unwrap();
    assert!(matches!(recv(&mut fresh).await, Message::Welcome { .. }));
    let Message::Layout { cols, buttons, .. } = recv(&mut fresh).await else {
        panic!("expected layout")
    };
    assert_eq!((cols, buttons[0].label.as_str()), (5, "Hello"));
}

#[tokio::test]
async fn an_edit_to_actions_alone_pushes_no_layout_and_applies_at_once() {
    let daemon = start().await;
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    let edited = config_with("\"ran hello\"", "\"ran the new action\"");
    assert_eq!(daemon.reconfigure(&edited), Ok(true));
    c.send(press("hello", 1)).await.unwrap();
    let Message::Result { text, .. } = recv(&mut c).await else {
        panic!("expected the press's result, not a layout")
    };
    assert_eq!(text, "ran the new action");
}

#[tokio::test]
async fn a_second_press_while_a_button_runs_gets_already_running() {
    let daemon = start_configured(
        &(config_with("[\"echo\", \"ran hello\"]", "[\"sleep\", \"1\"]") + SECOND),
    )
    .await;
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    c.send(press("hello", 1)).await.unwrap();
    c.send(press("hello", 2)).await.unwrap();
    c.send(press("second", 3)).await.unwrap();

    let Message::Result {
        seq,
        ok,
        code,
        summary,
        ms,
        ..
    } = recv(&mut c).await
    else {
        panic!("expected result")
    };
    assert_eq!(
        (seq, ok, code, summary.as_str(), ms),
        (2, false, -1, "already running", 0)
    );
    // Another button runs meanwhile, and finishes first.
    assert!(matches!(
        recv(&mut c).await,
        Message::Result {
            seq: 3,
            ok: true,
            ..
        }
    ));
    let Message::Result { seq, ok, ms, .. } = recv(&mut c).await else {
        panic!("expected result")
    };
    assert!(seq == 1 && ok && ms >= 1000, "seq {seq}, ok {ok}, {ms} ms");

    // Once the run ends the button is free again.
    c.send(press("hello", 4)).await.unwrap();
    assert!(matches!(
        recv(&mut c).await,
        Message::Result {
            seq: 4,
            ok: true,
            ..
        }
    ));
}

#[tokio::test]
async fn a_session_answers_pings_while_an_action_runs() {
    let daemon = start_configured(&config_with(
        "[\"echo\", \"ran hello\"]",
        "[\"sleep\", \"1\"]",
    ))
    .await;
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    c.send(press("hello", 1)).await.unwrap();
    c.send(Ws::Ping(vec![9].into())).await.unwrap();
    let frame = c.next().await.unwrap().unwrap();
    assert!(
        matches!(&frame, Ws::Pong(p) if p.as_ref() == [9]),
        "{frame:?}"
    );
    assert!(matches!(
        recv(&mut c).await,
        Message::Result {
            seq: 1,
            ok: true,
            ..
        }
    ));
}

/// The value the daemon must never log or send to the device.
const SECRET: &str = "Bearer device-sim-s3cret";

static LOG_RECORDS: AtomicUsize = AtomicUsize::new(0);
static LOG_LEAKS: AtomicUsize = AtomicUsize::new(0);
static LOG_INSECURE: AtomicUsize = AtomicUsize::new(0);
/// Every warning about a device report that was accepted.
static LOG_REPORTS: Mutex<Vec<String>> = Mutex::new(Vec::new());

/// Installs [`LeakCheck`] as the process's logger, once, whichever test gets here first.
fn install_logger() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        log::set_logger(&LeakCheck).expect("only LeakCheck sets a logger");
        log::set_max_level(log::LevelFilter::Trace);
    });
}

/// Counts every log record at every level, those that contain [`SECRET`], the warnings about
/// secrets sent over plain http, and the warnings that accept a device report.
struct LeakCheck;

impl log::Log for LeakCheck {
    fn enabled(&self, _: &log::Metadata<'_>) -> bool {
        true
    }

    fn log(&self, record: &log::Record<'_>) {
        LOG_RECORDS.fetch_add(1, Ordering::SeqCst);
        let text = record.args().to_string();
        if text.contains(SECRET) {
            LOG_LEAKS.fetch_add(1, Ordering::SeqCst);
        }
        if record.level() == log::Level::Warn && text.contains("as insecure_http allows") {
            LOG_INSECURE.fetch_add(1, Ordering::SeqCst);
        }
        if record.level() == log::Level::Warn && text.contains(": device report ") {
            LOG_REPORTS.lock().unwrap().push(text);
        }
    }

    fn flush(&self) {}
}

/// An HTTP/1.1 server that answers each request with its own head, which holds the headers.
async fn echo_server() -> SocketAddr {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    tokio::spawn(async move {
        loop {
            let (mut socket, _) = listener.accept().await.unwrap();
            tokio::spawn(async move {
                let mut head = Vec::new();
                let mut byte = [0u8; 1];
                while !head.ends_with(b"\r\n\r\n") {
                    if socket.read(&mut byte).await.unwrap_or(0) == 0 {
                        return;
                    }
                    head.push(byte[0]);
                }
                let reply = format!(
                    "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    head.len()
                );
                let _ = socket.write_all(reply.as_bytes()).await;
                let _ = socket.write_all(&head).await;
            });
        }
    });
    addr
}

#[tokio::test]
async fn a_secret_header_reaches_the_server_but_never_the_device_or_the_logs() {
    install_logger();

    let server = echo_server().await;
    let daemon = start().await;
    daemon.secrets.set("tok", SECRET).unwrap();
    let edited = CONFIG.to_string()
        + &format!(
            r#"
[[button]]
id = "web"
pos = [1, 0]
label = "Web"
action = {{ kind = "http", method = "GET", url = "http://{server}/state", secret_headers = {{ Authorization = "tok" }} }}
"#
        );
    assert_eq!(daemon.reconfigure(&edited), Ok(true));
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    c.send(press("web", 1)).await.unwrap();
    let result = recv(&mut c).await;
    let Message::Result {
        ok, summary, text, ..
    } = &result
    else {
        panic!("expected result")
    };
    assert!(*ok, "{summary}");
    // The echo proves the header went out. The device gets it blanked.
    assert!(
        text.to_ascii_lowercase()
            .contains("authorization: <secret>"),
        "{text}"
    );
    assert!(!format!("{result:?}").contains(SECRET));

    // A secret that is missing is named, never shown.
    let gone = edited.replace("\"tok\"", "\"missing\"");
    let errors = daemon.reconfigure(&gone).unwrap_err();
    assert!(errors.iter().all(|e| !e.contains(SECRET)), "{errors:?}");

    // Plain http to another machine needs the opt-in, and going live with it warns.
    let lan = edited.replace(
        &format!("url = \"http://{server}/state\","),
        "url = \"http://ha.lan/state\",",
    );
    assert_ne!(lan, edited);
    let errors = daemon.reconfigure(&lan).unwrap_err();
    assert!(errors[0].contains("in the clear"), "{errors:?}");
    assert_eq!(LOG_INSECURE.load(Ordering::SeqCst), 0);
    let opted_in = lan.replace("secret_headers =", "insecure_http = true, secret_headers =");
    assert_eq!(daemon.reconfigure(&opted_in), Ok(true));
    assert_eq!(LOG_INSECURE.load(Ordering::SeqCst), 1);

    assert!(
        LOG_RECORDS.load(Ordering::SeqCst) > 0,
        "the logger saw nothing"
    );
    assert_eq!(
        LOG_LEAKS.load(Ordering::SeqCst),
        0,
        "a log record held the secret"
    );
}

// ---- Device reports ----

/// A checked-valid crash report. `elf` is also how a test finds its own log line.
fn crash_report(id: &str, elf: &str) -> Report {
    Report {
        id: id.into(),
        kind: "crash".into(),
        reason: "panic".into(),
        task: "link".into(),
        pc: Some("0x4ff01234".into()),
        ra: Some("0x4ff05678".into()),
        cause: Some("LoadAccessFault".into()),
        addr: Some("0x00000000".into()),
        elf: Some(elf.into()),
        same_build: true,
    }
}

fn report(report: Report) -> Ws {
    Ws::text(serde_json::to_string(&Message::Report(report)).unwrap())
}

fn accepted_lines(elf: &str) -> Vec<String> {
    let lines = LOG_REPORTS.lock().unwrap();
    lines.iter().filter(|l| l.contains(elf)).cloned().collect()
}

fn ack(id: &str) -> Message {
    Message::ReportAck { id: id.into() }
}

/// A press that proves the session is alive and that nothing else was queued before its result.
async fn assert_next_is_a_press_result(c: &mut Client) {
    c.send(press("hello", 99)).await.unwrap();
    assert!(
        matches!(
            recv(c).await,
            Message::Result {
                seq: 99,
                ok: true,
                ..
            }
        ),
        "expected the press result next"
    );
}

#[tokio::test]
async fn a_report_is_logged_once_then_acked_with_sentry_off() {
    install_logger();
    let elf = "c0".repeat(32);
    let daemon = start().await;
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    c.send(report(crash_report("a1b2c3d4", &elf)))
        .await
        .unwrap();
    assert_eq!(recv(&mut c).await, ack("a1b2c3d4"));
    // The line is written before the ack goes out.
    let lines = accepted_lines(&elf);
    assert_eq!(lines.len(), 1, "{lines:?}");
    assert!(lines[0].contains(DEVICE) && lines[0].contains("LoadAccessFault"));
    assert_next_is_a_press_result(&mut c).await;
}

#[tokio::test]
async fn a_session_takes_two_reports_and_drops_the_rest_without_ending() {
    install_logger();
    let elf = "c1".repeat(32);
    let daemon = start().await;
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    for id in ["00000001", "00000002", "00000003", "00000004"] {
        c.send(report(crash_report(id, &elf))).await.unwrap();
    }
    assert_eq!(recv(&mut c).await, ack("00000001"));
    assert_eq!(recv(&mut c).await, ack("00000002"));
    assert_next_is_a_press_result(&mut c).await;
    assert_eq!(accepted_lines(&elf).len(), 2);

    // The limit is per session: a new one takes reports again.
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    c.send(report(crash_report("00000005", &elf)))
        .await
        .unwrap();
    assert_eq!(recv(&mut c).await, ack("00000005"));
}

#[tokio::test]
async fn a_malformed_report_is_dropped_unacked_and_the_session_goes_on() {
    install_logger();
    let elf = "c2".repeat(32);
    let daemon = start().await;
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    let mut bad_kind = crash_report("00000001", &elf);
    bad_kind.kind = "dump".into();
    let mut long_task = crash_report("00000002", &elf);
    long_task.task = "x".repeat(17);
    c.send(report(bad_kind)).await.unwrap();
    // The two drops use up the session's limit of two.
    c.send(report(long_task)).await.unwrap();
    assert_next_is_a_press_result(&mut c).await;
    assert!(accepted_lines(&elf).is_empty());

    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    c.send(Ws::text(r#"{"t":"report"}"#)).await.unwrap();
    c.send(report(crash_report("00000003", &elf)))
        .await
        .unwrap();
    assert_eq!(recv(&mut c).await, ack("00000003"));
}

#[tokio::test]
async fn a_report_is_acked_when_sentry_is_on_even_if_the_budget_refuses_it() {
    install_logger();
    let elf = "c3".repeat(32);
    let dir = tempfile::tempdir().unwrap();
    let secrets = Arc::new(Secrets::new(dir.path()));
    // A loopback DSN with nothing listening: the client queues the event and the send fails
    // quietly, which is all this test needs.
    let sentry = config::SentryConfig {
        dsn: "http://key@127.0.0.1:9/1".into(),
        pii: false,
    };
    let reporting = reporting::start(Some(&sentry), secrets).unwrap();
    let daemon = start_reporting(
        "127.0.0.1:0".parse().unwrap(),
        Duration::from_secs(30),
        CONFIG,
        Some(reporting.clone()),
    )
    .await;
    // The same crash in two sessions: the second is over the per-device cap.
    for id in ["00000001", "00000002"] {
        let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
        c.send(report(crash_report(id, &elf))).await.unwrap();
        assert_eq!(recv(&mut c).await, ack(id));
    }
    assert_eq!(accepted_lines(&elf).len(), 2);
    reporting.close();
}

// ---- Icons ----

const ICON_CONFIG: &str = r##"
version = 1
[layout]
cols = 5
rows = 3

[[button]]
id = "pic"
pos = [0, 0]
label = "Pic"
icon = "a.png"
action = { kind = "shell", argv = ["echo", "ran pic"] }

[[button]]
id = "twin"
pos = [1, 0]
label = "Twin"
icon = "a.png"
action = { kind = "shell", argv = ["true"] }

[[button]]
id = "plain"
pos = [2, 0]
label = "Plain"
action = { kind = "shell", argv = ["true"] }
"##;

/// Red, green and blue quadrants and a transparent one, `side` pixels square, as PNG bytes.
fn quadrants_png(side: u32) -> Vec<u8> {
    let picture =
        image::RgbaImage::from_fn(side, side, |x, y| match (x < side / 2, y < side / 2) {
            (true, true) => image::Rgba([255, 0, 0, 255]),
            (false, true) => image::Rgba([0, 255, 0, 255]),
            (true, false) => image::Rgba([0, 0, 255, 255]),
            (false, false) => image::Rgba([0, 0, 0, 0]),
        });
    let mut out = std::io::Cursor::new(Vec::new());
    picture.write_to(&mut out, image::ImageFormat::Png).unwrap();
    out.into_inner()
}

fn put_icon(daemon: &Daemon, name: &str, bytes: &[u8]) {
    let dir = daemon.dir.path().join("icons");
    fs::create_dir_all(&dir).unwrap();
    fs::write(dir.join(name), bytes).unwrap();
}

/// Starts a daemon whose config names `a.png`, then connects a device that has the first layout.
async fn start_with_icon() -> (Daemon, Client, String) {
    let daemon = start().await;
    put_icon(&daemon, "a.png", &quadrants_png(200));
    daemon.reconfigure(ICON_CONFIG).unwrap();
    let mut c = handshake_without_layout(daemon.addr).await;
    let Message::Layout { buttons, .. } = recv(&mut c).await else {
        panic!("expected layout")
    };
    let icon = buttons[0]
        .icon
        .clone()
        .expect("the first button has an icon");
    assert_eq!(
        buttons[1].icon.as_deref(),
        Some(icon.as_str()),
        "one file, one id"
    );
    assert_eq!(buttons[2].icon, None);
    (daemon, c, icon)
}

async fn handshake_without_layout(addr: SocketAddr) -> Client {
    let mut c = connect(addr).await;
    c.send(hello(1, DEVICE, Some(&TOKEN))).await.unwrap();
    assert!(matches!(recv(&mut c).await, Message::Welcome { .. }));
    c
}

#[tokio::test]
async fn icon_get_answers_with_the_frame_the_layout_named() {
    let (_daemon, mut c, id) = start_with_icon().await;
    c.send(icon_get(&[&id])).await.unwrap();
    let icon = recv_icon(&mut c).await;
    assert_eq!(encode_hex(&icon.id), id);
    // 5x3 buttons leave 116 px inside the padding, less a two-line label and a gap: 44.
    assert_eq!((icon.w, icon.h), (44, 44));
    let pixel = |x: usize, y: usize| {
        let at = (y * 44 + x) * 4;
        icon.pixels[at..at + 4].to_vec()
    };
    assert_eq!(pixel(8, 8), [0, 0, 255, 255], "red, as B, G, R, A");
    assert_eq!(pixel(35, 8), [0, 255, 0, 255]);
    assert_eq!(pixel(8, 35), [255, 0, 0, 255]);
    assert_eq!(pixel(35, 35), [0, 0, 0, 0], "the transparent quadrant");

    // A repeated id gets one frame, and asking again later gets it again.
    c.send(icon_get(&[&id, &id])).await.unwrap();
    assert_eq!(recv_icon(&mut c).await, icon);
    c.send(press("pic", 1)).await.unwrap();
    assert!(matches!(
        recv(&mut c).await,
        Message::Result {
            seq: 1,
            ok: true,
            ..
        }
    ));
}

#[tokio::test]
async fn icon_get_skips_ids_the_layout_does_not_name() {
    let (_daemon, mut c, id) = start_with_icon().await;
    let unknown = "0".repeat(32);
    c.send(icon_get(&[&unknown, &id])).await.unwrap();
    assert_eq!(encode_hex(&recv_icon(&mut c).await.id), id);
    c.send(icon_get(&[&unknown])).await.unwrap();
    c.send(press("pic", 2)).await.unwrap();
    // Nothing came for the unknown id, so the next message is the press's result.
    assert!(matches!(recv(&mut c).await, Message::Result { seq: 2, .. }));
}

#[tokio::test]
async fn a_malformed_or_oversize_icon_get_gets_bad_request_then_close() {
    let (_daemon, _c, id) = start_with_icon().await;
    let too_many = vec![id.as_str(); 41];
    let forty = vec![id.as_str(); 40];
    for bad in [
        vec!["xyz"],
        vec![&id[..31]],
        vec!["ABCDEF0123456789ABCDEF0123456789"],
        vec![""],
        too_many,
    ] {
        let (daemon, _, _) = (&_daemon, 0, 0);
        let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
        c.send(icon_get(&bad)).await.unwrap();
        let Message::Error { code, .. } = recv(&mut c).await else {
            panic!("expected error for {bad:?}")
        };
        assert_eq!(code, ErrorCode::BadRequest, "{bad:?}");
        expect_closed(&mut c).await;
    }
    // Forty ids is the most a layout can name, and is fine. Forty copies of one get one frame.
    let mut c = handshake(_daemon.addr, DEVICE, &TOKEN).await;
    c.send(icon_get(&forty)).await.unwrap();
    assert_eq!(encode_hex(&recv_icon(&mut c).await.id), id);
    c.send(press("pic", 9)).await.unwrap();
    assert!(matches!(recv(&mut c).await, Message::Result { seq: 9, .. }));
}

#[tokio::test]
async fn an_icon_edit_pushes_a_new_layout_and_a_bad_one_pushes_nothing() {
    let (daemon, mut c, old) = start_with_icon().await;

    // A new picture under the same name changes the id, so devices get a layout and fetch it.
    let mut other = quadrants_png(200);
    let last = other.len() - 20;
    other[last] ^= 0xff; // inside the IDAT stream or its checksum
    put_icon(&daemon, "a.png", &other);
    assert!(
        daemon.reconfigure(ICON_CONFIG).is_err(),
        "a corrupt PNG is refused"
    );
    put_icon(&daemon, "a.png", &{
        let picture = image::RgbaImage::from_pixel(8, 8, image::Rgba([1, 2, 3, 255]));
        let mut out = std::io::Cursor::new(Vec::new());
        picture.write_to(&mut out, image::ImageFormat::Png).unwrap();
        out.into_inner()
    });
    assert_eq!(daemon.reconfigure(ICON_CONFIG), Ok(true));
    let Message::Layout { rev, buttons, .. } = recv(&mut c).await else {
        panic!("expected the new layout, not a layout for the bad file")
    };
    assert_eq!(rev, 2, "the refused file pushed nothing");
    let new = buttons[0].icon.clone().unwrap();
    assert_ne!(new, old);
    c.send(icon_get(&[&old, &new])).await.unwrap();
    let icon = recv_icon(&mut c).await;
    assert_eq!((encode_hex(&icon.id), icon.w, icon.h), (new, 8, 8));

    // A config naming a missing file is refused whole.
    let errors = daemon
        .reconfigure(&ICON_CONFIG.replacen("a.png", "gone.png", 1))
        .unwrap_err();
    assert!(errors[0].contains("\"gone.png\": not found"), "{errors:?}");
    c.send(press("pic", 3)).await.unwrap();
    assert!(matches!(recv(&mut c).await, Message::Result { seq: 3, .. }));
}

#[tokio::test]
async fn a_new_session_gets_the_same_icon_ids() {
    let (daemon, _c, id) = start_with_icon().await;
    let mut fresh = handshake_without_layout(daemon.addr).await;
    let Message::Layout { buttons, .. } = recv(&mut fresh).await else {
        panic!("expected layout")
    };
    assert_eq!(buttons[0].icon.as_deref(), Some(id.as_str()));
    fresh.send(icon_get(&[&id])).await.unwrap();
    assert_eq!(encode_hex(&recv_icon(&mut fresh).await.id), id);
}

#[tokio::test]
async fn a_binary_frame_from_the_device_is_a_bad_request() {
    let (_daemon, mut c, _) = start_with_icon().await;
    c.send(Ws::binary(vec![1, 2, 3])).await.unwrap();
    let Message::Error { code, .. } = recv(&mut c).await else {
        panic!("expected error")
    };
    assert_eq!(code, ErrorCode::BadRequest);
    expect_closed(&mut c).await;
}

// ---- Tiles ----

/// What a tile prints: `state` from the config dir, and a line in `polls` for each poll.
const SHOW_STATE: &str = "echo poll >> polls; cat state";

/// `up` has a tile that runs `script` every `every_s` seconds, and a press that rewrites `state`.
/// `plain` has no tile. Shell actions run in the config dir.
fn tile_config(every_s: u64, script: &str) -> String {
    format!(
        r#"
version = 1
[layout]
cols = 5
rows = 3

[[button]]
id = "up"
pos = [0, 0]
label = "Up"
action = {{ kind = "shell", argv = ["sh", "-c", "echo pressed > state"] }}
tile = {{ every_s = {every_s}, source = {{ kind = "shell", argv = ["sh", "-c", {script:?}] }} }}

[[button]]
id = "plain"
pos = [1, 0]
label = "Plain"
action = {{ kind = "shell", argv = ["true"] }}
"#
    )
}

fn set_state(daemon: &Daemon, text: &str) {
    fs::write(daemon.dir.path().join("state"), text).unwrap();
}

/// How many polls the `polls` file has recorded.
fn polls(daemon: &Daemon) -> usize {
    fs::read_to_string(daemon.dir.path().join("polls")).map_or(0, |text| text.lines().count())
}

/// The next message, which must arrive within `secs`.
async fn recv_within(c: &mut Client, secs: u64) -> Message {
    tokio::time::timeout(Duration::from_secs(secs), recv(c))
        .await
        .expect("no message in time")
}

async fn recv_tile(c: &mut Client, secs: u64) -> (String, String, TileState) {
    match recv_within(c, secs).await {
        Message::Tile { btn, text, state } => (btn, text, state),
        other => panic!("expected a tile, got {other:?}"),
    }
}

/// A device that has `welcome` and the layout, whose first tile is still to come.
async fn tile_device(daemon: &Daemon) -> Client {
    let mut c = handshake_without_layout(daemon.addr).await;
    let Message::Layout { buttons, .. } = recv(&mut c).await else {
        panic!("expected the layout")
    };
    assert_eq!(
        buttons.iter().map(|b| b.tile).collect::<Vec<_>>(),
        [true, false],
        "only `up` has a tile"
    );
    c
}

fn tile(btn: &str, text: &str, state: TileState) -> (String, String, TileState) {
    (btn.into(), text.into(), state)
}

#[tokio::test]
async fn a_tile_value_reaches_the_device_and_only_changes_are_sent() {
    let daemon = start_configured(&tile_config(2, SHOW_STATE)).await;
    set_state(&daemon, "one\n");
    let mut c = tile_device(&daemon).await;
    assert_eq!(recv_tile(&mut c, 3).await, tile("up", "one", TileState::Ok));

    // A poll that finds "one" again sends nothing: wait for one, then listen.
    while polls(&daemon) < 2 {
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    let heard = tokio::time::timeout(Duration::from_millis(500), recv(&mut c)).await;
    assert!(
        heard.is_err(),
        "an unchanged value was sent again: {heard:?}"
    );

    set_state(&daemon, "two\n");
    assert_eq!(recv_tile(&mut c, 6).await, tile("up", "two", TileState::Ok));
}

#[tokio::test]
async fn a_failing_source_shows_bad_and_recovers_to_ok() {
    let script = "if test -e flag; then echo UP; else echo DOWN; exit 1; fi";
    let daemon = start_configured(&tile_config(2, script)).await;
    let mut c = tile_device(&daemon).await;
    assert_eq!(
        recv_tile(&mut c, 3).await,
        tile("up", "DOWN", TileState::Bad)
    );
    fs::write(daemon.dir.path().join("flag"), "").unwrap();
    assert_eq!(recv_tile(&mut c, 6).await, tile("up", "UP", TileState::Ok));
}

#[tokio::test]
async fn a_press_polls_its_tile_again_without_waiting_for_the_interval() {
    // A day between polls, so only the press can explain a second value.
    let daemon = start_configured(&tile_config(86_400, SHOW_STATE)).await;
    set_state(&daemon, "one\n");
    let mut c = tile_device(&daemon).await;
    assert_eq!(recv_tile(&mut c, 3).await, tile("up", "one", TileState::Ok));

    c.send(press("up", 1)).await.unwrap();
    let mut seen = Vec::new();
    for _ in 0..2 {
        seen.push(recv_within(&mut c, 3).await);
    }
    assert!(
        seen.iter().any(|m| matches!(
            m,
            Message::Result {
                seq: 1,
                ok: true,
                ..
            }
        )),
        "{seen:?}"
    );
    assert!(
        seen.iter().any(|m| matches!(
            m,
            Message::Tile { btn, text, state: TileState::Ok } if btn == "up" && text == "pressed"
        )),
        "{seen:?}"
    );
    assert_eq!(polls(&daemon), 2);
}

#[tokio::test]
async fn polling_stops_when_the_device_leaves_and_resumes_when_it_returns() {
    let mut daemon = start_configured(&tile_config(2, SHOW_STATE)).await;
    set_state(&daemon, "one\n");
    let mut c = tile_device(&daemon).await;
    assert_eq!(recv_tile(&mut c, 3).await, tile("up", "one", TileState::Ok));
    while polls(&daemon) < 2 {
        tokio::time::sleep(Duration::from_millis(100)).await;
    }

    drop(c);
    loop {
        if daemon.events.recv().await.unwrap() == DaemonEvent::DeviceDisconnected(DEVICE.into()) {
            break;
        }
    }
    // A poll that was under way is cancelled; give it a moment to be gone.
    tokio::time::sleep(Duration::from_millis(300)).await;
    let before = polls(&daemon);
    tokio::time::sleep(Duration::from_millis(2600)).await;
    assert_eq!(polls(&daemon), before, "polled with no device connected");

    let mut c = tile_device(&daemon).await;
    assert_eq!(recv_tile(&mut c, 3).await, tile("up", "one", TileState::Ok));
    assert!(polls(&daemon) > before);
}

#[tokio::test]
async fn a_revoked_session_gets_no_more_tiles() {
    let daemon = start_configured(&tile_config(2, SHOW_STATE)).await;
    set_state(&daemon, "one\n");
    let mut c = tile_device(&daemon).await;
    assert_eq!(recv_tile(&mut c, 3).await, tile("up", "one", TileState::Ok));

    set_state(&daemon, "two\n");
    assert!(daemon.devices.forget(DEVICE).unwrap());
    let Message::Error { code, .. } = recv_within(&mut c, 3).await else {
        panic!("a revoked session ends with unauthorized, not a tile")
    };
    assert_eq!(code, ErrorCode::Unauthorized);
    expect_closed_unanswered_after_error(&mut c).await;

    let before = polls(&daemon);
    tokio::time::sleep(Duration::from_millis(2600)).await;
    assert_eq!(polls(&daemon), before, "polled for a forgotten device");
}

/// After an `error`, the server may only close.
async fn expect_closed_unanswered_after_error(c: &mut Client) {
    loop {
        match c.next().await {
            None | Some(Err(_)) | Some(Ok(Ws::Close(_))) => return,
            Some(Ok(Ws::Ping(_) | Ws::Pong(_))) => {}
            Some(Ok(frame)) => panic!("expected the connection to end, got {frame:?}"),
        }
    }
}

#[tokio::test]
async fn a_new_layout_sends_every_tile_again_and_a_tile_edit_needs_no_layout() {
    let daemon = start_configured(&tile_config(2, SHOW_STATE)).await;
    set_state(&daemon, "one\n");
    let mut c = tile_device(&daemon).await;
    assert_eq!(recv_tile(&mut c, 3).await, tile("up", "one", TileState::Ok));

    // A new label rebuilds the device's grid, which blanks its tiles, so the value comes again.
    let edited = tile_config(2, SHOW_STATE).replacen("Up", "Uptime", 1);
    assert_eq!(daemon.reconfigure(&edited), Ok(true));
    assert!(matches!(
        recv_within(&mut c, 3).await,
        Message::Layout { rev: 2, .. }
    ));
    assert_eq!(recv_tile(&mut c, 3).await, tile("up", "one", TileState::Ok));

    // A new source for the same tile changes no layout, and the next message is its value.
    let edited = tile_config(2, "echo other").replacen("Up", "Uptime", 1);
    assert_eq!(daemon.reconfigure(&edited), Ok(true));
    assert_eq!(
        recv_tile(&mut c, 3).await,
        tile("up", "other", TileState::Ok)
    );

    // A new label and a slow new source: the layout rebuilds the device's grid, and the old
    // source's "other" must not be sent back into it while the new one is still running.
    let edited = tile_config(2, "sleep 1; echo third").replacen("Up", "Upper", 1);
    assert_eq!(daemon.reconfigure(&edited), Ok(true));
    assert!(matches!(
        recv_within(&mut c, 3).await,
        Message::Layout { rev: 3, .. }
    ));
    assert_eq!(
        recv_tile(&mut c, 4).await,
        tile("up", "third", TileState::Ok)
    );
}

#[tokio::test]
async fn only_the_tile_that_changed_is_sent() {
    let steady = r#"
[[button]]
id = "steady"
pos = [2, 0]
label = "Steady"
action = { kind = "shell", argv = ["true"] }
tile = { every_s = 2, source = { kind = "shell", argv = ["echo", "same"] } }
"#;
    let daemon = start_configured(&(tile_config(2, SHOW_STATE) + steady)).await;
    set_state(&daemon, "one\n");
    let mut c = handshake_without_layout(daemon.addr).await;
    assert!(matches!(recv(&mut c).await, Message::Layout { .. }));
    let mut first = vec![recv_tile(&mut c, 3).await, recv_tile(&mut c, 3).await];
    first.sort_by(|a, b| a.0.cmp(&b.0));
    assert_eq!(
        first,
        [
            tile("steady", "same", TileState::Ok),
            tile("up", "one", TileState::Ok)
        ]
    );

    // "steady" is polled again too, and finds the same, so one change is one message.
    set_state(&daemon, "two\n");
    assert_eq!(recv_tile(&mut c, 6).await, tile("up", "two", TileState::Ok));
    let heard = tokio::time::timeout(Duration::from_millis(700), recv(&mut c)).await;
    assert!(
        heard.is_err(),
        "an unchanged tile was sent again: {heard:?}"
    );
}
