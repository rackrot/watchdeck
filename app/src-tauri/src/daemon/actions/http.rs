//! HTTP actions: one request with redirects off, a timeout over the whole exchange, and at most
//! 64 KB of body read. `secret_headers` take their values from `secrets.toml`, and exact copies
//! of a value in the reply are blanked before the reply goes anywhere.

use std::collections::BTreeMap;
use std::error::Error;
use std::time::Duration;

use reqwest::header::{HeaderMap, HeaderName, HeaderValue};
use reqwest::{redirect, Client, Method, StatusCode};

use super::{excerpt, ActionResult};
use crate::daemon::secrets::{Secret, Secrets};

const KEEP_BYTES: usize = 64 * 1024;
/// What a secret's value becomes in the result text.
const REDACTED: &str = "<secret>";

pub(super) struct Request<'a> {
    pub method: &'a str,
    pub url: &'a str,
    pub body: Option<&'a str>,
    /// Header name -> value.
    pub headers: &'a BTreeMap<String, String>,
    /// Header name -> secret name.
    pub secret_headers: &'a BTreeMap<String, String>,
    pub timeout: Duration,
}

/// No proxy and no redirects, so a request goes only to the host in its URL. Over https only a
/// server that proves it is that host gets it; plain http is for this machine unless an action
/// sets `insecure_http`.
pub(super) fn client() -> reqwest::Result<Client> {
    Client::builder()
        .redirect(redirect::Policy::none())
        .no_proxy()
        .user_agent(concat!("watchdeck/", env!("CARGO_PKG_VERSION")))
        .build()
}

pub(super) async fn run(client: &Client, secrets: &Secrets, request: Request<'_>) -> ActionResult {
    let (mut headers, used) = match secret_headers(secrets, request.secret_headers) {
        Ok(found) => found,
        Err(why) => return ActionResult::failed(why),
    };
    for (name, value) in request.headers {
        let name = HeaderName::from_bytes(name.as_bytes()).expect("header names are checked");
        let value = HeaderValue::from_str(value).expect("header values are checked");
        headers.insert(name, value);
    }
    let method = Method::from_bytes(request.method.as_bytes()).expect("methods are checked");
    let mut builder = client.request(method, request.url).headers(headers);
    if let Some(body) = request.body {
        builder = builder.body(body.to_string());
    }
    let exchange = async {
        let mut response = builder.send().await?;
        let status = response.status();
        let mut body = Vec::new();
        let mut cut = false;
        while let Some(chunk) = response.chunk().await? {
            let room = KEEP_BYTES - body.len();
            if chunk.len() > room {
                body.extend_from_slice(&chunk[..room]);
                cut = true;
                break;
            }
            body.extend_from_slice(&chunk);
        }
        Ok::<_, reqwest::Error>((status, body, cut))
    };
    match tokio::time::timeout(request.timeout, exchange).await {
        Err(_) => ActionResult::failed(format!("timed out after {} s", request.timeout.as_secs())),
        // The URL stays out, in case it carries a token of its own.
        Ok(Err(e)) => ActionResult::failed(format!("request failed: {}", causes(&e.without_url()))),
        Ok(Ok((status, body, cut))) => answered(status, &body, cut, &used),
    }
}

fn answered(status: StatusCode, body: &[u8], cut: bool, secrets: &[Secret]) -> ActionResult {
    let text = redact(&String::from_utf8_lossy(body), secrets, cut);
    let mut summary = format!("HTTP {}", status.as_u16());
    if status.is_redirection() {
        summary.push_str(", redirect not followed");
    }
    if cut {
        summary.push_str(", body cut at 64 KB");
    }
    ActionResult {
        ok: status.is_success(),
        code: status.as_u16().into(),
        summary,
        text: excerpt(text.as_bytes()),
        ms: 0,
    }
}

/// `text` with each copy of a secret replaced by [`REDACTED`], overlapping copies as one. When the
/// body was `cut`, a secret's start at the very end goes too, since the rest of it was never
/// read. Secrets are printable ASCII, so every span starts and ends on a char boundary.
fn redact(text: &str, secrets: &[Secret], cut: bool) -> String {
    let mut spans = Vec::new();
    for secret in secrets.iter().map(Secret::expose) {
        let mut from = 0;
        while let Some(at) = text[from..].find(secret) {
            spans.push((from + at, from + at + secret.len()));
            from += at + 1;
        }
        if cut {
            if let Some(k) = (1..secret.len())
                .rev()
                .find(|&k| text.ends_with(&secret[..k]))
            {
                spans.push((text.len() - k, text.len()));
            }
        }
    }
    spans.sort_unstable();

    let mut out = String::with_capacity(text.len());
    let mut copied = 0;
    let mut spans = spans.into_iter().peekable();
    while let Some((start, mut end)) = spans.next() {
        while let Some(&(next, next_end)) = spans.peek() {
            if next > end {
                break;
            }
            end = end.max(next_end);
            spans.next();
        }
        out.push_str(&text[copied..start]);
        out.push_str(REDACTED);
        copied = end;
    }
    out.push_str(&text[copied..]);
    out
}

/// The headers to send, and the secrets in them. Errors name a secret, never its value.
fn secret_headers(
    secrets: &Secrets,
    wanted: &BTreeMap<String, String>,
) -> Result<(HeaderMap, Vec<Secret>), String> {
    let mut headers = HeaderMap::new();
    let mut used = Vec::new();
    if wanted.is_empty() {
        return Ok((headers, used));
    }
    let all = secrets
        .load()
        .map_err(|e| format!("could not read secrets: {e}"))?;
    for (header, name) in wanted {
        let secret = all
            .get(name)
            .ok_or_else(|| format!("secret {name:?} is not in secrets.toml"))?;
        let mut value =
            HeaderValue::from_str(secret.expose()).expect("secrets are printable ASCII");
        // Debug output of the header shows "Sensitive" instead.
        value.set_sensitive(true);
        let header = HeaderName::from_bytes(header.as_bytes()).expect("header names are checked");
        headers.insert(header, value);
        used.push(secret.clone());
    }
    Ok((headers, used))
}

/// `error` and each of its sources, joined.
fn causes(error: &dyn Error) -> String {
    let mut text = error.to_string();
    let mut source = error.source();
    while let Some(cause) = source {
        text = format!("{text}: {cause}");
        source = cause.source();
    }
    text
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};
    use std::time::Instant;

    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::TcpListener;

    use super::*;
    use crate::daemon::actions::TEXT_BYTES;

    const SECRET: &str = "Bearer test-s3cret-0123";

    /// A reply: bytes to send, then whether to hold the connection open without a word more.
    type Reply = (Vec<u8>, bool);

    /// An HTTP/1.1 responder on 127.0.0.1, one request per connection. `respond` maps each
    /// request head to a reply. Every head it sees is recorded.
    struct Responder {
        url: String,
        heads: Arc<Mutex<Vec<String>>>,
    }

    async fn responder(respond: fn(&str) -> Reply) -> Responder {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("http://{}", listener.local_addr().unwrap());
        let heads = Arc::new(Mutex::new(Vec::new()));
        let seen = heads.clone();
        tokio::spawn(async move {
            loop {
                let (mut socket, _) = listener.accept().await.unwrap();
                let seen = seen.clone();
                tokio::spawn(async move {
                    let mut head = Vec::new();
                    let mut byte = [0u8; 1];
                    while !head.ends_with(b"\r\n\r\n") {
                        if socket.read(&mut byte).await.unwrap_or(0) == 0 {
                            return;
                        }
                        head.push(byte[0]);
                    }
                    let head = String::from_utf8_lossy(&head).into_owned();
                    let length = head
                        .lines()
                        .find_map(|l| {
                            l.to_ascii_lowercase()
                                .strip_prefix("content-length: ")?
                                .parse()
                                .ok()
                        })
                        .unwrap_or(0);
                    let mut body = vec![0u8; length];
                    socket.read_exact(&mut body).await.unwrap();
                    seen.lock().unwrap().push(head.clone());
                    let (reply, hold) = respond(&head);
                    let _ = socket.write_all(&reply).await;
                    if hold {
                        tokio::time::sleep(Duration::from_secs(3600)).await;
                    }
                });
            }
        });
        Responder { url, heads }
    }

    fn reply(status: &str, headers: &str, body: &[u8]) -> Vec<u8> {
        let mut reply = format!(
            "HTTP/1.1 {status}\r\nContent-Length: {}\r\nConnection: close\r\n{headers}\r\n",
            body.len()
        )
        .into_bytes();
        reply.extend_from_slice(body);
        reply
    }

    struct Fixture {
        _dir: tempfile::TempDir,
        secrets: Secrets,
    }

    fn fixture() -> Fixture {
        let dir = tempfile::tempdir().unwrap();
        let secrets = Secrets::new(dir.path());
        secrets.set("tok", SECRET).unwrap();
        Fixture { _dir: dir, secrets }
    }

    async fn request(
        fixture: &Fixture,
        method: &str,
        url: &str,
        headers: &[(&str, &str)],
        timeout_s: u64,
    ) -> ActionResult {
        let secret_headers = headers
            .iter()
            .map(|(h, s)| (h.to_string(), s.to_string()))
            .collect();
        let request = Request {
            method,
            url,
            body: Some(r#"{"entity_id":"light.office"}"#),
            headers: &BTreeMap::new(),
            secret_headers: &secret_headers,
            timeout: Duration::from_secs(timeout_s),
        };
        run(&client().unwrap(), &fixture.secrets, request).await
    }

    /// Secrets with these values, as `secrets.toml` hands them out.
    fn secrets(values: &[&str]) -> Vec<Secret> {
        let dir = tempfile::tempdir().unwrap();
        let secrets = Secrets::new(dir.path());
        for (i, value) in values.iter().enumerate() {
            secrets.set(&format!("s{i}"), value).unwrap();
        }
        secrets.load().unwrap().into_values().collect()
    }

    #[test]
    fn copies_overlaps_and_a_cut_off_start_are_blanked() {
        let s = secrets(&["s3cr", "my-s3cr3t", "abcXYZ", "XYZ123", "aaa"]);
        for (text, cut, expected) in [
            // A secret inside a longer one, and a second copy.
            ("a my-s3cr3t b s3cr c", false, "a <secret> b <secret> c"),
            // Two secrets overlapping, and one overlapping itself.
            ("..abcXYZ123..", false, "..<secret>.."),
            ("xaaaay", false, "x<secret>y"),
            // A body cut mid-secret, alone or after a whole one.
            ("tail my-s3c", true, "tail <secret>"),
            ("ends abcXYZ12", true, "ends <secret>"),
            // Uncut, a secret's start at the end is just text.
            ("tail my-s3c", false, "tail my-s3c"),
        ] {
            assert_eq!(redact(text, &s, cut), expected, "{text:?}");
        }
    }

    #[tokio::test]
    async fn a_secret_cut_off_at_64_kb_is_blanked() {
        let server = responder(|_| {
            let mut body = vec![b'x'; KEEP_BYTES - 10];
            body.extend_from_slice(SECRET.as_bytes());
            body.extend_from_slice(b" and more");
            (reply("200 OK", "", &body), false)
        })
        .await;
        let r = request(
            &fixture(),
            "GET",
            &server.url,
            &[("Authorization", "tok")],
            5,
        )
        .await;
        assert_eq!(r.summary, "HTTP 200, body cut at 64 KB");
        let end = &r.text[r.text.len() - 40..];
        assert!(end.ends_with(REDACTED), "{end}");
        assert!(!r.text.contains(&SECRET[..10]), "{end}");
    }

    #[tokio::test]
    async fn plain_headers_are_sent_as_written() {
        let server = responder(|head| (reply("200 OK", "", head.as_bytes()), false)).await;
        let f = fixture();
        let headers = BTreeMap::from([("Content-Type".into(), "application/json".into())]);
        let secret_headers = BTreeMap::from([("Authorization".into(), "tok".into())]);
        let request = Request {
            method: "POST",
            url: &server.url,
            body: Some("{}"),
            headers: &headers,
            secret_headers: &secret_headers,
            timeout: Duration::from_secs(5),
        };
        let r = run(&client().unwrap(), &f.secrets, request).await;
        assert!(r.ok, "{r:?}");
        let head = server.heads.lock().unwrap()[0].to_ascii_lowercase();
        assert!(head.contains("content-type: application/json"), "{head}");
        assert!(head.contains("authorization: bearer"), "{head}");
        // Plain headers aren't secret, so the echo shows them.
        assert!(r.text.contains("application/json"), "{}", r.text);
    }

    #[tokio::test]
    async fn secret_headers_are_sent_and_blanked_from_the_reply() {
        // The reply echoes the request head, secret and all.
        let server = responder(|head| (reply("200 OK", "", head.as_bytes()), false)).await;
        let f = fixture();
        let url = format!("{}/echo", server.url);
        let r = request(&f, "POST", &url, &[("Authorization", "tok")], 5).await;
        assert_eq!(
            (r.ok, r.code, r.summary.as_str()),
            (true, 200, "HTTP 200"),
            "{r:?}"
        );

        let head = server.heads.lock().unwrap()[0].to_ascii_lowercase();
        assert!(head.starts_with("post /echo "), "{head}");
        assert!(
            head.contains(&format!("authorization: {}", SECRET.to_ascii_lowercase())),
            "{head}"
        );
        assert!(
            r.text
                .to_ascii_lowercase()
                .contains("authorization: <secret>"),
            "{}",
            r.text
        );
        assert!(!format!("{r:?}").contains(SECRET));
    }

    #[tokio::test]
    async fn a_redirect_is_reported_and_not_followed() {
        let server = responder(|head| {
            if head.starts_with("GET /first ") {
                (reply("302 Found", "Location: /second\r\n", b""), false)
            } else {
                (reply("200 OK", "", b"second"), false)
            }
        })
        .await;
        let r = request(&fixture(), "GET", &format!("{}/first", server.url), &[], 5).await;
        assert_eq!(
            (r.ok, r.code, r.summary.as_str()),
            (false, 302, "HTTP 302, redirect not followed")
        );
        assert_eq!(
            server.heads.lock().unwrap().len(),
            1,
            "the target was requested"
        );
    }

    #[tokio::test]
    async fn the_body_is_read_to_64_kb_and_no_further() {
        // It announces a megabyte, sends 100 KB and stalls, so reading past 64 KB would time out.
        let server = responder(|_| {
            let mut bytes = b"HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\n\r\n".to_vec();
            bytes.extend(std::iter::repeat_n(b'x', 100 * 1024));
            (bytes, true)
        })
        .await;
        let started = Instant::now();
        let r = request(&fixture(), "GET", &server.url, &[], 5).await;
        assert!(started.elapsed() < Duration::from_secs(2), "{r:?}");
        assert_eq!(
            (r.ok, r.summary.as_str()),
            (true, "HTTP 200, body cut at 64 KB")
        );
        assert_eq!(r.text, "x".repeat(TEXT_BYTES));
    }

    #[tokio::test]
    async fn a_silent_server_times_out() {
        let server = responder(|_| (Vec::new(), true)).await;
        let started = Instant::now();
        let r = request(&fixture(), "GET", &server.url, &[], 1).await;
        let waited = started.elapsed();
        assert_eq!(
            (r.ok, r.code, r.summary.as_str()),
            (false, -1, "timed out after 1 s")
        );
        assert!(
            waited >= Duration::from_secs(1) && waited < Duration::from_secs(2),
            "{waited:?}"
        );
    }

    #[tokio::test]
    async fn an_error_status_fails_with_its_body() {
        let server = responder(|_| (reply("503 Service Unavailable", "", b"down\n"), false)).await;
        let r = request(&fixture(), "GET", &server.url, &[], 5).await;
        assert_eq!((r.ok, r.code, r.summary.as_str()), (false, 503, "HTTP 503"));
        assert_eq!(r.text, "down");
    }

    #[tokio::test]
    async fn a_missing_secret_sends_nothing() {
        let server = responder(|_| (reply("200 OK", "", b""), false)).await;
        let r = request(
            &fixture(),
            "GET",
            &server.url,
            &[("Authorization", "nope")],
            5,
        )
        .await;
        assert_eq!(r.summary, "secret \"nope\" is not in secrets.toml");
        assert!(server.heads.lock().unwrap().is_empty());
    }

    #[tokio::test]
    async fn a_failed_connection_is_reported_without_the_url() {
        let closed = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = closed.local_addr().unwrap();
        drop(closed);
        let url = format!("http://{addr}/x?token=QUERY-TOKEN");
        let r = request(&fixture(), "GET", &url, &[("Authorization", "tok")], 5).await;
        assert!(!r.ok);
        assert!(r.summary.starts_with("request failed: "), "{}", r.summary);
        assert!(
            !r.summary.contains("QUERY-TOKEN") && !r.summary.contains(SECRET),
            "{}",
            r.summary
        );
    }
}
