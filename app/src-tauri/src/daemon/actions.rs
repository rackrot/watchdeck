//! Runs what a button does, as `config.toml` defines it: a shell command or an HTTP request. The
//! device supplies only a button id, so nothing it sends reaches argv, env, the URL, the headers or
//! the body.

mod http;
mod shell;

use std::collections::HashSet;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Instant;

use tokio::sync::{OwnedSemaphorePermit, Semaphore};

use super::config::Action;
use super::secrets::Secrets;

/// At most this many actions run at once, across all devices.
pub const MAX_RUNNING: usize = 8;
/// The result text is the last 1 KB of output.
const TEXT_BYTES: usize = 1024;
const SUMMARY_BYTES: usize = 96;

/// How a press went, as the `result` message carries it.
#[derive(Debug, Clone, PartialEq, Eq)]
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

/// The runners, with what they share: the HTTP client, the secrets, a slot per concurrent run
/// and the buttons that are running.
#[derive(Clone)]
pub struct Actions(Arc<Inner>);

struct Inner {
    http: reqwest::Client,
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
            http: http::client()?,
            secrets,
            cwd,
            slots: Arc::new(Semaphore::new(MAX_RUNNING)),
            running: Mutex::new(HashSet::new()),
        })))
    }

    /// Claims `button` for a run, or says why it can't start: it is already running, or
    /// [`MAX_RUNNING`] actions are.
    pub fn start(&self, button: &str) -> Result<Run, ActionResult> {
        let mut running = self.0.running.lock().expect("running set poisoned");
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
        let inner = &self.actions.0;
        let started = Instant::now();
        let mut result = match action {
            Action::Shell { argv, .. } => shell::run(argv, action.timeout(), &inner.cwd).await,
            Action::Http {
                method,
                url,
                body,
                headers,
                secret_headers,
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
                http::run(&inner.http, &inner.secrets, request).await
            }
        };
        result.ms = started.elapsed().as_millis().try_into().unwrap_or(u64::MAX);
        result
    }
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
fn printable(bytes: &[u8]) -> String {
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
