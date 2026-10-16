//! `--check <log>`: grades each case from the board's serial log. The harness puts a
//! `{"t":"mark","case":"<id>"}` message before each case and a `<id>/end` one after it, and the
//! board logs both on its `rx:` lines. A case's stretch runs from its start marker to its end
//! marker. A log without end markers (an older run) is graded the old way: a stretch runs to the
//! next marker of any kind, and nothing counts as between cases.
//!
//! The caller says which run the log should hold (a [`Plan`]). A case with too few stretches
//! fails, so an empty or garbled log cannot pass. Every stretch of every case is graded, and the
//! crash and reboot markers are scanned in every stretch except the wedge cases'. The lines
//! between an end marker and the next start are scanned for crashes only. The operator's reset
//! there (`rst:0x1 (POWERON)`) is reported as INFO, since the harness asks for one between cases.

use std::fmt::Write;

use super::cases::{spec_for, Rule, CASES};

const MARK: &str = r#""t":"mark","case":""#;

/// Ends a case id in its end marker.
pub const END: &str = "/end";

/// The operator's RTS pulse prints the ROM banner and this reset line.
const POWERON: &str = "rst:0x1 (POWERON";

/// Between cases, only these mean a crash. Any `rst:` but POWERON does too.
const CRASHES: &[&str] = &[
    "Guru Meditation",
    "abort()",
    "Backtrace:",
    "assert failed",
    "CORRUPT HEAP",
    "Task watchdog",
];

/// Output that means the board crashed or restarted.
const FAULTS: &[&str] = &[
    "Guru Meditation",
    "abort()",
    "Backtrace:",
    "assert failed",
    "CORRUPT HEAP",
    "Task watchdog",
    "ESP-ROM:",
    "rst:0x",
];

/// What the run that wrote the log should have produced.
pub struct Plan {
    pub ids: Vec<&'static str>,
    /// Stretches expected per case: 1, or the warm-up plus the rounds of a leak run.
    pub runs: usize,
    /// A leak run, which also needs one heap sample per run.
    pub leak: bool,
}

/// The id a marker line names, if the line is one.
fn mark_id(line: &str) -> Option<&str> {
    let rest = line.split_once(MARK)?.1;
    rest.split('"').next()
}

fn number_after(line: &str, key: &str) -> Option<u64> {
    let rest = line.split_once(key)?.1;
    rest.split(|c: char| !c.is_ascii_digit())
        .next()?
        .parse()
        .ok()
}

struct Stretch<'a> {
    id: &'a str,
    /// The 1-based line number of the marker.
    line: usize,
    lines: &'a [&'a str],
    /// The lines from its end marker to the next marker, and the 1-based number of the first.
    /// Empty when the stretch has no end marker.
    gap: &'a [&'a str],
    gap_line: usize,
}

/// Every stretch, in log order. Lines before the first marker belong to none, since the board
/// may have booted before the harness began.
fn stretches<'a>(lines: &'a [&'a str]) -> Vec<Stretch<'a>> {
    let marks: Vec<(usize, &str)> = lines
        .iter()
        .enumerate()
        .filter_map(|(i, l)| mark_id(l).map(|id| (i, id)))
        .collect();
    let mut all = Vec::new();
    for (n, &(start, id)) in marks.iter().enumerate() {
        if id.ends_with(END) {
            continue;
        }
        let next = marks.get(n + 1);
        let closed = next.is_some_and(|m| m.1.strip_suffix(END) == Some(id));
        let end = match next {
            Some(m) if closed => m.0 + 1,
            Some(m) => m.0,
            None => lines.len(),
        };
        let gap_end = if closed {
            marks.get(n + 2).map_or(lines.len(), |m| m.0)
        } else {
            end
        };
        all.push(Stretch {
            id,
            line: start + 1,
            lines: &lines[start..end],
            gap: &lines[end..gap_end],
            gap_line: end + 1,
        });
    }
    all
}

/// Why `seg` fails `rule`, one entry per problem.
fn problems(seg: &str, rule: &Rule) -> Vec<String> {
    let mut found = Vec::new();
    for want in rule.want {
        if !seg.contains(want) {
            found.push(format!("missing {want:?}"));
        }
    }
    for twice in rule.twice {
        if seg.matches(twice).count() < 2 {
            found.push(format!("expected {twice:?} at least twice"));
        }
    }
    for not in rule.not {
        if seg.contains(not) {
            found.push(format!("unexpected {not:?}"));
        }
    }
    found
}

/// The leak rounds: the `heap before connect` line after each `LEAK<n>` marker, as
/// (internal free, PSRAM free).
fn leak_samples(lines: &[&str]) -> Vec<(u64, u64)> {
    let mut samples = Vec::new();
    let mut waiting = false;
    for line in lines {
        if let Some(id) = mark_id(line) {
            waiting = id.starts_with("LEAK");
        } else if waiting && line.contains("heap before connect:") {
            if let (Some(i), Some(p)) = (
                number_after(line, "internal free "),
                number_after(line, "PSRAM free "),
            ) {
                samples.push((i, p));
                waiting = false;
            }
        }
    }
    samples
}

/// True if every step falls, over at least two steps.
fn keeps_falling(values: &[u64]) -> bool {
    values.len() >= 3 && values.windows(2).all(|w| w[1] < w[0])
}

/// Grades `log` against `plan` and returns the report and whether nothing failed.
pub fn grade(log: &str, plan: &Plan, after_s5: bool) -> (String, bool) {
    let lines: Vec<&str> = log.lines().collect();
    let all = stretches(&lines);
    let mut out = String::new();
    let mut ok = true;

    for spec in CASES.iter().filter(|c| plan.ids.contains(&c.id)) {
        let rule = match (&spec.after, after_s5) {
            (Some(after), true) => after,
            _ => &spec.today,
        };
        let mine: Vec<&Stretch> = all.iter().filter(|s| s.id == spec.id).collect();
        let mut found = Vec::new();
        if mine.len() < plan.runs {
            found.push(format!(
                "found {} of {} expected runs in the log",
                mine.len(),
                plan.runs
            ));
        }
        for (n, stretch) in mine.iter().enumerate() {
            let text = stretch.lines.join("\n");
            for problem in problems(&text, rule) {
                found.push(format!("run {} (line {}): {problem}", n + 1, stretch.line));
            }
        }
        let status = if !found.is_empty() {
            ok = false;
            "FAIL"
        } else if rule.want.is_empty() && rule.not.is_empty() {
            "INFO"
        } else {
            "PASS"
        };
        writeln!(out, "{:<5} {status}  {}", spec.id, spec.send).ok();
        for problem in found {
            writeln!(out, "        {problem}").ok();
        }
        if let Some(look) = spec.look {
            writeln!(out, "        look: {look}").ok();
        }
    }

    let mut faults = Vec::new();
    let mut resets = Vec::new();
    for stretch in &all {
        if !spec_for(stretch.id).is_some_and(|c| c.wedge) {
            for (i, line) in stretch.lines.iter().enumerate() {
                if let Some(fault) = FAULTS.iter().find(|f| line.contains(*f)) {
                    faults.push(format!(
                        "{fault:?} at line {} (case {})",
                        stretch.line + i,
                        stretch.id
                    ));
                }
            }
        }
        for (i, line) in stretch.gap.iter().enumerate() {
            let at = stretch.gap_line + i;
            if let Some(crash) = CRASHES.iter().find(|f| line.contains(*f)) {
                faults.push(format!(
                    "{crash:?} at line {at} (after case {})",
                    stretch.id
                ));
            } else if line.contains("rst:0x") {
                if line.contains(POWERON) {
                    resets.push(format!(
                        "operator reset at line {at} (after case {})",
                        stretch.id
                    ));
                } else {
                    faults.push(format!(
                        "a reset other than POWERON at line {at} (after case {})",
                        stretch.id
                    ));
                }
            }
        }
    }
    if faults.is_empty() {
        writeln!(
            out,
            "\nFAULT PASS  no crash or reboot marker outside the wedge cases"
        )
        .ok();
    } else {
        ok = false;
        writeln!(
            out,
            "\nFAULT FAIL  crash or reboot outside the wedge cases:"
        )
        .ok();
        for fault in faults {
            writeln!(out, "        {fault}").ok();
        }
    }
    for reset in resets {
        writeln!(out, "INFO  {reset}").ok();
    }

    if plan.leak {
        let samples = leak_samples(&lines);
        writeln!(
            out,
            "\nheap before connect, per leak round (internal free, PSRAM free):"
        )
        .ok();
        for (n, (internal, psram)) in samples.iter().enumerate() {
            writeln!(out, "  LEAK{n}  {internal:>9}  {psram:>10}").ok();
        }
        let internal: Vec<u64> = samples.iter().map(|s| s.0).collect();
        let psram: Vec<u64> = samples.iter().map(|s| s.1).collect();
        if samples.len() != plan.runs {
            ok = false;
            writeln!(
                out,
                "LEAK  FAIL  {} of {} heap samples",
                samples.len(),
                plan.runs
            )
            .ok();
        } else if keeps_falling(&internal) || keeps_falling(&psram) {
            ok = false;
            writeln!(out, "LEAK  FAIL  free heap fell at every round").ok();
        } else {
            writeln!(out, "LEAK  PASS  free heap did not fall at every round").ok();
        }
    }
    (out, ok)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mark(id: &str) -> String {
        format!(r#"I (1) link: rx: {{"t":"mark","case":"{id}"}}"#)
    }

    fn plan(ids: &[&'static str]) -> Plan {
        Plan {
            ids: ids.to_vec(),
            runs: 1,
            leak: false,
        }
    }

    const CACHED: &str = "I (3) link: icon aabbccdd received, validated and cached, 0 still wanted";

    #[test]
    fn grades_expected_lines() {
        let log = [
            mark("I1"),
            "W (2) proto: icon frame of 10 bytes is shorter than its header".into(),
            CACHED.into(),
            CACHED.into(),
            mark("I2"),
            "W (4) proto: binary frame is not an icon".into(),
            CACHED.into(),
        ]
        .join("\n");
        let (report, ok) = grade(&log, &plan(&["I1", "I2"]), false);
        assert!(!ok);
        assert!(report.contains("I1    PASS"), "{report}");
        // Only the liveness icon was cached, so the good frame for the case's own id was not.
        assert!(report.contains("I2    FAIL"), "{report}");
        assert!(report.contains("at least twice"), "{report}");
    }

    #[test]
    fn an_empty_or_unmarked_log_fails() {
        for log in ["", "boot noise\nmore noise"] {
            let (report, ok) = grade(log, &plan(&["I1", "T1"]), false);
            assert!(!ok, "{report}");
            assert!(report.contains("found 0 of 1"), "{report}");
        }
    }

    #[test]
    fn a_case_missing_from_the_log_fails_while_unplanned_ones_are_not_graded() {
        let log = [
            mark("I8"),
            "W (2) link: icon aabbccdd was not requested, dropped".into(),
        ]
        .join("\n");
        assert!(grade(&log, &plan(&["I8"]), false).1);
        assert!(!grade(&log, &plan(&["I8", "I9"]), false).1);
    }

    #[test]
    fn a_crash_outside_a_graded_stretch_fails() {
        let log = [
            mark("I8"),
            "W (2) link: icon aabbccdd was not requested, dropped".into(),
            mark("LEAK0"),
            "Guru Meditation Error: Core 0 panic'ed".into(),
        ]
        .join("\n");
        let (report, ok) = grade(&log, &plan(&["I8"]), false);
        assert!(!ok, "{report}");
        assert!(
            report.contains("FAULT FAIL") && report.contains("LEAK0"),
            "{report}"
        );
    }

    #[test]
    fn a_reboot_in_a_wedge_case_is_expected() {
        let log = [mark("I13"), "ESP-ROM:esp32p4-eco2".into(), mark("I13b")].join("\n");
        assert!(grade(&log, &plan(&["I13"]), false).1);
    }

    fn end(id: &str) -> String {
        mark(&format!("{id}{END}"))
    }

    const DROPPED: &str = "W (2) link: icon aabbccdd was not requested, dropped";

    #[test]
    fn a_case_is_graded_only_up_to_its_end_marker() {
        // The "already cached, ignored" line after the end marker belongs to no case.
        let log = [
            mark("I9"),
            end("I9"),
            "I (3) link: icon aabbccdd already cached, ignored".into(),
        ]
        .join("\n");
        let (report, ok) = grade(&log, &plan(&["I9"]), false);
        assert!(!ok, "{report}"); // I9 wants "already cached, ignored" inside its stretch
        let inside = [
            mark("I9"),
            "I (3) link: icon aabbccdd already cached, ignored".into(),
            end("I9"),
        ]
        .join("\n");
        assert!(grade(&inside, &plan(&["I9"]), false).1);
    }

    #[test]
    fn an_operator_reset_between_cases_is_info() {
        let log = [
            mark("I8"),
            DROPPED.into(),
            end("I8"),
            "ESP-ROM:esp32p4-eco2".into(),
            "rst:0x1 (POWERON),boot:0x1f".into(),
            mark("I9"),
        ]
        .join("\n");
        let (report, ok) = grade(&log, &plan(&["I8"]), false);
        assert!(ok, "{report}");
        assert!(
            report.contains("INFO  operator reset at line 5"),
            "{report}"
        );
        assert!(report.contains("FAULT PASS"), "{report}");
    }

    #[test]
    fn a_crash_or_other_reset_between_cases_fails() {
        for crash in [
            "Guru Meditation Error: Core 0 panic'ed",
            "rst:0xc (SW_CPU_RESET),boot:0x1f",
        ] {
            let log = [
                mark("I8"),
                DROPPED.into(),
                end("I8"),
                crash.into(),
                mark("I9"),
            ]
            .join("\n");
            let (report, ok) = grade(&log, &plan(&["I8"]), false);
            assert!(!ok, "{report}");
            assert!(
                report.contains("FAULT FAIL") && report.contains("line 4"),
                "{report}"
            );
        }
    }

    #[test]
    fn a_reset_inside_a_stretch_still_fails() {
        let log = [
            mark("I8"),
            DROPPED.into(),
            "rst:0x1 (POWERON)".into(),
            end("I8"),
        ]
        .join("\n");
        assert!(!grade(&log, &plan(&["I8"]), false).1);
    }

    #[test]
    fn t8_expects_the_tile_to_apply_to_the_kept_grid() {
        let applied = [
            mark("T8"),
            r#"I (1) link: rx: {"t":"tile","btn":"t1"}"#.into(),
        ]
        .join("\n");
        let ignored = [
            applied.clone(),
            "W (2) ui: tile for t1 ignored: no such tile button".into(),
        ]
        .join("\n");
        assert!(grade(&applied, &plan(&["T8"]), false).1);
        assert!(!grade(&ignored, &plan(&["T8"]), false).1);
    }

    #[test]
    fn the_after_s5_rule_replaces_the_current_one() {
        let log = [mark("I12"), CACHED.into()].join("\n");
        assert!(!grade(&log, &plan(&["I12"]), false).1); // today it also expects "already cached"
        assert!(grade(&log, &plan(&["I12"]), true).1);
    }

    fn sample(n: u32, internal: u64) -> String {
        format!(
            "{}\nI (9) link: heap before connect: internal free {internal} min 1, PSRAM free 5000 min 1",
            mark(&format!("LEAK{n}"))
        )
    }

    fn leak_plan() -> Plan {
        Plan {
            ids: vec!["I8"],
            runs: 4,
            leak: true,
        }
    }

    fn rounds(extra: impl Fn(usize) -> Option<&'static str>, samples: [u64; 4]) -> String {
        let mut log = Vec::new();
        for (n, internal) in samples.iter().enumerate() {
            log.push(mark("I8"));
            log.push("W (2) link: icon aabbccdd was not requested, dropped".to_string());
            if let Some(line) = extra(n) {
                log.push(line.to_string());
            }
            log.push(sample(n as u32, *internal));
        }
        log.join("\n")
    }

    #[test]
    fn leak_check_fails_only_when_every_round_falls() {
        let falling = rounds(|_| None, [900, 800, 700, 600]);
        let steady = rounds(|_| None, [900, 800, 850, 840]);
        assert!(!grade(&falling, &leak_plan(), false).1);
        assert!(grade(&steady, &leak_plan(), false).1);
    }

    #[test]
    fn a_crash_in_an_earlier_leak_round_fails() {
        let log = rounds(
            |n| (n == 1).then_some("Guru Meditation Error"),
            [900, 850, 880, 870],
        );
        let (report, ok) = grade(&log, &leak_plan(), false);
        assert!(!ok, "{report}");
        assert!(report.contains("FAULT FAIL"), "{report}");
    }

    #[test]
    fn missing_leak_samples_fail() {
        let log = rounds(|_| None, [900, 850, 880, 870]);
        let short: String = log
            .lines()
            .take(log.lines().count() - 2)
            .collect::<Vec<_>>()
            .join("\n");
        let (report, ok) = grade(&short, &leak_plan(), false);
        assert!(!ok, "{report}");
        assert!(report.contains("heap samples"), "{report}");
    }
}
