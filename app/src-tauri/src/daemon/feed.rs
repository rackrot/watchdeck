//! The feed store: the cards and notes that the integrations show on the -1 page, and the
//! snapshot every session sends from them. It lives in memory, so a daemon restart starts empty
//! and a polled source fills its cards again within a period.
//!
//! Every string that comes in is made printable ASCII and cut to the wire's limits here, not
//! refused, so one source's emoji costs a `?` and never the note.

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use tokio::sync::watch;
use tokio::time::Instant;

use super::protocol::{
    CardState, Feed, FeedCard, FeedNote, NoteLevel, MAX_FEED_BODY_CHARS, MAX_FEED_CARDS,
    MAX_FEED_ID_CHARS, MAX_FEED_LABEL_CHARS, MAX_FEED_NOTES, MAX_FEED_SOURCES, MAX_FEED_SRC_CHARS,
    MAX_FEED_TITLE_CHARS, MAX_FEED_VALUE_CHARS,
};

/// Notes the store keeps, newest first. The wire carries the newest `MAX_FEED_NOTES` of them.
pub const MAX_STORED_NOTES: usize = 32;
/// How long a note stays when its source doesn't say.
pub const DEFAULT_TTL: Duration = Duration::from_secs(24 * 60 * 60);
/// The longest a note may stay.
pub const MAX_TTL: Duration = Duration::from_secs(7 * 24 * 60 * 60);
const MIN_TTL: Duration = Duration::from_secs(1);
const NO_TITLE: &str = "(no title)";

/// A configured integration, for the cards' and notes' source name.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceInfo {
    pub id: String,
    pub label: String,
}

/// A stat card. Its wire id is the source's id, followed by `/` and `id` when `id` isn't empty:
/// a polled source with one card leaves it empty.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Card {
    pub id: String,
    pub label: String,
    pub value: String,
    pub state: CardState,
}

/// A notification. Its wire id is the source's id, `/`, and `id`; without an `id` the store
/// numbers it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Note {
    pub id: Option<String>,
    pub title: String,
    pub body: String,
    pub level: NoteLevel,
    /// How long it stays. `None` is [`DEFAULT_TTL`]; anything is held within 1 s and [`MAX_TTL`].
    pub ttl: Option<Duration>,
}

impl Note {
    pub fn new(title: impl Into<String>) -> Self {
        Self {
            id: None,
            title: title.into(),
            body: String::new(),
            level: NoteLevel::Info,
            ttl: None,
        }
    }
}

/// What a polled source shows: all its cards and notes, which replace what it showed before.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Snapshot {
    pub cards: Vec<Card>,
    pub notes: Vec<Note>,
}

/// One change a source asks for. A push receiver turns a request into these.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Update {
    /// Adds a note, or replaces the one with the same id.
    PutNote(Note),
    /// By the source's own id for it.
    RemoveNote(String),
    /// Adds a card, or replaces the one with the same id.
    PutCard(Card),
    /// By the source's own id for it.
    RemoveCard(String),
}

/// What an applied [`Update`] produced.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Applied {
    /// The wire id of the note that was added or replaced.
    Note(String),
    Done,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StoreError {
    /// The source isn't a configured integration.
    UnknownSource,
    /// There is no such note or card.
    NotFound,
    /// All card slots are taken.
    CardsFull,
}

struct StoredNote {
    source: String,
    /// The wire id, `<source>/<id>`.
    id: String,
    title: String,
    body: String,
    level: NoteLevel,
    at: u64,
    expires: Instant,
    /// Came from a polled source's snapshot, which can list it again after a dismiss.
    polled: bool,
}

#[derive(Default)]
struct Inner {
    /// In config order, which is also the order of the cards.
    sources: Vec<SourceInfo>,
    /// By source id, with the card ids the source gave.
    cards: HashMap<String, Vec<Card>>,
    /// Newest first.
    notes: Vec<StoredNote>,
    /// Wire ids of polled notes dismissed while their source still lists them.
    dismissed: HashSet<String>,
    numbered: u64,
}

pub struct FeedStore {
    inner: Mutex<Inner>,
    tx: watch::Sender<Arc<Feed>>,
}

impl FeedStore {
    pub fn new() -> Arc<Self> {
        let empty = Feed {
            sources: 0,
            cards: Vec::new(),
            notes: Vec::new(),
        };
        Arc::new(Self {
            inner: Mutex::new(Inner::default()),
            tx: watch::Sender::new(Arc::new(empty)),
        })
    }

    /// The snapshot every session sends, replaced on each change.
    pub fn subscribe(&self) -> watch::Receiver<Arc<Feed>> {
        self.tx.subscribe()
    }

    pub fn snapshot(&self) -> Arc<Feed> {
        self.tx.borrow().clone()
    }

    fn lock(&self) -> MutexGuard<'_, Inner> {
        self.inner.lock().expect("feed store poisoned")
    }

    /// Sets the configured integrations. What belongs to one that is gone goes with it.
    pub fn set_sources(&self, sources: Vec<SourceInfo>) {
        let mut inner = self.lock();
        inner
            .cards
            .retain(|id, _| sources.iter().any(|s| &s.id == id));
        inner
            .notes
            .retain(|n| sources.iter().any(|s| s.id == n.source));
        inner.sources = sources;
        self.publish(&inner);
    }

    /// Replaces `source`'s cards and polled notes with `snapshot`. A note the user dismissed stays
    /// gone for as long as the source lists it.
    pub fn replace(&self, source: &str, snapshot: Snapshot) {
        let mut inner = self.lock();
        if !inner.sources.iter().any(|s| s.id == source) {
            return;
        }
        inner.cards.insert(source.to_string(), snapshot.cards);
        let listed: Vec<String> = (snapshot.notes.iter())
            .filter_map(|n| n.id.as_deref())
            .map(|id| qualify(source, id))
            .collect();
        inner
            .notes
            .retain(|n| n.source != source || !n.polled || listed.contains(&n.id));
        inner.dismissed.retain(|id| listed.contains(id));
        for note in snapshot.notes {
            let Some(local) = note.id.clone() else {
                continue;
            };
            if inner.dismissed.contains(&qualify(source, &local)) {
                continue;
            }
            inner.put_note(source, note, true);
        }
        self.publish(&inner);
    }

    /// Marks every card of `source` as not fresh: its poll couldn't run for a while.
    pub fn mark_stale(&self, source: &str) {
        let mut inner = self.lock();
        for card in inner.cards.entry(source.to_string()).or_default() {
            card.value = "stale".into();
            card.state = CardState::Unknown;
        }
        self.publish(&inner);
    }

    /// Applies `update` as `source`.
    pub fn apply(&self, source: &str, update: Update) -> Result<Applied, StoreError> {
        let mut inner = self.lock();
        if !inner.sources.iter().any(|s| s.id == source) {
            return Err(StoreError::UnknownSource);
        }
        let applied = match update {
            Update::PutNote(note) => Applied::Note(inner.put_note(source, note, false)),
            Update::RemoveNote(local) => {
                let id = qualify(source, &local);
                let before = inner.notes.len();
                inner.notes.retain(|n| n.id != id);
                if inner.notes.len() == before {
                    return Err(StoreError::NotFound);
                }
                Applied::Done
            }
            Update::PutCard(card) => {
                let taken: usize = inner.cards.values().map(Vec::len).sum();
                let cards = inner.cards.entry(source.to_string()).or_default();
                match cards.iter_mut().find(|c| c.id == card.id) {
                    Some(old) => *old = card,
                    None if taken >= MAX_FEED_CARDS => return Err(StoreError::CardsFull),
                    None => cards.push(card),
                }
                Applied::Done
            }
            Update::RemoveCard(local) => {
                let cards = inner.cards.entry(source.to_string()).or_default();
                let before = cards.len();
                cards.retain(|c| c.id != local);
                if cards.len() == before {
                    return Err(StoreError::NotFound);
                }
                Applied::Done
            }
        };
        self.publish(&inner);
        Ok(applied)
    }

    /// Removes the note with wire id `id` for every session. An unknown id is ignored. Returns
    /// whether a note went.
    pub fn dismiss(&self, id: &str) -> bool {
        let mut inner = self.lock();
        let Some(at) = inner.notes.iter().position(|n| n.id == id) else {
            return false;
        };
        if inner.notes.remove(at).polled {
            inner.dismissed.insert(id.to_string());
        }
        self.publish(&inner);
        true
    }

    /// Drops the notes past their time. [`FeedStore::next_expiry`] says when to call it.
    pub fn sweep(&self) {
        let mut inner = self.lock();
        let now = Instant::now();
        let before = inner.notes.len();
        inner.notes.retain(|n| n.expires > now);
        if inner.notes.len() != before {
            self.publish(&inner);
        }
    }

    /// When the earliest note expires.
    pub fn next_expiry(&self) -> Option<Instant> {
        self.lock().notes.iter().map(|n| n.expires).min()
    }

    fn publish(&self, inner: &Inner) {
        let next = inner.wire(Instant::now());
        if let Err(why) = next.check() {
            log::error!("feed: not publishing a snapshot that breaks the wire rules: {why}");
            return;
        }
        self.tx.send_if_modified(|current| {
            if **current == next {
                return false;
            }
            *current = Arc::new(next);
            true
        });
    }
}

impl Inner {
    /// Adds `note` as `source`'s, or replaces its namesake, newest first. Returns the wire id.
    fn put_note(&mut self, source: &str, note: Note, polled: bool) -> String {
        let local = match note.id {
            Some(id) if !id.is_empty() => id,
            _ => {
                self.numbered += 1;
                self.numbered.to_string()
            }
        };
        let id = qualify(source, &local);
        self.notes.retain(|n| n.id != id);
        let ttl = note.ttl.unwrap_or(DEFAULT_TTL).clamp(MIN_TTL, MAX_TTL);
        let title = clean(&note.title, MAX_FEED_TITLE_CHARS);
        self.notes.insert(
            0,
            StoredNote {
                source: source.to_string(),
                id: id.clone(),
                title: if title.trim().is_empty() {
                    NO_TITLE.into()
                } else {
                    title
                },
                body: clean(&note.body, MAX_FEED_BODY_CHARS),
                level: note.level,
                at: unix_now(),
                expires: Instant::now() + ttl,
                polled,
            },
        );
        self.notes.truncate(MAX_STORED_NOTES);
        id
    }

    /// The snapshot at `now`: cards in config order, notes newest first, all within the wire's
    /// limits.
    fn wire(&self, now: Instant) -> Feed {
        let label = |source: &str| {
            let found = self.sources.iter().find(|s| s.id == source);
            clean(found.map_or("", |s| &s.label), MAX_FEED_SRC_CHARS)
        };
        let cards = (self.sources.iter())
            .flat_map(|s| {
                (self.cards.get(&s.id).into_iter().flatten()).map(|c| FeedCard {
                    id: qualify(&s.id, &c.id),
                    label: clean(&c.label, MAX_FEED_LABEL_CHARS),
                    value: clean(&c.value, MAX_FEED_VALUE_CHARS),
                    state: c.state,
                })
            })
            .take(MAX_FEED_CARDS)
            .collect();
        let notes = (self.notes.iter())
            .filter(|n| n.expires > now)
            .take(MAX_FEED_NOTES)
            .map(|n| FeedNote {
                id: n.id.clone(),
                title: n.title.clone(),
                body: n.body.clone(),
                level: n.level,
                src: label(&n.source),
                at: n.at,
            })
            .collect();
        Feed {
            sources: u32::try_from(self.sources.len())
                .unwrap_or(u32::MAX)
                .min(MAX_FEED_SOURCES),
            cards,
            notes,
        }
    }
}

/// `<source>/<local>` cut to the wire's id limit, or just `source` when `local` is empty.
fn qualify(source: &str, local: &str) -> String {
    let local = clean_id(local);
    if local.is_empty() {
        return clean_id(source);
    }
    clean_id(&format!("{source}/{local}"))
}

fn clean_id(id: &str) -> String {
    clean(id, MAX_FEED_ID_CHARS)
}

/// `text` as one line of printable ASCII, at most `max` characters: whitespace becomes a space and
/// anything else the device can't draw becomes `?`.
fn clean(text: &str, max: usize) -> String {
    text.chars()
        .map(|c| match c {
            ' '..='~' => c,
            '\n' | '\r' | '\t' => ' ',
            _ => '?',
        })
        .take(max)
        .collect()
}

fn unix_now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_secs())
}

#[cfg(test)]
mod tests {
    use tokio::time::advance;

    use super::*;

    fn store(ids: &[&str]) -> Arc<FeedStore> {
        let store = FeedStore::new();
        store.set_sources(
            ids.iter()
                .map(|id| SourceInfo {
                    id: id.to_string(),
                    label: id.to_uppercase(),
                })
                .collect(),
        );
        store
    }

    fn note(id: &str, title: &str) -> Note {
        Note {
            id: Some(id.into()),
            ..Note::new(title)
        }
    }

    fn card(id: &str, value: &str) -> Card {
        Card {
            id: id.into(),
            label: "Label".into(),
            value: value.into(),
            state: CardState::Ok,
        }
    }

    fn post(store: &FeedStore, source: &str, note: Note) -> String {
        match store.apply(source, Update::PutNote(note)).unwrap() {
            Applied::Note(id) => id,
            Applied::Done => panic!("a note has an id"),
        }
    }

    fn titles(store: &FeedStore) -> Vec<String> {
        store
            .snapshot()
            .notes
            .iter()
            .map(|n| n.title.clone())
            .collect()
    }

    #[test]
    fn thirty_three_notes_keep_the_newest_32_and_the_wire_gets_the_newest_12() {
        let store = store(&["ha"]);
        for i in 0..33 {
            post(&store, "ha", note(&format!("n{i}"), &format!("note {i}")));
        }
        assert_eq!(store.lock().notes.len(), MAX_STORED_NOTES);
        let wire = titles(&store);
        assert_eq!(wire.len(), MAX_FEED_NOTES);
        assert_eq!(wire[0], "note 32");
        assert_eq!(wire[11], "note 21");
        // The oldest one, note 0, fell off the store: its id is unknown.
        assert!(!store.dismiss("ha/n0"));
        assert!(store.lock().notes.iter().all(|n| n.id != "ha/n0"));
    }

    #[test]
    fn replacing_a_note_by_id_keeps_one_and_makes_it_the_newest() {
        let store = store(&["ha"]);
        post(&store, "ha", note("a", "first"));
        post(&store, "ha", note("b", "second"));
        let id = post(&store, "ha", note("a", "first again"));
        assert_eq!(id, "ha/a");
        assert_eq!(titles(&store), ["first again", "second"]);
    }

    #[test]
    fn a_note_without_an_id_is_numbered() {
        let store = store(&["ha"]);
        assert_eq!(post(&store, "ha", Note::new("x")), "ha/1");
        assert_eq!(post(&store, "ha", Note::new("y")), "ha/2");
    }

    #[test]
    fn a_dismiss_removes_the_note_and_an_unknown_id_is_ignored() {
        let store = store(&["ha"]);
        post(&store, "ha", note("a", "first"));
        assert!(store.dismiss("ha/a"));
        assert!(titles(&store).is_empty());
        assert!(!store.dismiss("ha/a"));
        assert!(!store.dismiss("nope"));
    }

    #[test]
    fn a_dismissed_polled_note_stays_gone_while_its_source_lists_it() {
        let store = store(&["g"]);
        let listing = || Snapshot {
            cards: vec![],
            notes: vec![note("alert", "Disk full")],
        };
        store.replace("g", listing());
        assert!(store.dismiss("g/alert"));
        store.replace("g", listing());
        assert!(
            titles(&store).is_empty(),
            "the next poll must not bring it back"
        );
        store.replace("g", Snapshot::default());
        store.replace("g", listing());
        assert_eq!(
            titles(&store),
            ["Disk full"],
            "listed again after it went away"
        );
    }

    #[tokio::test(start_paused = true)]
    async fn a_note_past_its_ttl_leaves_at_the_next_sweep() {
        let store = store(&["ha"]);
        let short = Note {
            ttl: Some(Duration::from_secs(60)),
            ..note("short", "short")
        };
        post(&store, "ha", short);
        post(&store, "ha", note("long", "long"));
        let at = store.next_expiry().unwrap();
        assert_eq!(at, Instant::now() + Duration::from_secs(60));
        advance(Duration::from_secs(61)).await;
        // The wire already leaves it out, and the sweep drops it for good.
        store.sweep();
        assert_eq!(titles(&store), ["long"]);
        assert_eq!(store.lock().notes.len(), 1);
        advance(DEFAULT_TTL).await;
        store.sweep();
        assert!(titles(&store).is_empty());
        assert_eq!(store.next_expiry(), None);
    }

    #[tokio::test(start_paused = true)]
    async fn a_ttl_is_held_within_one_second_and_seven_days() {
        let store = store(&["ha"]);
        let with = |ttl| Note {
            ttl: Some(ttl),
            ..Note::new("t")
        };
        post(&store, "ha", with(Duration::ZERO));
        assert_eq!(store.next_expiry().unwrap(), Instant::now() + MIN_TTL);
        store.dismiss("ha/1");
        post(&store, "ha", with(Duration::from_secs(365 * 24 * 3600)));
        assert_eq!(store.next_expiry().unwrap(), Instant::now() + MAX_TTL);
    }

    #[test]
    fn strings_are_made_printable_and_cut_to_the_wire_limits() {
        let store = store(&["ha"]);
        let long = "\u{1f600}é\n\t".repeat(100);
        let id = post(
            &store,
            "ha",
            Note {
                id: Some(long.clone()),
                title: long.clone(),
                body: long.clone(),
                ..Note::new("")
            },
        );
        let feed = store.snapshot();
        let n = &feed.notes[0];
        assert_eq!(n.id, id);
        assert_eq!(n.id.len(), MAX_FEED_ID_CHARS);
        assert_eq!(n.title.len(), MAX_FEED_TITLE_CHARS);
        assert_eq!(n.body.len(), MAX_FEED_BODY_CHARS);
        assert!(n.title.starts_with("??  "), "{:?}", n.title);
        assert_eq!(feed.check(), Ok(()));
    }

    #[test]
    fn an_empty_title_gets_a_stand_in() {
        let store = store(&["ha"]);
        post(&store, "ha", Note::new("   "));
        assert_eq!(titles(&store), [NO_TITLE]);
    }

    #[test]
    fn cards_follow_config_order_and_stop_at_eight() {
        let store = store(&["b", "a"]);
        for i in 0..5 {
            store
                .apply("a", Update::PutCard(card(&format!("a{i}"), "1")))
                .unwrap();
        }
        for i in 0..3 {
            store
                .apply("b", Update::PutCard(card(&format!("b{i}"), "2")))
                .unwrap();
        }
        let ids: Vec<String> = store
            .snapshot()
            .cards
            .iter()
            .map(|c| c.id.clone())
            .collect();
        assert_eq!(ids[..3], ["b/b0", "b/b1", "b/b2"]);
        assert_eq!(ids.len(), 8);
        // All eight slots are taken: a new card is refused, an existing one still updates.
        assert_eq!(
            store.apply("b", Update::PutCard(card("b9", "x"))),
            Err(StoreError::CardsFull)
        );
        store
            .apply("b", Update::PutCard(card("b0", "new")))
            .unwrap();
        assert_eq!(store.snapshot().cards[0].value, "new");
        store.apply("a", Update::RemoveCard("a0".into())).unwrap();
        assert_eq!(
            store.apply("a", Update::RemoveCard("a0".into())),
            Err(StoreError::NotFound)
        );
    }

    #[test]
    fn a_source_with_an_empty_card_id_is_named_by_the_source_alone() {
        let store = store(&["load"]);
        store.replace(
            "load",
            Snapshot {
                cards: vec![card("", "0.42")],
                notes: vec![],
            },
        );
        assert_eq!(store.snapshot().cards[0].id, "load");
        assert_eq!(store.snapshot().cards[0].label, "Label");
    }

    #[test]
    fn a_stale_source_shows_unknown_cards() {
        let store = store(&["load"]);
        store.replace(
            "load",
            Snapshot {
                cards: vec![card("", "7")],
                notes: vec![],
            },
        );
        store.mark_stale("load");
        let c = &store.snapshot().cards[0];
        assert_eq!((c.value.as_str(), c.state), ("stale", CardState::Unknown));
    }

    #[test]
    fn removing_a_source_removes_its_cards_and_notes() {
        let store = store(&["a", "b"]);
        post(&store, "a", note("x", "from a"));
        post(&store, "b", note("x", "from b"));
        store.apply("a", Update::PutCard(card("c", "1"))).unwrap();
        store.set_sources(vec![SourceInfo {
            id: "b".into(),
            label: "B".into(),
        }]);
        let feed = store.snapshot();
        assert_eq!(feed.sources, 1);
        assert!(feed.cards.is_empty());
        assert_eq!(titles(&store), ["from b"]);
        assert_eq!(
            store.apply("a", Update::PutNote(Note::new("x"))),
            Err(StoreError::UnknownSource)
        );
    }

    #[test]
    fn the_source_name_follows_the_label_in_the_config() {
        let store = store(&["ha"]);
        post(&store, "ha", Note::new("x"));
        assert_eq!(store.snapshot().notes[0].src, "HA");
        store.set_sources(vec![SourceInfo {
            id: "ha".into(),
            label: "Home".into(),
        }]);
        assert_eq!(store.snapshot().notes[0].src, "Home");
    }

    #[test]
    fn the_fullest_snapshot_is_under_16_kb() {
        let store = store(&["a", "b", "c", "d", "e", "f", "g", "h"]);
        let quotes = |n| "\"".repeat(n);
        for id in ["a", "b", "c", "d", "e", "f", "g", "h"] {
            store
                .apply(
                    id,
                    Update::PutCard(Card {
                        id: quotes(60),
                        label: quotes(40),
                        value: quotes(40),
                        state: CardState::Bad,
                    }),
                )
                .unwrap();
        }
        for i in 0..40 {
            post(
                &store,
                "h",
                Note {
                    id: Some(format!("{i}{}", quotes(70))),
                    title: quotes(90),
                    body: quotes(300),
                    level: NoteLevel::Error,
                    ttl: None,
                },
            );
        }
        let feed = store.snapshot();
        assert_eq!((feed.cards.len(), feed.notes.len()), (8, 12));
        let wire = serde_json::to_string(&crate::daemon::protocol::Message::Feed((*feed).clone()))
            .unwrap();
        println!("fullest feed frame: {} bytes", wire.len());
        assert!(wire.len() < 16 * 1024, "{} bytes", wire.len());
        assert_eq!(feed.check(), Ok(()));
    }

    #[test]
    fn an_unchanged_snapshot_does_not_wake_the_sessions() {
        let store = store(&["ha"]);
        let mut rx = store.subscribe();
        rx.borrow_and_update();
        store.set_sources(vec![SourceInfo {
            id: "ha".into(),
            label: "HA".into(),
        }]);
        assert!(!rx.has_changed().unwrap());
        post(&store, "ha", Note::new("x"));
        assert!(rx.has_changed().unwrap());
    }
}
