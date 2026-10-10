//! The one compiled-in action of S2. Nothing configurable runs until S3 has merged.

use std::process::Stdio;
use std::time::{Duration, Instant};

use futures_util::future::BoxFuture;
use tokio::process::Command;

use super::protocol::Button;

pub const HELLO_BTN: &str = "hello";
const TIMEOUT: Duration = Duration::from_secs(10);
const TEXT_TAIL_BYTES: usize = 1024;

pub struct ActionResult {
    pub ok: bool,
    /// Exit code, or -1 when there is none.
    pub code: i32,
    pub text: String,
    pub ms: u64,
}

/// Runs the action behind a button id for a device. The server only calls it for ids in its layout.
pub type Runner =
    std::sync::Arc<dyn Fn(String, String) -> BoxFuture<'static, ActionResult> + Send + Sync>;

pub fn default_runner() -> Runner {
    std::sync::Arc::new(|_btn, device| Box::pin(notify_hello(device)))
}

pub const COLS: u32 = 5;
pub const ROWS: u32 = 3;

pub fn buttons() -> Vec<Button> {
    vec![Button {
        id: HELLO_BTN.into(),
        col: 0,
        row: 0,
        label: "Hello".into(),
        bg: 1959605,
        fg: 16777215,
    }]
}

/// `notify-send watchdeck "Hello from <device>"`, spawned with args and never through a shell.
async fn notify_hello(device: String) -> ActionResult {
    let start = Instant::now();
    let mut cmd = Command::new("notify-send");
    cmd.args(["watchdeck", &format!("Hello from {device}")])
        .stdin(Stdio::null())
        .kill_on_drop(true);
    let (ok, code, text) = match tokio::time::timeout(TIMEOUT, cmd.output()).await {
        Err(_) => (false, -1, "timed out".to_string()),
        Ok(Err(e)) => {
            log::error!("notify-send failed to run: {e}");
            (false, -1, format!("failed to run notify-send: {e}"))
        }
        Ok(Ok(out)) => {
            let mut text = String::from_utf8_lossy(&out.stdout).into_owned();
            text.push_str(&String::from_utf8_lossy(&out.stderr));
            (
                out.status.success(),
                out.status.code().unwrap_or(-1),
                tail(&text, TEXT_TAIL_BYTES).to_string(),
            )
        }
    };
    ActionResult {
        ok,
        code,
        text,
        ms: start.elapsed().as_millis() as u64,
    }
}

/// The last `max` bytes of `s`, cut on a char boundary.
fn tail(s: &str, max: usize) -> &str {
    let mut start = s.len().saturating_sub(max);
    while !s.is_char_boundary(start) {
        start += 1;
    }
    &s[start..]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tail_keeps_the_end_on_a_char_boundary() {
        assert_eq!(tail("abcdef", 3), "def");
        assert_eq!(tail("abc", 10), "abc");
        assert_eq!(tail("aéé", 3), "é");
    }
}
