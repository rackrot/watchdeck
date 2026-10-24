//! The `push` source's routes: what a script, cron job or Home Assistant may post. The listener
//! (auth, limits) is in front of this and picks the integration; this only reads a request.

use serde::Deserialize;

use super::{PushError, PushRequest, Receiver};
use crate::daemon::feed::{Card, Note, Update, MAX_TTL};
use crate::daemon::protocol::{CardState, NoteLevel};

pub struct Routes;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct NoteBody {
    title: String,
    #[serde(default)]
    body: String,
    level: Option<NoteLevel>,
    id: Option<String>,
    ttl_s: Option<u64>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CardBody {
    label: String,
    value: String,
    state: Option<CardState>,
}

fn json<'a, T: Deserialize<'a>>(body: &'a [u8]) -> Result<T, PushError> {
    serde_json::from_slice(body).map_err(|e| PushError::new(400, format!("bad body: {e}")))
}

/// The one id segment after `prefix/`, or a 404.
fn segment<'a>(path: &'a str, prefix: &str) -> Result<&'a str, PushError> {
    match path
        .strip_prefix(prefix)
        .and_then(|rest| rest.strip_prefix('/'))
    {
        Some(id) if !id.is_empty() => Ok(id),
        _ => Err(PushError::new(404, "no such route")),
    }
}

impl Receiver for Routes {
    fn routes(&self) -> &'static [&'static str] {
        &["notes", "cards"]
    }

    fn accept(&self, req: &PushRequest) -> Result<Vec<Update>, PushError> {
        let path = req.path.as_str();
        match (req.method.as_str(), path) {
            ("POST", "notes") => {
                let body: NoteBody = json(&req.body)?;
                if body.title.trim().is_empty() {
                    return Err(PushError::new(400, "title is required"));
                }
                if body.ttl_s.is_some_and(|s| s == 0 || s > MAX_TTL.as_secs()) {
                    return Err(PushError::new(
                        400,
                        format!("ttl_s is not 1 to {}", MAX_TTL.as_secs()),
                    ));
                }
                Ok(vec![Update::PutNote(Note {
                    id: body.id,
                    title: body.title,
                    body: body.body,
                    level: body.level.unwrap_or(NoteLevel::Info),
                    ttl: body.ttl_s.map(std::time::Duration::from_secs),
                })])
            }
            ("DELETE", p) if p.starts_with("notes/") => {
                Ok(vec![Update::RemoveNote(segment(p, "notes")?.into())])
            }
            ("PUT", p) if p.starts_with("cards/") => {
                let id = segment(p, "cards")?;
                let body: CardBody = json(&req.body)?;
                Ok(vec![Update::PutCard(Card {
                    id: id.into(),
                    label: body.label,
                    value: body.value,
                    state: body.state.unwrap_or(CardState::Ok),
                })])
            }
            ("DELETE", p) if p.starts_with("cards/") => {
                Ok(vec![Update::RemoveCard(segment(p, "cards")?.into())])
            }
            _ => Err(PushError::new(404, "no such route")),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn req(method: &str, path: &str, body: &str) -> PushRequest {
        PushRequest {
            method: method.into(),
            path: path.into(),
            body: body.as_bytes().to_vec(),
        }
    }

    fn refused(r: PushRequest) -> (u16, String) {
        let e = Routes.accept(&r).unwrap_err();
        (e.status, e.reason)
    }

    #[test]
    fn a_note_needs_a_title_and_takes_the_documented_fields() {
        let ok = Routes
            .accept(&req(
                "POST",
                "notes",
                r#"{"title":"Backup done","body":"4 min","level":"ok","id":"backup","ttl_s":3600}"#,
            ))
            .unwrap();
        assert_eq!(
            ok,
            [Update::PutNote(Note {
                id: Some("backup".into()),
                title: "Backup done".into(),
                body: "4 min".into(),
                level: NoteLevel::Ok,
                ttl: Some(std::time::Duration::from_secs(3600)),
            })]
        );
        assert_eq!(refused(req("POST", "notes", r#"{"body":"x"}"#)).0, 400);
        assert_eq!(
            refused(req("POST", "notes", r#"{"title":" "}"#)).1,
            "title is required"
        );
        assert_eq!(
            refused(req("POST", "notes", r#"{"title":"t","tittle":"x"}"#)).0,
            400
        );
        assert_eq!(
            refused(req("POST", "notes", r#"{"title":"t","level":"loud"}"#)).0,
            400
        );
        assert_eq!(
            refused(req("POST", "notes", r#"{"title":"t","ttl_s":0}"#)).0,
            400
        );
        assert_eq!(refused(req("POST", "notes", "not json")).0, 400);
    }

    #[test]
    fn cards_and_deletes_route_by_method_and_path() {
        let put = Routes
            .accept(&req(
                "PUT",
                "cards/washer",
                r#"{"label":"Washer","value":"12 min"}"#,
            ))
            .unwrap();
        assert_eq!(
            put,
            [Update::PutCard(Card {
                id: "washer".into(),
                label: "Washer".into(),
                value: "12 min".into(),
                state: CardState::Ok,
            })]
        );
        assert_eq!(
            Routes.accept(&req("DELETE", "cards/washer", "")).unwrap(),
            [Update::RemoveCard("washer".into())]
        );
        assert_eq!(
            Routes.accept(&req("DELETE", "notes/backup", "")).unwrap(),
            [Update::RemoveNote("backup".into())]
        );
        for (method, path) in [
            ("GET", "notes"),
            ("POST", "cards/x"),
            ("PUT", "cards/"),
            ("DELETE", "notes"),
            ("POST", "grafana"),
        ] {
            assert_eq!(refused(req(method, path, "{}")).0, 404, "{method} {path}");
        }
    }
}
