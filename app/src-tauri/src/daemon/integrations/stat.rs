//! The `stat` source: a value polled like a tile's, shown as a card.

use std::time::Duration;

use futures_util::future::BoxFuture;

use super::Connector;
use crate::daemon::actions::Actions;
use crate::daemon::config::{Action, TileConfig};
use crate::daemon::feed::{Card, Snapshot};
use crate::daemon::protocol::{CardState, TileState};
use crate::daemon::tiles::{reading, Sample, TileValue};

/// Shown before the first reading.
const PENDING: &str = "--";

/// Takes the readings for real: [`Actions`] runs the source in one of the feed's slots.
#[derive(Clone)]
pub struct FeedSlots(pub Actions);

impl Sample for FeedSlots {
    async fn sample(&self, id: &str, tile: &TileConfig) -> Option<TileValue> {
        let run = match self.0.start_feed(id) {
            Ok(run) => run,
            Err(refused) => {
                log::debug!("feed {id:?}: skipped, {}", refused.summary);
                return None;
            }
        };
        let ran = run.poll(&tile.source).await;
        Some(reading(tile, &ran))
    }
}

pub struct Stat<S> {
    sampler: S,
    id: String,
    label: String,
    /// The source as a tile, which is what [`reading`] and the sampler know.
    tile: TileConfig,
}

impl<S: Sample> Stat<S> {
    pub fn new(
        sampler: S,
        id: &str,
        label: &str,
        every_s: u64,
        json_pointer: &Option<String>,
        source: &Action,
    ) -> Self {
        Self {
            sampler,
            id: id.into(),
            label: label.into(),
            tile: TileConfig {
                every_s,
                json_pointer: json_pointer.clone(),
                font: None,
                source: source.clone(),
            },
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

impl<S: Sample> Connector for Stat<S> {
    fn every(&self) -> Duration {
        Duration::from_secs(self.tile.every_s)
    }

    fn pending(&self) -> Vec<Card> {
        vec![self.card(PENDING, CardState::Unknown)]
    }

    fn poll(&self) -> BoxFuture<'_, Result<Snapshot, String>> {
        Box::pin(async move {
            let value = (self.sampler.sample(&self.id, &self.tile).await)
                .ok_or("the poll could not start")?;
            let state = match value.state {
                TileState::Ok => CardState::Ok,
                TileState::Bad => CardState::Bad,
            };
            Ok(Snapshot {
                cards: vec![self.card(&value.text, state)],
                notes: Vec::new(),
            })
        })
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::*;
    use crate::daemon::actions::{ActionResult, Ran};
    use crate::daemon::secrets::Secrets;

    /// Reads a finished run the way a tile does, whatever `ran` holds.
    #[derive(Clone)]
    struct Canned(Arc<Option<Ran>>);

    impl Sample for Canned {
        async fn sample(&self, _: &str, tile: &TileConfig) -> Option<TileValue> {
            (*self.0).as_ref().map(|ran| reading(tile, ran))
        }
    }

    fn ran(ok: bool, raw: &str, secret: Option<&str>) -> Canned {
        let secrets = match secret {
            Some(value) => {
                let dir = tempfile::tempdir().unwrap();
                let secrets = Secrets::new(dir.path());
                secrets.set("s", value).unwrap();
                vec![secrets.load().unwrap()["s"].clone()]
            }
            None => Vec::new(),
        };
        let result = ActionResult {
            ok,
            code: i32::from(!ok),
            summary: if ok { "exit 0" } else { "exit 1" }.into(),
            text: raw.into(),
            ms: 1,
        };
        Canned(Arc::new(Some(Ran::new(result, raw, secrets, false))))
    }

    fn shell() -> Action {
        Action::Shell {
            argv: vec!["true".into()],
            timeout_s: 10,
        }
    }

    async fn card(sampler: Canned, pointer: Option<&str>) -> Result<Card, String> {
        let stat = Stat::new(
            sampler,
            "load",
            "Load",
            10,
            &pointer.map(Into::into),
            &shell(),
        );
        let snapshot = stat.poll().await?;
        assert!(snapshot.notes.is_empty());
        assert_eq!(snapshot.cards.len(), 1);
        Ok(snapshot.cards.into_iter().next().unwrap())
    }

    #[tokio::test]
    async fn a_value_and_a_json_pointer_become_the_card() {
        let plain = card(ran(true, "0.42\nmore", None), None).await.unwrap();
        assert_eq!(
            plain,
            Card {
                id: String::new(),
                label: "Load".into(),
                value: "0.42".into(),
                state: CardState::Ok,
            }
        );
        let picked = card(ran(true, r#"{"a":{"b":7}}"#, None), Some("/a/b")).await;
        assert_eq!(picked.unwrap().value, "7");
        let missing = card(ran(true, r#"{"a":1}"#, None), Some("/x"))
            .await
            .unwrap();
        assert_eq!(missing.state, CardState::Bad);
    }

    #[tokio::test]
    async fn a_failed_run_is_a_bad_card() {
        let bad = card(ran(false, "boom\nsecond", None), None).await.unwrap();
        assert_eq!((bad.value.as_str(), bad.state), ("boom", CardState::Bad));
    }

    #[tokio::test]
    async fn a_secret_in_the_output_never_reaches_the_card() {
        let said = card(
            ran(true, "key=hunter2hunter2", Some("hunter2hunter2")),
            None,
        )
        .await
        .unwrap();
        assert!(!said.value.contains("hunter2"), "{said:?}");
        let failed = card(
            ran(false, "hunter2hunter2 refused", Some("hunter2hunter2")),
            None,
        )
        .await
        .unwrap();
        assert!(!failed.value.contains("hunter2"), "{failed:?}");
    }

    #[tokio::test]
    async fn a_poll_that_could_not_start_is_an_error_not_a_card() {
        let none = Canned(Arc::new(None));
        assert!(card(none, None).await.is_err());
    }
}
