//! Shell actions: argv spawned without a shell, in a process group of its own, with stdin closed.
//! A run that outlives its timeout gets SIGTERM and, 2 s later, SIGKILL, both sent to the whole
//! group so whatever it started goes too. The group is signalled only while its leader is
//! unreaped, because until then no other process can take its id. A leader still alive 5 s after
//! SIGKILL, as one stuck in the kernel can be, is given up on so its button and slot come free.

use std::io;
use std::os::unix::process::ExitStatusExt;
use std::path::Path;
use std::process::{ExitStatus, Stdio};
use std::time::Duration;

use nix::sys::signal::{killpg, Signal};
use nix::unistd::Pid;
use tokio::io::{AsyncRead, AsyncReadExt};
use tokio::process::Command;
use tokio::time::{sleep_until, Instant};

use super::{excerpt, ActionResult};

/// From SIGTERM to SIGKILL.
const KILL_GRACE: Duration = Duration::from_secs(2);
/// From SIGKILL to giving up on the leader. Tokio reaps it whenever it does exit.
const REAP_GRACE: Duration = Duration::from_secs(5);
/// How long output may still arrive after the leader exits, from processes it left running with
/// its pipes. The run ends without the rest.
const DRAIN_GRACE: Duration = Duration::from_millis(500);
/// Kept per stream: the last 64 KB. The rest is read and dropped, so the command never blocks.
const KEEP_BYTES: usize = 64 * 1024;

enum Phase {
    Running {
        until: Instant,
    },
    /// Timed out and sent SIGTERM.
    Terminating {
        kill_at: Instant,
    },
    /// Sent SIGKILL. Waiting for the leader.
    Killed {
        until: Instant,
    },
    /// The leader is reaped. Waiting for its pipes to close.
    Exited {
        status: ExitStatus,
        until: Instant,
    },
}

impl Phase {
    /// Whether to wait on the leader. Not while terminating, since reaping it would forbid the
    /// SIGKILL still due to the rest of its group.
    fn reaping(&self) -> bool {
        matches!(self, Phase::Running { .. } | Phase::Killed { .. })
    }

    fn deadline(&self) -> Instant {
        match *self {
            Phase::Running { until } | Phase::Killed { until } | Phase::Exited { until, .. } => {
                until
            }
            Phase::Terminating { kill_at } => kill_at,
        }
    }
}

pub(super) async fn run(argv: &[String], timeout: Duration, cwd: &Path) -> ActionResult {
    let (program, args) = argv.split_first().expect("argv is checked to be non-empty");
    let mut child = match Command::new(program)
        .args(args)
        .current_dir(cwd)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .process_group(0)
        .spawn()
    {
        Ok(child) => child,
        Err(e) => return ActionResult::failed(format!("could not start {program}: {e}")),
    };
    let pid = child.id().expect("a child not yet waited on has a pid");
    // Declared after the child so it drops first, while the leader is still unreaped.
    let mut group = Group(Some(Pid::from_raw(pid as i32)));
    let (mut stdout, mut stderr) = (child.stdout.take(), child.stderr.take());
    let (mut out, mut err) = (Tail::default(), Tail::default());
    let (mut out_chunk, mut err_chunk) = ([0u8; 8192], [0u8; 8192]);
    let mut phase = Phase::Running {
        until: Instant::now() + timeout,
    };
    let mut timed_out = false;

    // None when the leader was given up on.
    let status = loop {
        if let Phase::Exited { status, .. } = phase {
            if stdout.is_none() && stderr.is_none() {
                break Some(status);
            }
        }
        tokio::select! {
            read = read_from(&mut stdout, &mut out_chunk), if stdout.is_some() => match read {
                Ok(n) if n > 0 => out.push(&out_chunk[..n]),
                _ => stdout = None,
            },
            read = read_from(&mut stderr, &mut err_chunk), if stderr.is_some() => match read {
                Ok(n) if n > 0 => err.push(&err_chunk[..n]),
                _ => stderr = None,
            },
            waited = child.wait(), if phase.reaping() => match waited {
                Ok(status) => {
                    group.reaped();
                    phase = Phase::Exited { status, until: Instant::now() + DRAIN_GRACE };
                }
                // Whether the leader lives is unknown, so `group` kills the group on the way out.
                Err(e) => return ActionResult::failed(format!("lost track of {program}: {e}")),
            },
            () = sleep_until(phase.deadline()) => match phase {
                Phase::Running { .. } => {
                    timed_out = true;
                    group.signal(Signal::SIGTERM);
                    phase = Phase::Terminating { kill_at: Instant::now() + KILL_GRACE };
                }
                Phase::Terminating { .. } => {
                    group.signal(Signal::SIGKILL);
                    phase = Phase::Killed { until: Instant::now() + REAP_GRACE };
                }
                Phase::Killed { .. } => {
                    log::warn!("{program} outlived SIGKILL by {REAP_GRACE:?}; giving up on it");
                    break None;
                }
                Phase::Exited { status, .. } => break Some(status),
            },
        }
    };

    let mut output = out.0;
    output.extend_from_slice(&err.0);
    let (ok, code, summary) = if timed_out {
        let summary = format!("timed out after {} s", timeout.as_secs());
        (false, -1, summary)
    } else {
        let status = status.expect("only a run that timed out is given up on");
        match status.code() {
            Some(code) => (code == 0, code, format!("exit {code}")),
            None => {
                let signal = status.signal().unwrap_or_default();
                (false, -1, format!("killed by signal {signal}"))
            }
        }
    };
    ActionResult {
        ok,
        code,
        summary,
        text: excerpt(&output),
        ms: 0,
    }
}

/// Reads from `pipe`. Callers poll it only while the pipe is open.
async fn read_from<R: AsyncRead + Unpin>(
    pipe: &mut Option<R>,
    buf: &mut [u8],
) -> io::Result<usize> {
    match pipe {
        Some(pipe) => pipe.read(buf).await,
        None => std::future::pending().await,
    }
}

/// The run's process group, while its leader is unreaped and so still owns the id. If the run's
/// future is dropped before the reap, the whole group is killed.
struct Group(Option<Pid>);

impl Group {
    fn signal(&self, signal: Signal) {
        if let Some(pgid) = self.0 {
            if let Err(e) = killpg(pgid, signal) {
                log::debug!("killpg({pgid}, {signal:?}): {e}");
            }
        }
    }

    /// The leader is reaped, so from now on its id may name some other group.
    fn reaped(&mut self) {
        self.0 = None;
    }
}

impl Drop for Group {
    fn drop(&mut self) {
        self.signal(Signal::SIGKILL);
    }
}

#[derive(Default)]
struct Tail(Vec<u8>);

impl Tail {
    fn push(&mut self, bytes: &[u8]) {
        self.0.extend_from_slice(bytes);
        let excess = self.0.len().saturating_sub(KEEP_BYTES);
        self.0.drain(..excess);
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::time::Instant;

    use nix::sys::signal::kill;

    use super::*;
    use crate::daemon::actions::TEXT_BYTES;

    fn sh(script: &str) -> Vec<String> {
        vec!["sh".into(), "-c".into(), script.into()]
    }

    async fn run_sh(script: &str, timeout_s: u64) -> ActionResult {
        run(&sh(script), Duration::from_secs(timeout_s), Path::new("/")).await
    }

    /// Live (not zombie) processes in group `pgid`, from /proc.
    fn live_members(pgid: i32) -> Vec<i32> {
        let pids = fs::read_dir("/proc")
            .unwrap()
            .filter_map(|entry| entry.ok()?.file_name().to_str()?.parse::<i32>().ok());
        pids.filter(|pid| {
            let Ok(stat) = fs::read_to_string(format!("/proc/{pid}/stat")) else {
                return false;
            };
            // "pid (comm) state ppid pgrp ...", where comm may hold spaces and parentheses.
            let fields: Vec<&str> = stat[stat.rfind(')').unwrap() + 2..].split(' ').collect();
            fields[0] != "Z" && fields[2] == pgid.to_string()
        })
        .collect()
    }

    /// Waits up to 2 s for group `pgid` to have no live process. A zombie counts as gone, since
    /// its new parent may take a moment to reap it.
    async fn assert_group_gone(pgid: i32) {
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            let alive = live_members(pgid);
            if alive.is_empty() {
                return;
            }
            assert!(
                Instant::now() < deadline,
                "group {pgid} still has {alive:?}"
            );
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    }

    #[tokio::test]
    async fn exit_codes_and_both_streams_come_back() {
        let r = run_sh("echo out; echo err >&2; exit 3", 10).await;
        assert_eq!((r.ok, r.code, r.summary.as_str()), (false, 3, "exit 3"));
        assert_eq!(r.text, "out\nerr");
        let r = run_sh("echo fine", 10).await;
        assert_eq!(
            (r.ok, r.code, r.summary.as_str(), r.text.as_str()),
            (true, 0, "exit 0", "fine")
        );
    }

    #[tokio::test]
    async fn a_signal_death_has_no_exit_code() {
        let r = run_sh("kill -KILL $$", 10).await;
        assert_eq!(
            (r.ok, r.code, r.summary.as_str()),
            (false, -1, "killed by signal 9")
        );
    }

    #[tokio::test]
    async fn a_missing_program_fails_to_start() {
        let argv = ["/nonexistent/program".to_string()];
        let r = run(&argv, Duration::from_secs(10), Path::new("/")).await;
        assert_eq!((r.ok, r.code), (false, -1));
        assert!(
            r.summary
                .starts_with("could not start /nonexistent/program: "),
            "{}",
            r.summary
        );
    }

    #[tokio::test]
    async fn it_runs_in_cwd_with_stdin_closed() {
        let tmp = tempfile::tempdir().unwrap();
        // `cat` returns at once on a closed stdin, and would hang on an inherited one.
        let r = run(
            &sh("pwd -P; cat; echo eof"),
            Duration::from_secs(5),
            tmp.path(),
        )
        .await;
        let cwd = tmp.path().canonicalize().unwrap();
        assert_eq!(r.text, format!("{}\neof", cwd.display()), "{r:?}");
        assert!(r.ok);
    }

    #[tokio::test]
    async fn output_is_capped_and_both_pipes_are_drained() {
        // A megabyte on each stream, far more than a pipe holds, so the command can finish only
        // if both are read.
        let script = "head -c 1000000 /dev/zero | tr '\\0' a; echo; \
                      head -c 1000000 /dev/zero | tr '\\0' b >&2; echo done >&2";
        let r = run_sh(script, 20).await;
        assert!(r.ok, "{r:?}");
        assert_eq!(r.text.len(), TEXT_BYTES);
        assert!(r.text.ends_with("bbbdone"), "{}", &r.text[1000..]);
    }

    #[test]
    fn a_tail_keeps_the_last_64_kb() {
        let mut tail = Tail::default();
        tail.push(&vec![b'a'; KEEP_BYTES]);
        tail.push(b"xyz");
        assert_eq!(tail.0.len(), KEEP_BYTES);
        assert!(tail.0.ends_with(b"axyz"));
    }

    #[tokio::test]
    async fn a_timeout_kills_the_whole_group_grandchildren_included() {
        let started = Instant::now();
        let r = run_sh("echo $$; sleep 100 & sleep 100", 1).await;
        let waited = started.elapsed();
        assert_eq!(
            (r.ok, r.code, r.summary.as_str()),
            (false, -1, "timed out after 1 s")
        );
        // The timeout, then the 2 s before SIGKILL.
        assert!(
            waited >= Duration::from_secs(3) && waited < Duration::from_secs(4),
            "{waited:?}"
        );
        let leader: i32 = r.text.trim().parse().unwrap();
        assert_group_gone(leader).await;
    }

    #[tokio::test]
    async fn processes_that_ignore_sigterm_get_sigkill_two_seconds_later() {
        let tmp = tempfile::tempdir().unwrap();
        let pidfile = tmp.path().join("pid");
        // Ignored signals stay ignored across exec, so both sleeps ignore SIGTERM too.
        let script = format!(
            "trap '' TERM; echo $$ > '{}'; sleep 100 & sleep 100",
            pidfile.display()
        );
        let running =
            tokio::spawn(
                async move { run(&sh(&script), Duration::from_secs(1), Path::new("/")).await },
            );
        // 1 s after the SIGTERM, 1 s before the SIGKILL.
        tokio::time::sleep(Duration::from_secs(2)).await;
        let leader: i32 = fs::read_to_string(&pidfile)
            .unwrap()
            .trim()
            .parse()
            .unwrap();
        assert_eq!(
            live_members(leader).len(),
            3,
            "sh and both sleeps outlive SIGTERM"
        );

        let r = running.await.unwrap();
        assert_eq!(r.summary, "timed out after 1 s");
        assert_group_gone(leader).await;
    }

    #[tokio::test]
    async fn a_process_left_in_the_background_neither_holds_the_run_nor_dies() {
        let started = Instant::now();
        let r = run_sh("sleep 30 & echo $!", 10).await;
        assert!(
            started.elapsed() < Duration::from_secs(2),
            "the pipes held the run"
        );
        assert!(r.ok, "{r:?}");
        let background = Pid::from_raw(r.text.trim().parse().unwrap());
        assert!(
            kill(background, None).is_ok(),
            "a normal exit leaves its background processes alone"
        );
        kill(background, Signal::SIGKILL).unwrap();
    }
}
