//! Opt-in error reporting to Sentry. With no `[sentry]` table in `config.toml` nothing here
//! runs: no client, no `sentry::init`, no network, whatever `SENTRY_DSN` says. A table builds one
//! client at startup, and a change to it applies on the next start.
//!
//! Only `log::error!` lines and panics become events. While `pii` is off, [`Scrubber`] strips
//! what can identify the user from every event, and [`Budget`] keeps a loop from burning quota.

use std::collections::{HashMap, VecDeque};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use sentry::integrations::log::{LogFilter, SentryLogger};
use sentry::protocol::{Context, Envelope, EnvelopeItem, Event, Frame, Stacktrace, Value};
use sentry::{Client, ClientOptions, Hub, Transport, TransportFactory, TransportOptions};
use tokio::sync::watch;

use super::config::{Live, SentryConfig, CONFIG_REJECTED};
use super::secrets::Secrets;

/// How long an identical event is dropped after one is sent.
pub const DEDUPE: Duration = Duration::from_secs(10 * 60);
const MAX_KEYS: usize = 256;
const MAX_PER_HOUR: usize = 30;
const HOUR: Duration = Duration::from_secs(60 * 60);
/// Time the client gets to send what is queued when the app exits.
pub const CLOSE_TIMEOUT: Duration = Duration::from_secs(2);
const MAX_KEY_MESSAGE: usize = 256;

/// Whether `Warn` lines become breadcrumbs, which is only with `pii`. Set by [`start`].
static WARN_BREADCRUMBS: AtomicBool = AtomicBool::new(false);

/// The running client, kept in Tauri's managed state so the app can close it on exit.
pub struct Reporting(Arc<Client>);

impl Reporting {
    pub fn client(&self) -> &Arc<Client> {
        &self.0
    }

    /// Sends what is queued, waiting at most [`CLOSE_TIMEOUT`].
    pub fn close(&self) {
        self.0.close(Some(CLOSE_TIMEOUT));
    }
}

/// Builds the client and binds it to the main hub, or does nothing without a `[sentry]` table.
pub fn start(config: Option<&SentryConfig>, secrets: Arc<Secrets>) -> Option<Reporting> {
    let config = config?;
    let home = std::env::home_dir();
    let host = gethostname::gethostname().to_string_lossy().into_owned();
    let filter = Filter::new(config.pii, secrets, home, host);
    let client = Arc::new(Client::from(options(config, filter)));
    Hub::main().bind_client(Some(client.clone()));
    WARN_BREADCRUMBS.store(config.pii, Ordering::Relaxed);
    log::info!(
        "Sentry reporting is on for {}, pii {}",
        client.dsn().map_or("?", |dsn| dsn.host()),
        config.pii
    );
    Some(Reporting(client))
}

/// The client options. `SENTRY_DSN`, `SENTRY_RELEASE`, `SENTRY_ENVIRONMENT` and `SSL_VERIFY` feed
/// `apply_defaults`, so the values that matter are forced afterwards.
fn options(config: &SentryConfig, filter: Filter) -> ClientOptions {
    let mut opts = sentry::apply_defaults(ClientOptions::default());
    opts.dsn = Some(config.dsn.parse().expect("Config::check accepted the DSN"));
    opts.accept_invalid_certs = false;
    opts.release = Some(concat!("watchdeck@", env!("CARGO_PKG_VERSION")).into());
    opts.environment = Some(if cfg!(debug_assertions) {
        "development".into()
    } else {
        "production".into()
    });
    // `apply_defaults` always sets a transport.
    if let Some(inner) = opts.transport.take() {
        opts.transport = Some(Arc::new(TypedOnlyFactory(inner)));
    }
    let filter = Arc::new(filter);
    opts.before_send = Some(Arc::new(move |event| filter.process(event)));
    opts
}

/// Wraps the app's logger so `Error` lines become events. Everything else is ignored, except
/// `Warn` as a breadcrumb when `pii` is on.
pub fn logger(dest: Box<dyn log::Log>) -> SentryLogger<Box<dyn log::Log>> {
    SentryLogger::with_dest(dest).filter(|metadata| {
        log_filter(
            metadata.level(),
            metadata.target(),
            WARN_BREADCRUMBS.load(Ordering::Relaxed),
        )
    })
}

fn log_filter(level: log::Level, target: &str, pii: bool) -> LogFilter {
    match level {
        _ if target == CONFIG_REJECTED => LogFilter::Ignore,
        log::Level::Error => LogFilter::Event,
        log::Level::Warn if pii => LogFilter::Breadcrumb,
        _ => LogFilter::Ignore,
    }
}

/// Logs once if `[sentry]` differs from what the app started with, since it applies on restart.
pub async fn warn_on_change(mut live: watch::Receiver<Arc<Live>>, started: Option<SentryConfig>) {
    while live.changed().await.is_ok() {
        if live.borrow_and_update().sentry != started {
            log::warn!("[sentry] changed in config.toml; restart watchdeck to apply it");
            return;
        }
    }
}

/// What every event passes through: the scrubber while `pii` is off, then the budget.
struct Filter {
    scrubber: Option<Scrubber>,
    budget: Budget,
}

impl Filter {
    fn new(pii: bool, secrets: Arc<Secrets>, home: Option<PathBuf>, host: String) -> Self {
        let home = home
            .map(|h| h.to_string_lossy().into_owned())
            .filter(|h| h.len() > 1);
        Self {
            scrubber: (!pii).then_some(Scrubber {
                secrets,
                home,
                host,
            }),
            budget: Budget::new(),
        }
    }

    fn process(&self, mut event: Event<'static>) -> Option<Event<'static>> {
        // tauri-plugin-sentry stamps the webview's events `javascript`. The page URL and
        // referrer in `request` say where in the UI the user was.
        if event.platform == "javascript" {
            event.tags.insert("component".into(), "ui".into());
            event.request = None;
        }
        let event = match &self.scrubber {
            Some(scrubber) => scrubber.event(event)?,
            None => event,
        };
        self.budget
            .admit(&event_key(&event), DEDUPE)
            .then_some(event)
    }
}

/// Wraps the transport so only what `before_send` has seen leaves the process. The webview plugin
/// forwards some envelopes straight to `Client::send_envelope`: raw bytes when it can't parse
/// them, and non-event items such as transactions.
struct TypedOnlyFactory(Arc<dyn TransportFactory>);

impl TransportFactory for TypedOnlyFactory {
    fn create_transport_with_options(&self, options: TransportOptions) -> Arc<dyn Transport> {
        Arc::new(TypedOnly(self.0.create_transport_with_options(options)))
    }
}

struct TypedOnly(Arc<dyn Transport>);

impl Transport for TypedOnly {
    /// Passes events, their attachments and client reports (counts only). A raw envelope has no
    /// typed items, so it is dropped with the rest.
    fn send_envelope(&self, envelope: Envelope) {
        let mut items = envelope.items().peekable();
        let allowed = items.peek().is_some()
            && items.all(|item| {
                matches!(
                    item,
                    EnvelopeItem::Event(_)
                        | EnvelopeItem::Attachment(_)
                        | EnvelopeItem::ClientReport(_)
                )
            });
        if allowed {
            self.0.send_envelope(envelope);
        }
    }

    fn flush(&self, timeout: Duration) -> bool {
        self.0.flush(timeout)
    }

    fn shutdown(&self, timeout: Duration) -> bool {
        self.0.shutdown(timeout)
    }
}

/// Component, message and build, so the same failure repeating is one event. Firmware reports
/// (S3) key on the fault PC instead of the message.
fn event_key(event: &Event) -> String {
    let component = event.tags.get("component").map_or("daemon", String::as_str);
    let message = event
        .message
        .as_deref()
        .or_else(|| event.logentry.as_ref().map(|l| l.message.as_str()))
        .map(str::to_string)
        .or_else(|| {
            event
                .exception
                .first()
                .map(|e| format!("{}: {}", e.ty, e.value.as_deref().unwrap_or("")))
        })
        .unwrap_or_default();
    let message: String = message.chars().take(MAX_KEY_MESSAGE).collect();
    let release = event.release.as_deref().unwrap_or("");
    format!("{component}|{message}|{release}")
}

/// A bounded, expiring set of recent event keys, and a cap on events per hour for the process.
struct Budget {
    max_keys: usize,
    per_hour: usize,
    state: Mutex<BudgetState>,
}

#[derive(Default)]
struct BudgetState {
    /// Key -> when it may be sent again.
    seen: HashMap<String, Instant>,
    /// When each event of the last hour was admitted.
    sent: VecDeque<Instant>,
}

impl Budget {
    fn new() -> Self {
        Self::with_limits(MAX_KEYS, MAX_PER_HOUR)
    }

    fn with_limits(max_keys: usize, per_hour: usize) -> Self {
        Self {
            max_keys,
            per_hour,
            state: Mutex::default(),
        }
    }

    /// Whether to send an event with `key`, which then blocks repeats for `ttl`. The per-device
    /// firmware cap (S3) calls this with a one-hour `ttl`.
    fn admit(&self, key: &str, ttl: Duration) -> bool {
        self.admit_at(key, ttl, Instant::now())
    }

    fn admit_at(&self, key: &str, ttl: Duration, now: Instant) -> bool {
        let mut state = self.state.lock().expect("budget lock poisoned");
        state.seen.retain(|_, until| *until > now);
        while state
            .sent
            .front()
            .is_some_and(|at| now.duration_since(*at) >= HOUR)
        {
            state.sent.pop_front();
        }
        if state.seen.contains_key(key) || state.sent.len() >= self.per_hour {
            return false;
        }
        if state.seen.len() >= self.max_keys {
            let oldest = state
                .seen
                .iter()
                .min_by_key(|(_, until)| **until)
                .map(|(k, _)| k.clone());
            if let Some(oldest) = oldest {
                state.seen.remove(&oldest);
            }
        }
        state.seen.insert(key.to_string(), now + ttl);
        state.sent.push_back(now);
        true
    }
}

/// Strips what can identify the user from an event.
struct Scrubber {
    secrets: Arc<Secrets>,
    home: Option<String>,
    host: String,
}

impl Scrubber {
    /// The scrubbed event. If `secrets.toml` can't be read its values can't be removed, so the
    /// event is dropped.
    fn event(&self, mut event: Event<'static>) -> Option<Event<'static>> {
        let mut secrets: Vec<String> = self
            .secrets
            .load()
            .ok()?
            .values()
            .map(|s| s.expose().to_string())
            .collect();
        // Longest first, so a secret that contains another is removed whole.
        secrets.sort_by_key(|s| std::cmp::Reverse(s.len()));
        let text = |s: &str| self.text(s, &secrets);

        event.server_name = None;
        event.user = None;
        event.message = event.message.as_deref().map(text);
        if let Some(entry) = &mut event.logentry {
            entry.message = text(&entry.message);
            entry.params.iter_mut().for_each(|v| value(v, &text));
        }
        for exception in &mut event.exception.values {
            exception.value = exception.value.as_deref().map(text);
            if let Some(trace) = &mut exception.stacktrace {
                frames(trace, &text);
            }
        }
        if let Some(trace) = &mut event.stacktrace {
            frames(trace, &text);
        }
        for crumb in &mut event.breadcrumbs.values {
            crumb.message = crumb.message.as_deref().map(text);
            crumb.data.values_mut().for_each(|v| value(v, &text));
        }
        event.tags.values_mut().for_each(|t| *t = text(t));
        event.extra.values_mut().for_each(|v| value(v, &text));
        // sentry-log puts the log call's file path and key-values in `Other` contexts. The
        // typed contexts hold OS, device model and Rust version only.
        for context in event.contexts.values_mut() {
            if let Context::Other(map) = context {
                map.values_mut().for_each(|v| value(v, &text));
            }
        }
        Some(event)
    }

    /// Applies the steps in order. Secrets go first so a secret that holds an IP, a path or a
    /// quote is removed whole instead of in pieces.
    fn text(&self, s: &str, secrets: &[String]) -> String {
        let mut s = s.to_string();
        for secret in secrets {
            s = s.replace(secret.as_str(), "");
        }
        if let Some(home) = &self.home {
            s = s.replace(home.as_str(), "~");
        }
        if !self.host.is_empty() {
            s = s.replace(self.host.as_str(), "<host>");
        }
        empty_quotes(&device_ids(&ip_literals(&s)))
    }
}

fn frames(trace: &mut Stacktrace, text: &impl Fn(&str) -> String) {
    for Frame {
        filename, abs_path, ..
    } in &mut trace.frames
    {
        *filename = filename.as_deref().map(text);
        *abs_path = abs_path.as_deref().map(text);
    }
}

fn value(v: &mut Value, text: &impl Fn(&str) -> String) {
    match v {
        Value::String(s) => *s = text(s),
        Value::Array(items) => items.iter_mut().for_each(|v| value(v, text)),
        Value::Object(map) => map.values_mut().for_each(|v| value(v, text)),
        _ => {}
    }
}

/// Replaces IPv4 and IPv6 literals with `<ip>`. A literal glued to letters ("abc10.0.0.1") isn't
/// found, which is the cost of not using a regex engine.
fn ip_literals(s: &str) -> String {
    let is_ip_char = |c: char| c.is_ascii_hexdigit() || c == ':' || c == '.';
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while let Some(start) = rest.find(is_ip_char) {
        out.push_str(&rest[..start]);
        let run = &rest[start..];
        let end = run.find(|c| !is_ip_char(c)).unwrap_or(run.len());
        let (token, tail) = run.split_at(end);
        let before = s[..s.len() - rest.len() + start].chars().next_back();
        out.push_str(&redact_ip_token(token, before, tail.chars().next()));
        rest = tail;
    }
    out.push_str(rest);
    out
}

/// `token` is a run of hex digits, ':' and '.'. It may carry leading or trailing separators from
/// the sentence around it, and an IPv4 address may carry a `:port`. `before` and `after` are the
/// characters on either side of the run. An IPv6 literal needs a digit and can't touch an
/// identifier character, so `Grid::check` and `Live::from` stay as they are.
fn redact_ip_token(token: &str, before: Option<char>, after: Option<char>) -> String {
    use std::net::{IpAddr, Ipv4Addr};
    let ident = |c: Option<char>| c.is_some_and(|c| c.is_alphanumeric() || c == '_');
    let core = token.trim_end_matches(['.', ':']);
    let suffix = &token[core.len()..];
    let candidates = [core, core.trim_start_matches(['.', ':'])];
    for ip in candidates {
        let prefix = &core[..core.len() - ip.len()];
        match ip.parse::<IpAddr>() {
            Ok(IpAddr::V4(_)) => return format!("{prefix}<ip>{suffix}"),
            Ok(IpAddr::V6(_))
                if ip.bytes().any(|b| b.is_ascii_digit())
                    && !(prefix.is_empty() && ident(before))
                    && !(suffix.is_empty() && ident(after)) =>
            {
                return format!("{prefix}<ip>{suffix}");
            }
            _ => {}
        }
        if let Some((addr, port)) = ip.rsplit_once(':') {
            if addr.parse::<Ipv4Addr>().is_ok()
                && !port.is_empty()
                && port.bytes().all(|b| b.is_ascii_digit())
            {
                return format!("{prefix}<ip>:{port}{suffix}");
            }
        }
    }
    token.to_string()
}

/// Replaces `wd-` and the hex digits after it with `<device>`.
fn device_ids(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while let Some(at) = rest.find("wd-") {
        out.push_str(&rest[..at]);
        let after = &rest[at + 3..];
        let hex = after.bytes().take_while(u8::is_ascii_hexdigit).count();
        if hex == 0 {
            out.push_str("wd-");
        } else {
            out.push_str("<device>");
        }
        rest = &after[hex..];
    }
    out.push_str(rest);
    out
}

/// Empties every `"…"`, keeping the quotes. A backslash escapes the next character. An opening
/// quote with no closing one is left alone.
fn empty_quotes(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while let Some(open) = rest.find('"') {
        out.push_str(&rest[..=open]);
        let inner = &rest[open + 1..];
        let mut chars = inner.char_indices();
        let mut close = None;
        while let Some((i, c)) = chars.next() {
            match c {
                '\\' => {
                    chars.next();
                }
                '"' => {
                    close = Some(i);
                    break;
                }
                _ => {}
            }
        }
        match close {
            Some(i) => {
                out.push('"');
                rest = &inner[i + 1..];
            }
            None => {
                rest = inner;
                break;
            }
        }
    }
    out.push_str(rest);
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use sentry::protocol::{Breadcrumb, Exception, Level, LogEntry, Values};

    const HOME: &str = "/home/buck";
    const HOST: &str = "cachy-box";

    fn secrets_dir(entries: &[(&str, &str)]) -> (tempfile::TempDir, Arc<Secrets>) {
        let dir = tempfile::tempdir().unwrap();
        let secrets = Arc::new(Secrets::new(dir.path()));
        for (name, value) in entries {
            secrets.set(name, value).unwrap();
        }
        (dir, secrets)
    }

    fn scrubber(secrets: Arc<Secrets>) -> Scrubber {
        Scrubber {
            secrets,
            home: Some(HOME.into()),
            host: HOST.into(),
        }
    }

    fn text(entries: &[(&str, &str)], input: &str) -> String {
        let secrets: Vec<String> = entries.iter().map(|(_, v)| v.to_string()).collect();
        // `text` is handed the values, so this Secrets is never read.
        scrubber(Arc::new(Secrets::new(std::path::Path::new("/nonexistent")))).text(input, &secrets)
    }

    #[test]
    fn scrubs_each_kind_of_text() {
        let t = |s: &str| text(&[], s);
        assert_eq!(t("open /home/buck/.config/x"), "open ~/.config/x");
        assert_eq!(t("host cachy-box refused"), "host <host> refused");
        assert_eq!(t("peer 192.168.1.20 gone"), "peer <ip> gone");
        assert_eq!(t("peer 192.168.1.20:47820 gone"), "peer <ip>:47820 gone");
        assert_eq!(t("peer host:10.0.0.1."), "peer host:<ip>.");
        assert_eq!(t("to [::1]:80 and fe80::1"), "to [<ip>]:80 and <ip>");
        assert_eq!(t("[::1]:47820 2601:681::2829."), "[<ip>]:47820 <ip>.");
        assert_eq!(t("lost wd-e8f60ae276ab link"), "lost <device> link");
        assert_eq!(t("kept wd- and wd-zzz"), "kept wd- and wd-zzz");
        assert_eq!(t(r#"button "disk" at "a\"b""#), r#"button "" at """#);
        assert_eq!(t(r#"open " never closed"#), r#"open " never closed"#);
    }

    #[test]
    fn leaves_ordinary_text_alone() {
        let plain = "config reload failed at 12:30:45 for build 0.1.0 (deadbeef) in \
                     Grid::check, Live::from and std::io::Error";
        assert_eq!(text(&[], plain), plain);
    }

    #[test]
    fn secret_values_go_before_every_other_step() {
        // Without the first step this would become "token-<ip>-/home" pieces, and the quote
        // and host steps would leave the rest of the secret behind.
        let secret = "s3cret-10.0.0.1-\"q\"-cachy-box-/home/buck";
        let out = text(&[("k", secret)], &format!("sent {secret} ok"));
        assert_eq!(out, "sent  ok");
    }

    #[test]
    fn a_longer_secret_containing_a_shorter_one_goes_whole() {
        let (_dir, secrets) = secrets_dir(&[("a", "abcd"), ("b", "abcdwxyz")]);
        let event = Event {
            message: Some("x abcdwxyz y".into()),
            ..Default::default()
        };
        let event = scrubber(secrets).event(event).unwrap();
        assert_eq!(event.message.as_deref(), Some("x  y"));
    }

    fn dirty_event() -> Event<'static> {
        let dirty = format!("{HOME}/x {HOST} 10.1.2.3 wd-0123456789ab \"label\" hunter2");
        let frame = Frame {
            filename: Some(format!("{HOME}/src/a.rs")),
            abs_path: Some(format!("{HOME}/src/a.rs")),
            ..Default::default()
        };
        let trace = Stacktrace {
            frames: vec![frame],
            ..Default::default()
        };
        let mut data = sentry::protocol::Map::new();
        data.insert("k".into(), Value::Array(vec![Value::String(dirty.clone())]));
        Event {
            message: Some(dirty.clone()),
            logentry: Some(LogEntry {
                message: dirty.clone(),
                params: vec![Value::String(dirty.clone())],
            }),
            exception: Values::from(vec![Exception {
                ty: "E".into(),
                value: Some(dirty.clone()),
                stacktrace: Some(trace.clone()),
                ..Default::default()
            }]),
            stacktrace: Some(trace),
            breadcrumbs: Values::from(vec![Breadcrumb {
                message: Some(dirty.clone()),
                data,
                ..Default::default()
            }]),
            tags: [("t".to_string(), dirty.clone())].into(),
            extra: [("e".to_string(), Value::String(dirty.clone()))].into(),
            contexts: [(
                "Rust Log Metadata".to_string(),
                Context::Other([("logger.file".to_string(), Value::String(dirty))].into()),
            )]
            .into(),
            server_name: Some(HOST.into()),
            user: Some(Default::default()),
            level: Level::Error,
            ..Default::default()
        }
    }

    #[test]
    fn scrubs_every_part_of_an_event() {
        let (_dir, secrets) = secrets_dir(&[("k", "hunter2")]);
        let event = scrubber(secrets).event(dirty_event()).unwrap();
        let json = serde_json::to_string(&event).unwrap();
        for leak in [HOME, HOST, "10.1.2.3", "0123456789ab", "label", "hunter2"] {
            assert!(!json.contains(leak), "{leak} leaked: {json}");
        }
        assert!(event.server_name.is_none() && event.user.is_none());
        let abs = event.stacktrace.unwrap().frames[0].abs_path.clone();
        assert_eq!(abs.as_deref(), Some("~/src/a.rs"));
        assert_eq!(
            event.message.as_deref(),
            Some("~/x <host> <ip> <device> \"\" ")
        );
    }

    #[cfg(unix)]
    #[test]
    fn an_unreadable_secrets_file_drops_the_event() {
        let (dir, secrets) = secrets_dir(&[("k", "v")]);
        std::fs::write(dir.path().join(crate::daemon::secrets::FILE), "not toml").unwrap();
        assert!(scrubber(secrets).event(dirty_event()).is_none());
    }

    fn at(base: Instant, secs: u64) -> Instant {
        base + Duration::from_secs(secs)
    }

    #[test]
    fn budget_dedupes_repeats_and_lets_distinct_events_through() {
        let budget = Budget::new();
        let t0 = Instant::now();
        assert!(budget.admit_at("a", DEDUPE, t0));
        assert!(!budget.admit_at("a", DEDUPE, at(t0, 599)));
        assert!(budget.admit_at("b", DEDUPE, at(t0, 1)));
        assert!(budget.admit_at("a", DEDUPE, at(t0, 600)), "expired");
    }

    #[test]
    fn budget_caps_a_flood_at_thirty_an_hour() {
        let budget = Budget::new();
        let t0 = Instant::now();
        let sent = (0..100)
            .filter(|i| budget.admit_at(&format!("k{i}"), DEDUPE, at(t0, *i)))
            .count();
        assert_eq!(sent, 30);
        assert!(!budget.admit_at("late", DEDUPE, at(t0, 3599)));
        assert!(
            budget.admit_at("later", DEDUPE, at(t0, 3600)),
            "window slid"
        );
    }

    #[test]
    fn budget_never_holds_more_than_its_key_cap() {
        let budget = Budget::with_limits(4, 1000);
        let t0 = Instant::now();
        for i in 0..5 {
            assert!(budget.admit_at(&format!("k{i}"), DEDUPE, at(t0, i)));
        }
        let state = budget.state.lock().unwrap();
        assert_eq!(state.seen.len(), 4);
        assert!(!state.seen.contains_key("k0"), "the oldest key goes first");
        drop(state);
        assert!(
            !budget.admit_at("k4", DEDUPE, at(t0, 6)),
            "newest is still held"
        );
        assert!(
            budget.admit_at("k0", DEDUPE, at(t0, 6)),
            "evicted key is new again"
        );
    }

    #[test]
    fn log_filter_follows_the_plan() {
        use log::Level::{Debug, Error, Info, Warn};
        assert_eq!(log_filter(Error, "x", false), LogFilter::Event);
        assert_eq!(log_filter(Error, CONFIG_REJECTED, true), LogFilter::Ignore);
        assert_eq!(log_filter(Warn, "x", false), LogFilter::Ignore);
        assert_eq!(log_filter(Warn, "x", true), LogFilter::Breadcrumb);
        assert_eq!(log_filter(Info, "x", true), LogFilter::Ignore);
        assert_eq!(log_filter(Debug, "x", true), LogFilter::Ignore);
    }

    fn config(dsn: &str, pii: bool) -> SentryConfig {
        SentryConfig {
            dsn: dsn.into(),
            pii,
        }
    }

    const DSN: &str = "https://key@o1.ingest.example.com/42";

    #[test]
    fn no_table_means_no_client_even_with_a_dsn_in_the_environment() {
        std::env::set_var("SENTRY_DSN", DSN);
        let (_dir, secrets) = secrets_dir(&[]);
        assert!(start(None, secrets).is_none());
        assert!(Hub::main().client().is_none());
    }

    #[test]
    fn the_config_dsn_and_strict_tls_beat_the_environment() {
        std::env::set_var("SENTRY_DSN", "https://other@o2.example.com/9");
        std::env::set_var("SSL_VERIFY", "false");
        let (_dir, secrets) = secrets_dir(&[]);
        let filter = Filter::new(false, secrets, None, String::new());
        let opts = options(&config(DSN, false), filter);
        let dsn = opts.dsn.unwrap();
        assert_eq!(
            (dsn.public_key(), dsn.host()),
            ("key", "o1.ingest.example.com")
        );
        assert!(!opts.accept_invalid_certs);
        assert_eq!(
            opts.release.as_deref(),
            Some(concat!("watchdeck@", env!("CARGO_PKG_VERSION")))
        );
        let env = if cfg!(debug_assertions) {
            "development"
        } else {
            "production"
        };
        assert_eq!(opts.environment.as_deref(), Some(env));
    }

    fn captured(pii: bool, secrets: Arc<Secrets>, f: impl FnOnce()) -> Vec<Event<'static>> {
        let filter = Filter::new(pii, secrets, Some(HOME.into()), HOST.into());
        sentry::test::with_captured_events_options(f, options(&config(DSN, pii), filter))
    }

    #[test]
    fn events_are_scrubbed_and_deduped_end_to_end() {
        let (_dir, secrets) = secrets_dir(&[("k", "hunter2")]);
        let events = captured(false, secrets, || {
            for _ in 0..3 {
                sentry::capture_message("boom hunter2 at 10.9.8.7", Level::Error);
            }
            sentry::capture_message("boom again", Level::Error);
        });
        let messages: Vec<_> = events.iter().filter_map(|e| e.message.as_deref()).collect();
        assert_eq!(messages, ["boom  at <ip>", "boom again"]);
    }

    #[test]
    fn pii_on_sends_text_unscrubbed() {
        let (_dir, secrets) = secrets_dir(&[]);
        let events = captured(true, secrets, || {
            sentry::capture_message("boom at 10.9.8.7", Level::Error);
        });
        assert_eq!(events[0].message.as_deref(), Some("boom at 10.9.8.7"));
    }

    #[test]
    fn webview_events_are_tagged_ui_and_lose_their_request() {
        let (_dir, secrets) = secrets_dir(&[]);
        let events = captured(false, secrets, || {
            let mut event = Event {
                message: Some("boom at /home/buck/x".into()),
                platform: "javascript".into(),
                request: Some(Default::default()),
                level: Level::Error,
                ..Default::default()
            };
            event.request.as_mut().unwrap().url = "http://tauri.localhost/".parse().ok();
            sentry::capture_event(event);
            sentry::capture_message("daemon boom", Level::Error);
        });
        let component = |e: &Event| e.tags.get("component").cloned();
        assert_eq!(component(&events[0]).as_deref(), Some("ui"));
        assert!(events[0].request.is_none());
        assert_eq!(events[0].message.as_deref(), Some("boom at ~/x"));
        assert_eq!(component(&events[1]), None);
    }

    #[derive(Default)]
    struct Recorder(Mutex<Vec<Envelope>>);

    impl Transport for Recorder {
        fn send_envelope(&self, envelope: Envelope) {
            self.0.lock().unwrap().push(envelope);
        }
    }

    #[test]
    fn the_transport_drops_envelopes_that_skipped_before_send() {
        use sentry::protocol::{ClientReport, Transaction};
        let sink = Arc::new(Recorder::default());
        let transport = TypedOnly(sink.clone());
        let one = |item: EnvelopeItem| {
            let mut envelope = Envelope::new();
            envelope.add_item(item);
            envelope
        };
        transport.send_envelope(Envelope::from_bytes_raw(b"{}\n".to_vec()).unwrap());
        transport.send_envelope(Envelope::new());
        transport.send_envelope(one(Transaction::default().into()));
        let mut mixed = one(Event::default().into());
        mixed.add_item(Transaction::default());
        transport.send_envelope(mixed);
        assert!(sink.0.lock().unwrap().is_empty());

        transport.send_envelope(one(Event::default().into()));
        transport.send_envelope(one(ClientReport::new(Vec::<
            sentry::protocol::client_report::Item,
        >::new())
        .into()));
        assert_eq!(sink.0.lock().unwrap().len(), 2);
    }

    #[test]
    fn config_checks_refuse_bad_dsns_and_pii_defaults_off() {
        use crate::daemon::config::parse;
        let base = "version = 1\n[layout]\ncols = 1\nrows = 1\n";
        let ok = |dsn: &str| parse(&format!("{base}[sentry]\ndsn = \"{dsn}\"\n"), &Ok(vec![]));
        assert!(!ok(DSN).unwrap().sentry.unwrap().pii);
        assert!(ok("http://k@127.0.0.1:9000/1").is_ok());
        assert!(ok("http://k@localhost/1").is_ok());
        assert!(ok("http://k@[::1]:9000/1").is_ok());
        for bad in ["http://k@example.com/1", "not a dsn", ""] {
            let errors = ok(bad).unwrap_err();
            assert!(errors[0].starts_with("sentry.dsn"), "{errors:?}");
            assert!(!errors.join(" ").contains(bad) || bad.is_empty());
        }
        let unknown = parse(
            &format!("{base}[sentry]\ndsn = \"{DSN}\"\nextra = 1\n"),
            &Ok(vec![]),
        );
        assert!(unknown.is_err());
        assert!(
            parse(&format!("{base}[sentry]\npii = true\n"), &Ok(vec![])).is_err(),
            "dsn is required"
        );
    }
}
