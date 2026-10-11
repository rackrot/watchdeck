//! Live tiles: a button's `tile` source polled while a device is connected, and the latest value
//! of each for the sessions to push. One task per tile polls, waits `every_s`, and polls again, so
//! a tile never overlaps itself. A manager task starts and stops them as the config and the
//! number of connected devices change.

use std::collections::{BTreeMap, HashMap};
use std::future::Future;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use tokio::sync::{watch, Notify};
use tokio::task::JoinHandle;
use tokio::time::{sleep, sleep_until, Instant};

use super::actions::{printable, Actions, Ran};
use super::config::{Action, Live, TileConfig, MIN_EVERY_S};
use super::protocol::TileState;

/// The longest tile text, in characters. The device draws it on one line.
pub const MAX_TEXT: usize = 64;
/// A tile is polled again at least this long after its last poll started, whatever pokes it. A
/// device can press as fast as it likes, and a reconnect restarts every poller.
const COOLDOWN: Duration = Duration::from_secs(MIN_EVERY_S);
/// This many polls in a row that could not start mark the tile stale.
const STALE_AFTER: u32 = 3;
const STALE: &str = "stale";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TileValue {
    pub text: String,
    pub state: TileState,
}

/// A value and the tile definition it was read under, so a session can tell a reading of the
/// current source from one made before the config was edited.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Reading {
    pub tile: TileConfig,
    pub value: TileValue,
}

/// The latest reading of each polled tile, by button id. A tile that has not answered yet is
/// absent.
pub type Values = BTreeMap<String, Reading>;

/// What a session has told its device, by button id.
pub type Sent = BTreeMap<String, TileValue>;

/// Takes one reading of a tile. [`Actions`] runs the real source; tests stand in their own.
pub trait Sample: Clone + Send + Sync + 'static {
    /// `None` when no poll could start, such as when the tile slots are taken. The tile keeps its
    /// last value and tries again after `every_s`, and goes stale after [`STALE_AFTER`] in a row.
    fn sample(&self, id: &str, tile: &TileConfig)
        -> impl Future<Output = Option<TileValue>> + Send;
}

impl Sample for Actions {
    async fn sample(&self, id: &str, tile: &TileConfig) -> Option<TileValue> {
        let run = match self.start_tile(id) {
            Ok(run) => run,
            Err(refused) => {
                log::debug!("tile {id:?}: skipped, {}", refused.summary);
                return None;
            }
        };
        let ran = run.poll(&tile.source).await;
        Some(reading(tile, &ran))
    }
}

/// Aborts its task when dropped, so a stopped tile's poll is cancelled mid-run.
struct Task(Option<JoinHandle<()>>);

impl Task {
    fn new(handle: JoinHandle<()>) -> Self {
        Self(Some(handle))
    }

    /// Aborts the task and hands back its handle. The task's future, and what it holds (such as
    /// a run's claim on its tile), is gone only once the handle has resolved.
    fn stop(mut self) -> JoinHandle<()> {
        let handle = self.0.take().expect("a task is stopped once");
        handle.abort();
        handle
    }
}

impl Drop for Task {
    fn drop(&mut self) {
        if let Some(handle) = &self.0 {
            handle.abort();
        }
    }
}

type Pokes = Arc<Mutex<HashMap<String, Arc<Notify>>>>;
/// When each tile's last poll started. It outlives the pollers, so stopping and starting them
/// again can't be used to poll faster than [`COOLDOWN`].
type Starts = Arc<Mutex<HashMap<String, Instant>>>;

/// The running pollers. Dropping it stops them all.
pub struct Tiles {
    values: watch::Receiver<Values>,
    poker: Poker,
    _manager: Task,
}

/// Asks for a tile to be polled again, from wherever a press finishes.
#[derive(Clone)]
pub struct Poker(Pokes);

/// Polls the tiles of the live `config` while `connected` (the number of devices with a session)
/// is above 0. Must run inside a tokio runtime.
pub fn spawn<S: Sample>(
    sampler: S,
    config: watch::Receiver<Arc<Live>>,
    connected: watch::Receiver<usize>,
) -> Tiles {
    let (tx, values) = watch::channel(Values::new());
    let pokes = Pokes::default();
    let manager = tokio::spawn(manage(
        sampler,
        config,
        connected,
        Arc::new(tx),
        pokes.clone(),
    ));
    Tiles {
        values,
        poker: Poker(pokes),
        _manager: Task::new(manager),
    }
}

impl Tiles {
    /// The latest values. A session pushes them to its device whenever they change.
    pub fn values(&self) -> watch::Receiver<Values> {
        self.values.clone()
    }

    pub fn poker(&self) -> Poker {
        self.poker.clone()
    }
}

impl Poker {
    /// Polls `button`'s tile again soon, because a press may have changed what it shows: at once,
    /// or when [`COOLDOWN`] after its last poll began is up. Does nothing if the button has no tile
    /// or nothing is polling.
    pub fn poke(&self, button: &str) {
        if let Some(wake) = self.0.lock().expect("tile pokes poisoned").get(button) {
            wake.notify_one();
        }
    }
}

async fn manage<S: Sample>(
    sampler: S,
    mut config: watch::Receiver<Arc<Live>>,
    mut connected: watch::Receiver<usize>,
    values: Arc<watch::Sender<Values>>,
    pokes: Pokes,
) {
    let starts = Starts::default();
    let mut running: HashMap<String, (TileConfig, Task)> = HashMap::new();
    loop {
        let polling = *connected.borrow_and_update() > 0;
        let configured: Vec<(String, TileConfig)> = config
            .borrow_and_update()
            .buttons
            .iter()
            .filter_map(|b| Some((b.id.clone(), b.tile.clone()?)))
            .collect();
        let wanted: &[(String, TileConfig)] = if polling { &configured } else { &[] };
        starts
            .lock()
            .expect("tile starts poisoned")
            .retain(|id, _| configured.iter().any(|(c, _)| c == id));
        let was_polling = !running.is_empty();

        // Stop what is gone or changed, and wait until it is gone. A poller that is aborted but
        // not yet dropped still holds its tile's claim and could still write a value, so a
        // replacement started now would be refused its first poll.
        let gone: Vec<String> = running
            .iter()
            .filter(|(id, (tile, _))| !wanted.iter().any(|(w, t)| w == *id && t == tile))
            .map(|(id, _)| id.clone())
            .collect();
        let mut stopping = Vec::new();
        for id in &gone {
            if let Some((_, task)) = running.remove(id) {
                stopping.push(task.stop());
            }
        }
        for stopped in stopping {
            if let Err(e) = stopped.await {
                if e.is_panic() {
                    log::error!("a tile poller panicked: {e}");
                }
            }
        }
        // An edit is the user's, and polls at once. A tile that stops because no device is left
        // keeps its last poll time. Clear only after the old poller is gone, or its last start
        // could land after the clear and hold the replacement to the cooldown.
        for id in gone
            .iter()
            .filter(|id| wanted.iter().any(|(w, _)| w == *id))
        {
            starts.lock().expect("tile starts poisoned").remove(id);
        }
        // A changed source may answer differently, so its old value goes with it.
        values.send_if_modified(|v| {
            let before = v.len();
            v.retain(|id, _| running.contains_key(id));
            v.len() != before
        });
        for (id, tile) in wanted {
            if running.contains_key(id) {
                continue;
            }
            let wake = Arc::new(Notify::new());
            pokes
                .lock()
                .expect("tile pokes poisoned")
                .insert(id.clone(), wake.clone());
            let task = tokio::spawn(poll(
                sampler.clone(),
                id.clone(),
                tile.clone(),
                values.clone(),
                wake,
                starts.clone(),
            ));
            running.insert(id.clone(), (tile.clone(), Task::new(task)));
        }
        pokes
            .lock()
            .expect("tile pokes poisoned")
            .retain(|id, _| running.contains_key(id));

        match (was_polling, !running.is_empty()) {
            (false, true) => log::info!("polling {} tile(s)", running.len()),
            (true, false) => log::info!("no tiles to poll, stopped"),
            _ => {}
        }
        tokio::select! {
            changed = config.changed() => if changed.is_err() { return },
            changed = connected.changed() => if changed.is_err() { return },
        }
    }
}

/// Polls `tile` now, then again `every_s` after each poll ends, or sooner when poked, but never
/// less than [`COOLDOWN`] after the last poll began.
async fn poll<S: Sample>(
    sampler: S,
    id: String,
    tile: TileConfig,
    values: Arc<watch::Sender<Values>>,
    wake: Arc<Notify>,
    starts: Starts,
) {
    let every = Duration::from_secs(tile.every_s);
    let mut skipped = 0;
    loop {
        let not_before = starts
            .lock()
            .expect("tile starts poisoned")
            .get(&id)
            .map(|&began| began + COOLDOWN);
        if let Some(at) = not_before {
            sleep_until(at).await;
        }
        starts
            .lock()
            .expect("tile starts poisoned")
            .insert(id.clone(), Instant::now());

        let value = match sampler.sample(&id, &tile).await {
            Some(value) => {
                skipped = 0;
                Some(value)
            }
            None => {
                skipped += 1;
                // Not a fresh reading, so an old "ok" must not stand for long.
                (skipped == STALE_AFTER).then(|| TileValue {
                    text: STALE.into(),
                    state: TileState::Bad,
                })
            }
        };
        if let Some(value) = value {
            let reading = Reading {
                tile: tile.clone(),
                value,
            };
            let changed = values.send_if_modified(|all| {
                if all.get(&id) == Some(&reading) {
                    return false;
                }
                all.insert(id.clone(), reading.clone());
                true
            });
            if changed {
                let Reading { value, .. } = reading;
                log::info!("tile {id:?}: {:?} ({:?})", value.text, value.state);
            }
        }
        // A poke that came during the poll is kept, so the next poll starts at once.
        tokio::select! {
            () = sleep(every) => {}
            () = wake.notified() => {}
        }
    }
}

/// What a finished poll shows. A good poll shows its value: the part `json_pointer` picks out of
/// the output, or the output's first line. A bad one shows what went wrong, which for a shell
/// source is its first line of output if it printed one. The output is parsed as it was read,
/// then what was picked is redacted, then made printable: redacting first would miss a secret
/// that the reply escaped and the pointer then decodes.
fn reading(tile: &TileConfig, ran: &Ran) -> TileValue {
    let bad = |text: &str| TileValue {
        text: shown(text),
        state: TileState::Bad,
    };
    if !ran.result.ok {
        let said = match tile.source {
            Action::Shell { .. } => ran.redact(first_line(ran.raw())),
            Action::Http { .. } => String::new(),
        };
        return bad(if said.is_empty() {
            &ran.result.summary
        } else {
            &said
        });
    }
    let value = match &tile.json_pointer {
        None => Ok(first_line(ran.raw()).to_string()),
        Some(pointer) => pick(pointer, ran.raw()),
    };
    match value.map(|text| ran.redact(&text)) {
        Ok(text) if text.trim().is_empty() => TileValue {
            text: "ok".into(),
            state: TileState::Ok,
        },
        Ok(text) => TileValue {
            text: shown(&text),
            state: TileState::Ok,
        },
        Err(why) => bad(&why),
    }
}

fn first_line(text: &str) -> &str {
    text.lines()
        .map(str::trim)
        .find(|line| !line.is_empty())
        .unwrap_or("")
}

/// The string, number or boolean at `pointer` in the JSON `text`.
fn pick(pointer: &str, text: &str) -> Result<String, String> {
    use serde_json::Value;
    let json: Value = serde_json::from_str(text).map_err(|_| "not JSON".to_string())?;
    match json.pointer(pointer) {
        None => Err(format!("no {pointer} in the output")),
        Some(Value::String(s)) => Ok(s.clone()),
        Some(v @ (Value::Number(_) | Value::Bool(_))) => Ok(v.to_string()),
        Some(_) => Err(format!("{pointer} is not a string, number or boolean")),
    }
}

/// `text` as one trimmed line of printable ASCII, at most [`MAX_TEXT`] long.
fn shown(text: &str) -> String {
    let mut line = printable(text.as_bytes()).replace('\n', " ");
    line.truncate(MAX_TEXT);
    line.trim().to_string()
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicUsize, Ordering};

    use tokio::time::advance;

    use super::*;
    use crate::daemon::actions::ActionResult;
    use crate::daemon::config::parse;
    use crate::daemon::icons::IconSet;
    use crate::daemon::secrets::Secrets;

    /// A config with one button per `(id, every_s)`, each with a shell tile.
    fn live(tiles: &[(&str, u64)]) -> Arc<Live> {
        let mut text = String::from("version = 1\n[layout]\ncols = 8\nrows = 5\n");
        for (i, (id, every_s)) in tiles.iter().enumerate() {
            text += &format!(
                "[[button]]\nid = {id:?}\npos = [{i}, 0]\nlabel = {id:?}\n\
                 action = {{ kind = \"shell\", argv = [\"true\"] }}\n\
                 tile = {{ every_s = {every_s}, source = {{ kind = \"shell\", argv = [\"true\"] }} }}\n"
            );
        }
        let config = parse(&text, &Ok(Vec::new())).unwrap();
        Arc::new(Live {
            config,
            icons: IconSet::new(),
        })
    }

    /// Answers every poll with how many there have been so far, taking `takes` to do it.
    #[derive(Clone, Default)]
    struct Fake {
        calls: Arc<AtomicUsize>,
        in_flight: Arc<AtomicUsize>,
        most_in_flight: Arc<AtomicUsize>,
        takes: Duration,
        /// Every reading says this, instead of the number of polls so far.
        fixed: Option<&'static str>,
        /// The next this many polls can't start.
        skips: Arc<AtomicUsize>,
    }

    struct Flight(Arc<AtomicUsize>);

    impl Drop for Flight {
        fn drop(&mut self) {
            self.0.fetch_sub(1, Ordering::SeqCst);
        }
    }

    impl Fake {
        fn taking(secs: u64) -> Self {
            Self {
                takes: Duration::from_secs(secs),
                ..Self::default()
            }
        }

        fn calls(&self) -> usize {
            self.calls.load(Ordering::SeqCst)
        }
    }

    impl Sample for Fake {
        async fn sample(&self, _: &str, _: &TileConfig) -> Option<TileValue> {
            let n = self.calls.fetch_add(1, Ordering::SeqCst) + 1;
            if self
                .skips
                .try_update(Ordering::SeqCst, Ordering::SeqCst, |s| s.checked_sub(1))
                .is_ok()
            {
                return None;
            }
            let now = self.in_flight.fetch_add(1, Ordering::SeqCst) + 1;
            self.most_in_flight.fetch_max(now, Ordering::SeqCst);
            // Dropped with the poll, so an aborted poll leaves the count.
            let _flight = Flight(self.in_flight.clone());
            if !self.takes.is_zero() {
                sleep(self.takes).await;
            }
            Some(TileValue {
                text: self.fixed.map_or_else(|| n.to_string(), String::from),
                state: TileState::Ok,
            })
        }
    }

    struct Rig {
        tiles: Tiles,
        config: watch::Sender<Arc<Live>>,
        connected: watch::Sender<usize>,
    }

    /// Tiles polling through `fake`, started and settled at t = 0.
    async fn rig(fake: &Fake, tiles: &[(&str, u64)], connected: usize) -> Rig {
        let (config, config_rx) = watch::channel(live(tiles));
        let (connected, connected_rx) = watch::channel(connected);
        let rig = Rig {
            tiles: spawn(fake.clone(), config_rx, connected_rx),
            config,
            connected,
        };
        settle().await;
        rig
    }

    /// Lets every task that can run, run. Tokio's paused clock moves only when told to.
    async fn settle() {
        for _ in 0..20 {
            tokio::task::yield_now().await;
        }
    }

    /// Moves the paused clock by `secs`, one second at a time so each step's polls run.
    async fn pass(secs: u64) {
        for _ in 0..secs {
            advance(Duration::from_secs(1)).await;
            settle().await;
        }
    }

    #[tokio::test(start_paused = true)]
    async fn a_tile_polls_at_once_and_then_every_interval() {
        let fake = Fake::default();
        let rig = rig(&fake, &[("a", 5)], 1).await;
        assert_eq!(fake.calls(), 1);
        assert_eq!(rig.tiles.values().borrow()["a"].value.text, "1");

        advance(Duration::from_millis(4999)).await;
        settle().await;
        assert_eq!(fake.calls(), 1, "not before the interval is up");
        advance(Duration::from_millis(1)).await;
        settle().await;
        assert_eq!(fake.calls(), 2);
        assert_eq!(rig.tiles.values().borrow()["a"].value.text, "2");

        pass(10).await;
        assert_eq!(fake.calls(), 4);
    }

    #[tokio::test(start_paused = true)]
    async fn each_tile_keeps_its_own_interval() {
        let fake = Fake::default();
        let rig = rig(&fake, &[("fast", 2), ("slow", 10)], 1).await;
        pass(10).await;
        // fast at 0, 2, 4, 6, 8, 10 and slow at 0, 10
        assert_eq!(fake.calls(), 8);
        assert_eq!(rig.tiles.values().borrow().len(), 2);
    }

    #[tokio::test(start_paused = true)]
    async fn a_slow_poll_never_overlaps_the_next() {
        // Polls take 12 s against a 5 s interval.
        let fake = Fake::taking(12);
        let _rig = rig(&fake, &[("a", 5)], 1).await;
        pass(60).await;
        // Started at 0, 17, 34 and 51: the interval runs from the end of a poll.
        assert_eq!(fake.calls(), 4);
        assert_eq!(fake.most_in_flight.load(Ordering::SeqCst), 1);
    }

    #[tokio::test(start_paused = true)]
    async fn nothing_polls_without_a_device_and_polling_stops_when_the_last_one_leaves() {
        let fake = Fake::taking(3);
        let rig = rig(&fake, &[("a", 5)], 0).await;
        pass(100).await;
        assert_eq!(fake.calls(), 0, "no device, no polls");
        assert!(rig.tiles.values().borrow().is_empty());

        rig.connected.send(1).unwrap();
        pass(4).await;
        assert_eq!(fake.calls(), 1);
        assert_eq!(rig.tiles.values().borrow().len(), 1);

        // A second device changes nothing.
        rig.connected.send(2).unwrap();
        pass(1).await;
        assert_eq!(fake.calls(), 1);

        // The last one leaves during a poll.
        rig.connected.send(1).unwrap();
        pass(4).await;
        assert_eq!(fake.calls(), 2);
        rig.connected.send(0).unwrap();
        settle().await;
        assert_eq!(
            fake.in_flight.load(Ordering::SeqCst),
            0,
            "the poll is cancelled"
        );
        assert!(
            rig.tiles.values().borrow().is_empty(),
            "stale values are dropped"
        );
        pass(100).await;
        assert_eq!(fake.calls(), 2, "and nothing polls after that");

        // It starts over when a device returns.
        rig.connected.send(1).unwrap();
        pass(4).await;
        assert_eq!(fake.calls(), 3);
        assert_eq!(rig.tiles.values().borrow()["a"].value.text, "3");
    }

    #[tokio::test(start_paused = true)]
    async fn a_press_polls_the_button_again_at_once() {
        let fake = Fake::default();
        let rig = rig(&fake, &[("a", 60), ("b", 60)], 1).await;
        assert_eq!(fake.calls(), 2);

        pass(30).await;
        rig.tiles.poker().poke("a");
        settle().await;
        assert_eq!(fake.calls(), 3, "only the pressed button's tile polls");
        assert_eq!(rig.tiles.values().borrow()["a"].value.text, "3");

        rig.tiles.poker().poke("no-such-button");
        settle().await;
        assert_eq!(fake.calls(), 3);

        // The poked poll restarts the interval: a is next due at 90 s, b at 60 s.
        pass(30).await;
        assert_eq!(fake.calls(), 4, "b alone");
        pass(29).await;
        assert_eq!(fake.calls(), 4);
        pass(1).await;
        assert_eq!(fake.calls(), 5, "a, 60 s after its poked poll");
    }

    #[tokio::test(start_paused = true)]
    async fn a_press_during_a_poll_polls_again_right_after_it_and_only_once() {
        let fake = Fake::taking(3);
        let rig = rig(&fake, &[("a", 60)], 1).await;
        pass(1).await;
        let poker = rig.tiles.poker();
        poker.poke("a");
        poker.poke("a");
        pass(1).await;
        assert_eq!(fake.calls(), 1, "the poll in flight is not doubled");
        assert_eq!(fake.most_in_flight.load(Ordering::SeqCst), 1);
        pass(1).await; // the first poll ends at 3 s, and the poked one starts
        assert_eq!(fake.calls(), 2);
        pass(5).await;
        assert_eq!(fake.calls(), 2, "two pokes bought one extra poll");
    }

    #[tokio::test(start_paused = true)]
    async fn a_poke_with_nothing_polling_does_nothing_and_leaves_nothing_behind() {
        let fake = Fake::default();
        let rig = rig(&fake, &[("a", 60)], 0).await;
        rig.tiles.poker().poke("a");
        rig.connected.send(1).unwrap();
        settle().await;
        assert_eq!(fake.calls(), 1, "a stale poke does not add a poll");
        pass(30).await;
        assert_eq!(fake.calls(), 1);
    }

    #[tokio::test(start_paused = true)]
    async fn config_edits_start_stop_and_restart_only_what_changed() {
        let fake = Fake::default();
        let rig = rig(&fake, &[("a", 60), ("b", 60)], 1).await;
        assert_eq!(fake.calls(), 2);

        // b goes, a is untouched, c is new.
        rig.config.send(live(&[("a", 60), ("c", 60)])).unwrap();
        settle().await;
        assert_eq!(fake.calls(), 3, "only c polls");
        let values = rig.tiles.values().borrow().clone();
        assert_eq!(values.keys().collect::<Vec<_>>(), ["a", "c"]);

        // A new interval restarts that tile and nothing else.
        rig.config.send(live(&[("a", 30), ("c", 60)])).unwrap();
        settle().await;
        assert_eq!(
            fake.calls(),
            4,
            "an edit polls at once, whatever the cooldown"
        );
        pass(30).await;
        assert_eq!(fake.calls(), 5, "a again, 30 s on");

        // No tiles at all.
        rig.config.send(live(&[])).unwrap();
        settle().await;
        assert!(rig.tiles.values().borrow().is_empty());
        pass(120).await;
        assert_eq!(fake.calls(), 5);
    }

    #[tokio::test(start_paused = true)]
    async fn dropping_tiles_stops_the_pollers() {
        let fake = Fake::default();
        let rig = rig(&fake, &[("a", 5)], 1).await;
        let Rig { tiles, .. } = rig;
        drop(tiles);
        settle().await;
        pass(60).await;
        assert_eq!(fake.calls(), 1);
    }

    #[tokio::test(start_paused = true)]
    async fn an_unchanged_reading_notifies_nobody() {
        let fake = Fake {
            fixed: Some("same"),
            ..Fake::default()
        };
        let rig = rig(&fake, &[("a", 5)], 1).await;
        let mut seen = rig.tiles.values();
        assert_eq!(seen.borrow_and_update()["a"].value.text, "same");
        pass(5).await;
        assert_eq!(fake.calls(), 2);
        assert!(
            !seen.has_changed().unwrap(),
            "the same reading woke a session"
        );
    }

    // ---- Cooldown ----

    #[tokio::test(start_paused = true)]
    async fn a_poke_within_two_seconds_of_a_poll_waits_for_the_two_seconds() {
        let fake = Fake::default();
        let rig = rig(&fake, &[("a", 60)], 1).await;
        advance(Duration::from_millis(500)).await;
        rig.tiles.poker().poke("a");
        settle().await;
        assert_eq!(fake.calls(), 1);
        advance(Duration::from_millis(1499)).await;
        settle().await;
        assert_eq!(fake.calls(), 1);
        advance(Duration::from_millis(1)).await;
        settle().await;
        assert_eq!(fake.calls(), 2);
        // A hammering device gets one poll per cooldown, not one per press.
        let poker = rig.tiles.poker();
        for _ in 0..40 {
            poker.poke("a");
            advance(Duration::from_millis(100)).await;
            settle().await;
        }
        assert_eq!(fake.calls(), 4, "4 s of pokes bought 2 polls");
    }

    #[tokio::test(start_paused = true)]
    async fn leaving_and_returning_cannot_poll_faster_than_the_cooldown() {
        let fake = Fake::default();
        let rig = rig(&fake, &[("a", 60)], 1).await;
        assert_eq!(fake.calls(), 1);
        advance(Duration::from_millis(500)).await;
        rig.connected.send(0).unwrap();
        settle().await;
        advance(Duration::from_millis(500)).await;
        rig.connected.send(1).unwrap();
        settle().await;
        assert_eq!(fake.calls(), 1, "the poll at t = 0 is under 2 s old");
        advance(Duration::from_millis(1000)).await;
        settle().await;
        assert_eq!(fake.calls(), 2, "and goes ahead at t = 2 s");

        // Many cycles in a row cost one poll per 2 s at most.
        for _ in 0..20 {
            rig.connected.send(0).unwrap();
            settle().await;
            advance(Duration::from_millis(100)).await;
            rig.connected.send(1).unwrap();
            settle().await;
        }
        assert!(fake.calls() <= 2 + 1, "{} polls in 2 s", fake.calls());
    }

    // ---- Stale tiles ----

    #[tokio::test(start_paused = true)]
    async fn three_skipped_polls_in_a_row_mark_the_tile_stale() {
        let fake = Fake::default();
        let rig = rig(&fake, &[("a", 5)], 1).await;
        let shown = |rig: &Rig| {
            let v = rig.tiles.values().borrow()["a"].value.clone();
            (v.text, v.state)
        };
        assert_eq!(shown(&rig), ("1".to_string(), TileState::Ok));

        fake.skips.store(3, Ordering::SeqCst);
        pass(14).await; // skips at 5 s and 10 s
        assert_eq!(shown(&rig), ("1".to_string(), TileState::Ok));
        pass(1).await; // the third, at 15 s
        assert_eq!(shown(&rig), ("stale".to_string(), TileState::Bad));
        pass(5).await; // a poll that starts again replaces it
        assert_eq!(shown(&rig), ("5".to_string(), TileState::Ok));

        // One skip in between resets the count.
        fake.skips.store(2, Ordering::SeqCst);
        pass(10).await;
        assert_eq!(shown(&rig).1, TileState::Ok);
        pass(5).await;
        fake.skips.store(2, Ordering::SeqCst);
        pass(10).await;
        assert_eq!(shown(&rig).1, TileState::Ok, "never three in a row");
    }

    // ---- Replacing a poller ----

    /// Stands in for `Actions` where it matters: one claim per tile, held until the poll's future
    /// is dropped. An odd `every_s` polls slowly.
    #[derive(Clone, Default)]
    struct Claims {
        held: Arc<Mutex<std::collections::HashSet<String>>>,
        fresh: Arc<AtomicUsize>,
    }

    struct Release(Arc<Mutex<std::collections::HashSet<String>>>, String);

    impl Drop for Release {
        fn drop(&mut self) {
            self.0.lock().unwrap().remove(&self.1);
        }
    }

    impl Sample for Claims {
        async fn sample(&self, id: &str, tile: &TileConfig) -> Option<TileValue> {
            if !self.held.lock().unwrap().insert(id.to_string()) {
                return None; // "already running"
            }
            let _release = Release(self.held.clone(), id.to_string());
            if tile.every_s % 2 == 1 {
                sleep(Duration::from_secs(3600)).await;
            }
            self.fresh.fetch_add(1, Ordering::SeqCst);
            Some(TileValue {
                text: "x".into(),
                state: TileState::Ok,
            })
        }
    }

    async fn until(what: &str, mut done: impl FnMut() -> bool) {
        for _ in 0..200 {
            if done() {
                return;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        panic!("timed out waiting for {what}");
    }

    /// An edited tile must poll at once even while the poll it replaces is still in flight and
    /// holds the tile's claim. On a multi-thread runtime an aborted future is dropped later, so
    /// a replacement that doesn't wait for that is refused its claim and sleeps `every_s`.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn an_edited_tile_polls_at_once_though_the_old_poll_held_the_claim() {
        let claims = Claims::default();
        let (config, config_rx) = watch::channel(live(&[("a", 61)]));
        let (_connected, connected_rx) = watch::channel(1);
        let _tiles = spawn(claims.clone(), config_rx, connected_rx);
        until("the slow poll to claim", || {
            claims.held.lock().unwrap().contains("a")
        })
        .await;

        for i in 0..20 {
            // A fast edit over a slow poll that is in flight...
            let before = claims.fresh.load(Ordering::SeqCst);
            config.send(live(&[("a", 100 + 2 * i)])).unwrap();
            until("the edited tile's first poll", || {
                claims.fresh.load(Ordering::SeqCst) > before
            })
            .await;
            // ...then a slow one over the idle fast one, to have a poll in flight again.
            config.send(live(&[("a", 101 + 2 * i)])).unwrap();
            until("the slow poll to claim", || {
                claims.held.lock().unwrap().contains("a")
            })
            .await;
        }
    }

    // ---- Slots ----

    #[test]
    fn tile_polls_leave_two_slots_for_presses() {
        let actions = actions();
        let polls: Vec<_> = (0..crate::daemon::actions::MAX_TILE_RUNNING)
            .map(|i| actions.start_tile(&format!("t{i}")).unwrap())
            .collect();
        let refused = actions.start_tile("t6").err().expect("a seventh poll");
        assert_eq!(refused.summary, "busy: 6 tile polls are running");
        // The two slots left are for presses.
        let first = actions.start("p1").unwrap();
        let second = actions.start("p2").unwrap();
        assert!(actions.start("p3").is_err());
        drop((first, second));
        drop(polls);
        actions.start_tile("t6").expect("freed slots poll again");
    }

    #[tokio::test]
    async fn a_poll_at_the_tile_cap_is_skipped() {
        let actions = actions();
        let _polls: Vec<_> = (0..crate::daemon::actions::MAX_TILE_RUNNING)
            .map(|i| actions.start_tile(&format!("t{i}")).unwrap())
            .collect();
        assert_eq!(actions.sample("x", &tile(None, sh("echo up"))).await, None);
    }

    // ---- Secrets ----

    fn secret(value: &str) -> crate::daemon::secrets::Secret {
        let dir = tempfile::tempdir().unwrap();
        let secrets = Secrets::new(dir.path());
        secrets.set("s", value).unwrap();
        secrets.load().unwrap()["s"].clone()
    }

    fn http_ran(raw: &str, secret_value: &str) -> Ran {
        let result = ActionResult {
            ok: true,
            code: 200,
            summary: "HTTP 200".into(),
            text: raw.into(),
            ms: 1,
        };
        Ran::new(result, raw, vec![secret(secret_value)], false)
    }

    #[test]
    fn a_json_pointer_cannot_decode_a_secret_the_reply_escaped() {
        let via = |pointer: &str, raw: &str| {
            shows(
                &tile(Some(pointer), http()),
                &http_ran(raw, "Bearer abc/def"),
            )
        };
        let hidden = |text: &str| (text.to_string(), TileState::Ok);
        // An escaped slash, and an escaped letter: literal redaction of the raw reply misses both.
        assert_eq!(via("/v", r#"{"v":"Bearer abc\/def"}"#), hidden("<secret>"));
        assert_eq!(
            via("/v", r#"{"v":"\u0042earer abc/def"}"#),
            hidden("<secret>")
        );
        assert_eq!(
            via("/v", r#"{"v":"id=Bearer abc\/def;"}"#),
            hidden("id=<secret>;")
        );
        // The plain echo still goes, with or without a pointer.
        assert_eq!(via("/v", r#"{"v":"Bearer abc/def"}"#), hidden("<secret>"));
        let plain = shows(
            &tile(None, http()),
            &http_ran("Bearer abc/def\nnext", "Bearer abc/def"),
        );
        assert_eq!(plain, hidden("<secret>"));
    }

    #[test]
    fn the_pointer_reads_the_raw_output_so_non_ascii_keys_match() {
        let t = tile(Some("/n\u{e4}me"), http());
        let ran = http_ran("{\"n\u{e4}me\":\"v\"}", "x");
        assert_eq!(shows(&t, &ran), ("v".to_string(), TileState::Ok));
    }

    #[test]
    fn a_shell_pointer_over_noisy_stderr_is_not_json() {
        let t = tile(Some("/a"), shell());
        let ran = result(true, "exit 0", "{\"a\":\"x\"}\n  % Total    % Received\n");
        assert_eq!(shows(&t, &ran), ("not JSON".to_string(), TileState::Bad));
    }

    // ---- What a poll shows ----

    fn tile(pointer: Option<&str>, source: Action) -> TileConfig {
        TileConfig {
            every_s: 5,
            json_pointer: pointer.map(String::from),
            source,
        }
    }

    fn shell() -> Action {
        Action::Shell {
            argv: vec!["true".into()],
            timeout_s: 10,
        }
    }

    fn http() -> Action {
        Action::Http {
            method: "GET".into(),
            url: "https://h/".into(),
            body: None,
            headers: Default::default(),
            secret_headers: Default::default(),
            insecure_http: false,
            timeout_s: 10,
        }
    }

    fn result(ok: bool, summary: &str, text: &str) -> Ran {
        let result = ActionResult {
            ok,
            code: if ok { 0 } else { 1 },
            summary: summary.into(),
            text: text.into(),
            ms: 3,
        };
        Ran::new(result, text, Vec::new(), false)
    }

    fn shows(tile: &TileConfig, result: &Ran) -> (String, TileState) {
        let v = reading(tile, result);
        (v.text, v.state)
    }

    #[test]
    fn a_good_poll_shows_the_first_line_of_output() {
        let t = tile(None, shell());
        let ok = |text: &str| (text.to_string(), TileState::Ok);
        assert_eq!(
            shows(&t, &result(true, "exit 0", "0.42\nmore\n")),
            ok("0.42")
        );
        assert_eq!(
            shows(&t, &result(true, "exit 0", "\n  \n  up 3 days  \nx")),
            ok("up 3 days")
        );
        assert_eq!(shows(&t, &result(true, "exit 0", "")), ok("ok"));
        assert_eq!(shows(&t, &result(true, "HTTP 200", "pong")), ok("pong"));
    }

    #[test]
    fn a_bad_shell_poll_shows_its_first_line_or_else_the_summary() {
        let t = tile(None, shell());
        let bad = |text: &str| (text.to_string(), TileState::Bad);
        assert_eq!(
            shows(&t, &result(false, "exit 1", "DOWN\ndetails")),
            bad("DOWN")
        );
        assert_eq!(shows(&t, &result(false, "exit 3", "")), bad("exit 3"));
        assert_eq!(
            shows(&t, &result(false, "timed out after 10 s", "")),
            bad("timed out after 10 s")
        );
    }

    #[test]
    fn a_bad_http_poll_shows_its_summary_not_the_body() {
        let t = tile(Some("/state"), http());
        assert_eq!(
            shows(
                &t,
                &result(false, "HTTP 503", "<html>Service Unavailable</html>")
            ),
            ("HTTP 503".to_string(), TileState::Bad)
        );
    }

    #[test]
    fn a_json_pointer_picks_a_string_number_or_boolean() {
        let body =
            r#"{"state":"on","n":{"load":[0.42,1.5]},"up":true,"i":7,"none":null,"obj":{"a":1}}"#;
        let pick = |p: &str| shows(&tile(Some(p), http()), &result(true, "HTTP 200", body));
        let ok = |t: &str| (t.to_string(), TileState::Ok);
        assert_eq!(pick("/state"), ok("on"));
        assert_eq!(pick("/n/load/1"), ok("1.5"));
        assert_eq!(pick("/up"), ok("true"));
        assert_eq!(pick("/i"), ok("7"));
        for (pointer, why) in [
            ("/missing", "no /missing in the output"),
            ("/n/load/9", "no /n/load/9 in the output"),
            ("/none", "/none is not a string, number or boolean"),
            ("/obj", "/obj is not a string, number or boolean"),
            ("/n/load", "/n/load is not a string, number or boolean"),
        ] {
            assert_eq!(
                pick(pointer),
                (why.to_string(), TileState::Bad),
                "{pointer}"
            );
        }
        let not_json = shows(
            &tile(Some("/a"), http()),
            &result(true, "HTTP 200", "<html>"),
        );
        assert_eq!(not_json, ("not JSON".to_string(), TileState::Bad));
        // The pointer applies to shell output too.
        let sh = shows(
            &tile(Some("/a"), shell()),
            &result(true, "exit 0", r#"{"a":"x"}"#),
        );
        assert_eq!(sh, ("x".to_string(), TileState::Ok));
    }

    #[test]
    fn text_is_one_short_line_of_printable_ascii() {
        let t = tile(Some("/s"), http());
        let long = format!(r#"{{"s":"{}"}}"#, "x".repeat(200));
        let (text, state) = shows(&t, &result(true, "HTTP 200", &long));
        assert_eq!((text.len(), state), (MAX_TEXT, TileState::Ok));
        let (text, _) = shows(
            &t,
            &result(true, "HTTP 200", r#"{"s":"x\u00e9\n\tok\u001b"}"#),
        );
        assert_eq!(text, "x?  ok?");
        // A summary can carry a long error, which gets the same cap.
        let long_error = result(false, &"e".repeat(96), "");
        assert_eq!(
            reading(&tile(None, http()), &long_error).text.len(),
            MAX_TEXT
        );
    }

    // ---- The real source ----

    fn actions() -> Actions {
        let dir = std::env::temp_dir();
        Actions::new(Arc::new(Secrets::new(&dir)), dir).unwrap()
    }

    fn sh(script: &str) -> Action {
        Action::Shell {
            argv: vec!["sh".into(), "-c".into(), script.into()],
            timeout_s: 5,
        }
    }

    #[tokio::test]
    async fn actions_poll_a_real_source() {
        let actions = actions();
        let got = actions
            .sample("t", &tile(None, sh("echo 0.42; echo rest")))
            .await;
        assert_eq!(
            got,
            Some(TileValue {
                text: "0.42".into(),
                state: TileState::Ok
            })
        );
        let got = actions
            .sample("t", &tile(None, sh("echo DOWN; exit 1")))
            .await
            .unwrap();
        assert_eq!((got.text.as_str(), got.state), ("DOWN", TileState::Bad));
        // 4 KB of output still parses as JSON, which the 1 KB press text could not.
        let big = r#"printf '{"pad":"%s","v":"found"}' "$(head -c 4000 /dev/zero | tr '\0' x)""#;
        let got = actions
            .sample("t", &tile(Some("/v"), sh(big)))
            .await
            .unwrap();
        assert_eq!(got.text, "found");
    }

    #[tokio::test]
    async fn a_poll_and_a_press_of_one_button_do_not_block_each_other() {
        let actions = actions();
        let press = actions.start("a").unwrap();
        let polled = actions.sample("a", &tile(None, sh("echo up"))).await;
        assert_eq!(polled.unwrap().text, "up");
        // The other way round: a poll in progress leaves the button free to press.
        drop(press);
        let poll = actions.start_tile("a").unwrap();
        actions.start("a").expect("a press ignores the poll");
        // And the tile itself never runs twice at once, or past the action limit.
        assert_eq!(actions.sample("a", &tile(None, sh("echo up"))).await, None);
        drop(poll);
        assert!(actions
            .sample("a", &tile(None, sh("echo up")))
            .await
            .is_some());
    }

    #[tokio::test]
    async fn a_poll_that_cannot_get_a_slot_keeps_the_last_value() {
        let actions = actions();
        let held: Vec<_> = (0..crate::daemon::actions::MAX_RUNNING)
            .map(|i| actions.start(&format!("b{i}")).unwrap())
            .collect();
        assert_eq!(actions.sample("t", &tile(None, sh("echo up"))).await, None);
        drop(held);
    }
}
