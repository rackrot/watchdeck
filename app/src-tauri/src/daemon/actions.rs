//! Runs what a button does, as `config.toml` defines it: a shell command or an HTTP request. The
//! device supplies only a button id, so nothing it sends reaches argv, env, the URL, the headers or
//! the body.

mod http;
mod shell;

use std::collections::HashSet;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Instant;

use serde::Serialize;
use tokio::sync::{OwnedSemaphorePermit, Semaphore};

use super::config::Action;
use super::secrets::{Secret, Secrets};

/// At most this many actions run at once, across all devices.
pub const MAX_RUNNING: usize = 8;
/// Tile polls take at most this many of those, so a press always finds two free.
pub const MAX_TILE_RUNNING: usize = 6;
/// Feed polls take at most this many of the tile slots, so tiles keep `MAX_TILE_RUNNING` minus
/// this.
pub const MAX_FEED_RUNNING: usize = 2;
const TILE_PREFIX: &str = "tile:";
const FEED_PREFIX: &str = "feed:";
/// The result text is the last 1 KB of output.
const TEXT_BYTES: usize = 1024;
const SUMMARY_BYTES: usize = 96;

/// How a press went, as the `result` message carries it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ActionResult {
    pub ok: bool,
    /// Exit code or HTTP status, or -1 when there is neither.
    pub code: i32,
    /// One line on what happened: "exit 0", "HTTP 503", "timed out after 10 s".
    pub summary: String,
    /// The end of the output, as printable ASCII and newlines.
    pub text: String,
    pub ms: u64,
}

impl ActionResult {
    /// A press that ran nothing, or a run that failed before it produced output.
    pub fn failed(summary: impl Into<String>) -> Self {
        Self {
            ok: false,
            code: -1,
            summary: one_line(&summary.into()),
            text: String::new(),
            ms: 0,
        }
    }
}

/// A finished run with what a tile needs beyond the press text: the output as read, up to 64 KB
/// per stream, which is neither redacted nor made printable, and what it takes to redact a
/// value derived from it. A tile parses `raw` first and redacts what it picked, because
/// [`ActionResult::text`] is redacted by literal match and an escaped echo (`\/`, `\u0041`)
/// would pass that and then decode into the secret.
pub struct Ran {
    pub result: ActionResult,
    raw: String,
    /// The secrets the request carried.
    secrets: Vec<Secret>,
    /// The body was cut at 64 KB, so a secret may start at its very end.
    cut: bool,
}

impl Ran {
    #[cfg(test)]
    pub(super) fn new(result: ActionResult, raw: &str, secrets: Vec<Secret>, cut: bool) -> Self {
        Self {
            result,
            raw: raw.into(),
            secrets,
            cut,
        }
    }

    fn of(result: ActionResult) -> Self {
        Self {
            result,
            raw: String::new(),
            secrets: Vec::new(),
            cut: false,
        }
    }

    pub fn raw(&self) -> &str {
        &self.raw
    }

    /// `text` with every copy of a secret this run used blanked.
    pub fn redact(&self, text: &str) -> String {
        http::redact(text, &self.secrets, self.cut)
    }
}

/// The runners, with what they share: the HTTP client, the secrets, a slot per concurrent run
/// and the buttons that are running.
#[derive(Clone)]
pub struct Actions(Arc<Inner>);

struct Inner {
    http: reqwest::Client,
    /// Only for actions that set `insecure_tls`; it accepts any certificate.
    http_unverified: reqwest::Client,
    secrets: Arc<Secrets>,
    /// Where shell commands run.
    cwd: PathBuf,
    slots: Arc<Semaphore>,
    running: Mutex<HashSet<String>>,
}

/// One button's claim on a run. Dropping it frees the button and the slot.
pub struct Run {
    actions: Actions,
    button: String,
    /// Always `Some` until the drop.
    slot: Option<OwnedSemaphorePermit>,
}

impl Actions {
    /// Shell commands run in `cwd` with the daemon's environment.
    pub fn new(secrets: Arc<Secrets>, cwd: PathBuf) -> reqwest::Result<Self> {
        Ok(Self(Arc::new(Inner {
            http: http::client(false)?,
            http_unverified: http::client(true)?,
            secrets,
            cwd,
            slots: Arc::new(Semaphore::new(MAX_RUNNING)),
            running: Mutex::new(HashSet::new()),
        })))
    }

    /// Claims the tile of button `id` for a poll. The key has a ':' that no button id can, so a
    /// press never finds its button "already running" because of a poll, and the other way
    /// round. At most [`MAX_TILE_RUNNING`] polls run at once, which leaves presses two slots.
    pub fn start_tile(&self, id: &str) -> Result<Run, ActionResult> {
        let mut running = self.0.running.lock().expect("running set poisoned");
        if polls(&running, &[TILE_PREFIX, FEED_PREFIX]) >= MAX_TILE_RUNNING {
            return Err(ActionResult::failed(format!(
                "busy: {MAX_TILE_RUNNING} tile polls are running"
            )));
        }
        self.claim(&mut running, &format!("{TILE_PREFIX}{id}"))
    }

    /// Claims the feed source `id` for a poll. A feed poll takes one of the tile slots, and at
    /// most [`MAX_FEED_RUNNING`] run at once, so tiles always keep the rest.
    pub fn start_feed(&self, id: &str) -> Result<Run, ActionResult> {
        let mut running = self.0.running.lock().expect("running set poisoned");
        if polls(&running, &[FEED_PREFIX]) >= MAX_FEED_RUNNING {
            return Err(ActionResult::failed(format!(
                "busy: {MAX_FEED_RUNNING} feed polls are running"
            )));
        }
        if polls(&running, &[TILE_PREFIX, FEED_PREFIX]) >= MAX_TILE_RUNNING {
            return Err(ActionResult::failed(format!(
                "busy: {MAX_TILE_RUNNING} tile polls are running"
            )));
        }
        self.claim(&mut running, &format!("{FEED_PREFIX}{id}"))
    }

    /// Claims `button` for a run, or says why it can't start: it is already running, or
    /// [`MAX_RUNNING`] actions are.
    pub fn start(&self, button: &str) -> Result<Run, ActionResult> {
        let mut running = self.0.running.lock().expect("running set poisoned");
        self.claim(&mut running, button)
    }

    /// [`Actions::start`] with the running set already locked.
    fn claim(&self, running: &mut HashSet<String>, button: &str) -> Result<Run, ActionResult> {
        if running.contains(button) {
            return Err(ActionResult::failed("already running"));
        }
        let slot = self.0.slots.clone().try_acquire_owned().map_err(|_| {
            ActionResult::failed(format!("busy: {MAX_RUNNING} actions are running"))
        })?;
        running.insert(button.to_string());
        Ok(Run {
            actions: self.clone(),
            button: button.to_string(),
            slot: Some(slot),
        })
    }
}

impl Run {
    pub async fn run(self, action: &Action) -> ActionResult {
        self.execute(action).await.result
    }

    /// Like [`Run::run`], for a tile, which reads its value from the raw output.
    pub async fn poll(self, action: &Action) -> Ran {
        self.execute(action).await
    }

    async fn execute(self, action: &Action) -> Ran {
        let inner = &self.actions.0;
        let started = Instant::now();
        let mut ran = match action {
            Action::Shell { argv, .. } => shell::ran(argv, action.timeout(), &inner.cwd).await,
            Action::Http {
                method,
                url,
                body,
                headers,
                secret_headers,
                insecure_tls,
                ..
            } => {
                let request = http::Request {
                    method,
                    url,
                    body: body.as_deref(),
                    headers,
                    secret_headers,
                    timeout: action.timeout(),
                };
                let client = if *insecure_tls {
                    &inner.http_unverified
                } else {
                    &inner.http
                };
                http::ran(client, &inner.secrets, request).await
            }
        };
        ran.result.ms = started.elapsed().as_millis().try_into().unwrap_or(u64::MAX);
        ran
    }
}

/// How many of the running keys start with one of `prefixes`.
fn polls(running: &HashSet<String>, prefixes: &[&str]) -> usize {
    running
        .iter()
        .filter(|k| prefixes.iter().any(|p| k.starts_with(p)))
        .count()
}

impl Drop for Run {
    fn drop(&mut self) {
        let mut running = self.actions.0.running.lock().expect("running set poisoned");
        // The slot goes first, under the lock, so a press that finds the button free also
        // finds its slot free and isn't told "busy".
        self.slot.take();
        running.remove(&self.button);
    }
}

/// `bytes` as text the device font can draw: printable ASCII and newlines. Tabs become spaces,
/// carriage returns go, and anything else becomes `?`.
pub(super) fn printable(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes)
        .chars()
        .filter_map(|c| match c {
            '\n' | ' '..='~' => Some(c),
            '\t' => Some(' '),
            '\r' => None,
            _ => Some('?'),
        })
        .collect()
}

/// The result text: the last [`TEXT_BYTES`] of `output`, printable, without trailing blank space.
fn excerpt(output: &[u8]) -> String {
    let text = printable(output);
    let text = text.trim_end();
    // Printable text is ASCII, so any byte index is a char boundary.
    text[text.len().saturating_sub(TEXT_BYTES)..].to_string()
}

/// `text` as a printable line of at most [`SUMMARY_BYTES`].
fn one_line(text: &str) -> String {
    let mut line = printable(text.as_bytes()).replace('\n', " ");
    line.truncate(SUMMARY_BYTES);
    line
}

#[cfg(test)]
mod tests {
    use super::*;

    fn actions() -> Actions {
        let dir = std::env::temp_dir();
        Actions::new(Arc::new(Secrets::new(&dir)), dir).unwrap()
    }

    #[test]
    fn a_button_runs_once_at_a_time() {
        let actions = actions();
        let run = actions.start("a").unwrap();
        let again = actions
            .start("a")
            .err()
            .expect("a second run must be refused");
        assert_eq!(again, ActionResult::failed("already running"));
        assert_eq!((again.ok, again.code), (false, -1));
        actions.start("b").expect("another button may run");
        drop(run);
        actions
            .start("a")
            .expect("the button is free once its run ends");
    }

    #[test]
    fn feed_polls_take_two_of_the_six_tile_slots() {
        let actions = actions();
        let feeds: Vec<Run> = (0..MAX_FEED_RUNNING)
            .map(|i| actions.start_feed(&format!("f{i}")).unwrap())
            .collect();
        let busy = actions
            .start_feed("more")
            .err()
            .expect("a third is refused");
        assert_eq!(busy.summary, "busy: 2 feed polls are running");
        // Tiles keep four of their six, and the presses keep their two.
        let tiles: Vec<Run> = (0..MAX_TILE_RUNNING - MAX_FEED_RUNNING)
            .map(|i| actions.start_tile(&format!("t{i}")).unwrap())
            .collect();
        assert!(actions.start_tile("more").is_err());
        let presses = (
            actions.start("press-a").unwrap(),
            actions.start("press-b").unwrap(),
        );
        drop((feeds, tiles, presses));
        // A feed poll is refused while six tile polls run, too.
        let tiles: Vec<Run> = (0..MAX_TILE_RUNNING)
            .map(|i| actions.start_tile(&format!("t{i}")).unwrap())
            .collect();
        assert!(actions.start_feed("f").is_err());
        drop(tiles);
    }

    #[test]
    fn at_most_eight_actions_run_at_once() {
        let actions = actions();
        let runs: Vec<Run> = (0..MAX_RUNNING)
            .map(|i| actions.start(&format!("b{i}")).unwrap())
            .collect();
        let busy = actions
            .start("more")
            .err()
            .expect("a ninth run must be refused");
        assert_eq!(busy.summary, "busy: 8 actions are running");
        drop(runs);
        actions.start("more").unwrap();
        assert!(actions.0.running.lock().unwrap().len() <= 1);
    }

    #[test]
    fn a_button_that_just_finished_never_reads_as_busy() {
        let actions = actions();
        // The other runs hold all slots but one, which "x" competes for only with itself.
        let _others: Vec<Run> = (1..MAX_RUNNING)
            .map(|i| actions.start(&format!("b{i}")).unwrap())
            .collect();
        let busy = std::sync::atomic::AtomicUsize::new(0);
        std::thread::scope(|s| {
            for _ in 0..2 {
                s.spawn(|| {
                    for _ in 0..50_000 {
                        match actions.start("x") {
                            Ok(run) => drop(run),
                            Err(r) if r.summary == "already running" => {}
                            Err(_) => {
                                busy.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                            }
                        }
                    }
                });
            }
        });
        assert_eq!(busy.into_inner(), 0);
    }

    #[test]
    fn output_becomes_printable_ascii() {
        assert_eq!(printable(b"ok\tdone\r\n"), "ok done\n");
        assert_eq!(printable("x\u{e9}y \u{1b}[31m".as_bytes()), "x?y ?[31m");
        assert_eq!(printable(b"\xff\x00\x7f"), "???");
    }

    #[test]
    fn the_excerpt_is_the_last_kilobyte_trimmed() {
        let mut output = vec![b'a'; 3000];
        output.extend_from_slice(b"END\n\n");
        let text = excerpt(&output);
        assert_eq!(text.len(), TEXT_BYTES);
        assert!(text.ends_with("aEND"), "{text}");
        assert_eq!(excerpt(b"short\n"), "short");
    }

    #[test]
    fn a_summary_is_one_short_printable_line() {
        let summary = one_line(&format!("a\nb\u{e9}{}", "x".repeat(200)));
        assert!(summary.starts_with("a b?x"));
        assert_eq!(summary.len(), SUMMARY_BYTES);
    }
}
