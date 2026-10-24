//! The sources behind the -1 page, built from `[[integration]]`. One interface holds them all:
//!
//! - a [`Connector`] is a polled source. The manager polls it on its schedule while a device is
//!   connected and hands what it returns to the [feed store](super::feed), so a connector only
//!   reads and maps. The manager owns the cooldown, the stale mark and the tasks.
//! - a [`Receiver`] is a source that gets requests. A listener authenticates the request, which
//!   picks the integration, and [`FeedHandle::receive`] applies what the receiver made of it.
//!
//! Adding a source is a `kind` in [`IntegrationConfig`] with its rules, an implementation of one
//! of the two traits, and one arm in [`build`]. Nothing in the store, the sessions or the board
//! changes.

mod push;
mod stat;

use std::collections::HashMap;
use std::future::pending;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::future::BoxFuture;
use tokio::sync::watch;
use tokio::time::{sleep, sleep_until, Instant};

pub use stat::FeedSlots;

use super::config::{IntegrationConfig, Live};
use super::feed::{
    Applied, Card, FeedStore, Snapshot, SourceInfo, StoreError, Update, MAX_SOURCE_CARDS,
};
use super::protocol::Feed;
use super::tiles::{Sample, Task, COOLDOWN, STALE_AFTER};

/// A polled source.
pub trait Connector: Send + Sync + 'static {
    /// The wait from the end of one poll to the start of the next.
    fn every(&self) -> Duration;

    /// The cards to show before the first reading, and after polling stops. A connector with
    /// cards says what they are here, so the board has the slot before there is a value.
    fn pending(&self) -> Vec<Card>;

    /// Reads the source once. `Ok` replaces what the source showed. `Err` means the poll could
    /// not run at all, such as when the slots were taken: the cards keep their last values, and
    /// after [`STALE_AFTER`] in a row they show as unknown. A source that ran and failed is an
    /// `Ok` with a card that says so. Strings are redacted here and cut by the store.
    fn poll(&self) -> BoxFuture<'_, Result<Snapshot, String>>;
}

/// An inbound request that a listener has authenticated as one integration's.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PushRequest {
    pub method: String,
    /// The path under `/v1/`, such as `notes` or `cards/washer`.
    pub path: String,
    pub body: Vec<u8>,
}

/// Why a request got no change, as an HTTP status and a short reason for the caller.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PushError {
    pub status: u16,
    pub reason: String,
}

impl PushError {
    pub fn new(status: u16, reason: impl Into<String>) -> Self {
        Self {
            status,
            reason: reason.into(),
        }
    }
}

impl From<StoreError> for PushError {
    fn from(error: StoreError) -> Self {
        match error {
            StoreError::UnknownSource => Self::new(404, "no such integration"),
            StoreError::NotFound => Self::new(404, "not found"),
            StoreError::CardsFull => Self::new(409, "all card slots are taken"),
            StoreError::SourceCardsFull => Self::new(
                409,
                format!("this integration has {MAX_SOURCE_CARDS} cards, the most one may have"),
            ),
            StoreError::NotesFull => {
                Self::new(409, "all note slots hold other integrations' notes")
            }
            StoreError::BadId => Self::new(400, "id is too long"),
        }
    }
}

/// A source that gets requests.
pub trait Receiver: Send + Sync + 'static {
    /// The first path segments under `/v1/` that this kind answers, such as `notes`. A token is
    /// refused on any other route, as if it were wrong.
    fn routes(&self) -> &'static [&'static str];

    /// What a request changes. The caller applies the updates in order and stops at the first
    /// that fails.
    fn accept(&self, req: &PushRequest) -> Result<Vec<Update>, PushError>;
}

#[derive(Clone)]
pub enum Source {
    Polled(Arc<dyn Connector>),
    Push(Arc<dyn Receiver>),
}

/// The registry: the source for `config`, one arm per kind. `sampler` takes the readings of the
/// kinds that run a command or a request.
pub fn build<S: Sample>(config: &IntegrationConfig, sampler: &S) -> Source {
    match config {
        IntegrationConfig::Push { .. } => Source::Push(Arc::new(push::Routes)),
        IntegrationConfig::Stat {
            id,
            label,
            every_s,
            json_pointer,
            source,
        } => Source::Polled(Arc::new(stat::Stat::new(
            sampler.clone(),
            id,
            label,
            *every_s,
            json_pointer,
            source,
        ))),
    }
}

type Receivers = Arc<Mutex<HashMap<String, Arc<dyn Receiver>>>>;
/// When each source's last poll started. It outlives the pollers, so stopping and starting them
/// again can't be used to poll faster than [`COOLDOWN`].
type Starts = Arc<Mutex<HashMap<String, Instant>>>;

/// What the sessions, the window and the push listeners hold: the store and the receivers.
#[derive(Clone)]
pub struct FeedHandle {
    store: Arc<FeedStore>,
    receivers: Receivers,
}

impl FeedHandle {
    /// The snapshot every session sends, replaced on each change.
    pub fn subscribe(&self) -> watch::Receiver<Arc<Feed>> {
        self.store.subscribe()
    }

    pub fn snapshot(&self) -> Arc<Feed> {
        self.store.snapshot()
    }

    /// Removes the note with wire id `id` for every board. An unknown id is ignored.
    pub fn dismiss(&self, id: &str) -> bool {
        self.store.dismiss(id)
    }

    /// Applies `update` as the push integration `integration`, such as a note from this process.
    pub fn post(&self, integration: &str, update: Update) -> Result<Applied, PushError> {
        if !self.is_push(integration) {
            return Err(StoreError::UnknownSource.into());
        }
        Ok(self.store.apply(integration, update)?)
    }

    /// Runs `req` through the push integration `integration` and applies what it asks for.
    pub fn receive(&self, integration: &str, req: &PushRequest) -> Result<Vec<Applied>, PushError> {
        let receiver = self
            .receivers
            .lock()
            .expect("receivers poisoned")
            .get(integration)
            .cloned()
            .ok_or(StoreError::UnknownSource)?;
        receiver
            .accept(req)?
            .into_iter()
            .map(|update| Ok(self.store.apply(integration, update)?))
            .collect()
    }

    /// Whether the push integration `integration` answers `path` (under `/v1/`), by its first
    /// segment. False for an integration that isn't a push one.
    pub fn answers(&self, integration: &str, path: &str) -> bool {
        let first = path.split('/').next().unwrap_or_default();
        (self.receivers.lock().expect("receivers poisoned"))
            .get(integration)
            .is_some_and(|r| r.routes().contains(&first))
    }

    fn is_push(&self, integration: &str) -> bool {
        (self.receivers.lock().expect("receivers poisoned")).contains_key(integration)
    }
}

/// The running manager and note expiry. Dropping it stops them.
pub struct Integrations {
    handle: FeedHandle,
    _manager: Task,
    _expiry: Task,
}

impl Integrations {
    pub fn handle(&self) -> FeedHandle {
        self.handle.clone()
    }
}

/// Builds the sources of the live `config` and polls the polled ones while `connected` (the
/// number of devices with a session) is above 0. The store knows the configured sources before
/// this returns. Must run inside a tokio runtime.
pub fn spawn<S: Sample>(
    sampler: S,
    config: watch::Receiver<Arc<Live>>,
    connected: watch::Receiver<usize>,
) -> Integrations {
    let store = FeedStore::new();
    let receivers = Receivers::default();
    let mut manager = Manager {
        sampler,
        store: store.clone(),
        receivers: receivers.clone(),
        built: HashMap::new(),
        running: HashMap::new(),
        starts: Starts::default(),
    };
    // Before the first session can read the store, which a task's first turn would not be.
    manager.apply_config(&config.borrow());
    let expiry = tokio::spawn(expire(store.clone()));
    Integrations {
        handle: FeedHandle { store, receivers },
        _manager: Task::new(tokio::spawn(manager.run(config, connected))),
        _expiry: Task::new(expiry),
    }
}

struct Built {
    config: IntegrationConfig,
    source: Source,
}

struct Manager<S> {
    sampler: S,
    store: Arc<FeedStore>,
    receivers: Receivers,
    /// Every configured integration.
    built: HashMap<String, Built>,
    /// The polling tasks, each with the connector it runs.
    running: HashMap<String, (Arc<dyn Connector>, Task)>,
    starts: Starts,
}

impl<S: Sample> Manager<S> {
    async fn run(
        mut self,
        mut config: watch::Receiver<Arc<Live>>,
        mut connected: watch::Receiver<usize>,
    ) {
        loop {
            let live = config.borrow_and_update().clone();
            let polling = *connected.borrow_and_update() > 0;
            self.apply_config(&live);
            self.reconcile_polls(polling).await;
            tokio::select! {
                changed = config.changed() => if changed.is_err() { return },
                changed = connected.changed() => if changed.is_err() { return },
            }
        }
    }

    /// Builds what is new or edited and forgets what is gone. An edited polled source starts over
    /// with its pending cards, so its old reading doesn't stand for the new source.
    fn apply_config(&mut self, live: &Live) {
        let configured = &live.integrations;
        self.store.set_sources(
            (configured.iter())
                .map(|i| SourceInfo {
                    id: i.id().into(),
                    label: i.label().into(),
                })
                .collect(),
        );
        self.built
            .retain(|id, _| configured.iter().any(|i| i.id() == id));
        for config in configured {
            let id = config.id();
            if self.built.get(id).is_some_and(|b| b.config == *config) {
                continue;
            }
            let source = build(config, &self.sampler);
            let was_polled = matches!(
                self.built.get(id),
                Some(Built {
                    source: Source::Polled(_),
                    ..
                })
            );
            match &source {
                Source::Polled(connector) => self.reset_cards(id, connector),
                // A push source that used to be polled has no use for its old cards.
                Source::Push(_) if was_polled => self.store.replace(id, Snapshot::default()),
                Source::Push(_) => {}
            }
            self.built.insert(
                id.to_string(),
                Built {
                    config: config.clone(),
                    source,
                },
            );
        }
        let receivers = (self.built.iter())
            .filter_map(|(id, b)| match &b.source {
                Source::Push(receiver) => Some((id.clone(), receiver.clone())),
                Source::Polled(_) => None,
            })
            .collect();
        *self.receivers.lock().expect("receivers poisoned") = receivers;
    }

    fn reset_cards(&self, id: &str, connector: &Arc<dyn Connector>) {
        self.store.replace(
            id,
            Snapshot {
                cards: connector.pending(),
                notes: Vec::new(),
            },
        );
    }

    /// Starts the polling task of each polled source while `polling`, and stops the ones that
    /// are gone, edited, or have no device left to show them.
    async fn reconcile_polls(&mut self, polling: bool) {
        let wanted: HashMap<&String, &Arc<dyn Connector>> = (self.built.iter())
            .filter(|_| polling)
            .filter_map(|(id, b)| match &b.source {
                Source::Polled(connector) => Some((id, connector)),
                Source::Push(_) => None,
            })
            .collect();
        let was_polling = !self.running.is_empty();

        // Wait until a stopped poll is gone: a poll that is aborted but not dropped could still
        // write a card, over the one a replacement has just reset.
        let gone: Vec<String> = (self.running.iter())
            .filter(|(id, (connector, _))| {
                !wanted.get(id).is_some_and(|w| Arc::ptr_eq(w, connector))
            })
            .map(|(id, _)| id.clone())
            .collect();
        let mut stopping = Vec::new();
        for id in &gone {
            if let Some((_, task)) = self.running.remove(id) {
                stopping.push(task.stop());
            }
        }
        for stopped in stopping {
            if let Err(e) = stopped.await {
                if e.is_panic() {
                    log::error!("a feed poller panicked: {e}");
                }
            }
        }
        // An edit is the user's, and polls at once. A source that stops because no device is left
        // keeps its last poll time.
        let edited: Vec<&String> = gone.iter().filter(|id| wanted.contains_key(id)).collect();
        self.starts
            .lock()
            .expect("feed starts poisoned")
            .retain(|id, _| self.built.contains_key(id) && !edited.contains(&id));
        // Now that no old poll can write, whatever the source shows starts over as what it is
        // now: pending if still polled, empty if it became a push source, nothing if it's gone.
        for id in &gone {
            match self.built.get(id).map(|b| &b.source) {
                Some(Source::Polled(connector)) => self.reset_cards(id, connector),
                Some(Source::Push(_)) => self.store.replace(id, Snapshot::default()),
                None => {}
            }
        }

        for (id, connector) in wanted {
            if self.running.contains_key(id) {
                continue;
            }
            let task = tokio::spawn(poll(
                id.clone(),
                connector.clone(),
                self.store.clone(),
                self.starts.clone(),
            ));
            self.running
                .insert(id.clone(), (connector.clone(), Task::new(task)));
        }
        match (was_polling, !self.running.is_empty()) {
            (false, true) => log::info!("polling {} feed source(s)", self.running.len()),
            (true, false) => log::info!("no feed sources to poll, stopped"),
            _ => {}
        }
    }
}

/// Polls `connector` now, then again `every` after each poll ends, but never less than
/// [`COOLDOWN`] after the last poll began.
async fn poll(id: String, connector: Arc<dyn Connector>, store: Arc<FeedStore>, starts: Starts) {
    let every = connector.every();
    let mut skipped = 0;
    loop {
        let not_before = (starts.lock().expect("feed starts poisoned"))
            .get(&id)
            .map(|&began| began + COOLDOWN);
        if let Some(at) = not_before {
            sleep_until(at).await;
        }
        starts
            .lock()
            .expect("feed starts poisoned")
            .insert(id.clone(), Instant::now());

        match connector.poll().await {
            Ok(snapshot) => {
                skipped = 0;
                store.replace(&id, snapshot);
            }
            Err(why) => {
                skipped += 1;
                log::debug!("feed {id:?}: skipped, {why}");
                // Not a fresh reading, so an old "ok" must not stand for long.
                if skipped == STALE_AFTER {
                    log::info!("feed {id:?}: stale after {STALE_AFTER} skipped polls");
                    store.mark_stale(&id);
                }
            }
        }
        sleep(every).await;
    }
}

/// Drops each note when its time is up, so a board doesn't show one past its time until
/// something else changes.
async fn expire(store: Arc<FeedStore>) {
    loop {
        let next = store.next_expiry();
        tokio::select! {
            () = async {
                match next {
                    Some(at) => sleep_until(at).await,
                    None => pending().await,
                }
            } => store.sweep(),
            () = store.deadline_moved() => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;
    use std::sync::atomic::{AtomicUsize, Ordering};

    use tokio::time::advance;

    use super::*;
    use crate::daemon::config::{parse, TileConfig};
    use crate::daemon::icons::IconSet;
    use crate::daemon::protocol::{CardState, TileState};
    use crate::daemon::tiles::TileValue;

    fn live(text: &str) -> Arc<Live> {
        let text = format!("version = 1\n[layout]\ncols = 2\nrows = 1\n{text}");
        let secrets = BTreeMap::from([("push-ha".to_string(), 64usize)]);
        let config = parse(&text, &Ok(secrets)).unwrap();
        Arc::new(Live {
            config,
            icons: IconSet::new(),
            screen: None,
        })
    }

    fn stat(id: &str, every_s: u64) -> String {
        format!(
            "[[integration]]\nid = \"{id}\"\nkind = \"stat\"\nlabel = \"{id}\"\n\
             every_s = {every_s}\nsource = {{ kind = \"shell\", argv = [\"true\"] }}\n"
        )
    }

    const PUSH: &str = "[[integration]]\nid = \"ha\"\nkind = \"push\"\nlabel = \"Home\"\n\
                        token_secret = \"push-ha\"\n";

    /// Answers each poll with the number so far, or skips it.
    #[derive(Clone, Default)]
    struct Fake {
        calls: Arc<AtomicUsize>,
        skips: Arc<AtomicUsize>,
        state: Arc<Mutex<Option<TileState>>>,
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
            Some(TileValue {
                text: n.to_string(),
                state: self.state.lock().unwrap().unwrap_or(TileState::Ok),
            })
        }
    }

    struct Rig {
        feed: FeedHandle,
        fake: Fake,
        config: watch::Sender<Arc<Live>>,
        connected: watch::Sender<usize>,
        _integrations: Integrations,
    }

    fn rig(text: &str) -> Rig {
        let fake = Fake::default();
        let (config, config_rx) = watch::channel(live(text));
        let (connected, connected_rx) = watch::channel(0);
        let integrations = spawn(fake.clone(), config_rx, connected_rx);
        Rig {
            feed: integrations.handle(),
            fake,
            config,
            connected,
            _integrations: integrations,
        }
    }

    /// Lets the manager and poll tasks run until they are all waiting.
    async fn settle() {
        for _ in 0..20 {
            tokio::task::yield_now().await;
        }
    }

    fn values(feed: &FeedHandle) -> Vec<(String, String, CardState)> {
        (feed.snapshot().cards.iter())
            .map(|c| (c.id.clone(), c.value.clone(), c.state))
            .collect()
    }

    #[tokio::test(start_paused = true)]
    async fn nothing_polls_without_a_session_and_the_cards_wait_unknown() {
        let rig = rig(&stat("load", 10));
        settle().await;
        advance(Duration::from_secs(60)).await;
        settle().await;
        assert_eq!(rig.fake.calls.load(Ordering::SeqCst), 0);
        // The source and its slot are known at once, before any session.
        assert_eq!(rig.feed.snapshot().sources, 1);
        assert_eq!(
            values(&rig.feed),
            [("load".into(), "--".into(), CardState::Unknown)]
        );
    }

    #[tokio::test(start_paused = true)]
    async fn a_session_starts_one_poll_per_source_and_the_last_one_leaving_stops_them() {
        let rig = rig(&format!("{}{}", stat("a", 10), stat("b", 10)));
        rig.connected.send_replace(1);
        settle().await;
        assert_eq!(rig.fake.calls.load(Ordering::SeqCst), 2);
        assert_eq!(rig.feed.snapshot().cards.len(), 2);
        assert_eq!(rig.feed.snapshot().cards[0].state, CardState::Ok);

        advance(Duration::from_secs(10)).await;
        settle().await;
        assert_eq!(
            rig.fake.calls.load(Ordering::SeqCst),
            4,
            "once each per period"
        );

        rig.connected.send_replace(0);
        settle().await;
        advance(Duration::from_secs(60)).await;
        settle().await;
        assert_eq!(rig.fake.calls.load(Ordering::SeqCst), 4);
        assert_eq!(values(&rig.feed)[0].1, "--", "back to pending");
    }

    #[tokio::test(start_paused = true)]
    async fn a_failed_run_is_a_bad_card_and_three_skips_make_it_unknown() {
        let rig = rig(&stat("load", 2));
        *rig.fake.state.lock().unwrap() = Some(TileState::Bad);
        rig.connected.send_replace(1);
        settle().await;
        assert_eq!(
            values(&rig.feed),
            [("load".into(), "1".into(), CardState::Bad)]
        );

        *rig.fake.state.lock().unwrap() = None;
        rig.fake.skips.store(3, Ordering::SeqCst);
        for _ in 0..2 {
            advance(Duration::from_secs(2)).await;
            settle().await;
        }
        assert_eq!(
            values(&rig.feed)[0].2,
            CardState::Bad,
            "two skips keep the last value"
        );
        advance(Duration::from_secs(2)).await;
        settle().await;
        assert_eq!(
            values(&rig.feed),
            [("load".into(), "stale".into(), CardState::Unknown)]
        );
        // The next reading ends it.
        advance(Duration::from_secs(2)).await;
        settle().await;
        assert_eq!(values(&rig.feed)[0].2, CardState::Ok);
    }

    #[tokio::test(start_paused = true)]
    async fn an_edited_source_restarts_and_drops_its_old_card() {
        let rig = rig(&format!("{}{}", stat("a", 10), stat("b", 10)));
        rig.connected.send_replace(1);
        settle().await;
        assert_eq!(rig.fake.calls.load(Ordering::SeqCst), 2);

        // Only b changes: a keeps its task, b starts over and polls at once.
        rig.config
            .send_replace(live(&format!("{}{}", stat("a", 10), stat("b", 20))));
        settle().await;
        assert_eq!(rig.fake.calls.load(Ordering::SeqCst), 3);

        // A source that is gone takes its card with it.
        rig.config.send_replace(live(&stat("b", 20)));
        settle().await;
        let ids: Vec<String> = values(&rig.feed).into_iter().map(|v| v.0).collect();
        assert_eq!(ids, ["b"]);
        assert_eq!(rig.feed.snapshot().sources, 1);
    }

    #[tokio::test(start_paused = true)]
    async fn an_edit_between_sessions_resets_the_card_to_pending() {
        let rig = rig(&stat("load", 10));
        rig.connected.send_replace(1);
        settle().await;
        assert_eq!(values(&rig.feed)[0].1, "1");
        rig.config.send_replace(live(&stat("load", 30)));
        settle().await;
        // Stopped, reset, restarted and read again, so the reading is the second.
        assert_eq!(values(&rig.feed)[0].1, "2");
    }

    #[tokio::test(start_paused = true)]
    async fn the_in_process_api_posts_as_a_push_integration_only() {
        let rig = rig(&format!("{PUSH}{}", stat("load", 10)));
        let id = rig
            .feed
            .post(
                "ha",
                Update::PutNote(crate::daemon::feed::Note::new("Backup done")),
            )
            .unwrap();
        assert_eq!(id, Applied::Note("ha/1".into()));
        assert_eq!(rig.feed.snapshot().notes[0].src, "Home");
        // A polled source takes no posts, and neither does an unknown one.
        for source in ["load", "nope"] {
            let refused = rig
                .feed
                .post(source, Update::PutNote(crate::daemon::feed::Note::new("x")))
                .unwrap_err();
            assert_eq!(refused.status, 404, "{source}");
        }
        // Dismissed from anywhere, it leaves the one snapshot all sessions read.
        assert!(rig.feed.dismiss("ha/1"));
        assert!(rig.feed.snapshot().notes.is_empty());
    }

    #[tokio::test(start_paused = true)]
    async fn a_request_goes_through_the_receiver_into_the_store() {
        let rig = rig(PUSH);
        assert!(rig.feed.answers("ha", "notes") && rig.feed.answers("ha", "cards/x"));
        assert!(!rig.feed.answers("ha", "grafana") && !rig.feed.answers("ha", ""));
        assert!(!rig.feed.answers("nope", "notes"));
        let post = |path: &str, method: &str, body: &str| {
            rig.feed.receive(
                "ha",
                &PushRequest {
                    method: method.into(),
                    path: path.into(),
                    body: body.as_bytes().to_vec(),
                },
            )
        };
        let id = post("notes", "POST", r#"{"title":"Hi","id":"a"}"#).unwrap();
        assert_eq!(id, [Applied::Note("ha/a".into())]);
        assert_eq!(post("notes/a", "DELETE", "").unwrap(), [Applied::Done]);
        assert_eq!(post("notes/a", "DELETE", "").unwrap_err().status, 404);
        assert_eq!(post("notes", "POST", "{}").unwrap_err().status, 400);
        assert_eq!(
            rig.feed
                .receive(
                    "nope",
                    &PushRequest {
                        method: "POST".into(),
                        path: "notes".into(),
                        body: vec![]
                    }
                )
                .unwrap_err()
                .status,
            404
        );
        for i in 0..=MAX_SOURCE_CARDS {
            let r = post(
                &format!("cards/c{i}"),
                "PUT",
                r#"{"label":"L","value":"1"}"#,
            );
            assert_eq!(r.is_err(), i == MAX_SOURCE_CARDS, "card {i}");
            if let Err(e) = r {
                assert_eq!(e.status, 409);
            }
        }
    }

    #[tokio::test(start_paused = true)]
    async fn a_replaced_note_with_a_nearer_deadline_still_expires_on_time() {
        let rig = rig(PUSH);
        let put = |ttl: Duration| {
            let note = crate::daemon::feed::Note {
                id: Some("a".into()),
                ttl: Some(ttl),
                ..crate::daemon::feed::Note::new("same")
            };
            rig.feed.post("ha", Update::PutNote(note)).unwrap();
        };
        put(Duration::from_secs(24 * 3600));
        settle().await;
        // Everything a board sees is the same, so only the deadline moved.
        let mut rx = rig.feed.subscribe();
        rx.borrow_and_update();
        put(Duration::from_secs(30));
        settle().await;
        advance(Duration::from_secs(31)).await;
        settle().await;
        assert!(rig.feed.snapshot().notes.is_empty());
    }

    #[tokio::test(start_paused = true)]
    async fn a_source_that_changes_kind_loses_its_old_card() {
        let rig = rig(&stat("load", 10));
        rig.connected.send_replace(1);
        settle().await;
        assert_eq!(rig.feed.snapshot().cards.len(), 1);
        let push = PUSH.replace("\"ha\"", "\"load\"");
        rig.config.send_replace(live(&push));
        settle().await;
        let feed = rig.feed.snapshot();
        assert_eq!((feed.sources, feed.cards.len()), (1, 0));
        // And the other way, the card comes back as a placeholder.
        rig.config.send_replace(live(&stat("load", 10)));
        settle().await;
        assert_eq!(rig.feed.snapshot().cards.len(), 1);
    }

    /// Takes a feed slot for real and holds it for a second, counting how many hold one at once.
    #[derive(Clone)]
    struct Slow {
        actions: crate::daemon::actions::Actions,
        in_flight: Arc<AtomicUsize>,
        most: Arc<AtomicUsize>,
    }

    impl Sample for Slow {
        async fn sample(&self, id: &str, _: &TileConfig) -> Option<TileValue> {
            let _run = self.actions.start_feed(id).ok()?;
            let now = self.in_flight.fetch_add(1, Ordering::SeqCst) + 1;
            self.most.fetch_max(now, Ordering::SeqCst);
            sleep(Duration::from_secs(1)).await;
            self.in_flight.fetch_sub(1, Ordering::SeqCst);
            Some(TileValue {
                text: "1".into(),
                state: TileState::Ok,
            })
        }
    }

    #[tokio::test(start_paused = true)]
    async fn at_most_two_feed_polls_run_at_once() {
        let dir = std::env::temp_dir();
        let slow = Slow {
            actions: crate::daemon::actions::Actions::new(
                Arc::new(crate::daemon::secrets::Secrets::new(&dir)),
                dir,
            )
            .unwrap(),
            in_flight: Arc::default(),
            most: Arc::default(),
        };
        let text: String = ["a", "b", "c", "d", "e"]
            .iter()
            .map(|id| stat(id, 2))
            .collect();
        let (_config, config_rx) = watch::channel(live(&text));
        let (connected, connected_rx) = watch::channel(1);
        let integrations = spawn(slow.clone(), config_rx, connected_rx);
        settle().await;
        assert_eq!(slow.most.load(Ordering::SeqCst), 2);
        for _ in 0..4 {
            advance(Duration::from_secs(1)).await;
            settle().await;
        }
        assert_eq!(slow.most.load(Ordering::SeqCst), 2);
        // Some polls were refused a slot, so their cards wait; others have read.
        let states: Vec<CardState> = (integrations.handle().snapshot().cards.iter())
            .map(|c| c.state)
            .collect();
        assert!(states.contains(&CardState::Ok), "{states:?}");
        drop(connected);
    }

    #[tokio::test(start_paused = true)]
    async fn the_expiry_task_drops_a_note_when_its_time_is_up() {
        let rig = rig(PUSH);
        rig.feed
            .post(
                "ha",
                Update::PutNote(crate::daemon::feed::Note {
                    ttl: Some(Duration::from_secs(30)),
                    ..crate::daemon::feed::Note::new("short")
                }),
            )
            .unwrap();
        settle().await;
        assert_eq!(rig.feed.snapshot().notes.len(), 1);
        let mut rx = rig.feed.subscribe();
        rx.borrow_and_update();
        advance(Duration::from_secs(31)).await;
        settle().await;
        assert!(
            rx.has_changed().unwrap(),
            "sessions are woken, not left to a later change"
        );
        assert!(rig.feed.snapshot().notes.is_empty());
    }
}
