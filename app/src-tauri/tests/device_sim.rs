//! A fake device driving the daemon over a real socket, without mDNS. It pins the daemon's cert
//! the way the device does: the exact cert bytes, with no name or date checks.

use std::io;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, OnceLock};
use std::time::{Duration, Instant};

use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

use futures_util::{SinkExt, StreamExt};
use sha2::{Digest, Sha256};
use tokio::net::TcpStream;
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
use watchdeck_lib::daemon::actions::{ActionResult, Runner};
use watchdeck_lib::daemon::pairing::{Pairing, PairingState};
use watchdeck_lib::daemon::protocol::{
    decode_hex, encode_hex, pairing_code, ErrorCode, Message, Token,
};
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

fn stub_runner() -> Runner {
    Arc::new(|btn, device| {
        Box::pin(async move {
            ActionResult {
                ok: true,
                code: 0,
                text: format!("ran {btn} for {device}"),
                ms: 1,
            }
        })
    })
}

struct Daemon {
    addr: SocketAddr,
    events: broadcast::Receiver<DaemonEvent>,
    devices: Devices,
    pairing: Arc<Pairing>,
    _state: tempfile::TempDir,
}

/// A daemon on an ephemeral port with `DEVICE` paired, whose pairing listener binds `pair_addr`
/// for at most `window` once started, and whose actions `runner` runs.
async fn start_with(pair_addr: SocketAddr, window: Duration, runner: Runner) -> Daemon {
    let state = tempfile::tempdir().unwrap();
    let tls = identity().server_config().unwrap();
    let store = Store::load(state.path()).unwrap();
    let server = Server::bind("127.0.0.1:0", tls, runner, store)
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
        _state: state,
    };
    tokio::spawn(server.run());
    daemon
}

async fn start() -> Daemon {
    start_with(
        "127.0.0.1:0".parse().unwrap(),
        Duration::from_secs(30),
        stub_runner(),
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
    assert_eq!((b.label.as_str(), b.bg, b.fg), ("Hello", 1959605, 16777215));
}

#[tokio::test]
async fn press_hello_gets_one_ok_result() {
    let daemon = start().await;
    let addr = daemon.addr;
    let mut c = handshake(addr, DEVICE, &TOKEN).await;
    c.send(press("hello", 17)).await.unwrap();
    assert_eq!(
        recv(&mut c).await,
        Message::Result {
            btn: "hello".into(),
            seq: 17,
            ok: true,
            code: 0,
            text: format!("ran hello for {DEVICE}"),
            ms: 1,
        }
    );
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
            text: "unknown button".into(),
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

#[tokio::test]
async fn presses_queued_behind_a_forget_never_run() {
    let runs = Arc::new(AtomicUsize::new(0));
    let counter = runs.clone();
    let slow: Runner = Arc::new(move |_btn, _device| {
        let runs = counter.clone();
        Box::pin(async move {
            runs.fetch_add(1, Ordering::SeqCst);
            tokio::time::sleep(Duration::from_millis(200)).await;
            ActionResult {
                ok: true,
                code: 0,
                text: String::new(),
                ms: 200,
            }
        })
    });
    let daemon = start_with(
        "127.0.0.1:0".parse().unwrap(),
        Duration::from_secs(30),
        slow,
    )
    .await;
    let mut c = handshake(daemon.addr, DEVICE, &TOKEN).await;
    for seq in 1..=4 {
        c.send(press("hello", seq)).await.unwrap();
    }

    // Forget while the first press runs, with the other three queued behind it.
    let deadline = Instant::now() + Duration::from_secs(2);
    while runs.load(Ordering::SeqCst) == 0 {
        assert!(Instant::now() < deadline, "the first press never ran");
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
    assert!(daemon.devices.forget(DEVICE).unwrap());
    let started = runs.load(Ordering::SeqCst);

    assert!(matches!(recv(&mut c).await, Message::Result { seq: 1, .. }));
    assert_eq!(recv(&mut c).await, unauthorized());
    expect_closed(&mut c).await;
    assert_eq!(started, 1);
    assert_eq!(
        runs.load(Ordering::SeqCst),
        started,
        "a press started after forget returned"
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

async fn assert_port_closed(addr: SocketAddr) {
    let err = TcpStream::connect(addr)
        .await
        .expect_err("the pairing port must be closed");
    assert_eq!(err.kind(), io::ErrorKind::ConnectionRefused);
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
    assert_port_closed(pair_addr).await;

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
    assert_port_closed(pair_addr).await;
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
    assert_port_closed(pair_addr).await;
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
    assert_port_closed(pair_addr).await;
    assert!(daemon.pairing.confirm(&code).is_err());
    assert_eq!(daemon.devices.list().len(), 1);
}

#[tokio::test]
async fn the_pairing_port_is_open_only_while_the_dialog_is() {
    let free = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let pair_addr = free.local_addr().unwrap();
    drop(free);
    let daemon = start_with(pair_addr, Duration::from_secs(30), stub_runner()).await;
    let mut advertised = daemon.pairing.advertised();

    assert_port_closed(pair_addr).await;
    assert_eq!(*advertised.borrow_and_update(), None);
    assert!(
        daemon.pairing.confirm("123456").is_err(),
        "no window is open"
    );

    assert_eq!(daemon.pairing.start().await.unwrap(), pair_addr);
    TcpStream::connect(pair_addr).await.unwrap();
    assert_eq!(*advertised.borrow_and_update(), Some(pair_addr.port()));

    daemon.pairing.cancel().await;
    assert_port_closed(pair_addr).await;
    assert_eq!(*advertised.borrow_and_update(), None);
}

// Real clock: the window must close on its own.
#[tokio::test]
async fn an_unused_pairing_window_closes_when_it_times_out() {
    let daemon = start_with(
        "127.0.0.1:0".parse().unwrap(),
        Duration::from_secs(1),
        stub_runner(),
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
    assert_port_closed(pair_addr).await;
}

#[tokio::test]
async fn a_device_waiting_on_the_code_is_told_when_the_window_times_out() {
    let daemon = start_with(
        "127.0.0.1:0".parse().unwrap(),
        Duration::from_secs(1),
        stub_runner(),
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
