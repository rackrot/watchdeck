//! The `grafana` source: Grafana's webhook contact point POSTs its alerts to `/v1/grafana`. A
//! firing alert becomes a note and a resolved one removes it, both keyed by the alert's
//! fingerprint, so the repeat notifications Grafana sends while an alert fires replace one note.
//!
//! The payload is read as Grafana documents it,
//! <https://grafana.com/docs/grafana/latest/alerting/configure-notifications/manage-contact-points/integrations/webhook-notifier/>.
//! Only `alerts` is required. Every other field, at the top and in an alert, is ignored if
//! unknown, since Grafana adds fields between versions.

use std::collections::{BTreeMap, HashMap};

use serde::Deserialize;
use sha2::{Digest, Sha256};

use super::{PushError, PushRequest, Receiver};
use crate::daemon::feed::{Note, Update};
use crate::daemon::protocol::{encode_hex, NoteLevel};

/// A payload holds every alert of a group, about 1 KB each with its labels, annotations, URLs and
/// values, so 8 KB is a handful. 64 KB is a few dozen: past it Grafana's own "Max Alerts"
/// setting is the tool, and this is still a hard bound.
const MAX_BODY_BYTES: usize = 64 * 1024;
/// A note id is at most this many characters, so `<integration>/<id>` fits the wire's 63.
const MAX_ID: usize = 31;
const UNNAMED: &str = "(alert)";

pub struct Grafana {
    /// A severity label (lower case) -> the note level.
    levels: HashMap<String, NoteLevel>,
}

impl Grafana {
    /// `levels` are the config's, over the built-in `critical` and `warning`.
    pub fn new(levels: &BTreeMap<String, NoteLevel>) -> Self {
        let mut all = HashMap::from([
            ("critical".to_string(), NoteLevel::Error),
            ("warning".to_string(), NoteLevel::Warn),
        ]);
        all.extend(levels.iter().map(|(k, v)| (k.to_ascii_lowercase(), *v)));
        Self { levels: all }
    }

    fn level(&self, alert: &Alert) -> NoteLevel {
        alert
            .labels
            .get("severity")
            .and_then(|s| self.levels.get(&s.to_ascii_lowercase()))
            .copied()
            .unwrap_or(NoteLevel::Info)
    }
}

#[derive(Deserialize)]
struct Payload {
    alerts: Vec<Alert>,
}

#[derive(Deserialize)]
struct Alert {
    status: String,
    #[serde(default)]
    labels: HashMap<String, String>,
    #[serde(default)]
    annotations: HashMap<String, String>,
    #[serde(default)]
    fingerprint: String,
}

impl Alert {
    fn label(&self, name: &str) -> &str {
        self.labels.get(name).map_or("", |v| v.trim())
    }

    fn annotation(&self, name: &str) -> &str {
        self.annotations.get(name).map_or("", |v| v.trim())
    }

    /// The fingerprint when it is a plain id, else a hash of it and the alert's name, so any
    /// payload gives an id the wire accepts and the same alert gives the same one.
    fn id(&self) -> String {
        let plain = !self.fingerprint.is_empty()
            && self.fingerprint.len() <= MAX_ID
            && (self.fingerprint.bytes())
                .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'_' | b'-' | b'.'));
        if plain {
            return self.fingerprint.clone();
        }
        let mut hash = Sha256::new();
        hash.update(&self.fingerprint);
        hash.update([0]);
        hash.update(self.label("alertname"));
        encode_hex(&hash.finalize()[..8])
    }

    fn title(&self) -> String {
        match (self.label("alertname"), self.annotation("summary")) {
            ("", "") => UNNAMED.into(),
            (name, "") => name.into(),
            ("", summary) => summary.into(),
            (name, summary) => format!("{name}: {summary}"),
        }
    }
}

impl Receiver for Grafana {
    fn routes(&self) -> &'static [&'static str] {
        &["grafana"]
    }

    fn max_body_bytes(&self) -> usize {
        MAX_BODY_BYTES
    }

    fn accept(&self, req: &PushRequest) -> Result<Vec<Update>, PushError> {
        // Grafana's contact point sends POST or PUT, as set in its form.
        if req.path != "grafana" || !matches!(req.method.as_str(), "POST" | "PUT") {
            return Err(PushError::new(404, "no such route"));
        }
        let payload: Payload = serde_json::from_slice(&req.body)
            .map_err(|e| PushError::new(400, format!("bad body: {e}")))?;
        payload
            .alerts
            .iter()
            .enumerate()
            .map(|(i, alert)| match alert.status.as_str() {
                "firing" => Ok(Update::PutNote(Note {
                    id: Some(alert.id()),
                    title: alert.title(),
                    body: alert.annotation("description").into(),
                    level: self.level(alert),
                    ttl: None,
                })),
                "resolved" => Ok(Update::ClearNote(alert.id())),
                other => Err(PushError::new(
                    400,
                    format!("alert {i} has status {other:?}, not firing or resolved"),
                )),
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn req(body: &str) -> PushRequest {
        PushRequest {
            method: "POST".into(),
            path: "grafana".into(),
            body: body.as_bytes().to_vec(),
        }
    }

    // Grafana's webhook contact point payload, with the example values and field set of its
    // documentation (two alerts, `firing` then `resolved`):
    // <https://grafana.com/docs/grafana/latest/alerting/configure-notifications/manage-contact-points/integrations/webhook-notifier/>
    const FIRING: &str = include_str!("../../../tests/fixtures/grafana_firing.json");
    const RESOLVED: &str = include_str!("../../../tests/fixtures/grafana_resolved.json");

    #[test]
    fn a_firing_payload_adds_notes_with_the_mapped_levels_and_a_resolved_one_clears_them() {
        let grafana = Grafana::new(&BTreeMap::new());
        let firing = grafana.accept(&req(FIRING)).unwrap();
        assert_eq!(
            firing,
            [
                Update::PutNote(Note {
                    id: Some("c6eadffa33fcdf37".into()),
                    title: "High memory usage: This alert was triggered for zone us-1".into(),
                    body: "The system has high memory usage".into(),
                    level: NoteLevel::Error,
                    ttl: None,
                }),
                Update::PutNote(Note {
                    id: Some("bc4e0e8cc3a6f2a1".into()),
                    title: "High CPU usage: This alert was triggered for zone eu-1".into(),
                    body: "The system has high CPU usage".into(),
                    level: NoteLevel::Warn,
                    ttl: None,
                }),
            ]
        );
        assert_eq!(
            grafana.accept(&req(RESOLVED)).unwrap(),
            [
                Update::ClearNote("c6eadffa33fcdf37".into()),
                Update::ClearNote("bc4e0e8cc3a6f2a1".into()),
            ]
        );
    }

    #[test]
    fn unknown_fields_anywhere_still_parse_but_a_missing_alerts_does_not() {
        let grafana = Grafana::new(&BTreeMap::new());
        let body = r#"{"future":{"a":1},"alerts":[{"status":"firing","fingerprint":"f","newField":[1],"labels":{"alertname":"X"}}],"another":true}"#;
        assert_eq!(grafana.accept(&req(body)).unwrap().len(), 1);
        let missing = grafana
            .accept(&req(r#"{"status":"firing","receiver":"x"}"#))
            .unwrap_err();
        assert_eq!(missing.status, 400);
        assert!(missing.reason.contains("alerts"), "{}", missing.reason);
    }

    fn put(update: &Update) -> &Note {
        match update {
            Update::PutNote(note) => note,
            other => panic!("not a note: {other:?}"),
        }
    }

    #[test]
    fn an_alert_without_a_plain_fingerprint_gets_a_stable_id_the_wire_accepts() {
        let grafana = Grafana::new(&BTreeMap::new());
        let body =
            r#"{"alerts":[{"status":"firing","fingerprint":"a b/ü","labels":{"alertname":"X"}}]}"#;
        let first = grafana.accept(&req(body)).unwrap();
        let second = grafana.accept(&req(body)).unwrap();
        let id = put(&first[0]).id.clone().unwrap();
        assert_eq!(first, second);
        assert_eq!(id.len(), 16);
        assert!(id.bytes().all(|c| c.is_ascii_hexdigit()), "{id}");
    }

    #[test]
    fn a_title_joins_the_name_and_summary_and_survives_missing_parts() {
        let grafana = Grafana::new(&BTreeMap::new());
        let title = |alert: &str| {
            let body = format!(r#"{{"alerts":[{{"status":"firing","fingerprint":"f",{alert}}}]}}"#);
            put(&grafana.accept(&req(&body)).unwrap()[0]).title.clone()
        };
        assert_eq!(
            title(r#""labels":{"alertname":"Disk"},"annotations":{"summary":"90% full"}"#),
            "Disk: 90% full"
        );
        assert_eq!(title(r#""labels":{"alertname":"Disk"}"#), "Disk");
        assert_eq!(
            title(r#""annotations":{"summary":"Only this"}"#),
            "Only this"
        );
        assert_eq!(title(r#""labels":{}"#), UNNAMED);
    }

    #[test]
    fn severities_map_to_levels_and_the_config_overrides_them() {
        let custom = BTreeMap::from([
            ("page".to_string(), NoteLevel::Error),
            ("Warning".to_string(), NoteLevel::Info),
        ]);
        let grafana = Grafana::new(&custom);
        let level = |severity: &str| {
            let body = format!(
                r#"{{"alerts":[{{"status":"firing","fingerprint":"f","labels":{{"severity":"{severity}"}}}}]}}"#
            );
            put(&grafana.accept(&req(&body)).unwrap()[0]).level
        };
        assert_eq!(level("critical"), NoteLevel::Error);
        assert_eq!(level("CRITICAL"), NoteLevel::Error);
        assert_eq!(level("page"), NoteLevel::Error);
        assert_eq!(level("warning"), NoteLevel::Info, "the config wins");
        assert_eq!(level("minor"), NoteLevel::Info);
        let bare = r#"{"alerts":[{"status":"firing","fingerprint":"f"}]}"#;
        assert_eq!(
            put(&grafana.accept(&req(bare)).unwrap()[0]).level,
            NoteLevel::Info
        );
    }

    #[test]
    fn a_bad_status_or_method_or_body_is_refused() {
        let grafana = Grafana::new(&BTreeMap::new());
        let status = |s: &str| {
            grafana
                .accept(&req(&format!(r#"{{"alerts":[{{"status":"{s}"}}]}}"#)))
                .unwrap_err()
        };
        assert_eq!(status("pending").status, 400);
        assert_eq!(grafana.accept(&req("{}")).unwrap_err().status, 400);
        assert_eq!(grafana.accept(&req("[1]")).unwrap_err().status, 400);
        let get = PushRequest {
            method: "GET".into(),
            ..req("{}")
        };
        assert_eq!(grafana.accept(&get).unwrap_err().status, 404);
        let elsewhere = PushRequest {
            path: "notes".into(),
            ..req("{}")
        };
        assert_eq!(grafana.accept(&elsewhere).unwrap_err().status, 404);
        // Empty is fine: nothing to do.
        assert!(grafana.accept(&req(r#"{"alerts":[]}"#)).unwrap().is_empty());
    }
}
