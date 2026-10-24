//! The push API's listeners: `127.0.0.1:47822` over plain HTTP while any push integration is
//! configured, and with `[push] lan = true` also `0.0.0.0:47823` over HTTPS with the daemon's own
//! certificate. Never plain HTTP on the LAN, since the token would cross it in clear. Neither port
//! is advertised.
//!
//! Every request names its integration by its bearer token, a `secrets.toml` value of at least
//! [`MIN_TOKEN_BYTES`]. The routes are the integration's [`Receiver`](super::integrations)'s.
//! Every refusal of a token gets the same 401, so a caller can't tell a missing token from a
//! wrong one, or a wrong one from one used on another integration's route. No log line holds a
//! token, a header value or a body.

use std::collections::HashMap;
use std::convert::Infallible;
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use aws_lc_rs::constant_time::verify_slices_are_equal;
use http_body_util::{BodyExt, Full, LengthLimitError, Limited};
use hyper::body::{Body, Bytes, Incoming};
use hyper::header::{HeaderValue, AUTHORIZATION, CONTENT_TYPE};
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{Method, Request, Response, StatusCode};
use hyper_util::rt::{TokioIo, TokioTimer};
use sha2::{Digest, Sha256};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{watch, Semaphore};
use tokio::task::JoinSet;
use tokio::time::{sleep, timeout, Instant};
use tokio_rustls::rustls::ServerConfig;
use tokio_rustls::TlsAcceptor;

use super::config::{IntegrationConfig, Live, MIN_TOKEN_BYTES};
use super::feed::Applied;
use super::integrations::{FeedHandle, PushRequest};
use super::protocol::{encode_hex, TOKEN_BYTES};
use super::secrets::{Secret, Secrets};
use super::tiles::Task;

pub const LOOPBACK_PORT: u16 = 47822;
pub const LAN_PORT: u16 = 47823;
/// The largest body read. One byte more is a 413.
pub const MAX_BODY_BYTES: usize = 8 * 1024;
/// From the TCP accept to the end of the headers (for HTTPS, the handshake gets as long again).
pub const HEADER_TIMEOUT: Duration = Duration::from_secs(5);
/// The body, once the headers are in.
const BODY_TIMEOUT: Duration = Duration::from_secs(5);
/// A backstop over the whole connection, such as a client that won't read the reply.
const CONNECTION_TIMEOUT: Duration = Duration::from_secs(15);
/// Connections at once, over both listeners. Beyond this a new socket is dropped.
pub const MAX_CONNECTIONS: usize = 32;
/// The request line and headers, in bytes (hyper's smallest buffer), and how many headers.
const MAX_HEAD_BYTES: usize = 8 * 1024;
const MAX_HEADERS: usize = 32;
/// Requests a second, per peer address and per integration, with bursts of `BURST`.
const RATE: f64 = 10.0;
const BURST: f64 = 30.0;
/// A peer whose tokens are refused this many times in `FAIL_WINDOW` gets 429 for `BAN`.
pub const MAX_FAILED_AUTHS: u32 = 20;
const FAIL_WINDOW: Duration = Duration::from_secs(60);
const BAN: Duration = Duration::from_secs(60);
/// Peers the limits remember. A new peer beyond this, with every slot busy, gets 429.
const MAX_PEERS: usize = 4096;
/// After the reply, how long and how much the listener reads and drops before closing. Closing a
/// socket with unread input resets it, and the reset can discard the reply (a 413 or a 401 that
/// was sent before the body was read).
const LINGER: Duration = Duration::from_secs(1);
const MAX_LINGER_BYTES: usize = 64 * 1024;
/// How soon a listener that couldn't bind tries again.
const RETRY: Duration = Duration::from_secs(10);
/// The one body of every refused token.
const UNAUTHORIZED: &str = r#"{"error":"unauthorized"}"#;
const TOO_MANY: &str = "too many requests";

/// Where the two listeners bind.
#[derive(Debug, Clone, Copy)]
pub struct Addrs {
    pub loopback: SocketAddr,
    pub lan: SocketAddr,
}

impl Default for Addrs {
    fn default() -> Self {
        Self {
            loopback: (Ipv4Addr::LOCALHOST, LOOPBACK_PORT).into(),
            lan: (Ipv4Addr::UNSPECIFIED, LAN_PORT).into(),
        }
    }
}

/// The addresses the listeners hold now.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Bound {
    pub loopback: Option<SocketAddr>,
    pub lan: Option<SocketAddr>,
}

/// The running listeners. Dropping it closes them and every connection.
pub struct PushApi {
    bound: watch::Receiver<Bound>,
    _manager: Task,
}

impl PushApi {
    pub fn bound(&self) -> watch::Receiver<Bound> {
        self.bound.clone()
    }
}

/// Opens and closes the listeners as the live `config` gains and loses push integrations and
/// `[push] lan`. Tokens are read from `secrets` on every request, so a new one works at once.
/// The LAN listener serves `tls`. Must run inside a tokio runtime.
pub fn spawn(
    config: watch::Receiver<Arc<Live>>,
    secrets: Arc<Secrets>,
    feed: FeedHandle,
    tls: Arc<ServerConfig>,
    addrs: Addrs,
) -> PushApi {
    let (bound, bound_rx) = watch::channel(Bound::default());
    let gate = Arc::new(Gate {
        config: config.clone(),
        secrets,
        feed,
        limits: Mutex::new(Limits::default()),
        slots: Arc::new(Semaphore::new(MAX_CONNECTIONS)),
    });
    let manager = tokio::spawn(manage(config, gate, TlsAcceptor::from(tls), addrs, bound));
    PushApi {
        bound: bound_rx,
        _manager: Task::new(manager),
    }
}

/// Which listeners `live` wants: (loopback, LAN).
fn wanted(live: &Live) -> (bool, bool) {
    let any = (live.integrations.iter()).any(|i| matches!(i, IntegrationConfig::Push { .. }));
    let lan = live.push.as_ref().is_some_and(|p| p.lan);
    (any, any && lan)
}

struct Listening {
    addr: SocketAddr,
    task: Task,
}

async fn manage(
    mut config: watch::Receiver<Arc<Live>>,
    gate: Arc<Gate>,
    tls: TlsAcceptor,
    addrs: Addrs,
    bound: watch::Sender<Bound>,
) {
    let mut loopback = None;
    let mut lan = None;
    loop {
        let (want_loopback, want_lan) = wanted(&config.borrow_and_update());
        let mut failed = false;
        failed |= !reconcile(&mut loopback, want_loopback, addrs.loopback, None, &gate).await;
        failed |= !reconcile(&mut lan, want_lan, addrs.lan, Some(tls.clone()), &gate).await;
        bound.send_replace(Bound {
            loopback: loopback.as_ref().map(|l: &Listening| l.addr),
            lan: lan.as_ref().map(|l: &Listening| l.addr),
        });
        tokio::select! {
            changed = config.changed() => if changed.is_err() { return },
            () = sleep(RETRY), if failed => {}
        }
    }
}

/// Opens or closes one listener to match `want`. False when it should be open and couldn't bind.
async fn reconcile(
    slot: &mut Option<Listening>,
    want: bool,
    addr: SocketAddr,
    tls: Option<TlsAcceptor>,
    gate: &Arc<Gate>,
) -> bool {
    let scheme = if tls.is_some() { "https" } else { "http" };
    match (slot.take(), want) {
        (Some(open), true) => *slot = Some(open),
        (Some(open), false) => {
            // Wait until the socket is gone, so a quick toggle can bind the port again.
            if let Err(e) = open.task.stop().await {
                if e.is_panic() {
                    log::error!("the push listener on {} panicked: {e}", open.addr);
                }
            }
            log::info!("push API on {} ({scheme}) closed", open.addr);
        }
        (None, true) => {
            match TcpListener::bind(addr).await {
                Ok(listener) => {
                    let local = listener.local_addr().unwrap_or(addr);
                    log::info!("push API listening on {local} ({scheme})");
                    let task = tokio::spawn(listen(listener, tls, gate.clone()));
                    *slot = Some(Listening {
                        addr: local,
                        task: Task::new(task),
                    });
                }
                Err(e) => {
                    log::error!("push API can't listen on {addr} ({scheme}), trying again in {RETRY:?}: {e}");
                    return false;
                }
            }
        }
        (None, false) => {}
    }
    true
}

/// Accepts connections until the task is stopped, which also drops every connection.
async fn listen(listener: TcpListener, tls: Option<TlsAcceptor>, gate: Arc<Gate>) {
    let mut connections = JoinSet::new();
    loop {
        tokio::select! {
            accepted = listener.accept() => match accepted {
                Ok((stream, peer)) => match gate.slots.clone().try_acquire_owned() {
                    Ok(permit) => {
                        let (gate, tls) = (gate.clone(), tls.clone());
                        connections.spawn(async move {
                            connection(stream, peer, tls, gate).await;
                            drop(permit);
                        });
                    }
                    Err(_) => log::debug!("{peer}: push connection dropped, {MAX_CONNECTIONS} open"),
                },
                Err(e) => {
                    log::error!("push accept failed: {e}");
                    sleep(Duration::from_millis(100)).await;
                }
            },
            Some(joined) = connections.join_next() => {
                if let Err(e) = joined {
                    log::error!("push connection task failed: {e}");
                }
            }
        }
    }
}

async fn connection(
    stream: TcpStream,
    peer: SocketAddr,
    tls: Option<TlsAcceptor>,
    gate: Arc<Gate>,
) {
    let Some(tls) = tls else {
        return serve(stream, peer, gate).await;
    };
    match timeout(HEADER_TIMEOUT, tls.accept(stream)).await {
        Ok(Ok(stream)) => serve(stream, peer, gate).await,
        Ok(Err(e)) => log::debug!("{peer}: push TLS handshake failed: {e}"),
        Err(_) => log::debug!("{peer}: push TLS handshake did not finish in {HEADER_TIMEOUT:?}"),
    }
}

/// One request, then the reply, then a short drain and the close.
async fn serve<S>(stream: S, peer: SocketAddr, gate: Arc<Gate>)
where
    S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let service = service_fn(move |req| {
        let gate = gate.clone();
        async move { Ok::<_, Infallible>(gate.answer(peer.ip(), req).await) }
    });
    let conn = http1::Builder::new()
        .timer(TokioTimer::new())
        .header_read_timeout(HEADER_TIMEOUT)
        .keep_alive(false)
        .max_buf_size(MAX_HEAD_BYTES)
        .max_headers(MAX_HEADERS)
        .serve_connection(TokioIo::new(stream), service)
        .without_shutdown();
    match timeout(CONNECTION_TIMEOUT, conn).await {
        Ok(Ok(parts)) => linger(parts.io.into_inner()).await,
        // hyper's errors name what went wrong, never a header's value.
        Ok(Err(e)) => log::debug!("{peer}: push connection ended: {e}"),
        Err(_) => log::debug!("{peer}: push connection took over {CONNECTION_TIMEOUT:?}"),
    }
}

/// Ends the write side, then reads and drops what the client still sends, briefly.
async fn linger<S: AsyncRead + AsyncWrite + Unpin>(mut stream: S) {
    if stream.shutdown().await.is_err() {
        return;
    }
    let drain = async {
        let mut buf = [0u8; 4096];
        let mut left = MAX_LINGER_BYTES;
        while left > 0 {
            match stream.read(&mut buf).await {
                Ok(0) | Err(_) => break,
                Ok(n) => left = left.saturating_sub(n),
            }
        }
    };
    // Either way the socket closes now.
    let _ = timeout(LINGER, drain).await;
}

/// What both listeners share: the tokens' sources, the feed, and the limits.
struct Gate {
    config: watch::Receiver<Arc<Live>>,
    secrets: Arc<Secrets>,
    feed: FeedHandle,
    limits: Mutex<Limits>,
    slots: Arc<Semaphore>,
}

impl Gate {
    fn limits(&self) -> MutexGuard<'_, Limits> {
        self.limits.lock().expect("push limits poisoned")
    }

    async fn answer(&self, ip: IpAddr, req: Request<Incoming>) -> Response<Full<Bytes>> {
        if !self.limits().admit(ip, Instant::now()) {
            log::debug!("{ip}: push request refused, 429");
            return error(StatusCode::TOO_MANY_REQUESTS, TOO_MANY);
        }
        let integration = self.authenticate(req.headers().get(AUTHORIZATION));
        // The route only counts once the token has picked the integration that must answer it.
        let path = req.uri().path().strip_prefix("/v1/").unwrap_or_default();
        let Some(integration) = integration.filter(|id| self.feed.answers(id, path)) else {
            self.limits().refused(ip, Instant::now());
            log::debug!("{ip}: push request refused, 401");
            return unauthorized();
        };
        let route = route(path);
        let method = method_name(req.method());
        let reply = self.run(&integration, req).await;
        log::info!(
            "push {integration}: {method} {route} {}",
            reply.status().as_u16()
        );
        reply
    }

    /// The request as integration `integration`, which answers its route.
    async fn run(&self, integration: &str, req: Request<Incoming>) -> Response<Full<Bytes>> {
        if !self.limits().admit_integration(integration, Instant::now()) {
            return error(StatusCode::TOO_MANY_REQUESTS, TOO_MANY);
        }
        let Some(path) = req.uri().path().strip_prefix("/v1/").and_then(decode_path) else {
            return error(
                StatusCode::BAD_REQUEST,
                "the path is not valid percent-encoded UTF-8",
            );
        };
        let method = req.method().as_str().to_string();
        if req.body().size_hint().lower() > MAX_BODY_BYTES as u64 {
            return too_large();
        }
        let body = match timeout(
            BODY_TIMEOUT,
            Limited::new(req.into_body(), MAX_BODY_BYTES).collect(),
        )
        .await
        {
            Ok(Ok(body)) => body.to_bytes(),
            Ok(Err(e)) if e.downcast_ref::<LengthLimitError>().is_some() => return too_large(),
            Ok(Err(_)) => return error(StatusCode::BAD_REQUEST, "the body could not be read"),
            Err(_) => return error(StatusCode::REQUEST_TIMEOUT, "the body took too long"),
        };
        let request = PushRequest {
            method,
            path,
            body: body.to_vec(),
        };
        match self.feed.receive(integration, &request) {
            Ok(applied) => match applied.iter().find_map(|a| match a {
                Applied::Note(id) => Some(id),
                Applied::Done => None,
            }) {
                Some(id) => json(StatusCode::CREATED, &serde_json::json!({ "id": id })),
                None => empty(StatusCode::NO_CONTENT),
            },
            Err(e) => error(
                StatusCode::from_u16(e.status).unwrap_or(StatusCode::INTERNAL_SERVER_ERROR),
                &e.reason,
            ),
        }
    }

    /// The push integration whose token `header` carries, if exactly one has it. Every token is
    /// compared, each as a SHA-256 digest so the compare is the same length every time.
    fn authenticate(&self, header: Option<&HeaderValue>) -> Option<String> {
        let presented = header.and_then(|v| v.to_str().ok()).and_then(bearer);
        let digest = Sha256::digest(presented.unwrap_or_default().as_bytes());
        let tokens = self.tokens();
        let matched: Vec<&str> = (tokens.iter())
            .filter(|(_, token)| {
                verify_slices_are_equal(&digest, &Sha256::digest(token.expose().as_bytes())).is_ok()
            })
            .map(|(id, _)| id.as_str())
            .collect();
        match matched[..] {
            [id] if presented.is_some() => Some(id.to_string()),
            [_, _, ..] => {
                log::warn!(
                    "push integrations {} have the same token, so it is refused for all of them",
                    matched.join(", ")
                );
                None
            }
            _ => None,
        }
    }

    /// Each push integration's token, read now. A token shorter than [`MIN_TOKEN_BYTES`] opens
    /// nothing, even when the live config was checked against an older `secrets.toml`.
    fn tokens(&self) -> Vec<(String, Secret)> {
        let wanted: Vec<(String, String)> = (self.config.borrow().integrations.iter())
            .filter_map(|i| match i {
                IntegrationConfig::Push {
                    id, token_secret, ..
                } => Some((id.clone(), token_secret.clone())),
                IntegrationConfig::Stat { .. } => None,
            })
            .collect();
        let all = match self.secrets.load() {
            Ok(all) => all,
            Err(e) => {
                // Secrets' errors name a line or a secret, never a value.
                log::warn!("push: no token can be checked: {e}");
                return Vec::new();
            }
        };
        (wanted.into_iter())
            .filter_map(|(id, name)| {
                let token = all.get(&name)?;
                (token.expose().len() >= MIN_TOKEN_BYTES).then(|| (id, token.clone()))
            })
            .collect()
    }
}

/// The token of an `Authorization: Bearer <token>` value. The scheme is case-insensitive.
fn bearer(value: &str) -> Option<&str> {
    let (scheme, token) = value.split_once(' ')?;
    (scheme.eq_ignore_ascii_case("bearer") && !token.is_empty()).then_some(token)
}

/// `path` with its `%XX` escapes decoded, or `None` when an escape is broken or the result isn't
/// UTF-8.
fn decode_path(path: &str) -> Option<String> {
    let bytes = path.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' {
            let hex = bytes.get(i + 1..i + 3)?;
            if !hex.iter().all(u8::is_ascii_hexdigit) {
                return None;
            }
            let hex = std::str::from_utf8(hex).ok()?;
            out.push(u8::from_str_radix(hex, 16).ok()?);
            i += 3;
        } else {
            out.push(bytes[i]);
            i += 1;
        }
    }
    String::from_utf8(out).ok()
}

/// The route for the log, such as `/v1/notes/{id}`: ids are the caller's, so they stay out.
fn route(path: &str) -> String {
    match path.split_once('/') {
        Some((first, _)) => format!("/v1/{first}/{{id}}"),
        None => format!("/v1/{path}"),
    }
}

/// The method for the log. A made-up method is any token a client chose, so it stays out.
fn method_name(method: &Method) -> &'static str {
    match *method {
        Method::GET => "GET",
        Method::POST => "POST",
        Method::PUT => "PUT",
        Method::DELETE => "DELETE",
        Method::PATCH => "PATCH",
        Method::HEAD => "HEAD",
        _ => "other",
    }
}

fn empty(status: StatusCode) -> Response<Full<Bytes>> {
    let mut reply = Response::new(Full::new(Bytes::new()));
    *reply.status_mut() = status;
    reply
}

fn json(status: StatusCode, body: &serde_json::Value) -> Response<Full<Bytes>> {
    with_body(status, body.to_string())
}

fn with_body(status: StatusCode, body: String) -> Response<Full<Bytes>> {
    let mut reply = Response::new(Full::new(Bytes::from(body)));
    *reply.status_mut() = status;
    reply
        .headers_mut()
        .insert(CONTENT_TYPE, HeaderValue::from_static("application/json"));
    reply
}

fn error(status: StatusCode, reason: &str) -> Response<Full<Bytes>> {
    json(status, &serde_json::json!({ "error": reason }))
}

fn unauthorized() -> Response<Full<Bytes>> {
    with_body(StatusCode::UNAUTHORIZED, UNAUTHORIZED.into())
}

fn too_large() -> Response<Full<Bytes>> {
    error(
        StatusCode::PAYLOAD_TOO_LARGE,
        &format!("the body is over {MAX_BODY_BYTES} bytes"),
    )
}

/// A token bucket: `BURST` requests at once, refilled at `RATE` a second.
#[derive(Debug, Clone, Copy)]
struct Bucket {
    tokens: f64,
    at: Instant,
}

impl Bucket {
    fn full(now: Instant) -> Self {
        Self {
            tokens: BURST,
            at: now,
        }
    }

    fn take(&mut self, now: Instant) -> bool {
        let refill = now.saturating_duration_since(self.at).as_secs_f64() * RATE;
        self.tokens = (self.tokens + refill).min(BURST);
        self.at = now;
        if self.tokens < 1.0 {
            return false;
        }
        self.tokens -= 1.0;
        true
    }

    /// Whether it would still be short of full now, so forgetting it would lift a limit.
    fn in_use(&self, now: Instant) -> bool {
        self.tokens + now.saturating_duration_since(self.at).as_secs_f64() * RATE < BURST
    }
}

#[derive(Debug)]
struct Peer {
    bucket: Bucket,
    /// Refused tokens since `since`.
    failed: u32,
    since: Instant,
    banned_until: Option<Instant>,
}

impl Peer {
    fn new(now: Instant) -> Self {
        Self {
            bucket: Bucket::full(now),
            failed: 0,
            since: now,
            banned_until: None,
        }
    }

    fn in_use(&self, now: Instant) -> bool {
        self.banned_until.is_some_and(|until| now < until)
            || (self.failed > 0 && now.saturating_duration_since(self.since) < FAIL_WINDOW)
            || self.bucket.in_use(now)
    }
}

#[derive(Debug, Default)]
struct Limits {
    peers: HashMap<IpAddr, Peer>,
    integrations: HashMap<String, Bucket>,
}

impl Limits {
    /// Whether `ip` may make a request now, before its token is looked at.
    fn admit(&mut self, ip: IpAddr, now: Instant) -> bool {
        let Some(peer) = self.peer(ip, now) else {
            return false;
        };
        if peer.banned_until.is_some_and(|until| now < until) {
            return false;
        }
        peer.banned_until = None;
        peer.bucket.take(now)
    }

    /// Whether `integration` may make a request now.
    fn admit_integration(&mut self, integration: &str, now: Instant) -> bool {
        (self.integrations)
            .entry(integration.to_string())
            .or_insert_with(|| Bucket::full(now))
            .take(now)
    }

    /// Counts a refused token from `ip`.
    fn refused(&mut self, ip: IpAddr, now: Instant) {
        let Some(peer) = self.peer(ip, now) else {
            return;
        };
        if now.saturating_duration_since(peer.since) >= FAIL_WINDOW {
            peer.failed = 0;
            peer.since = now;
        }
        peer.failed += 1;
        if peer.failed >= MAX_FAILED_AUTHS {
            log::warn!("{ip}: {MAX_FAILED_AUTHS} refused push tokens, refusing it for {BAN:?}");
            peer.banned_until = Some(now + BAN);
            peer.failed = 0;
            peer.since = now;
        }
    }

    /// `ip`'s entry. `None` when the table is full and no entry can go without lifting a limit.
    fn peer(&mut self, ip: IpAddr, now: Instant) -> Option<&mut Peer> {
        if !self.peers.contains_key(&ip) && self.peers.len() >= MAX_PEERS {
            self.peers.retain(|_, peer| peer.in_use(now));
            if self.peers.len() >= MAX_PEERS {
                return None;
            }
        }
        Some(self.peers.entry(ip).or_insert_with(|| Peer::new(now)))
    }
}

/// The secret that holds push integration `integration`'s token.
pub fn token_secret_name(integration: &str) -> String {
    format!("push-{integration}")
}

/// Makes a new token for push integration `integration`, 32 random bytes as 64 hex digits, and
/// stores it as the secret [`token_secret_name`]. The caller shows it once; nothing logs it.
pub fn create_token(secrets: &Secrets, integration: &str) -> Result<String, String> {
    let name = token_secret_name(integration);
    if !(name.bytes()).all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-') {
        return Err(format!(
            "The token is stored as the secret {name:?}, and a secret name has only letters, \
             digits, '_' or '-'. Give the integration an id without '.'."
        ));
    }
    let mut bytes = [0u8; TOKEN_BYTES];
    aws_lc_rs::rand::fill(&mut bytes).map_err(|_| "no system randomness".to_string())?;
    let token = encode_hex(&bytes);
    secrets.set(&name, &token)?;
    Ok(token)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_a_bearer_value_carries_a_token() {
        assert_eq!(bearer("Bearer abc"), Some("abc"));
        assert_eq!(bearer("bearer abc"), Some("abc"));
        assert_eq!(bearer("Bearer a b"), Some("a b"));
        for refused in ["Basic abc", "Bearer", "Bearer ", "Bearerabc", "abc", ""] {
            assert_eq!(bearer(refused), None, "{refused:?}");
        }
    }

    #[test]
    fn paths_decode_their_escapes_and_refuse_broken_ones() {
        assert_eq!(
            decode_path("notes/a%20b%2Fc").as_deref(),
            Some("notes/a b/c")
        );
        assert_eq!(decode_path("cards/x").as_deref(), Some("cards/x"));
        for broken in ["notes/%", "notes/%2", "notes/%zz", "notes/%+1", "notes/%ff"] {
            assert_eq!(decode_path(broken), None, "{broken:?}");
        }
    }

    #[test]
    fn the_log_sees_the_route_but_not_the_ids() {
        assert_eq!(route("notes"), "/v1/notes");
        assert_eq!(route("notes/secret-ish"), "/v1/notes/{id}");
        assert_eq!(method_name(&Method::DELETE), "DELETE");
        assert_eq!(
            method_name(&Method::from_bytes(b"TOKENLIKE").unwrap()),
            "other"
        );
    }

    #[test]
    fn a_bucket_allows_its_burst_then_its_rate() {
        let start = Instant::now();
        let mut bucket = Bucket::full(start);
        assert!((0..30).all(|_| bucket.take(start)));
        assert!(!bucket.take(start));
        let later = start + Duration::from_millis(100);
        assert!(bucket.take(later), "one more after a tenth of a second");
        assert!(!bucket.take(later));
        assert!(bucket.in_use(later));
        assert!(!bucket.in_use(later + Duration::from_secs(3)));
    }

    #[test]
    fn twenty_refused_tokens_ban_a_peer_for_a_minute() {
        let start = Instant::now();
        let ip: IpAddr = Ipv4Addr::new(192, 0, 2, 1).into();
        let other: IpAddr = Ipv4Addr::new(192, 0, 2, 2).into();
        let mut limits = Limits::default();
        for _ in 0..MAX_FAILED_AUTHS {
            assert!(limits.admit(ip, start));
            limits.refused(ip, start);
        }
        assert!(!limits.admit(ip, start));
        assert!(limits.admit(other, start), "only the one peer");
        assert!(!limits.admit(ip, start + BAN - Duration::from_secs(1)));
        assert!(limits.admit(ip, start + BAN));
    }

    #[test]
    fn refusals_spread_over_more_than_a_minute_never_ban() {
        let mut now = Instant::now();
        let ip: IpAddr = Ipv4Addr::new(192, 0, 2, 1).into();
        let mut limits = Limits::default();
        for _ in 0..3 * MAX_FAILED_AUTHS {
            assert!(limits.admit(ip, now));
            limits.refused(ip, now);
            now += Duration::from_secs(4);
        }
    }

    #[test]
    fn a_full_peer_table_forgets_idle_peers_but_never_a_banned_one() {
        let start = Instant::now();
        let mut limits = Limits::default();
        let banned: IpAddr = Ipv4Addr::new(10, 0, 0, 1).into();
        for _ in 0..MAX_FAILED_AUTHS {
            limits.refused(banned, start);
        }
        for i in 1..MAX_PEERS as u32 {
            assert!(limits.admit(IpAddr::V4((0x0b00_0000 + i).into()), start));
        }
        let new: IpAddr = Ipv4Addr::new(12, 0, 0, 1).into();
        assert!(!limits.admit(new, start), "every peer is still limited");
        let later = start + Duration::from_secs(5);
        assert!(limits.admit(new, later), "idle peers made room");
        assert!(!limits.admit(banned, later), "the ban stays");
    }

    #[test]
    fn a_new_token_is_64_hex_digits_stored_under_its_integration() {
        let tmp = tempfile::tempdir().unwrap();
        let secrets = Secrets::new(tmp.path());
        let first = create_token(&secrets, "ha").unwrap();
        assert_eq!(first.len(), 64);
        assert!(first.bytes().all(|b| b.is_ascii_hexdigit()));
        assert_eq!(secrets.load().unwrap()["push-ha"].expose(), first);
        let second = create_token(&secrets, "ha").unwrap();
        assert_ne!(first, second);
        assert_eq!(secrets.load().unwrap()["push-ha"].expose(), second);
        let refused = create_token(&secrets, "home.assistant").unwrap_err();
        assert!(refused.contains("push-home.assistant"), "{refused}");
    }
}
