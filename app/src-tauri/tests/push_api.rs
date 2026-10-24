//! The push API against real listeners on free ports: auth, limits, the LAN listener's TLS, the
//! listeners following the config, and the CLI. Every config, secret and identity lives in a temp
//! dir. Every log line any test writes is captured, and one test checks that none holds a token.

use std::fs;
use std::net::SocketAddr;
use std::sync::{Arc, Mutex, Once, OnceLock};
use std::time::{Duration, Instant};

use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::watch;
use tokio::time::timeout;
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
use watchdeck_lib::cli::{self, Env};
use watchdeck_lib::daemon::actions::Actions;
use watchdeck_lib::daemon::config::{self, Configs};
use watchdeck_lib::daemon::integrations::{self, FeedHandle, FeedSlots, Integrations};
use watchdeck_lib::daemon::protocol::Feed;
use watchdeck_lib::daemon::push::{self, Addrs, Bound, PushApi, MAX_CONNECTIONS};
use watchdeck_lib::daemon::secrets::Secrets;
use watchdeck_lib::daemon::tls::Identity;

const TOKEN_HA: &str = "ha-token-0123456789abcdef0123456789abcdef";
const TOKEN_B: &str = "b-token-fedcba9876543210fedcba9876543210";
/// Long enough to be a token, and nobody's.
const WRONG: &str = "wrong-token-00000000000000000000000000000";
const UNAUTHORIZED: &str = r#"{"error":"unauthorized"}"#;

fn config_text(lan: bool, pushes: &[(&str, &str)]) -> String {
    let mut text = "version = 1\n[layout]\ncols = 2\nrows = 1\n".to_string();
    if lan {
        text.push_str("[push]\nlan = true\n");
    }
    for (id, label) in pushes {
        text.push_str(&format!(
            "[[integration]]\nid = \"{id}\"\nkind = \"push\"\nlabel = \"{label}\"\n\
             token_secret = \"push-{id}\"\n"
        ));
    }
    text
}

const BOTH: &[(&str, &str)] = &[("ha", "Home"), ("b", "Bee")];

/// Every log line of this test binary, from every test.
static LINES: Mutex<Vec<String>> = Mutex::new(Vec::new());

struct Capture;

impl log::Log for Capture {
    fn enabled(&self, _: &log::Metadata) -> bool {
        true
    }

    fn log(&self, record: &log::Record) {
        let line = format!("{} {}: {}", record.level(), record.target(), record.args());
        LINES.lock().unwrap().push(line);
    }

    fn flush(&self) {}
}

fn capture_logs() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        log::set_logger(&Capture).unwrap();
        log::set_max_level(log::LevelFilter::Trace);
    });
}

/// The identity every test serves, made once through the real load path.
fn identity() -> &'static Identity {
    static IDENTITY: OnceLock<Identity> = OnceLock::new();
    IDENTITY.get_or_init(|| {
        let dir = tempfile::tempdir().unwrap();
        Identity::load_or_create(dir.path()).unwrap()
    })
}

struct Rig {
    dir: tempfile::TempDir,
    configs: Arc<Configs>,
    feed: FeedHandle,
    push: PushApi,
    _integrations: Integrations,
    /// Held, or the integrations' manager would see the sessions' count end and stop.
    _connected: watch::Sender<usize>,
}

impl Rig {
    /// Replaces `config.toml` and reloads it.
    fn reconfigure(&self, text: &str) {
        fs::write(self.dir.path().join(config::FILE), text).unwrap();
        self.configs.reload().unwrap();
    }

    /// The bound addresses once `ready` holds for them.
    async fn bound(&self, ready: impl Fn(&Bound) -> bool) -> Bound {
        let mut rx = self.push.bound();
        let bound = *timeout(Duration::from_secs(5), rx.wait_for(|b| ready(b)))
            .await
            .expect("the listeners settle")
            .unwrap();
        bound
    }

    async fn loopback(&self) -> SocketAddr {
        self.bound(|b| b.loopback.is_some()).await.loopback.unwrap()
    }

    fn feed(&self) -> Arc<Feed> {
        self.feed.snapshot()
    }
}

/// The push API with `config` live and both tokens in `secrets.toml`, its loopback listener on a
/// free port and its LAN listener on `lan`.
async fn start_with(config: &str, lan: SocketAddr) -> Rig {
    capture_logs();
    let dir = tempfile::tempdir().unwrap();
    fs::write(dir.path().join(config::FILE), config).unwrap();
    let secrets = Arc::new(Secrets::new(dir.path()));
    secrets.set("push-ha", TOKEN_HA).unwrap();
    secrets.set("push-b", TOKEN_B).unwrap();
    let configs = Configs::open(dir.path(), secrets.clone()).unwrap();
    assert!(
        configs.status().borrow().errors.is_empty(),
        "the test config must be valid"
    );
    let actions = Actions::new(secrets.clone(), dir.path().to_path_buf()).unwrap();
    let (connected, connected_rx) = watch::channel(0);
    let integrations = integrations::spawn(FeedSlots(actions), configs.subscribe(), connected_rx);
    let feed = integrations.handle();
    let push = push::spawn(
        configs.subscribe(),
        secrets,
        feed.clone(),
        identity().server_config().unwrap(),
        Addrs {
            loopback: "127.0.0.1:0".parse().unwrap(),
            lan,
        },
    );
    Rig {
        dir,
        configs,
        feed,
        push,
        _integrations: integrations,
        _connected: connected,
    }
}

async fn start() -> Rig {
    start_with(&config_text(false, BOTH), "127.0.0.1:0".parse().unwrap()).await
}

/// A port nothing listens on, as far as anyone can know.
fn free_port() -> SocketAddr {
    let probe = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    probe.local_addr().unwrap()
}

fn raw(method: &str, path: &str, auth: Option<&str>, body: &str) -> Vec<u8> {
    let auth = auth.map_or(String::new(), |a| format!("Authorization: {a}\r\n"));
    format!(
        "{method} {path} HTTP/1.1\r\nHost: watchdeck\r\n{auth}Content-Length: {}\r\n\r\n{body}",
        body.len()
    )
    .into_bytes()
}

/// Writes `request`, reads the whole reply (the listener closes after one) and returns its status
/// and body. A reset instead of a reply fails the test.
async fn exchange<S: AsyncRead + AsyncWrite + Unpin>(
    mut stream: S,
    request: &[u8],
) -> (u16, String) {
    stream.write_all(request).await.unwrap();
    let mut reply = Vec::new();
    timeout(Duration::from_secs(10), stream.read_to_end(&mut reply))
        .await
        .expect("a reply in time")
        .expect("a reply, not a reset");
    let text = String::from_utf8(reply).unwrap();
    let (head, body) = text.split_once("\r\n\r\n").expect("an HTTP reply");
    let status = head.split(' ').nth(1).unwrap().parse().unwrap();
    (status, body.to_string())
}

async fn send(addr: SocketAddr, request: &[u8]) -> (u16, String) {
    exchange(TcpStream::connect(addr).await.unwrap(), request).await
}

/// One request with `token` as a bearer token.
async fn call(
    addr: SocketAddr,
    token: &str,
    method: &str,
    path: &str,
    body: &str,
) -> (u16, String) {
    let auth = format!("Bearer {token}");
    send(addr, &raw(method, path, Some(&auth), body)).await
}

async fn post_note(addr: SocketAddr, token: &str, body: &str) -> (u16, String) {
    call(addr, token, "POST", "/v1/notes", body).await
}

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

/// A TLS connection to `addr` that accepts only the daemon's certificate.
async fn connect_tls(addr: SocketAddr) -> tokio_rustls::client::TlsStream<TcpStream> {
    connect_tls_over(addr, TcpStream::connect(addr).await.unwrap()).await
}

/// [`connect_tls`] over the open connection `tcp` to `addr`.
async fn connect_tls_over(
    addr: SocketAddr,
    tcp: TcpStream,
) -> tokio_rustls::client::TlsStream<TcpStream> {
    let provider = Arc::new(aws_lc_rs::default_provider());
    let verifier = Pinned {
        cert: CertificateDer::from_pem_slice(identity().cert_pem().as_bytes()).unwrap(),
        algs: provider.signature_verification_algorithms,
    };
    let config = ClientConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .unwrap()
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(verifier))
        .with_no_client_auth();
    TlsConnector::from(Arc::new(config))
        .connect(ServerName::IpAddress(addr.ip().into()), tcp)
        .await
        .expect("the LAN listener presents the daemon's certificate")
}

async fn refused(addr: SocketAddr) -> bool {
    matches!(
        TcpStream::connect(addr).await,
        Err(e) if e.kind() == std::io::ErrorKind::ConnectionRefused
    )
}

#[tokio::test]
async fn every_refused_token_gets_the_same_401() {
    let rig = start().await;
    let addr = rig.loopback().await;
    let note = r#"{"title":"x"}"#;
    let cases: Vec<(&str, Vec<u8>)> = vec![
        ("no header", raw("POST", "/v1/notes", None, note)),
        (
            "Basic",
            raw(
                "POST",
                "/v1/notes",
                Some(&format!("Basic {TOKEN_HA}")),
                note,
            ),
        ),
        (
            "a wrong token",
            raw("POST", "/v1/notes", Some(&format!("Bearer {WRONG}")), note),
        ),
        (
            "an empty bearer",
            raw("POST", "/v1/notes", Some("Bearer "), note),
        ),
        (
            "half a token",
            raw(
                "POST",
                "/v1/notes",
                Some(&format!("Bearer {}", &TOKEN_HA[..20])),
                note,
            ),
        ),
        (
            "a route the integration lacks",
            raw(
                "POST",
                "/v1/grafana",
                Some(&format!("Bearer {TOKEN_HA}")),
                "{}",
            ),
        ),
        (
            "a path outside the API",
            raw("GET", "/", Some(&format!("Bearer {TOKEN_HA}")), ""),
        ),
    ];
    for (case, request) in cases {
        assert_eq!(
            send(addr, &request).await,
            (401, UNAUTHORIZED.to_string()),
            "{case}"
        );
    }
    assert!(rig.feed().notes.is_empty(), "nothing got through");
}

#[tokio::test]
async fn notes_are_added_replaced_and_deleted() {
    let rig = start().await;
    let addr = rig.loopback().await;
    let created = post_note(addr, TOKEN_HA, r#"{"title":"Backup done","id":"backup"}"#).await;
    assert_eq!(created, (201, r#"{"id":"ha/backup"}"#.to_string()));
    let feed = rig.feed();
    assert_eq!(feed.notes.len(), 1);
    assert_eq!(
        (feed.notes[0].title.as_str(), feed.notes[0].src.as_str()),
        ("Backup done", "Home")
    );

    let replaced = post_note(
        addr,
        TOKEN_HA,
        r#"{"title":"Backup failed","id":"backup","level":"error"}"#,
    )
    .await;
    assert_eq!(replaced.0, 201);
    let feed = rig.feed();
    assert_eq!(feed.notes.len(), 1, "replaced by id, not added");
    assert_eq!(feed.notes[0].title, "Backup failed");

    // A note without an id is numbered.
    assert_eq!(
        post_note(addr, TOKEN_HA, r#"{"title":"Hi"}"#).await,
        (201, r#"{"id":"ha/1"}"#.to_string())
    );

    assert_eq!(
        call(addr, TOKEN_HA, "DELETE", "/v1/notes/backup", "").await,
        (204, String::new())
    );
    assert_eq!(
        call(addr, TOKEN_HA, "DELETE", "/v1/notes/backup", "")
            .await
            .0,
        404
    );
    assert_eq!(
        call(addr, TOKEN_HA, "DELETE", "/v1/notes/nope", "").await.0,
        404
    );
    assert_eq!(rig.feed().notes.len(), 1);
}

#[tokio::test]
async fn cards_are_set_and_deleted_and_ids_are_percent_decoded() {
    let rig = start().await;
    let addr = rig.loopback().await;
    let card = r#"{"label":"Washer","value":"12 min","state":"ok"}"#;
    assert_eq!(
        call(addr, TOKEN_HA, "PUT", "/v1/cards/wash%20er", card).await,
        (204, String::new())
    );
    let feed = rig.feed();
    assert_eq!(
        (feed.cards[0].id.as_str(), feed.cards[0].value.as_str()),
        ("ha/wash er", "12 min")
    );
    assert_eq!(
        call(addr, TOKEN_HA, "DELETE", "/v1/cards/wash%20er", "")
            .await
            .0,
        204
    );
    assert_eq!(
        call(addr, TOKEN_HA, "DELETE", "/v1/cards/wash%20er", "")
            .await
            .0,
        404
    );
    assert_eq!(
        call(addr, TOKEN_HA, "DELETE", "/v1/cards/%zz", "").await.0,
        400
    );
    // A route the integration has, with a method it doesn't take.
    assert_eq!(call(addr, TOKEN_HA, "GET", "/v1/notes", "").await.0, 404);
}

#[tokio::test]
async fn a_token_writes_only_as_its_own_integration() {
    let rig = start().await;
    let addr = rig.loopback().await;
    assert_eq!(
        post_note(addr, TOKEN_HA, r#"{"title":"Mine","id":"x"}"#)
            .await
            .0,
        201
    );
    // b's delete of "x" is b's "x", which doesn't exist.
    assert_eq!(
        call(addr, TOKEN_B, "DELETE", "/v1/notes/x", "").await.0,
        404
    );
    assert_eq!(
        post_note(addr, TOKEN_B, r#"{"title":"Theirs","id":"x"}"#).await,
        (201, r#"{"id":"b/x"}"#.to_string())
    );
    let feed = rig.feed();
    let notes: Vec<(&str, &str)> = (feed.notes.iter())
        .map(|n| (n.id.as_str(), n.src.as_str()))
        .collect();
    assert_eq!(notes, [("b/x", "Bee"), ("ha/x", "Home")]);
}

#[tokio::test]
async fn bodies_over_8_kb_and_bad_json_are_refused() {
    let rig = start().await;
    let addr = rig.loopback().await;
    let padded = |len: usize| {
        let frame = r#"{"title":"t","body":""}"#;
        format!(
            r#"{{"title":"t","body":"{}"}}"#,
            "x".repeat(len - frame.len())
        )
    };
    let at_limit = padded(8 * 1024);
    assert_eq!(at_limit.len(), 8192);
    assert_eq!(post_note(addr, TOKEN_HA, &at_limit).await.0, 201);
    let over = padded(8 * 1024 + 1);
    let (status, body) = post_note(addr, TOKEN_HA, &over).await;
    assert_eq!(status, 413, "{body}");
    // Refused from its length, unread, and the reply still arrives whole: the listener drains
    // before it closes, so the close is no reset.
    let (status, body) = post_note(addr, TOKEN_HA, &padded(48 * 1024)).await;
    assert_eq!(status, 413, "{body}");

    // A chunked body has no length to check first, so the cap applies as it is read.
    let chunk = "x".repeat(5000);
    let chunked = format!(
        "POST /v1/notes HTTP/1.1\r\nHost: watchdeck\r\nAuthorization: Bearer {TOKEN_HA}\r\n\
         Transfer-Encoding: chunked\r\n\r\n{len:x}\r\n{chunk}\r\n{len:x}\r\n{chunk}\r\n0\r\n\r\n",
        len = chunk.len()
    );
    assert_eq!(send(addr, chunked.as_bytes()).await.0, 413);

    let (status, body) = post_note(addr, TOKEN_HA, r#"{"title":"t","tittle":"x"}"#).await;
    assert_eq!(status, 400, "an unknown field");
    assert!(body.contains("tittle"), "the reason says which: {body}");
    assert_eq!(
        post_note(addr, TOKEN_HA, r#"{"body":"x"}"#).await.0,
        400,
        "no title"
    );
    assert_eq!(rig.feed().notes.len(), 1);
}

#[tokio::test]
async fn a_slow_header_is_dropped_at_five_seconds() {
    let rig = start().await;
    let addr = rig.loopback().await;
    let (mut read, mut write) = TcpStream::connect(addr).await.unwrap().into_split();
    let started = Instant::now();
    // Ten seconds of header, a byte a second.
    let writer = tokio::spawn(async move {
        write
            .write_all(b"POST /v1/notes HTTP/1.1\r\nX-Slow: ")
            .await
            .unwrap();
        for _ in 0..10 {
            tokio::time::sleep(Duration::from_secs(1)).await;
            if write.write_all(b"x").await.is_err() {
                return;
            }
        }
    });
    let mut buf = Vec::new();
    // A close or a reset both end it; what matters is when.
    let _ = timeout(Duration::from_secs(12), read.read_to_end(&mut buf)).await;
    let took = started.elapsed();
    writer.abort();
    assert!(
        took >= Duration::from_millis(4500) && took < Duration::from_millis(7000),
        "dropped after {took:?}"
    );
    assert!(!String::from_utf8_lossy(&buf).contains("201"));
}

#[tokio::test]
async fn on_the_lan_port_the_handshake_counts_toward_the_five_seconds() {
    let rig = start_with(&config_text(true, BOTH), "127.0.0.1:0".parse().unwrap()).await;
    let lan = rig.bound(|b| b.lan.is_some()).await.lan.unwrap();
    let tcp = TcpStream::connect(lan).await.unwrap();
    let started = Instant::now();
    tokio::time::sleep(Duration::from_secs(3)).await;
    let tls = connect_tls_over(lan, tcp).await;
    let (mut read, _write) = tokio::io::split(tls);
    let mut buf = Vec::new();
    let _ = timeout(Duration::from_secs(12), read.read_to_end(&mut buf)).await;
    let took = started.elapsed();
    assert!(
        took >= Duration::from_millis(4500) && took < Duration::from_millis(6500),
        "dropped after {took:?}"
    );
}

#[tokio::test]
async fn bursts_past_thirty_get_429() {
    let rig = start().await;
    let addr = rig.loopback().await;
    let mut statuses = Vec::new();
    for _ in 0..40 {
        statuses.push(
            post_note(addr, TOKEN_HA, r#"{"title":"t","id":"same"}"#)
                .await
                .0,
        );
    }
    assert!(statuses[..30].iter().all(|&s| s == 201), "{statuses:?}");
    assert!(statuses[30..].contains(&429), "{statuses:?}");
}

/// A connection to `addr` from the loopback address `source`, such as `127.0.0.2`: any address in
/// 127/8 reaches the listener, and stands in for a LAN peer.
async fn connect_from(source: &str, addr: SocketAddr) -> TcpStream {
    let socket = tokio::net::TcpSocket::new_v4().unwrap();
    socket.bind(format!("{source}:0").parse().unwrap()).unwrap();
    socket.connect(addr).await.unwrap()
}

async fn post_from(source: &str, addr: SocketAddr, auth: Option<&str>) -> (u16, String) {
    let request = raw("POST", "/v1/notes", auth, r#"{"title":"t"}"#);
    exchange(connect_from(source, addr).await, &request).await
}

/// Whether the listener closes `stream` at once, without a reply.
async fn dropped(mut stream: TcpStream) -> bool {
    let mut buf = [0u8; 16];
    let read = timeout(Duration::from_secs(1), stream.read(&mut buf)).await;
    matches!(read, Ok(Ok(0) | Err(_)))
}

#[tokio::test]
async fn twenty_refused_bearers_shut_a_peer_out_but_never_localhost() {
    let rig = start().await;
    let addr = rig.loopback().await;
    let wrong = format!("Bearer {WRONG}");
    let good = format!("Bearer {TOKEN_HA}");
    for i in 0..push::MAX_FAILED_AUTHS {
        assert_eq!(
            post_from("127.0.0.1", addr, Some(&wrong)).await.0,
            401,
            "try {i}"
        );
        assert_eq!(
            post_from("127.0.0.2", addr, Some(&wrong)).await.0,
            401,
            "try {i}"
        );
    }
    // Every local sender is 127.0.0.1, so it is never shut out.
    assert_eq!(post_from("127.0.0.1", addr, Some(&good)).await.0, 201);
    // Another address is, at accept, even with the right token.
    let shut_out = connect_from("127.0.0.2", addr).await;
    assert!(dropped(shut_out).await, "dropped before any reply");
    assert_eq!(
        post_from("127.0.0.3", addr, Some(&good)).await.0,
        201,
        "only that peer"
    );
}

#[tokio::test]
async fn requests_without_a_bearer_never_shut_a_peer_out() {
    let rig = start().await;
    let addr = rig.loopback().await;
    for _ in 0..push::MAX_FAILED_AUTHS + 5 {
        assert_eq!(post_from("127.0.0.2", addr, None).await.0, 401);
        assert_eq!(
            post_from("127.0.0.4", addr, Some(&format!("Basic {WRONG}")))
                .await
                .0,
            401
        );
    }
    let good = format!("Bearer {TOKEN_HA}");
    assert_eq!(post_from("127.0.0.2", addr, Some(&good)).await.0, 201);
    assert_eq!(post_from("127.0.0.4", addr, Some(&good)).await.0, 201);
}

#[tokio::test]
async fn a_browser_request_is_refused_and_costs_the_peer_nothing() {
    let rig = start().await;
    let addr = rig.loopback().await;
    let good = format!("Bearer {TOKEN_HA}");
    // Past the burst of 30, so a request that counted would end in 429.
    for i in 0..40 {
        let header = if i % 2 == 0 {
            "Origin: https://evil.example"
        } else {
            "Sec-Fetch-Site: cross-site"
        };
        let request = format!(
            "POST /v1/notes HTTP/1.1\r\nHost: watchdeck\r\n{header}\r\nAuthorization: {good}\r\n\
             Content-Length: 13\r\n\r\n{{\"title\":\"t\"}}"
        );
        let (status, body) =
            exchange(connect_from("127.0.0.2", addr).await, request.as_bytes()).await;
        assert_eq!(
            (status, body.as_str()),
            (403, r#"{"error":"browsers may not call the push API"}"#)
        );
    }
    assert!(rig.feed().notes.is_empty());
    assert_eq!(post_from("127.0.0.2", addr, Some(&good)).await.0, 201);
}

#[tokio::test]
async fn one_peer_holds_at_most_four_connections() {
    let rig = start().await;
    let addr = rig.loopback().await;
    let mut held = Vec::new();
    for _ in 0..push::MAX_PEER_CONNECTIONS {
        held.push(connect_from("127.0.0.2", addr).await);
    }
    tokio::time::sleep(Duration::from_millis(200)).await;
    assert!(dropped(connect_from("127.0.0.2", addr).await).await);
    let good = format!("Bearer {TOKEN_HA}");
    assert_eq!(
        post_from("127.0.0.3", addr, Some(&good)).await.0,
        201,
        "others still get in"
    );
    drop(held.pop());
    tokio::time::sleep(Duration::from_millis(200)).await;
    assert_eq!(
        post_from("127.0.0.2", addr, Some(&good)).await.0,
        201,
        "a slot came back"
    );
}

#[tokio::test]
async fn each_listener_has_its_own_32_connections() {
    let rig = start_with(&config_text(true, BOTH), "127.0.0.1:0".parse().unwrap()).await;
    let bound = rig.bound(|b| b.loopback.is_some() && b.lan.is_some()).await;
    let (addr, lan) = (bound.loopback.unwrap(), bound.lan.unwrap());
    // From 127.0.0.1, which has no per-peer cap, so only the listener's slots stop it.
    let mut held = Vec::new();
    for _ in 0..MAX_CONNECTIONS {
        held.push(TcpStream::connect(addr).await.unwrap());
    }
    tokio::time::sleep(Duration::from_millis(200)).await;
    assert!(dropped(TcpStream::connect(addr).await.unwrap()).await);
    // The LAN port's slots are its own.
    let request = raw(
        "POST",
        "/v1/notes",
        Some(&format!("Bearer {TOKEN_HA}")),
        r#"{"title":"t"}"#,
    );
    assert_eq!(exchange(connect_tls(lan).await, &request).await.0, 201);

    drop(held.pop());
    tokio::time::sleep(Duration::from_millis(200)).await;
    assert_eq!(post_note(addr, TOKEN_HA, r#"{"title":"t"}"#).await.0, 201);
}

#[tokio::test]
async fn a_changed_token_works_within_a_second_with_no_config_change() {
    let rig = start().await;
    let addr = rig.loopback().await;
    let secrets = Secrets::new(rig.dir.path());
    let rotated = "rotated-token-11111111111111111111111111111";
    secrets.set("push-ha", rotated).unwrap();
    let started = Instant::now();
    while post_note(addr, rotated, r#"{"title":"t"}"#).await.0 != 201 {
        assert!(
            started.elapsed() < Duration::from_secs(2),
            "the new token never worked"
        );
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    assert_eq!(
        post_note(addr, TOKEN_HA, r#"{"title":"t"}"#).await.0,
        401,
        "the old one is gone"
    );
}

#[tokio::test]
async fn the_listeners_follow_the_config_and_the_lan_one_speaks_only_tls() {
    let lan = free_port();
    let rig = start_with(&config_text(false, BOTH), lan).await;
    let loopback = rig.loopback().await;
    assert_eq!(rig.bound(|_| true).await.lan, None);
    assert!(refused(lan).await, "no LAN listener without lan = true");

    rig.reconfigure(&config_text(true, BOTH));
    let bound = rig.bound(|b| b.lan.is_some()).await;
    assert_eq!(bound.lan, Some(lan));
    assert_eq!(
        bound.loopback,
        Some(loopback),
        "the loopback one is left alone"
    );
    let request = raw(
        "POST",
        "/v1/notes",
        Some(&format!("Bearer {TOKEN_HA}")),
        r#"{"title":"Over TLS"}"#,
    );
    assert_eq!(exchange(connect_tls(lan).await, &request).await.0, 201);
    assert_eq!(rig.feed().notes[0].title, "Over TLS");

    // Plain HTTP to the LAN port never gets an HTTP answer.
    let mut plain = TcpStream::connect(lan).await.unwrap();
    plain.write_all(&request).await.unwrap();
    let mut reply = Vec::new();
    let _ = timeout(Duration::from_secs(10), plain.read_to_end(&mut reply)).await;
    assert!(
        !reply.starts_with(b"HTTP/"),
        "{:?}",
        String::from_utf8_lossy(&reply)
    );
    assert_eq!(rig.feed().notes.len(), 1);
    let warned = format!(
        "plain HTTP on port {} from 127.0.0.1; replace that sender's token",
        lan.port()
    );
    assert!(
        LINES.lock().unwrap().iter().any(|l| l.contains(&warned)),
        "the sender is named so its token can be replaced"
    );

    rig.reconfigure(&config_text(false, BOTH));
    rig.bound(|b| b.lan.is_none()).await;
    assert!(refused(lan).await, "closed when lan is turned off");

    // With no push integration, neither listens, even with lan = true.
    rig.reconfigure(&config_text(true, &[]));
    rig.bound(|b| b.loopback.is_none() && b.lan.is_none()).await;
    assert!(refused(loopback).await && refused(lan).await);

    rig.reconfigure(&config_text(true, BOTH));
    let bound = rig.bound(|b| b.loopback.is_some() && b.lan.is_some()).await;
    assert_eq!(bound.lan, Some(lan), "the port can be taken again at once");
    assert_eq!(
        post_note(bound.loopback.unwrap(), TOKEN_B, r#"{"title":"back"}"#)
            .await
            .0,
        201
    );
}

/// Runs the CLI with `args` against `env` off the runtime's threads, as a script would.
async fn run_cli(args: &[&str], env: Env) -> (i32, String, String) {
    let args: Vec<String> = args.iter().map(|a| a.to_string()).collect();
    tokio::task::spawn_blocking(move || {
        let (mut out, mut err) = (Vec::new(), Vec::new());
        let code = cli::run(&args, &env, &mut out, &mut err);
        (
            code,
            String::from_utf8(out).unwrap(),
            String::from_utf8(err).unwrap(),
        )
    })
    .await
    .unwrap()
}

fn env_token(addr: SocketAddr, token: &str) -> Env {
    Env {
        addr,
        token: Some(token.into()),
        dir: None,
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn the_cli_posts_with_the_token_from_the_environment() {
    let rig = start().await;
    let addr = rig.loopback().await;
    let (code, out, err) = run_cli(
        &[
            "notify",
            "--id",
            "b1",
            "--level",
            "ok",
            "Backup done",
            "4 min",
        ],
        env_token(addr, TOKEN_HA),
    )
    .await;
    assert_eq!((code, out.as_str()), (0, "ha/b1\n"), "{err}");
    let feed = rig.feed();
    assert_eq!(
        (feed.notes[0].title.as_str(), feed.notes[0].body.as_str()),
        ("Backup done", "4 min")
    );

    let (code, _, err) = run_cli(
        &["card", "--state", "bad", "washer", "12 min"],
        env_token(addr, TOKEN_HA),
    )
    .await;
    assert_eq!(code, 0, "{err}");
    assert_eq!(rig.feed().cards[0].id, "ha/washer");

    assert_eq!(
        run_cli(&["dismiss", "b1"], env_token(addr, TOKEN_HA))
            .await
            .0,
        0
    );
    assert!(rig.feed().notes.is_empty());

    // Refusals exit 1 with the daemon's reason.
    let (code, _, err) = run_cli(&["dismiss", "b1"], env_token(addr, TOKEN_HA)).await;
    assert_eq!(code, 1);
    assert!(err.contains("404") && err.contains("not found"), "{err}");
    let (code, _, err) = run_cli(
        &["notify", "--level", "loud", "t"],
        env_token(addr, TOKEN_HA),
    )
    .await;
    assert_eq!(code, 1);
    assert!(err.contains("400") && err.contains("loud"), "{err}");
    let (code, _, err) = run_cli(&["notify", "t"], env_token(addr, WRONG)).await;
    assert_eq!(code, 1);
    assert!(
        err.contains("401") && err.contains("unauthorized") && !err.contains(WRONG),
        "{err}"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn the_cli_reads_the_token_from_the_config_dir() {
    let rig = start().await;
    let addr = rig.loopback().await;
    let files = |addr| Env {
        addr,
        token: None,
        dir: Some(rig.dir.path().to_path_buf()),
    };
    let (code, out, err) =
        run_cli(&["notify", "--integration", "b", "From files"], files(addr)).await;
    assert_eq!((code, out.as_str()), (0, "b/1\n"), "{err}");
    assert_eq!(rig.feed().notes[0].src, "Bee");

    let (code, _, err) = run_cli(&["notify", "t"], files(addr)).await;
    assert_eq!(code, 1);
    assert!(err.contains("several push integrations (ha, b)"), "{err}");
    let (code, _, err) = run_cli(&["notify", "--integration", "nope", "t"], files(addr)).await;
    assert_eq!(code, 1);
    assert!(err.contains("no push integration \"nope\""), "{err}");

    // With one push integration, it needs no --integration.
    rig.reconfigure(&config_text(false, &[("ha", "Home")]));
    let (code, out, err) = run_cli(&["notify", "Only one"], files(addr)).await;
    assert_eq!((code, out.as_str()), (0, "ha/2\n"), "{err}");
    for text in [&out, &err] {
        assert!(!text.contains(TOKEN_HA) && !text.contains(TOKEN_B));
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn the_cli_exits_2_when_nothing_listens() {
    capture_logs();
    let (code, out, err) = run_cli(&["notify", "hi"], env_token(free_port(), TOKEN_HA)).await;
    assert_eq!((code, out.as_str()), (2, ""));
    assert!(
        err.contains("nothing is listening") && !err.contains(TOKEN_HA),
        "{err}"
    );
}

#[tokio::test(flavor = "multi_thread")]
async fn no_log_line_holds_a_token() {
    let rig = start().await;
    let addr = rig.loopback().await;
    // The token in every place a careless caller might put it.
    for token in [TOKEN_HA, TOKEN_B, WRONG] {
        post_note(
            addr,
            token,
            &format!(r#"{{"title":"{token}","body":"{token}"}}"#),
        )
        .await;
        call(addr, token, "DELETE", &format!("/v1/notes/{token}"), "").await;
        call(
            addr,
            token,
            "PUT",
            &format!("/v1/cards/{token}?t={token}"),
            &format!(r#"{{"label":"{token}","value":"{token}","bogus":"{token}"}}"#),
        )
        .await;
        call(addr, token, "POST", &format!("/v1/{token}"), "").await;
        send(
            addr,
            &raw(token, "/v1/notes", Some(&format!("Bearer {token}")), "{}"),
        )
        .await;
        send(
            addr,
            &raw("POST", "/v1/notes", Some(&format!("Basic {token}")), "{}"),
        )
        .await;
        send(
            addr,
            &raw("POST", "/v1/notes", Some(&format!("{token} {token}")), "{}"),
        )
        .await;
        run_cli(&["notify", token], env_token(addr, token)).await;
    }
    let lines = LINES.lock().unwrap().clone();
    assert!(
        lines
            .iter()
            .any(|l| l.contains("push ha: POST /v1/notes 201")),
        "the capture sees the push log: {lines:#?}"
    );
    assert!(lines
        .iter()
        .any(|l| l.contains("push ha: DELETE /v1/notes/{id} 404")));
    for line in &lines {
        for token in [TOKEN_HA, TOKEN_B, WRONG] {
            assert!(!line.contains(token), "a token in the log: {line}");
            // Nor a good part of one.
            assert!(
                !line.contains(&token[..16]),
                "part of a token in the log: {line}"
            );
        }
    }
}
