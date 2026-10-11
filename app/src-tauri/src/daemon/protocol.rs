//! Device protocol v1. The wire format is documented in `site/src/protocol.md`.

use std::fmt;

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

pub const VERSION: u32 = 1;

/// Largest message, and largest frame, in either direction.
pub const MAX_MESSAGE_BYTES: usize = 16 * 1024;

/// Bytes in a device token, and in SHA-256 digests (`commit`, `c_host`).
pub const TOKEN_BYTES: usize = 32;
/// Bytes in each pairing nonce (`n_dev`, `n_host`).
pub const NONCE_BYTES: usize = 16;

/// First field of the pairing code's input, which binds the code to this protocol version.
const PAIR_LABEL: &str = "watchdeck-pair-v1";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ErrorCode {
    Version,
    BadRequest,
    Unauthorized,
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

/// A device's token as sent in `hello`: 64 lowercase hex digits. Debug never prints it.
#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(transparent)]
pub struct Token(pub String);

impl fmt::Debug for Token {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Token(<redacted>)")
    }
}

/// Every message in either direction, on either port, tagged by `t`. Unknown fields are
/// ignored, and an unknown `t` parses as [`Message::Unknown`] so the session can go on.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "t", rename_all = "snake_case")]
pub enum Message {
    Hello {
        v: u32,
        device: String,
        /// Missing in a hello from an unpaired device, which the daemon refuses like a wrong one.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        token: Option<Token>,
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
    PairReq {
        v: u32,
        device: String,
        commit: String,
    },
    PairOffer {
        name: String,
        cert: String,
        c_host: String,
    },
    PairNonce {
        n_dev: String,
    },
    PairReveal {
        n_host: String,
    },
    PairResult {
        ok: bool,
    },
    #[serde(other)]
    Unknown,
}

/// `wd-` plus the device's Wi-Fi MAC as 12 lowercase hex digits, exactly as the firmware writes it.
pub fn valid_device_id(device: &str) -> bool {
    device.strip_prefix("wd-").is_some_and(|mac| {
        mac.len() == 12 && mac.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
    })
}

pub fn encode_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// Exactly `N` bytes from `2N` lowercase hex digits, or `None`.
pub fn decode_hex<const N: usize>(hex: &str) -> Option<[u8; N]> {
    let digit = |c: u8| match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        _ => None,
    };
    let hex = hex.as_bytes();
    if hex.len() != 2 * N {
        return None;
    }
    let (pairs, _) = hex.as_chunks::<2>();
    let mut out = [0u8; N];
    for (byte, [hi, lo]) in out.iter_mut().zip(pairs) {
        *byte = digit(*hi)? << 4 | digit(*lo)?;
    }
    Some(out)
}

/// The 6-digit pairing code both sides derive at the end of the exchange: the first 4 bytes of
/// SHA-256 over every input, as a big-endian u32, mod 10^6. Each input goes in as a u16-BE
/// length and its bytes. `commit`, `n_dev` and `n_host` are raw bytes, not hex.
pub fn pairing_code(
    device: &str,
    commit: &[u8; TOKEN_BYTES],
    cert_pem: &str,
    n_dev: &[u8; NONCE_BYTES],
    n_host: &[u8; NONCE_BYTES],
) -> String {
    let mut hash = Sha256::new();
    let fields: [&[u8]; 6] = [
        PAIR_LABEL.as_bytes(),
        device.as_bytes(),
        commit,
        cert_pem.as_bytes(),
        n_dev,
        n_host,
    ];
    for field in fields {
        let len = u16::try_from(field.len()).expect("every pairing field is capped below 64 KB");
        hash.update(len.to_be_bytes());
        hash.update(field);
    }
    let digest = hash.finalize();
    let n = u32::from_be_bytes([digest[0], digest[1], digest[2], digest[3]]);
    format!("{:06}", n % 1_000_000)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    // The fixtures are the JSON examples from site/src/protocol.md.
    const FIXTURES: [&str; 11] = [
        include_str!("../../tests/fixtures/hello.json"),
        include_str!("../../tests/fixtures/welcome.json"),
        include_str!("../../tests/fixtures/error.json"),
        include_str!("../../tests/fixtures/layout.json"),
        include_str!("../../tests/fixtures/press.json"),
        include_str!("../../tests/fixtures/result.json"),
        include_str!("../../tests/fixtures/pair_req.json"),
        include_str!("../../tests/fixtures/pair_offer.json"),
        include_str!("../../tests/fixtures/pair_nonce.json"),
        include_str!("../../tests/fixtures/pair_reveal.json"),
        include_str!("../../tests/fixtures/pair_result.json"),
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

    #[test]
    fn a_hello_without_a_token_parses_and_debug_redacts_one() {
        let msg: Message = serde_json::from_str(
            r#"{"t":"hello","v":1,"device":"wd-1","fw":"0.2.0","screen":[1,1]}"#,
        )
        .unwrap();
        assert!(matches!(msg, Message::Hello { token: None, .. }));

        let fixture = include_str!("../../tests/fixtures/hello.json");
        let msg: Message = serde_json::from_str(fixture).unwrap();
        let Message::Hello {
            token: Some(Token(token)),
            ..
        } = &msg
        else {
            panic!("the fixture carries a token")
        };
        let debug = format!("{msg:?}");
        assert!(!debug.contains(token.as_str()), "{debug}");
        assert!(debug.contains("<redacted>"), "{debug}");
    }

    #[test]
    fn hex_decodes_only_exact_lowercase() {
        assert_eq!(decode_hex::<2>("00ff"), Some([0x00, 0xff]));
        assert_eq!(encode_hex(&[0x00, 0xff]), "00ff");
        for bad in ["00FF", "00f", "00fff0", "0g00", ""] {
            assert_eq!(decode_hex::<2>(bad), None, "{bad:?}");
        }
    }

    #[test]
    fn a_device_id_is_wd_and_12_lowercase_hex_digits() {
        assert!(valid_device_id("wd-e8f60ae276ab"));
        for bad in [
            "",
            "wd-",
            "wd-1",
            "wd-e8f60ae276a",
            "wd-e8f60ae276ab0",
            "wd-E8F60AE276AB",
            "wd-e8f60ae276ag",
            "WD-e8f60ae276ab",
            "xx-e8f60ae276ab",
            "wd-e8f60ae276ab ",
        ] {
            assert!(!valid_device_id(bad), "{bad:?}");
        }
    }

    /// The vector in site/src/protocol.md, computed independently with Python's hashlib.
    #[test]
    fn pairing_code_matches_the_published_vector() {
        let token: [u8; 32] = std::array::from_fn(|i| i as u8);
        let commit: [u8; 32] = Sha256::digest(token).into();
        assert_eq!(
            encode_hex(&commit),
            "630dcd2966c4336691125448bbb25b4ff412a49c732db2c8abc1b8581bd710dd"
        );
        let n_dev: [u8; 16] = std::array::from_fn(|i| 0x20 + i as u8);
        let n_host: [u8; 16] = std::array::from_fn(|i| 0x30 + i as u8);
        let cert = "-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----\n";
        assert_eq!(
            pairing_code("wd-000000000001", &commit, cert, &n_dev, &n_host),
            "333724"
        );
    }

    #[test]
    fn every_pairing_input_changes_the_code() {
        let commit = [1u8; 32];
        let (n_dev, n_host) = ([2u8; 16], [3u8; 16]);
        let base = pairing_code("wd-1", &commit, "cert", &n_dev, &n_host);
        let variants = [
            pairing_code("wd-2", &commit, "cert", &n_dev, &n_host),
            pairing_code("wd-1", &[9u8; 32], "cert", &n_dev, &n_host),
            pairing_code("wd-1", &commit, "cerT", &n_dev, &n_host),
            pairing_code("wd-1", &commit, "cert", &[9u8; 16], &n_host),
            pairing_code("wd-1", &commit, "cert", &n_dev, &[9u8; 16]),
        ];
        assert_eq!(base.len(), 6);
        for (i, variant) in variants.iter().enumerate() {
            assert_ne!(*variant, base, "input {i}");
        }
    }
}
