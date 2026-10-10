//! A fake device driving the daemon over a real socket, without mDNS. It pins the daemon's cert
//! the way the device does: the exact cert bytes, with no name or date checks.

use std::io;
use std::net::SocketAddr;
use std::sync::{Arc, OnceLock};
use std::time::{Duration, Instant};

use tokio::io::{AsyncReadExt, AsyncWriteExt};

use futures_util::{SinkExt, StreamExt};
use tokio::net::TcpStream;
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
use watchdeck_lib::daemon::protocol::{ErrorCode, Message};
use watchdeck_lib::daemon::server::{DaemonEvent, Server};
use watchdeck_lib::daemon::tls::Identity;

type Client = WebSocketStream<TlsStream<TcpStream>>;

const DEVICE: &str = "wd-000000000001";

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

async fn start() -> (SocketAddr, tokio::sync::broadcast::Receiver<DaemonEvent>) {
    let tls = identity().server_config().unwrap();
    let server = Server::bind("127.0.0.1:0", tls, stub_runner())
        .await
        .unwrap();
    let addr = server.local_addr().unwrap();
    let events = server.subscribe();
    tokio::spawn(server.run());
    (addr, events)
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

fn hello(v: u32, device: &str) -> Ws {
    Ws::text(
        serde_json::to_string(&Message::Hello {
            v,
            device: device.into(),
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

async fn recv(c: &mut Client) -> Message {
    loop {
        match c.next().await.expect("stream ended").unwrap() {
            Ws::Text(t) => return serde_json::from_str(t.as_str()).unwrap(),
            Ws::Ping(_) | Ws::Pong(_) => {}
            other => panic!("unexpected frame {other:?}"),
        }
    }
}

/// Reads until the server ends the connection, however it chooses to (close frame, EOF or reset).
async fn expect_closed(c: &mut Client) {
    loop {
        match c.next().await {
            None | Some(Err(_)) | Some(Ok(Ws::Close(_))) => return,
            Some(Ok(_)) => {}
        }
    }
}

async fn handshake(addr: SocketAddr, device: &str) -> Client {
    let mut c = connect(addr).await;
    c.send(hello(1, device)).await.unwrap();
    assert!(matches!(recv(&mut c).await, Message::Welcome { .. }));
    assert!(matches!(recv(&mut c).await, Message::Layout { .. }));
    c
}

#[tokio::test]
async fn a_client_pinning_another_cert_fails_the_handshake() {
    let (addr, _events) = start().await;
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
    let (addr, _events) = start().await;
    assert!(connect_async(format!("ws://{addr}/")).await.is_err());
    // The refused attempt must not have taken the listener down.
    handshake(addr, DEVICE).await;
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
    let (addr, _events) = start().await;
    let started = Instant::now();
    let tcp = TcpStream::connect(addr).await.unwrap();
    let waited = time_to_close(tcp, started).await;
    assert!(waited >= Duration::from_secs(5), "{waited:?}");
    assert!(waited < Duration::from_secs(6), "{waited:?}");
}

#[tokio::test]
async fn a_client_hello_that_stalls_midway_is_closed_after_five_seconds() {
    let (addr, _events) = start().await;
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
    let (addr, _events) = start().await;
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
    c.send(hello(1, DEVICE)).await.unwrap();
    assert!(matches!(recv(&mut c).await, Message::Welcome { .. }));
}

#[tokio::test]
async fn hello_gets_welcome_then_the_layout() {
    let (addr, _events) = start().await;
    let mut c = connect(addr).await;
    c.send(hello(1, DEVICE)).await.unwrap();

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
    let (addr, _events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
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
    let (addr, _events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
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
    let (addr, _events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
    c.send(Ws::text(r#"{"t":"future","x":1}"#)).await.unwrap();
    c.send(press("hello", 1)).await.unwrap();
    assert!(matches!(
        recv(&mut c).await,
        Message::Result { ok: true, .. }
    ));
}

#[tokio::test]
async fn wrong_version_gets_an_error_then_close() {
    let (addr, _events) = start().await;
    let mut c = connect(addr).await;
    c.send(hello(2, DEVICE)).await.unwrap();
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
        let (addr, _events) = start().await;
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
    let (addr, _events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
    c.send(press(&"a".repeat(65), 1)).await.unwrap();
    let Message::Error { code, .. } = recv(&mut c).await else {
        panic!("expected error")
    };
    assert_eq!(code, ErrorCode::BadRequest);
    expect_closed(&mut c).await;
}

#[tokio::test]
async fn malformed_hello_gets_bad_request_then_close() {
    let (addr, _events) = start().await;
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
    let (addr, _events) = start().await;
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
    let (addr, mut events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
    assert_eq!(
        events.recv().await.unwrap(),
        DaemonEvent::DeviceConnected(DEVICE.into())
    );
    let started = Instant::now();
    expect_closed(&mut c).await;
    let waited = started.elapsed();
    assert!(waited >= Duration::from_secs(29), "{waited:?}");
    assert!(waited < Duration::from_secs(31), "{waited:?}");
    assert_eq!(
        events.recv().await.unwrap(),
        DaemonEvent::DeviceDisconnected(DEVICE.into())
    );
}

#[tokio::test]
async fn pings_are_answered_with_pongs() {
    let (addr, _events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
    c.send(Ws::Ping(vec![7].into())).await.unwrap();
    let frame = c.next().await.unwrap().unwrap();
    assert!(matches!(frame, Ws::Pong(p) if p.as_ref() == [7]));
}

#[tokio::test]
async fn a_message_over_16_kb_closes_the_session() {
    let (addr, _events) = start().await;
    let mut c = handshake(addr, DEVICE).await;
    let big = format!(
        r#"{{"t":"press","btn":"{}","seq":1}}"#,
        "a".repeat(16 * 1024)
    );
    c.send(Ws::text(big)).await.unwrap();
    expect_closed(&mut c).await;
}

#[tokio::test]
async fn reconnecting_device_replaces_its_old_session() {
    let (addr, mut events) = start().await;
    let mut old = handshake(addr, DEVICE).await;
    let mut new = handshake(addr, DEVICE).await;

    expect_closed(&mut old).await;

    // The new session still works.
    new.send(press("hello", 1)).await.unwrap();
    assert!(matches!(
        recv(&mut new).await,
        Message::Result { ok: true, .. }
    ));

    assert_eq!(
        events.recv().await.unwrap(),
        DaemonEvent::DeviceConnected(DEVICE.into())
    );
    assert_eq!(
        events.recv().await.unwrap(),
        DaemonEvent::DeviceDisconnected(DEVICE.into())
    );
    assert_eq!(
        events.recv().await.unwrap(),
        DaemonEvent::DeviceConnected(DEVICE.into())
    );
}
