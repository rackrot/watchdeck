//! A stand-in for the daemon that sends the paired board malformed frames, to see that its
//! parsers hold. Plan: `plans/hw-gaps.md`, slice S2. It is an example, so `cargo test` builds it
//! but never runs it.
//!
//! ```text
//! fault_daemon --device wd-xxxxxxxxxxxx [--cases I1,T3] [--leak] [--pairing] [--dir DIR]
//! fault_daemon --check board.log [--leak | --pairing] [--cases I1,T3] [--after-s5]
//! ```
//!
//! It serves the real identity from the config dir and checks the board's token against
//! `state.json`, so the board needs no re-pairing. It refuses to run unless the tray app has
//! quit, and it never creates an identity or sends `error`, which would make the board erase its
//! pairing. The board's side of each case is graded from its serial log by `--check`.

// A crate root resolves `mod` relative to its own directory, so the paths are spelled out.
#[path = "fault_daemon/cases.rs"]
mod cases;
#[path = "fault_daemon/check.rs"]
mod check;
#[path = "fault_daemon/pair.rs"]
mod pair;
#[path = "fault_daemon/session.rs"]
mod session;

use std::net::SocketAddr;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::Duration;
use std::{env, fs};

use tokio::net::TcpListener;
use tokio::sync::watch;
use tokio::time::timeout;
use tokio_rustls::TlsAcceptor;
use watchdeck_lib::daemon::mdns::{self, MdnsStatus};
use watchdeck_lib::daemon::pairing::PAIR_PORT;
use watchdeck_lib::daemon::protocol::valid_device_id;
use watchdeck_lib::daemon::server::PORT;
use watchdeck_lib::daemon::store::{Store, STATE_FILE};
use watchdeck_lib::daemon::tls::{Identity, CERT_FILE, KEY_FILE};

use cases::{is_session_case, spec_for, CASES};
use session::{liveness, Harness, Lost, Res, Rig};

const USAGE: &str = "usage: fault_daemon --device wd-<12 hex> [--cases ID,ID] [--leak | --pairing] [--dir DIR]\n       fault_daemon --check LOG [--leak | --pairing] [--cases ID,ID] [--after-s5]";
const LEAK_ROUNDS: u32 = 3;
const LEAK_REPS: u32 = 20;

struct Args {
    device: String,
    dir: PathBuf,
    cases: Vec<&'static str>,
    pairing: bool,
    leak: bool,
}

enum Mode {
    Check {
        log: PathBuf,
        after_s5: bool,
        plan: check::Plan,
    },
    Run(Args),
}

fn parse(mut args: impl Iterator<Item = String>) -> Result<Mode, String> {
    let (mut device, mut dir, mut only, mut check) = (None, None, None, None);
    let (mut pairing, mut leak, mut after_s5) = (false, false, false);
    while let Some(flag) = args.next() {
        let mut value = |name: &str| args.next().ok_or(format!("{name} needs a value"));
        match flag.as_str() {
            "--device" => device = Some(value("--device")?),
            "--dir" => dir = Some(PathBuf::from(value("--dir")?)),
            "--cases" => only = Some(value("--cases")?),
            "--check" => check = Some(PathBuf::from(value("--check")?)),
            "--pairing" => pairing = true,
            "--leak" => leak = true,
            "--after-s5" => after_s5 = true,
            _ => return Err(format!("unknown argument {flag}")),
        }
    }
    if pairing && leak {
        return Err("--pairing and --leak are separate runs".into());
    }
    // The cases this kind of run covers. `--check` takes the same flags as the run it grades.
    let covered = |id: &str| {
        spec_for(id).is_some_and(|c| match (pairing, leak) {
            (true, _) => !is_session_case(id),
            (_, true) => is_session_case(id) && c.leak,
            _ => is_session_case(id),
        })
    };
    let cases: Vec<&'static str> = match only {
        Some(list) => list
            .split(',')
            .map(|id| {
                spec_for(id)
                    .filter(|spec| covered(spec.id))
                    .map(|spec| spec.id)
                    .ok_or(format!("{id} is not a case of this kind of run"))
            })
            .collect::<Result<_, _>>()?,
        None => CASES
            .iter()
            .map(|c| c.id)
            .filter(|id| covered(id))
            .collect(),
    };
    if let Some(log) = check {
        let plan = check::Plan {
            ids: cases,
            runs: if leak { LEAK_ROUNDS as usize + 1 } else { 1 },
            leak,
        };
        return Ok(Mode::Check {
            log,
            after_s5,
            plan,
        });
    }
    let device = device.ok_or("--device is required")?;
    if !valid_device_id(&device) {
        return Err(format!(
            "{device:?} is not a device id (wd- and 12 hex digits)"
        ));
    }
    let dir = match dir {
        Some(dir) => dir,
        None => PathBuf::from(env::var("HOME").map_err(|_| "HOME is not set; pass --dir")?)
            .join(".config/dev.rackrot.watchdeck"),
    };
    Ok(Mode::Run(Args {
        device,
        dir,
        cases,
        pairing,
        leak,
    }))
}

/// Refuses unless the identity exists and `state.json` pairs `device`. Runs before anything can
/// create a file, because `Identity::load_or_create` makes a new identity when none exists, and
/// a new identity makes every device pair again.
fn guard_files(dir: &Path, device: &str) -> Result<Store, String> {
    for file in [CERT_FILE, KEY_FILE, STATE_FILE] {
        if !dir.join(file).is_file() {
            return Err(format!(
                "{} is missing; the harness never creates an identity or a pairing",
                dir.join(file).display()
            ));
        }
    }
    let store = Store::load(dir).map_err(|e| e.to_string())?;
    if !store.ids().any(|id| id == device) {
        return Err(format!("{STATE_FILE} does not pair {device}"));
    }
    Ok(store)
}

/// Binds the port, which is also the proof that no daemon holds it.
async fn bind(addr: SocketAddr) -> Result<TcpListener, String> {
    TcpListener::bind(addr)
        .await
        .map_err(|e| format!("cannot bind {addr} ({e}); quit the tray app first"))
}

/// Registers the mDNS service and waits until avahi has it.
async fn advertise(identity: &Identity, pairing: bool) -> Res<watch::Sender<Option<u16>>> {
    let (pair_tx, pair_rx) = watch::channel(pairing.then_some(PAIR_PORT));
    let (status_tx, mut status_rx) = watch::channel(MdnsStatus::Starting);
    tokio::spawn(mdns::advertise(
        "fault-harness".into(),
        PORT,
        identity.fp(),
        pair_rx,
        status_tx,
    ));
    let registered = status_rx.wait_for(|s| matches!(s, MdnsStatus::Registered(_)));
    if timeout(Duration::from_secs(10), registered).await.is_err() {
        return Err(format!("mDNS did not register: {:?}", *status_rx.borrow()).into());
    }
    Ok(pair_tx)
}

/// Runs `id` `reps` times and then the liveness check. A lost link is replaced, so the next
/// case starts on a live session.
async fn execute(rig: &mut Rig, id: &str, reps: u32) -> Result<String, String> {
    let note = match cases::run(rig, id, reps).await {
        Ok(note) => note,
        Err(e) => {
            if e.is::<Lost>() {
                rig.next_session(Duration::from_secs(60))
                    .await
                    .map_err(|r| r.to_string())?;
            }
            return Err(e.to_string());
        }
    };
    if let Err(e) = liveness(&mut rig.sess).await {
        rig.reconnect(Duration::from_secs(60))
            .await
            .map_err(|r| r.to_string())?;
        return Err(format!("liveness check failed: {e}"));
    }
    Ok(note)
}

async fn run_cases(rig: &mut Rig, ids: &[&str]) -> usize {
    let mut failed = 0;
    for id in ids {
        let spec = spec_for(id).expect("ids were validated");
        println!("\n== {id}: {}", spec.send);
        match execute(rig, id, 1).await {
            Ok(note) => println!("{id}: ok, {note}"),
            Err(e) => {
                failed += 1;
                println!("{id}: FAIL, {e}");
            }
        }
        if let Some(look) = spec.look {
            println!("    look at the board: {look}");
            if let Err(e) = rig.sess.serve_until_quiet(Duration::from_secs(3)).await {
                println!("    {e}");
            }
        }
    }
    failed
}

/// Marks a leak sample, then closes and reconnects, so the board logs `heap before connect`.
async fn sample(rig: &mut Rig, round: u32) -> Res<()> {
    println!("\n== leak sample {round}");
    rig.sess.mark(&format!("LEAK{round}")).await?;
    rig.sess.serve_until_quiet(Duration::from_secs(1)).await?;
    rig.reconnect(Duration::from_secs(60)).await?;
    Ok(())
}

async fn run_leak(rig: &mut Rig, ids: &[&str]) -> Res<usize> {
    let mut failed = run_cases(rig, ids).await;
    sample(rig, 0).await?;
    for round in 1..=LEAK_ROUNDS {
        for id in ids {
            println!("== round {round}: {id} x {LEAK_REPS}");
            if let Err(e) = execute(rig, id, LEAK_REPS).await {
                failed += 1;
                println!("{id}: FAIL, {e}");
            }
        }
        sample(rig, round).await?;
    }
    Ok(failed)
}

async fn run(args: Args) -> Res<ExitCode> {
    let store = guard_files(&args.dir, &args.device)?;
    let wss = bind(SocketAddr::from(([0, 0, 0, 0], PORT))).await?;
    let plain = if args.pairing {
        Some(bind(SocketAddr::from(([0, 0, 0, 0], PAIR_PORT))).await?)
    } else {
        None
    };
    // Both files exist (checked above), so this loads and creates nothing.
    let identity = Identity::load_or_create(&args.dir)?;
    let _pair_tx = advertise(&identity, args.pairing).await?;

    if let Some(plain) = plain {
        pair::run(plain, identity.cert_pem()).await?;
        println!("\nPairing mode done. Grade the log with --check; the board is unpaired until you pair it again.");
        return Ok(ExitCode::SUCCESS);
    }

    let h = Harness {
        listener: wss,
        acceptor: TlsAcceptor::from(identity.server_config()?),
        store,
        device: args.device,
    };
    println!("Advertising on {PORT}; waiting up to 120 s for the board to connect.");
    let sess = h.accept(Duration::from_secs(120)).await?;
    let mut rig = Rig { h, sess };
    rig.bring_up().await?;
    println!("Board connected from {}.", rig.sess.peer);

    let failed = if args.leak {
        run_leak(&mut rig, &args.cases).await?
    } else {
        run_cases(&mut rig, &args.cases).await
    };
    rig.sess.close().await;
    println!("\n{failed} host-side failure(s). Now run --check on the serial log.");
    Ok(if failed == 0 {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    })
}

fn main() -> ExitCode {
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .expect("a runtime starts");
    let code = rt.block_on(real_main());
    // A prompt abandoned by Ctrl-C leaves a thread blocked on stdin, which a normal drop would wait for.
    rt.shutdown_background();
    code
}

async fn real_main() -> ExitCode {
    let mode = match parse(env::args().skip(1)) {
        Ok(mode) => mode,
        Err(e) => {
            eprintln!("{e}\n{USAGE}");
            return ExitCode::from(2);
        }
    };
    let result = match mode {
        Mode::Check {
            log,
            after_s5,
            plan,
        } => fs::read(&log).map_err(Into::into).map(|bytes| {
            let (report, ok) = check::grade(&String::from_utf8_lossy(&bytes), &plan, after_s5);
            print!("{report}");
            if ok {
                ExitCode::SUCCESS
            } else {
                ExitCode::FAILURE
            }
        }),
        Mode::Run(args) => run(args).await,
    };
    result.unwrap_or_else(|e| {
        eprintln!("error: {e}");
        ExitCode::FAILURE
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use watchdeck_lib::daemon::tls::Identity;

    const DEVICE: &str = "wd-000000000001";

    fn names(dir: &Path) -> Vec<String> {
        fs::read_dir(dir)
            .unwrap()
            .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
            .collect()
    }

    #[test]
    fn an_empty_dir_is_refused_and_stays_empty() {
        let dir = tempfile::tempdir().unwrap();
        let err = guard_files(dir.path(), DEVICE).err().unwrap();
        assert!(err.contains("never creates"), "{err}");
        assert!(names(dir.path()).is_empty());
    }

    #[test]
    fn a_missing_key_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        fs::write(dir.path().join(CERT_FILE), "cert").unwrap();
        assert!(guard_files(dir.path(), DEVICE)
            .err()
            .unwrap()
            .contains(KEY_FILE));
    }

    #[test]
    fn a_missing_state_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        Identity::load_or_create(dir.path()).unwrap();
        assert!(guard_files(dir.path(), DEVICE)
            .err()
            .unwrap()
            .contains(STATE_FILE));
    }

    #[test]
    fn only_a_paired_device_passes() {
        let dir = tempfile::tempdir().unwrap();
        Identity::load_or_create(dir.path()).unwrap();
        Store::load(dir.path())
            .unwrap()
            .insert(DEVICE, [7; 32])
            .unwrap();
        assert!(guard_files(dir.path(), DEVICE).is_ok());
        let err = guard_files(dir.path(), "wd-000000000002").err().unwrap();
        assert!(err.contains("does not pair"), "{err}");
    }

    #[tokio::test]
    async fn a_taken_port_is_refused() {
        let held = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let err = bind(held.local_addr().unwrap()).await.err().unwrap();
        assert!(err.contains("quit the tray app"), "{err}");
    }

    #[test]
    fn arguments_are_validated() {
        let run = |args: &[&str]| parse(args.iter().map(|a| a.to_string()));
        assert!(run(&["--device", DEVICE, "--cases", "I1,T3"]).is_ok());
        assert!(run(&["--device", DEVICE, "--cases", "P1"]).is_err());
        assert!(run(&["--device", "nope"]).is_err());
        assert!(run(&["--device", DEVICE, "--leak", "--pairing"]).is_err());
        assert!(matches!(run(&["--check", "x.log"]), Ok(Mode::Check { .. })));
        let plan = |args: &[&str]| match run(args) {
            Ok(Mode::Check { plan, .. }) => plan,
            _ => panic!("not a check"),
        };
        let normal = plan(&["--check", "x.log"]);
        assert!(normal.ids.contains(&"I13") && !normal.ids.contains(&"P1") && normal.runs == 1);
        let pairing = plan(&["--check", "x.log", "--pairing"]);
        assert_eq!(pairing.ids, ["P1", "P2", "P3", "P4", "P5"]);
        let leak = plan(&["--check", "x.log", "--leak"]);
        assert!(leak.leak && leak.runs == 4 && !leak.ids.contains(&"I10"));
        assert!(run(&["--check", "x.log", "--leak", "--cases", "I10"]).is_err());
    }
}
