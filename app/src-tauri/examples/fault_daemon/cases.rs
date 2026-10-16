//! The case table and what each case sends. The table also carries the log expectations that
//! `--check` grades, so the two cannot drift apart.

use std::time::Duration;

use tokio::time::Instant;
use tokio_tungstenite::tungstenite::protocol::frame::coding::Data as OpData;
use watchdeck_lib::daemon::protocol::{ICON_HEADER_BYTES, MAX_ICON_FRAME_BYTES};

use super::session::{buttons, fresh_icon, icon_x, prompt, Res, Rig};

/// Substrings that must (`want`) and must not (`not`) appear in a case's stretch of the board log.
pub struct Rule {
    pub want: &'static [&'static str],
    pub not: &'static [&'static str],
}

const fn rule(want: &'static [&'static str], not: &'static [&'static str]) -> Rule {
    Rule { want, not }
}

pub struct Spec {
    pub id: &'static str,
    pub send: &'static str,
    /// What the board should log on the current firmware.
    pub today: Rule,
    /// What it should log once S4 and S5 are in, where that differs.
    pub after: Option<Rule>,
    /// What to see on the screen, which the log cannot show.
    pub look: Option<&'static str>,
    /// The case keeps the link up, so the leak rounds repeat it.
    pub leak: bool,
    /// The case wedges or reboots the board, so crash and reboot markers are not faults.
    pub wedge: bool,
}

const CACHED: &str = "received, validated and cached";
const NOT_DROPPED: &str = "dropped unparsable";

const fn spec(id: &'static str, send: &'static str, today: Rule) -> Spec {
    Spec {
        id,
        send,
        today,
        after: None,
        look: None,
        leak: true,
        wedge: false,
    }
}

/// In run order. Cases that close the link or wedge the board come last.
pub const CASES: &[Spec] = &[
    spec(
        "I1",
        "binary frame of 10 bytes",
        rule(&["shorter than its header", CACHED], &[]),
    ),
    spec(
        "I2",
        "icon frame with bad magic",
        rule(&["binary frame is not an icon", CACHED], &[]),
    ),
    spec(
        "I3",
        "icon frame with w = 0",
        rule(&["is outside 1..256", CACHED], &[]),
    ),
    spec(
        "I4",
        "icon frame with w = 257",
        rule(&["is outside 1..256", CACHED], &[]),
    ),
    spec(
        "I5",
        "icon frame one byte too long",
        rule(&["should be", CACHED], &[]),
    ),
    spec(
        "I6",
        "icon frame one byte too short",
        rule(&["should be", CACHED], &[]),
    ),
    spec(
        "I7",
        "icon frame with a flipped pixel byte",
        rule(&["does not match its pixels", CACHED], &[]),
    ),
    spec(
        "I8",
        "valid frame for an unrequested id",
        rule(&["was not requested, dropped"], &[]),
    ),
    spec(
        "I9",
        "valid frame for a cached id",
        rule(&["already cached, ignored"], &[]),
    ),
    spec(
        "T1",
        "tile for an unknown button",
        rule(&["tile for nope ignored: no such tile button"], &[]),
    ),
    spec(
        "T2",
        "tile for a button that is not a tile",
        rule(&["tile for plain ignored"], &[]),
    ),
    Spec {
        look: Some("T3: the previous value (keep) stays"),
        ..spec(
            "T3",
            "tile with a numeric text",
            rule(&["dropped unparsable message"], &[]),
        )
    },
    Spec {
        look: Some("T4: the tile shows the first 64 characters"),
        ..spec(
            "T4",
            "tile text of 200 characters",
            rule(&["rx: {\"t\":\"tile\""], &[NOT_DROPPED]),
        )
    },
    Spec {
        look: Some("T5: the tile turns grey for each of the three"),
        ..spec(
            "T5",
            "tile state absent, \"weird\", 5",
            rule(&["rx: {\"t\":\"tile\""], &[NOT_DROPPED]),
        )
    },
    spec(
        "T6",
        "tile btn of 64 characters",
        rule(&["dropped unparsable message"], &[]),
    ),
    Spec {
        look: Some("T7: the tile shows ? for each non-ASCII character"),
        ..spec(
            "T7",
            "tile text with non-ASCII characters",
            rule(&["rx: {\"t\":\"tile\""], &[NOT_DROPPED]),
        )
    },
    spec(
        "T9",
        "valid tile followed by trailing garbage",
        rule(&["rx: {\"t\":\"tile\""], &[NOT_DROPPED]),
    ),
    Spec {
        look: Some("L1: the t1 label ends in ? (after S4); today watch for garbage or a crash"),
        ..spec(
            "L1",
            "label of 46 A plus e-acute",
            rule(&["layout rev"], &[]),
        )
    },
    Spec {
        look: Some("L2: the t1 label ends in ? (after S4); today watch for garbage or a crash"),
        ..spec(
            "L2",
            "layout label ending in byte 0xF0",
            rule(&["rx: {\"t\":\"layout\""], &[]),
        )
    },
    Spec {
        look: Some("L3: the status line shows the name with a final ? (after S4)"),
        ..spec(
            "L3",
            "welcome name of 46 A plus e-acute",
            rule(&["rx: {\"t\":\"welcome\""], &[]),
        )
    },
    Spec {
        after: Some(rule(&[CACHED], &["already cached, ignored"])),
        ..spec(
            "I12",
            "first fragment (FIN = 0) holding a full valid icon",
            rule(&[CACHED, "already cached, ignored"], &[]),
        )
    },
    Spec {
        leak: false,
        ..spec(
            "T8",
            "tile before the first layout",
            rule(&["tile for t1 ignored"], &[]),
        )
    },
    Spec {
        leak: false,
        ..spec(
            "I10",
            "binary frame of 262,169 bytes",
            rule(&["exceeds 262168, closing", "link lost: bad frame"], &[]),
        )
    },
    Spec {
        leak: false,
        after: Some(rule(
            &[
                "was not requested, dropped",
                "exceeds 262168, closing",
                "link lost: bad frame",
            ],
            &["event queue full"],
        )),
        ..spec(
            "I11",
            "20 unrequested frames, then 262,169 bytes",
            rule(
                &["was not requested, dropped", "exceeds 262168, closing"],
                &[],
            ),
        )
    },
    Spec {
        leak: false,
        after: Some(rule(&["no welcome"], &[])),
        ..spec(
            "L4",
            "no welcome for 60 s after the hello",
            rule(&[], &["no welcome", "link lost"]),
        )
    },
    Spec {
        leak: false,
        wedge: true,
        look: Some("I13: the board wedges today; reboot it with an RTS pulse"),
        ..spec(
            "I13",
            "header claiming 262,168 bytes, 1,000 sent, 60 s pause",
            rule(&[], &[]),
        )
    },
    Spec {
        leak: false,
        wedge: true,
        after: Some(rule(&["link lost", "connecting to"], &[])),
        ..spec(
            "I13b",
            "I13, then an nft drop of all board traffic",
            rule(&[], &[]),
        )
    },
    // Pairing mode (`--pairing`), on the plain socket.
    Spec {
        look: Some("P1: the offer's name ends in ? (after S4); today watch for garbage or a crash"),
        leak: false,
        ..spec(
            "P1",
            "pair_offer with a 46 A plus e-acute name",
            rule(&["pairing code"], &[]),
        )
    },
    Spec {
        leak: false,
        ..spec(
            "P2",
            "binary frame on the pairing socket",
            rule(&["takes text only"], &[]),
        )
    },
    Spec {
        leak: false,
        ..spec(
            "P3",
            "text frame of 20,000 bytes",
            rule(&["exceeds 16384, closing"], &[]),
        )
    },
    Spec {
        look: Some("P4: tap No; after a reboot the board must still be unpaired"),
        leak: false,
        ..spec(
            "P4",
            "forged pair_result ok",
            rule(&["\"t\":\"pair_result\",\"ok\":true"], &[]),
        )
    },
    Spec {
        leak: false,
        wedge: true,
        ..spec(
            "P5",
            "frame header, 1,000 bytes, then silence",
            rule(&[], &[]),
        )
    },
];

pub fn spec_for(id: &str) -> Option<&'static Spec> {
    CASES.iter().find(|c| c.id == id)
}

/// Whether the case runs on the paired wss socket (everything but pairing mode).
pub fn is_session_case(id: &str) -> bool {
    !id.starts_with('P')
}

const SHORT: Duration = Duration::from_millis(300);
pub const LONG_NAME: &str = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\u{e9}"; // 46 A plus a 2-byte character

/// Runs the host side of `id` `reps` times on the live session, after the marker.
/// An error is a host-side failure. The caller checks liveness and recovers.
pub async fn run(rig: &mut Rig, id: &str, reps: u32) -> Res<String> {
    if id == "I13" || id == "I13b" {
        prompt(&format!(
            "{id} wedges the board. Confirm the capture is running and the tray app is quit."
        ))
        .await?;
    }
    // These two mark their own fresh session.
    if !matches!(id, "T8" | "L4") {
        rig.sess.mark(id).await?;
    }
    let mut note = String::new();
    for _ in 0..reps {
        note = once(rig, id).await?;
    }
    Ok(note)
}

async fn once(rig: &mut Rig, id: &str) -> Res<String> {
    match id {
        "I1" | "I2" | "I3" | "I4" | "I5" | "I6" | "I7" => icon_fault(rig, id).await,
        "I8" => {
            rig.sess.send_binary(fresh_icon().encode()).await?;
            rig.sess.quiet(SHORT).await?;
            Ok("sent a valid frame nobody asked for".into())
        }
        "I9" => {
            rig.sess.send_binary(icon_x().encode()).await?;
            rig.sess.quiet(SHORT).await?;
            Ok("sent X again".into())
        }
        "I10" => oversize(rig, 0).await,
        "I11" => oversize(rig, 20).await,
        "I12" => fragment(rig).await,
        "T1" => tile(rig, r#"{"t":"tile","btn":"nope","text":"x","state":"ok"}"#).await,
        "T2" => tile(rig, r#"{"t":"tile","btn":"plain","text":"x","state":"ok"}"#).await,
        "T3" => {
            tile(rig, r#"{"t":"tile","btn":"t1","text":"keep","state":"ok"}"#).await?;
            tile(rig, r#"{"t":"tile","btn":"t1","text":5,"state":"ok"}"#).await
        }
        "T4" => {
            let text = "x".repeat(200);
            tile(rig, &format!(r#"{{"t":"tile","btn":"t1","text":"{text}","state":"ok"}}"#)).await
        }
        "T5" => {
            tile(rig, r#"{"t":"tile","btn":"t1","text":"no state"}"#).await?;
            tile(rig, r#"{"t":"tile","btn":"t1","text":"weird","state":"weird"}"#).await?;
            tile(rig, r#"{"t":"tile","btn":"t1","text":"five","state":5}"#).await
        }
        "T6" => {
            let btn = "b".repeat(64);
            tile(rig, &format!(r#"{{"t":"tile","btn":"{btn}","text":"x","state":"ok"}}"#)).await
        }
        "T7" => tile(rig, "{\"t\":\"tile\",\"btn\":\"t1\",\"text\":\"h\u{e9}llo \u{2713} \u{1f600}\",\"state\":\"ok\"}").await,
        "T8" => tile_first(rig).await,
        "T9" => tile(rig, r#"{"t":"tile","btn":"t1","text":"tail","state":"ok"} trailing"#).await,
        "L1" => {
            rig.sess.layout(buttons(LONG_NAME, None)).await?;
            rig.sess.quiet(Duration::from_millis(500)).await?;
            Ok("applied a layout with the cut label".into())
        }
        "L2" => {
            let mut json = br#"{"t":"layout","rev":99,"cols":4,"rows":1,"buttons":[{"id":"t1","col":0,"row":0,"label":"AAAA"#
                .to_vec();
            json.push(0xf0);
            json.extend_from_slice(br#"","bg":0,"fg":16777215,"tile":true}]}"#);
            rig.sess.send_text_bytes(json).await?;
            rig.sess.quiet(Duration::from_millis(500)).await?;
            Ok("sent a text frame holding a lone 0xF0".into())
        }
        "L3" => {
            rig.sess.welcome(LONG_NAME).await?;
            rig.sess.quiet(Duration::from_millis(500)).await?;
            rig.sess.welcome("fault-harness").await?;
            Ok("welcomed with the cut name, then restored it".into())
        }
        "L4" => no_welcome(rig).await,
        "I13" => wedge(rig, false).await,
        "I13b" => wedge(rig, true).await,
        other => Err(format!("no such case {other}").into()),
    }
}

async fn tile(rig: &mut Rig, json: &str) -> Res<String> {
    rig.sess.send_text(json).await?;
    rig.sess.quiet(SHORT).await?;
    Ok("sent".into())
}

/// A layout naming a fresh icon Z, a bad frame for it, then the good frame and the same layout
/// again. The board must still want Z after the bad frame, then cache it, then ask for nothing.
async fn icon_fault(rig: &mut Rig, id: &str) -> Res<String> {
    let s = &mut rig.sess;
    let z = fresh_icon();
    let zid = s.register(&z);
    let layout = buttons("T1", Some(("z", &zid)));
    s.layout(layout.clone()).await?;
    let asked = s.next_get(Duration::from_secs(5)).await?;
    if asked != [zid.clone()] {
        return Err(format!("expected icon_get for [Z], got {asked:?}").into());
    }
    let good = z.encode();
    let mut bad = good.clone();
    match id {
        "I1" => bad.truncate(10),
        "I2" => bad[..4].copy_from_slice(b"WDX1"),
        "I3" => bad[20..22].copy_from_slice(&0u16.to_le_bytes()),
        "I4" => bad[20..22].copy_from_slice(&257u16.to_le_bytes()),
        "I5" => bad.push(0),
        "I6" => {
            bad.pop();
        }
        _ => bad[ICON_HEADER_BYTES] ^= 0xff, // I7: the first pixel byte
    }
    s.send_binary(bad).await?;
    s.quiet(SHORT).await?;
    s.send_binary(good).await?;
    s.quiet(SHORT).await?;
    s.layout(layout).await?;
    s.quiet(Duration::from_millis(500))
        .await
        .map_err(|e| format!("Z was not cached after the good frame: {e}"))?;
    Ok("bad frame dropped, want kept, good frame cached, no second icon_get".into())
}

/// `extra` unrequested frames, then a binary frame one byte over the icon limit.
async fn oversize(rig: &mut Rig, extra: usize) -> Res<String> {
    for _ in 0..extra {
        rig.sess.send_binary(fresh_icon().encode()).await?;
    }
    let big = vec![0u8; MAX_ICON_FRAME_BYTES + 1];
    // The board may close mid-send, which is the point.
    if let Err(e) = rig.sess.send_binary(big).await {
        println!("    send ended early: {e}");
    }
    if rig.sess.lost_within(Duration::from_secs(5)).await {
        let secs = rig.next_session(Duration::from_secs(20)).await?;
        Ok(format!("closed; new hello after {secs:.1} s"))
    } else {
        Ok("the link stayed up: the close was lost (F5)".into())
    }
}

/// A full icon frame in a first fragment (FIN = 0), then an empty final continuation.
async fn fragment(rig: &mut Rig) -> Res<String> {
    let s = &mut rig.sess;
    let z = fresh_icon();
    let zid = s.register(&z);
    s.layout(buttons("T1", Some(("z", &zid)))).await?;
    s.next_get(Duration::from_secs(5)).await?;
    s.send_frame(false, OpData::Binary, z.encode()).await?;
    s.send_frame(true, OpData::Continue, Vec::new()).await?;
    s.quiet(SHORT).await?;
    s.send_binary(z.encode()).await?; // caches Z if the fragment was dropped
    s.quiet(SHORT).await?;
    Ok("sent the fragment, then the whole frame".into())
}

/// A tile on a fresh session before any layout.
async fn tile_first(rig: &mut Rig) -> Res<String> {
    rig.sess.close().await;
    rig.replace(Duration::from_secs(60)).await?;
    rig.sess.mark("T8").await?;
    rig.sess.welcome("fault-harness").await?;
    rig.sess
        .send_text(r#"{"t":"tile","btn":"t1","text":"early","state":"ok"}"#)
        .await?;
    rig.sess.quiet(SHORT).await?;
    rig.sess.layout(buttons("T1", None)).await?;
    rig.sess
        .serve_until_quiet(Duration::from_millis(1500))
        .await?;
    Ok("tile sent between welcome and layout".into())
}

/// A hello with no welcome, held for 60 s.
async fn no_welcome(rig: &mut Rig) -> Res<String> {
    rig.sess.close().await;
    rig.replace(Duration::from_secs(60)).await?;
    rig.sess.mark("L4").await?;
    let start = Instant::now();
    if rig.sess.lost_within(Duration::from_secs(60)).await {
        let secs = start.elapsed().as_secs();
        rig.next_session(Duration::from_secs(60)).await?;
        Ok(format!("the board gave up after {secs} s"))
    } else {
        rig.bring_up().await?;
        Ok("the board waited 60 s and was still there (F3)".into())
    }
}

/// The stalled frame. With `drop`, the operator also cuts the network, and the harness times how
/// long the board takes to come back once it is restored.
async fn wedge(rig: &mut Rig, drop: bool) -> Res<String> {
    rig.sess.stall(0x82, 262_168, 1000).await?;
    if !drop {
        println!("    PRESS A BUTTON ON THE BOARD NOW (waiting 60 s)");
        rig.sleep_deaf(60).await;
        prompt("Reboot the board with an RTS pulse (r + Enter in the capture terminal), then press Enter.")
            .await?;
        rig.next_session(Duration::from_secs(120)).await?;
        return Ok("rebooted by the operator".into());
    }
    let board = rig.sess.peer.ip();
    prompt(&format!(
        "Drop all traffic with the board:\n\
         sudo nft add table inet wdfault\n\
         sudo nft add chain inet wdfault in '{{ type filter hook input priority -10; }}'\n\
         sudo nft add rule inet wdfault in ip saddr {board} drop\n\
         sudo nft add chain inet wdfault out '{{ type filter hook output priority -10; }}'\n\
         sudo nft add rule inet wdfault out ip daddr {board} drop"
    ))
    .await?;
    rig.sleep_deaf(40).await;
    prompt("Remove the rule: sudo nft delete table inet wdfault").await?;
    let secs = rig.next_session(Duration::from_secs(90)).await?;
    Ok(format!(
        "reconnected {secs:.1} s after the rule was removed"
    ))
}
