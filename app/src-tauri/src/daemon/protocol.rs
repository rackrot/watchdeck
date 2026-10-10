//! Device protocol v1 (S2 subset). The wire format is documented in `site/src/protocol.md`.

use serde::{Deserialize, Serialize};

pub const VERSION: u32 = 1;

/// Largest message, and largest frame, in either direction.
pub const MAX_MESSAGE_BYTES: usize = 16 * 1024;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ErrorCode {
    Version,
    BadRequest,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Button {
    pub id: String,
    pub col: u32,
    pub row: u32,
    pub label: String,
    pub bg: u32,
    pub fg: u32,
}

/// Every message in either direction, tagged by `t`. Unknown fields are ignored,
/// and an unknown `t` parses as [`Message::Unknown`] so the session can go on.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "t", rename_all = "snake_case")]
pub enum Message {
    Hello {
        v: u32,
        device: String,
        fw: String,
        screen: [u32; 2],
    },
    Welcome {
        v: u32,
        name: String,
    },
    Error {
        code: ErrorCode,
        msg: String,
    },
    Layout {
        rev: u32,
        cols: u32,
        rows: u32,
        buttons: Vec<Button>,
    },
    Press {
        btn: String,
        seq: u64,
    },
    Result {
        btn: String,
        seq: u64,
        ok: bool,
        code: i32,
        text: String,
        ms: u64,
    },
    #[serde(other)]
    Unknown,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    // The fixtures are the JSON examples from site/src/protocol.md.
    const FIXTURES: [&str; 6] = [
        include_str!("../../tests/fixtures/hello.json"),
        include_str!("../../tests/fixtures/welcome.json"),
        include_str!("../../tests/fixtures/error.json"),
        include_str!("../../tests/fixtures/layout.json"),
        include_str!("../../tests/fixtures/press.json"),
        include_str!("../../tests/fixtures/result.json"),
    ];

    #[test]
    fn every_fixture_round_trips() {
        for fixture in FIXTURES {
            let msg: Message = serde_json::from_str(fixture).unwrap();
            assert_ne!(msg, Message::Unknown, "{fixture}");
            let expected: Value = serde_json::from_str(fixture).unwrap();
            assert_eq!(serde_json::to_value(&msg).unwrap(), expected);
        }
    }

    #[test]
    fn unknown_fields_are_ignored() {
        let msg: Message =
            serde_json::from_str(r#"{"t":"press","btn":"a","seq":1,"extra":[1,2]}"#).unwrap();
        assert_eq!(
            msg,
            Message::Press {
                btn: "a".into(),
                seq: 1
            }
        );
    }

    #[test]
    fn unknown_type_parses_as_unknown() {
        let msg: Message = serde_json::from_str(r#"{"t":"tile","btn":"a","text":"1"}"#).unwrap();
        assert_eq!(msg, Message::Unknown);
    }

    #[test]
    fn missing_tag_or_field_is_an_error() {
        assert!(serde_json::from_str::<Message>(r#"{"btn":"a"}"#).is_err());
        assert!(serde_json::from_str::<Message>(r#"{"t":"press","btn":"a"}"#).is_err());
    }
}
