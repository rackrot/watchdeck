//! The `prometheus` source: one instant query (`GET /api/v1/query`), polled, whose first value is
//! shown as a card. The request is an HTTP action built by the config
//! ([`IntegrationConfig::prometheus_action`](crate::daemon::config::IntegrationConfig)), so the
//! client, the timeouts, `insecure_tls`, `secret_headers` and their redaction are the actions'.
//!
//! The reply is read as the API documents it, <https://prometheus.io/docs/prometheus/latest/querying/api/>:
//! a `vector` result is a list of series, each with `value: [time, "number"]`, and a `scalar`
//! result is `[time, "number"]` itself. A query with no series shows "no data", bad.

use std::time::Duration;

use futures_util::future::BoxFuture;
use serde_json::Value;

use super::{Connector, Fetch};
use crate::daemon::config::Action;
use crate::daemon::feed::{Card, Snapshot};
use crate::daemon::protocol::CardState;

/// Shown before the first reading.
const PENDING: &str = "--";
const NO_DATA: &str = "no data";

/// How a reading is shown and judged.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Display {
    pub decimals: Option<u8>,
    pub suffix: String,
    pub bad_above: Option<f64>,
    pub bad_below: Option<f64>,
}

pub struct Prometheus<S> {
    sampler: S,
    id: String,
    label: String,
    every: Duration,
    /// The query request.
    action: Action,
    display: Display,
}

impl<S: Fetch> Prometheus<S> {
    pub fn new(
        sampler: S,
        id: &str,
        label: &str,
        every_s: u64,
        action: Action,
        display: Display,
    ) -> Self {
        Self {
            sampler,
            id: id.into(),
            label: label.into(),
            every: Duration::from_secs(every_s),
            action,
            display,
        }
    }

    fn card(&self, value: &str, state: CardState) -> Card {
        Card {
            id: String::new(),
            label: self.label.clone(),
            value: value.into(),
            state,
        }
    }
}

impl<S: Fetch> Connector for Prometheus<S> {
    fn every(&self) -> Duration {
        self.every
    }

    fn pending(&self) -> Vec<Card> {
        vec![self.card(PENDING, CardState::Unknown)]
    }

    fn poll(&self) -> BoxFuture<'_, Result<Snapshot, String>> {
        Box::pin(async move {
            let ran = (self.sampler.fetch(&self.id, &self.action).await)
                .ok_or("the poll could not start")?;
            let (text, state) = if ran.result.ok {
                match show(ran.raw(), &self.display) {
                    Ok(shown) => shown,
                    Err(why) => (why, CardState::Bad),
                }
            } else {
                (failure(&ran.result.summary, ran.raw()), CardState::Bad)
            };
            Ok(Snapshot {
                cards: vec![self.card(&ran.redact(&text), state)],
                notes: Vec::new(),
            })
        })
    }
}

/// The card text and state for a successful reply body, or why it can't be read. Anything taken
/// from the body is redacted by the caller.
fn show(body: &str, display: &Display) -> Result<(String, CardState), String> {
    let reply: Value = serde_json::from_str(body).map_err(|_| "not JSON".to_string())?;
    if reply["status"] != "success" {
        return Err(reply["error"]
            .as_str()
            .map_or("the query failed".into(), |e| format!("query failed: {e}")));
    }
    let data = &reply["data"];
    let sample = match data["resultType"].as_str() {
        Some("vector") => match data["result"].as_array() {
            Some(series) if series.is_empty() => return Ok((NO_DATA.into(), CardState::Bad)),
            Some(series) if series.len() > 1 => {
                return Err(format!("{} series, expected 1", series.len()))
            }
            Some(series) => &series[0]["value"],
            None => return Err("no result list in the reply".into()),
        },
        Some("scalar") => &data["result"],
        Some(other) => return Err(format!("a {other} result isn't a single value")),
        None => return Err("no resultType in the reply".into()),
    };
    let text = sample[1].as_str().ok_or("no value in the result")?;
    let number: f64 = text
        .parse()
        .map_err(|_| "the value is not a number".to_string())?;
    let shown = match display.decimals {
        Some(d) if number.is_finite() => format!("{number:.*}", usize::from(d)),
        _ => text.to_string(),
    };
    let state = if !number.is_finite() {
        // NaN and the infinities pass no comparison, so a threshold can't vouch for them.
        if display.bad_above.is_some() || display.bad_below.is_some() {
            CardState::Bad
        } else {
            CardState::Unknown
        }
    } else if display.bad_above.is_some_and(|limit| number > limit)
        || display.bad_below.is_some_and(|limit| number < limit)
    {
        CardState::Bad
    } else {
        CardState::Ok
    };
    Ok((format!("{shown}{}", display.suffix), state))
}

/// What a non-2xx reply says: its status line, and the `error` Prometheus puts in the body (it
/// answers 400, 422 and 503 with `{"status":"error","errorType":...,"error":...}`). Anything
/// taken from the body is redacted by the caller, and the store cuts the card's text.
fn failure(summary: &str, body: &str) -> String {
    let reply: Value = serde_json::from_str(body).unwrap_or(Value::Null);
    match reply["error"].as_str().filter(|e| !e.trim().is_empty()) {
        Some(error) => format!("{summary}: {}", error.trim()),
        None => summary.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};

    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::TcpListener;

    use super::*;
    use crate::daemon::actions::Actions;
    use crate::daemon::config::IntegrationConfig;
    use crate::daemon::integrations::{build, FeedSlots, Source};
    use crate::daemon::secrets::Secrets;

    // The replies are the shapes of the Prometheus HTTP API's instant query,
    // <https://prometheus.io/docs/prometheus/latest/querying/api/#instant-queries>: a vector's
    // series has `value: [time, "number"]`, a scalar's result is `[time, "number"]`, and a query
    // with no series has an empty `result`. The numbers are the doc's example timestamps.
    const VECTOR: &str = include_str!("../../../tests/fixtures/prometheus_vector.json");
    const SCALAR: &str = include_str!("../../../tests/fixtures/prometheus_scalar.json");
    const EMPTY: &str = include_str!("../../../tests/fixtures/prometheus_empty.json");
    const SECRET: &str = "Bearer s3cret-value-1234";

    /// A server on 127.0.0.1 that answers every request with `status` and `body`, and records
    /// each request head.
    struct Server {
        url: String,
        heads: Arc<Mutex<Vec<String>>>,
    }

    async fn serve(status: &str, body: &str) -> Server {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("http://{}", listener.local_addr().unwrap());
        let heads = Arc::new(Mutex::new(Vec::new()));
        let seen = heads.clone();
        let reply = format!(
            "HTTP/1.1 {status}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\
             Connection: close\r\n\r\n{body}",
            body.len()
        );
        tokio::spawn(async move {
            loop {
                let (mut socket, _) = listener.accept().await.unwrap();
                let (seen, reply) = (seen.clone(), reply.clone());
                tokio::spawn(async move {
                    let mut head = Vec::new();
                    let mut byte = [0u8; 1];
                    while !head.ends_with(b"\r\n\r\n") {
                        if socket.read(&mut byte).await.unwrap_or(0) == 0 {
                            return;
                        }
                        head.push(byte[0]);
                    }
                    seen.lock()
                        .unwrap()
                        .push(String::from_utf8_lossy(&head).into_owned());
                    let _ = socket.write_all(reply.as_bytes()).await;
                });
            }
        });
        Server { url, heads }
    }

    /// The real HTTP actions, with the secret `tok`. The directory holds `secrets.toml`.
    fn slots() -> (FeedSlots, tempfile::TempDir) {
        let dir = tempfile::tempdir().unwrap();
        let secrets = Arc::new(Secrets::new(dir.path()));
        secrets.set("tok", SECRET).unwrap();
        (
            FeedSlots(Actions::new(secrets, dir.path().into()).unwrap()),
            dir,
        )
    }

    fn config_for(url: &str, query: &str, extra: &str) -> IntegrationConfig {
        toml::from_str(&format!(
            "id = \"prom\"\nkind = \"prometheus\"\nlabel = \"Prom\"\nurl = \"{url}\"\n\
             query = '{query}'\nevery_s = 30\n{extra}"
        ))
        .unwrap()
    }

    /// Polls `config` once through the registry and the real HTTP actions and returns its card.
    async fn poll(config: &IntegrationConfig) -> Card {
        let (slots, _dir) = slots();
        let Source::Polled(connector) = build(config, &slots) else {
            panic!("prometheus is polled");
        };
        let snapshot = connector.poll().await.unwrap();
        assert!(snapshot.notes.is_empty());
        assert_eq!(snapshot.cards.len(), 1);
        snapshot.cards.into_iter().next().unwrap()
    }

    async fn card(url: &str, extra: &str) -> Card {
        poll(&config_for(url, "up", extra)).await
    }

    fn shown(card: &Card) -> (&str, CardState) {
        (card.value.as_str(), card.state)
    }

    #[tokio::test]
    async fn a_vector_and_a_scalar_give_the_value_with_decimals_and_suffix() {
        let vector = serve("200 OK", VECTOR).await;
        let plain = card(&vector.url, "").await;
        assert_eq!(shown(&plain), ("91.23456", CardState::Ok));
        assert_eq!((plain.id.as_str(), plain.label.as_str()), ("", "Prom"));
        let rounded = card(&vector.url, "decimals = 1\nsuffix = \"%\"").await;
        assert_eq!(shown(&rounded), ("91.2%", CardState::Ok));
        let whole = card(&vector.url, "decimals = 0").await;
        assert_eq!(whole.value, "91");

        let scalar = serve("200 OK", SCALAR).await;
        let padded = card(&scalar.url, "decimals = 3\nsuffix = \" MB\"").await;
        assert_eq!(shown(&padded), ("42.500 MB", CardState::Ok));
    }

    #[tokio::test]
    async fn no_series_is_no_data_and_bad() {
        let server = serve("200 OK", EMPTY).await;
        let empty = card(&server.url, "suffix = \"%\"").await;
        assert_eq!(shown(&empty), ("no data", CardState::Bad));
    }

    #[tokio::test]
    async fn the_thresholds_make_a_value_bad_only_past_them() {
        let server = serve("200 OK", VECTOR).await;
        for (keys, state) in [
            ("bad_above = 90", CardState::Bad),
            ("bad_above = 91.23456", CardState::Ok),
            ("bad_above = 95.5", CardState::Ok),
            ("bad_below = 100", CardState::Bad),
            ("bad_below = 91", CardState::Ok),
            ("bad_below = 10\nbad_above = 90.5", CardState::Bad),
        ] {
            assert_eq!(card(&server.url, keys).await.state, state, "{keys}");
        }
    }

    #[tokio::test]
    async fn an_error_status_is_bad_with_its_summary() {
        let server = serve(
            "500 Internal Server Error",
            r#"{"status":"error","errorType":"internal","error":"boom"}"#,
        )
        .await;
        assert_eq!(
            shown(&card(&server.url, "").await),
            ("HTTP 500: boom", CardState::Bad)
        );
        // Prometheus answers 400, 422 and 503 with its reason in the body. The server's own text
        // is redacted like any other, and the store cuts what the card can't hold.
        let parse = serve(
            "400 Bad Request",
            &format!(
                r#"{{"status":"error","errorType":"bad_data","error":"parse error at char 5 {SECRET}"}}"#
            ),
        )
        .await;
        let refused = card(&parse.url, "secret_headers = { Authorization = \"tok\" }").await;
        assert_eq!(refused.state, CardState::Bad);
        assert_eq!(refused.value, "HTTP 400: parse error at char 5 <secret>");
        let gone = serve("503 Service Unavailable", "").await;
        assert_eq!(
            shown(&card(&gone.url, "").await),
            ("HTTP 503", CardState::Bad)
        );
    }

    #[tokio::test]
    async fn several_series_are_bad_and_a_non_finite_value_is_unknown_unless_a_threshold_is_set() {
        let two = serve(
            "200 OK",
            r#"{"status":"success","data":{"resultType":"vector","result":[
                {"metric":{},"value":[1,"1"]},{"metric":{},"value":[1,"2"]}]}}"#,
        )
        .await;
        assert_eq!(
            shown(&card(&two.url, "").await),
            ("2 series, expected 1", CardState::Bad)
        );
        for (value, keys, state) in [
            ("NaN", "", CardState::Unknown),
            ("+Inf", "suffix = \"%\"", CardState::Unknown),
            ("NaN", "bad_above = 90", CardState::Bad),
            ("-Inf", "bad_below = 0", CardState::Bad),
        ] {
            let body = format!(
                r#"{{"status":"success","data":{{"resultType":"scalar","result":[1,"{value}"]}}}}"#
            );
            let server = serve("200 OK", &body).await;
            let card = card(&server.url, keys).await;
            assert_eq!(card.state, state, "{value} {keys}");
            assert!(card.value.starts_with(value), "{card:?}");
        }
    }

    #[tokio::test]
    async fn a_reply_that_is_not_a_value_is_bad_and_says_why() {
        for (body, why) in [
            ("not json", "not JSON"),
            (
                r#"{"status":"error","error":"parse error"}"#,
                "query failed: parse error",
            ),
            (
                r#"{"status":"success","data":{"resultType":"matrix","result":[]}}"#,
                "a matrix result isn't a single value",
            ),
            (
                r#"{"status":"success","data":{"resultType":"vector","result":[{"metric":{},"value":[1,"abc"]}]}}"#,
                "the value is not a number",
            ),
        ] {
            let server = serve("200 OK", body).await;
            assert_eq!(
                shown(&card(&server.url, "").await),
                (why, CardState::Bad),
                "{body}"
            );
        }
    }

    #[tokio::test]
    async fn the_query_is_url_encoded_under_the_servers_path() {
        let server = serve("200 OK", VECTOR).await;
        let query = r#"sum(rate(http{job="a b"}[5m])) & up"#;
        let encoded = "sum%28rate%28http%7Bjob%3D%22a+b%22%7D%5B5m%5D%29%29+%26+up";
        let with_path = config_for(&format!("{}/prom/", server.url), query, "");
        let Some(Action::Http { url, .. }) = with_path.prometheus_action() else {
            panic!("an http action");
        };
        assert_eq!(
            url,
            format!("{}/prom/api/v1/query?query={encoded}", server.url)
        );
        // What the server receives is that target, and the `&` did not split the parameter.
        poll(&with_path).await;
        let head = server.heads.lock().unwrap()[0].clone();
        assert_eq!(
            head.lines().next().unwrap(),
            format!("GET /prom/api/v1/query?query={encoded} HTTP/1.1")
        );
        // Without a path, the API path starts at the root.
        let bare = config_for(&server.url, query, "");
        let Some(Action::Http { url, .. }) = bare.prometheus_action() else {
            panic!("an http action");
        };
        assert_eq!(url, format!("{}/api/v1/query?query={encoded}", server.url));
    }

    #[tokio::test]
    async fn secret_headers_are_sent_and_never_shown() {
        let server = serve("200 OK", VECTOR).await;
        card(&server.url, "secret_headers = { Authorization = \"tok\" }").await;
        let head = server.heads.lock().unwrap()[0].to_ascii_lowercase();
        assert!(
            head.contains(&format!("authorization: {}", SECRET.to_ascii_lowercase())),
            "{head}"
        );

        // A server that echoes the secret in its error gets it blanked from the card.
        let echo = serve(
            "200 OK",
            &format!(r#"{{"status":"error","error":"bad credentials {SECRET} refused"}}"#),
        )
        .await;
        let leaked = card(&echo.url, "secret_headers = { Authorization = \"tok\" }").await;
        assert_eq!(leaked.state, CardState::Bad);
        assert!(!leaked.value.contains("s3cret"), "{leaked:?}");
        assert!(leaked.value.contains("<secret>"), "{leaked:?}");
    }

    #[tokio::test]
    async fn a_server_that_is_down_is_a_bad_card_without_the_url() {
        let closed = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("http://{}", closed.local_addr().unwrap());
        drop(closed);
        let down = card(&url, "").await;
        assert_eq!(down.state, CardState::Bad);
        assert!(down.value.starts_with("request failed: "), "{down:?}");
        assert!(!down.value.contains("127.0.0.1"), "{down:?}");
    }
}
