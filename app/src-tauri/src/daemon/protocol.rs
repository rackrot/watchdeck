//! Device protocol v1. The wire format is documented in `site/src/protocol.md`.

use std::fmt;
use std::sync::{Arc, OnceLock};

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

pub const VERSION: u32 = 1;

/// The capabilities a session keeps from `hello.caps`; any other name is dropped.
pub const CAPS: [&str; 2] = [CAP_FONTS, CAP_PAGES];
/// The device draws every font in `fonts_gen::FONT_IDS` and reads the layout's font fields.
pub const CAP_FONTS: &str = "fonts";
/// The device assembles a set of up to 4 `layout` pages and lays out the shorter grid for 2 or
/// more.
pub const CAP_PAGES: &str = "pages";

/// Largest message, and largest frame, in either direction.
pub const MAX_MESSAGE_BYTES: usize = 16 * 1024;

/// An icon frame's header: `WDI1`, the 16-byte id, then width and height as u16 LE.
pub const ICON_HEADER_BYTES: usize = 24;
/// Bytes in an icon id, which is also what `icon` in a layout spells as 32 hex digits.
pub const ICON_ID_BYTES: usize = 16;
/// Largest icon side, in pixels. The device refuses anything bigger.
pub const ICON_MAX_SIDE: usize = 256;
/// Largest icon frame, which the device allows for binary frames while text stays at 16 KB.
pub const MAX_ICON_FRAME_BYTES: usize = ICON_HEADER_BYTES + ICON_MAX_SIDE * ICON_MAX_SIDE * 4;
const ICON_MAGIC: &[u8; 4] = b"WDI1";
/// The id and dimensions end here: the id's hash covers the frame from this offset on.
const ICON_HASHED_FROM: usize = 4 + ICON_ID_BYTES;

/// Bytes in a device token, and in SHA-256 digests (`commit`, `c_host`).
pub const TOKEN_BYTES: usize = 32;
/// Bytes in each pairing nonce (`n_dev`, `n_host`).
pub const NONCE_BYTES: usize = 16;

/// Reports a session accepts. The device resends until acked, so two is room for a crash and an
/// abnormal reset.
pub const MAX_REPORTS_PER_SESSION: u32 = 2;
const REPORT_KINDS: [&str; 2] = ["crash", "reset"];
const REPORT_REASONS: [&str; 5] = ["panic", "int_wdt", "task_wdt", "wdt", "brownout"];
const MAX_REPORT_TASK_CHARS: usize = 16;
const MAX_REPORT_CAUSE_CHARS: usize = 32;

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
    /// The icon's id as 32 lowercase hex digits, absent for a button without one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub icon: Option<String>,
    /// The button shows a live value, which arrives as `tile` messages.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub tile: bool,
    /// The label's font, sent only when it differs from the message's `font`. Needs the `fonts` cap.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub label_font: Option<String>,
    /// A tile's value font, sent only when it differs from the label's. Needs the `fonts` cap.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub value_font: Option<String>,
}

/// How a tile's last poll went, which the device shows as a color. A device treats any other
/// string as unknown, which is also how a tile looks before its first update.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TileState {
    Ok,
    Bad,
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
        /// What the device supports beyond the base protocol (see [`CAPS`]). A hello without the
        /// field has none, and names this daemon doesn't know are ignored by the session.
        #[serde(default, skip_serializing_if = "Vec::is_empty")]
        caps: Vec<String>,
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
        /// This page's index, 0-based, below `pages`. Needs the `pages` cap.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        page: Option<u32>,
        /// How many pages the set has, 1 to 4. Needs the `pages` cap.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        pages: Option<u32>,
        /// This page's id, 1 to 63 bytes. Needs the `pages` cap.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        page_id: Option<String>,
        cols: u32,
        rows: u32,
        /// The grid's default font, absent for Montserrat. Needs the `fonts` cap.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        font: Option<String>,
        buttons: Vec<Button>,
    },
    /// The device has built the grid of the layout with this `rev`. An older device never sends it.
    LayoutAck {
        rev: u32,
    },
    /// The icons the device is missing. The daemon answers each known id with a binary frame.
    IconGet {
        ids: Vec<String>,
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
        summary: String,
        text: String,
        ms: u64,
    },
    /// A tile's current value, for a button whose layout entry has `tile`.
    Tile {
        btn: String,
        text: String,
        state: TileState,
    },
    /// A crash or abnormal reset from an earlier boot. Unchecked until [`Report::check`] passes.
    Report(Report),
    ReportAck {
        id: String,
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

/// A device's report as it arrives. A missing or wrong value is caught by [`Report::check`], not
/// by parsing, so a bad report is dropped without ending the session.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Report {
    #[serde(default)]
    pub id: String,
    #[serde(default)]
    pub kind: String,
    #[serde(default)]
    pub reason: String,
    #[serde(default)]
    pub task: String,
    /// `pc` to `same_build` are required for a `crash` and ignored for a `reset`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pc: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ra: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cause: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub addr: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub elf: Option<String>,
    /// The crashed firmware is the one now running, so its version is the `hello`'s `fw`.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub same_build: bool,
}

fn printable(s: &str, max_chars: usize) -> bool {
    s.chars().count() <= max_chars && s.bytes().all(|b| (b' '..=b'~').contains(&b))
}

/// `0x` and exactly 8 lowercase hex digits, a 32-bit address.
fn is_address(s: &str) -> bool {
    s.strip_prefix("0x")
        .is_some_and(|hex| decode_hex::<4>(hex).is_some())
}

impl Report {
    /// This report with the fields its kind doesn't use cleared: a reset has no crash fields.
    pub fn normalized(&self) -> Self {
        let mut report = self.clone();
        if report.kind == "reset" {
            report.pc = None;
            report.ra = None;
            report.cause = None;
            report.addr = None;
            report.elf = None;
            report.same_build = false;
        }
        report
    }

    /// Why this report is unacceptable. The text never repeats the report's values.
    pub fn check(&self) -> Result<(), String> {
        if decode_hex::<4>(&self.id).is_none() {
            return Err("id is not 8 lowercase hex digits".into());
        }
        if !REPORT_KINDS.contains(&self.kind.as_str()) {
            return Err("kind is not crash or reset".into());
        }
        if !REPORT_REASONS.contains(&self.reason.as_str()) {
            return Err("reason is not a known reset reason".into());
        }
        if !printable(&self.task, MAX_REPORT_TASK_CHARS) {
            return Err(format!(
                "task is not printable ASCII of at most {MAX_REPORT_TASK_CHARS} characters"
            ));
        }
        if self.kind == "reset" {
            return Ok(());
        }
        for (name, value) in [("pc", &self.pc), ("ra", &self.ra), ("addr", &self.addr)] {
            if !value.as_deref().is_some_and(is_address) {
                return Err(format!("{name} is not 0x and 8 lowercase hex digits"));
            }
        }
        if !self
            .elf
            .as_deref()
            .is_some_and(|elf| decode_hex::<32>(elf).is_some())
        {
            return Err("elf is not 64 lowercase hex digits".into());
        }
        let cause = self.cause.as_deref().unwrap_or("");
        if cause.is_empty() || !printable(cause, MAX_REPORT_CAUSE_CHARS) {
            return Err(format!(
                "cause is not 1 to {MAX_REPORT_CAUSE_CHARS} printable ASCII characters"
            ));
        }
        Ok(())
    }
}

/// The log line for a report that passed [`Report::check`].
impl fmt::Display for Report {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "device report {}: {} {} in task {:?}",
            self.id, self.kind, self.reason, self.task
        )?;
        // A reset's crash fields are ignored, so they may not have been checked.
        if let (true, Some(pc), Some(ra), Some(cause), Some(addr), Some(elf)) = (
            self.kind == "crash",
            &self.pc,
            &self.ra,
            &self.cause,
            &self.addr,
            &self.elf,
        ) {
            write!(
                f,
                ", {cause} at pc {pc} ra {ra} addr {addr}, elf {elf}, same_build {}",
                self.same_build
            )?;
        }
        Ok(())
    }
}

/// A button icon in the device's pixel format: ARGB8888 as LVGL stores it, which is the bytes
/// B, G, R, A per pixel with straight alpha. Its id is the first 16 bytes of SHA-256 over the
/// width and height as u16 LE and then the pixels, so equal pictures share an id.
#[derive(Clone, PartialEq, Eq)]
pub struct IconFrame {
    pub id: [u8; ICON_ID_BYTES],
    pub w: u16,
    pub h: u16,
    pub pixels: Vec<u8>,
    /// The frame as sent, built on first use and shared by every session.
    wire: OnceLock<Arc<[u8]>>,
}

impl fmt::Debug for IconFrame {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "IconFrame({} {}x{})",
            encode_hex(&self.id),
            self.w,
            self.h
        )
    }
}

/// Why a binary frame is not an icon.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IconFrameError {
    TooShort,
    BadMagic,
    BadSize,
    BadLength,
    BadHash,
}

impl fmt::Display for IconFrameError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::TooShort => "shorter than the header",
            Self::BadMagic => "not an icon frame",
            Self::BadSize => "width or height outside 1 to 256",
            Self::BadLength => "length does not match the width and height",
            Self::BadHash => "id does not match the pixels",
        })
    }
}

fn icon_id(dims_and_pixels: &[u8]) -> [u8; ICON_ID_BYTES] {
    let digest = Sha256::digest(dims_and_pixels);
    std::array::from_fn(|i| digest[i])
}

impl IconFrame {
    /// `pixels` is `w * h * 4` bytes of B, G, R, A.
    pub fn new(w: u16, h: u16, pixels: Vec<u8>) -> Self {
        assert_eq!(pixels.len(), usize::from(w) * usize::from(h) * 4);
        let mut hashed = Vec::with_capacity(4 + pixels.len());
        hashed.extend_from_slice(&w.to_le_bytes());
        hashed.extend_from_slice(&h.to_le_bytes());
        hashed.extend_from_slice(&pixels);
        Self {
            id: icon_id(&hashed),
            w,
            h,
            pixels,
            wire: OnceLock::new(),
        }
    }

    /// The encoded frame, built once.
    pub fn wire(&self) -> Arc<[u8]> {
        self.wire.get_or_init(|| self.encode().into()).clone()
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut frame = Vec::with_capacity(ICON_HEADER_BYTES + self.pixels.len());
        frame.extend_from_slice(ICON_MAGIC);
        frame.extend_from_slice(&self.id);
        frame.extend_from_slice(&self.w.to_le_bytes());
        frame.extend_from_slice(&self.h.to_le_bytes());
        frame.extend_from_slice(&self.pixels);
        frame
    }

    /// Checks a frame the way the device does: header, size limits, exact length, then the hash.
    pub fn decode(frame: &[u8]) -> Result<Self, IconFrameError> {
        if frame.len() < ICON_HEADER_BYTES {
            return Err(IconFrameError::TooShort);
        }
        if &frame[..4] != ICON_MAGIC {
            return Err(IconFrameError::BadMagic);
        }
        let w = u16::from_le_bytes([frame[20], frame[21]]);
        let h = u16::from_le_bytes([frame[22], frame[23]]);
        let side = 1..=ICON_MAX_SIDE;
        if !side.contains(&usize::from(w)) || !side.contains(&usize::from(h)) {
            return Err(IconFrameError::BadSize);
        }
        if frame.len() != ICON_HEADER_BYTES + usize::from(w) * usize::from(h) * 4 {
            return Err(IconFrameError::BadLength);
        }
        let id: [u8; ICON_ID_BYTES] = std::array::from_fn(|i| frame[4 + i]);
        if icon_id(&frame[ICON_HASHED_FROM..]) != id {
            return Err(IconFrameError::BadHash);
        }
        Ok(Self {
            id,
            w,
            h,
            pixels: frame[ICON_HEADER_BYTES..].to_vec(),
            wire: OnceLock::new(),
        })
    }
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
    const FIXTURES: [&str; 18] = [
        include_str!("../../tests/fixtures/hello.json"),
        include_str!("../../tests/fixtures/welcome.json"),
        include_str!("../../tests/fixtures/error.json"),
        include_str!("../../tests/fixtures/layout.json"),
        include_str!("../../tests/fixtures/layout_fonts.json"),
        include_str!("../../tests/fixtures/layout_page.json"),
        include_str!("../../tests/fixtures/layout_ack.json"),
        include_str!("../../tests/fixtures/icon_get.json"),
        include_str!("../../tests/fixtures/press.json"),
        include_str!("../../tests/fixtures/result.json"),
        include_str!("../../tests/fixtures/tile.json"),
        include_str!("../../tests/fixtures/report.json"),
        include_str!("../../tests/fixtures/report_ack.json"),
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
    fn a_layout_page_carries_its_set_fields_and_a_plain_layout_prints_none() {
        let page = include_str!("../../tests/fixtures/layout_page.json");
        let Message::Layout {
            page,
            pages,
            page_id,
            font,
            ..
        } = serde_json::from_str(page).unwrap()
        else {
            panic!("expected a layout")
        };
        assert_eq!(
            (page, pages, page_id.as_deref(), font.as_deref()),
            (Some(1), Some(3), Some("media"), Some("inter"))
        );

        let plain = include_str!("../../tests/fixtures/layout.json");
        let Message::Layout {
            page,
            pages,
            page_id,
            ..
        } = serde_json::from_str(plain).unwrap()
        else {
            panic!("expected a layout")
        };
        assert_eq!((page, pages, page_id), (None, None, None));
        let printed =
            serde_json::to_value(serde_json::from_str::<Message>(plain).unwrap()).unwrap();
        for key in ["page", "pages", "page_id"] {
            assert!(printed.get(key).is_none(), "{key}");
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
        let msg: Message = serde_json::from_str(r#"{"t":"gauge","btn":"a","text":"1"}"#).unwrap();
        assert_eq!(msg, Message::Unknown);
    }

    #[test]
    fn missing_tag_or_field_is_an_error() {
        assert!(serde_json::from_str::<Message>(r#"{"btn":"a"}"#).is_err());
        assert!(serde_json::from_str::<Message>(r#"{"t":"press","btn":"a"}"#).is_err());
    }

    #[test]
    fn a_layout_ack_rev_must_be_a_non_negative_integer() {
        let parse = |rev: &str| {
            serde_json::from_str::<Message>(&format!(r#"{{"t":"layout_ack","rev":{rev}}}"#))
        };
        assert_eq!(parse("0").unwrap(), Message::LayoutAck { rev: 0 });
        for bad in ["-1", "1.5", "\"2\"", "null", "4294967296"] {
            assert!(parse(bad).is_err(), "{bad}");
        }
        assert!(serde_json::from_str::<Message>(r#"{"t":"layout_ack"}"#).is_err());
    }

    #[test]
    fn a_hello_without_caps_has_none_and_prints_none() {
        let text = r#"{"t":"hello","v":1,"device":"wd-1","fw":"0.2.0","screen":[1,1]}"#;
        let Message::Hello { caps, .. } = serde_json::from_str(text).unwrap() else {
            panic!("expected a hello")
        };
        assert!(caps.is_empty());
        let msg: Message = serde_json::from_str(text).unwrap();
        assert!(!serde_json::to_string(&msg).unwrap().contains("caps"));

        // Names pass through the parser as they came; the session drops those it doesn't know.
        let text = text.replace(
            r#""screen":[1,1]"#,
            r#""screen":[1,1],"caps":["fonts","bogus"]"#,
        );
        let Message::Hello { caps, .. } = serde_json::from_str(&text).unwrap() else {
            panic!("expected a hello")
        };
        assert_eq!(caps, ["fonts", "bogus"]);
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

    fn fixture_report() -> Report {
        match serde_json::from_str(include_str!("../../tests/fixtures/report.json")).unwrap() {
            Message::Report(report) => report,
            other => panic!("not a report: {other:?}"),
        }
    }

    fn reset_report() -> Report {
        Report {
            id: "00000001".into(),
            kind: "reset".into(),
            reason: "brownout".into(),
            task: String::new(),
            pc: None,
            ra: None,
            cause: None,
            addr: None,
            elf: None,
            same_build: false,
        }
    }

    #[test]
    fn the_fixture_report_is_acceptable_and_a_bare_reset_is_too() {
        assert_eq!(fixture_report().check(), Ok(()));
        assert_eq!(reset_report().check(), Ok(()));
        assert_eq!(
            fixture_report().to_string(),
            "device report a1b2c3d4: crash panic in task \"link\", LoadAccessFault at pc \
             0x4ff01234 ra 0x4ff05678 addr 0x00000000, elf \
             0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0, same_build true"
        );
    }

    #[test]
    fn a_report_missing_fields_parses_and_fails_the_check() {
        let msg: Message = serde_json::from_str(r#"{"t":"report"}"#).unwrap();
        let Message::Report(report) = msg else {
            panic!("not a report")
        };
        assert!(report.check().is_err());
        // A crash report cut short of its crash fields.
        let msg: Message = serde_json::from_str(
            r#"{"t":"report","id":"a1b2c3d4","kind":"crash","reason":"panic","task":"x"}"#,
        )
        .unwrap();
        let Message::Report(report) = msg else {
            panic!("not a report")
        };
        assert_eq!(
            report.check(),
            Err("pc is not 0x and 8 lowercase hex digits".into())
        );
    }

    #[test]
    fn every_report_rule_refuses_its_violation() {
        let edit = |f: &dyn Fn(&mut Report)| {
            let mut report = fixture_report();
            f(&mut report);
            report.check().unwrap_err()
        };
        for bad in ["", "a1b2c3d", "a1b2c3d45", "A1B2C3D4", "a1b2c3dg"] {
            assert!(edit(&|r| r.id = bad.into()).starts_with("id "), "{bad:?}");
        }
        for bad in ["", "Crash", "dump"] {
            assert!(
                edit(&|r| r.kind = bad.into()).starts_with("kind "),
                "{bad:?}"
            );
        }
        for bad in ["", "other", "PANIC"] {
            assert!(
                edit(&|r| r.reason = bad.into()).starts_with("reason "),
                "{bad:?}"
            );
        }
        // 16 characters pass, 17 and control characters or non-ASCII don't.
        assert_eq!(edit_ok(&|r| r.task = "a".repeat(16)), Ok(()));
        for bad in ["a".repeat(17), "li\nk".into(), "lïnk".into()] {
            assert!(
                edit(&|r| r.task = bad.clone()).starts_with("task "),
                "{bad:?}"
            );
        }
        for bad in [
            "4ff01234",
            "0x4ff0123",
            "0x4ff012345",
            "0x4FF01234",
            "0x4ff0123g",
        ] {
            assert!(
                edit(&|r| r.pc = Some(bad.into())).starts_with("pc "),
                "{bad:?}"
            );
            assert!(
                edit(&|r| r.ra = Some(bad.into())).starts_with("ra "),
                "{bad:?}"
            );
            assert!(
                edit(&|r| r.addr = Some(bad.into())).starts_with("addr "),
                "{bad:?}"
            );
        }
        assert!(edit(&|r| r.pc = None).starts_with("pc "));
        for bad in [
            Some("ab".repeat(31)),
            Some("AB".repeat(32)),
            Some("ab".repeat(33)),
            None,
        ] {
            assert!(
                edit(&|r| r.elf = bad.clone()).starts_with("elf "),
                "{bad:?}"
            );
        }
        for bad in [
            None,
            Some(String::new()),
            Some("x".repeat(33)),
            Some("a\tb".into()),
        ] {
            assert!(
                edit(&|r| r.cause = bad.clone()).starts_with("cause "),
                "{bad:?}"
            );
        }
        // The message names the field and never repeats the value.
        let why = edit(&|r| r.pc = Some("secret-value".into()));
        assert!(!why.contains("secret"), "{why}");
    }

    fn edit_ok(f: &dyn Fn(&mut Report)) -> Result<(), String> {
        let mut report = fixture_report();
        f(&mut report);
        report.check()
    }

    #[test]
    fn a_reset_ignores_its_crash_fields() {
        let mut report = reset_report();
        report.pc = Some("junk\n".into());
        report.elf = Some("junk".into());
        assert_eq!(report.check(), Ok(()));
        assert!(!report.to_string().contains("junk"));
        report.same_build = true;
        assert_eq!(report.normalized(), reset_report());
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

    // ---- Icon frames ----

    /// A 2x1 icon: opaque red, then fully transparent.
    fn two_pixels() -> IconFrame {
        IconFrame::new(2, 1, vec![0, 0, 255, 255, 0, 0, 0, 0])
    }

    /// The frame in site/src/protocol.md, with the id computed independently with Python's hashlib.
    #[test]
    fn an_icon_frame_matches_the_published_vector() {
        let icon = two_pixels();
        assert_eq!(encode_hex(&icon.id), "d95bd7f461bd7bc39755b97d11f8a270");
        let frame = icon.encode();
        assert_eq!(&frame[..4], b"WDI1");
        assert_eq!(&frame[20..24], [2, 0, 1, 0]);
        assert_eq!(frame.len(), 24 + 8);
        assert_eq!(IconFrame::decode(&frame).unwrap(), icon);
    }

    #[test]
    fn a_largest_icon_round_trips_at_the_frame_cap() {
        let icon = IconFrame::new(256, 256, vec![7; 256 * 256 * 4]);
        let frame = icon.encode();
        assert_eq!(frame.len(), MAX_ICON_FRAME_BYTES);
        assert_eq!(IconFrame::decode(&frame).unwrap(), icon);
    }

    #[test]
    fn a_frame_that_breaks_any_rule_is_refused() {
        let good = two_pixels().encode();
        let with = |edit: &dyn Fn(&mut Vec<u8>)| {
            let mut frame = good.clone();
            edit(&mut frame);
            IconFrame::decode(&frame)
        };
        assert_eq!(
            IconFrame::decode(&good[..23]),
            Err(IconFrameError::TooShort)
        );
        assert_eq!(IconFrame::decode(&[]), Err(IconFrameError::TooShort));
        assert_eq!(with(&|f| f[0] = b'X'), Err(IconFrameError::BadMagic));
        assert_eq!(with(&|f| f[20] = 0), Err(IconFrameError::BadSize)); // w = 0
        assert_eq!(with(&|f| f[22] = 0), Err(IconFrameError::BadSize)); // h = 0
        assert_eq!(with(&|f| f[21] = 2), Err(IconFrameError::BadSize)); // w = 514
        assert_eq!(with(&|f| f.push(0)), Err(IconFrameError::BadLength));
        assert_eq!(
            with(&|f| {
                f.pop();
            }),
            Err(IconFrameError::BadLength)
        );
        assert_eq!(with(&|f| f[4] ^= 1), Err(IconFrameError::BadHash)); // id
        assert_eq!(with(&|f| f[24] ^= 1), Err(IconFrameError::BadHash)); // pixel
                                                                         // The same pixels read as another shape have another id.
        assert_eq!(
            with(&|f| f.splice(20..24, [1, 0, 2, 0]).for_each(drop)),
            Err(IconFrameError::BadHash)
        );
    }

    #[test]
    fn an_oversize_icon_header_is_refused_before_its_length_is_computed() {
        let mut frame = two_pixels().encode();
        frame[20..24].copy_from_slice(&[0xff, 0xff, 0xff, 0xff]);
        assert_eq!(IconFrame::decode(&frame), Err(IconFrameError::BadSize));
    }
}
