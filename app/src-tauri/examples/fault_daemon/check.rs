//! `--check <log>`: grades each case from the board's serial log. The harness puts a
//! `{"t":"mark","case":"<id>"}` message before each case, and the board logs it on its `rx:` line,
//! so the lines up to the next marker belong to that case.

use std::fmt::Write;

use super::cases::{Rule, CASES};

const MARK: &str = r#""t":"mark","case":""#;

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

/// The text of the last stretch of the log that follows a marker for `id`.
fn segment(lines: &[&str], id: &str) -> Option<String> {
    let start = lines.iter().rposition(|l| mark_id(l) == Some(id))?;
    let end = lines[start + 1..]
        .iter()
        .position(|l| mark_id(l).is_some())
        .map_or(lines.len(), |n| start + 1 + n);
    Some(lines[start..end].join("\n"))
}

/// Why `seg` fails `rule`, one entry per problem.
fn problems(seg: &str, rule: &Rule, wedge: bool) -> Vec<String> {
    let mut found = Vec::new();
    for want in rule.want {
        if !seg.contains(want) {
            found.push(format!("missing {want:?}"));
        }
    }
    for not in rule.not {
        if seg.contains(not) {
            found.push(format!("unexpected {not:?}"));
        }
    }
    if !wedge {
        for fault in FAULTS {
            if seg.contains(fault) {
                found.push(format!("fault marker {fault:?}"));
            }
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

/// Grades `log` and returns the report and whether nothing failed.
pub fn grade(log: &str, after_s5: bool) -> (String, bool) {
    let lines: Vec<&str> = log.lines().collect();
    let mut out = String::new();
    let mut ok = true;
    for spec in CASES {
        let Some(seg) = segment(&lines, spec.id) else {
            writeln!(
                out,
                "{:<5} SKIP  {} (no marker in the log)",
                spec.id, spec.send
            )
            .ok();
            continue;
        };
        let rule = match (&spec.after, after_s5) {
            (Some(after), true) => after,
            _ => &spec.today,
        };
        let found = problems(&seg, rule, spec.wedge);
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
    let samples = leak_samples(&lines);
    if !samples.is_empty() {
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
        if keeps_falling(&internal) || keeps_falling(&psram) {
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

    #[test]
    fn grades_expected_lines_and_flags_faults() {
        let log = [
            mark("I1"),
            "W (2) proto: icon frame of 10 bytes is shorter than its header".into(),
            "I (3) link: icon aabbccdd received, validated and cached, 0 still wanted".into(),
            mark("I2"),
            "W (4) proto: binary frame is not an icon".into(),
            "Guru Meditation Error: Core 0 panic'ed".into(),
        ]
        .join("\n");
        let (report, ok) = grade(&log, false);
        assert!(!ok);
        assert!(report.contains("I1    PASS"), "{report}");
        assert!(report.contains("I2    FAIL"), "{report}");
        assert!(report.contains("Guru Meditation"), "{report}");
        assert!(report.contains("I3    SKIP"), "{report}");
    }

    #[test]
    fn the_after_s5_rule_replaces_the_current_one() {
        let log = [
            mark("I12"),
            "I (1) link: icon aabbccdd received, validated and cached".into(),
        ]
        .join("\n");
        assert!(!grade(&log, false).1); // today it also expects "already cached"
        assert!(grade(&log, true).1);
    }

    #[test]
    fn leak_check_fails_only_when_every_round_falls() {
        let sample = |n: u32, internal: u64| {
            format!(
                "{}\nI (9) link: heap before connect: internal free {internal} min 1, PSRAM free 5000 min 1",
                mark(&format!("LEAK{n}"))
            )
        };
        let falling = [
            sample(0, 900),
            sample(1, 800),
            sample(2, 700),
            sample(3, 600),
        ]
        .join("\n");
        let steady = [
            sample(0, 900),
            sample(1, 800),
            sample(2, 850),
            sample(3, 840),
        ]
        .join("\n");
        assert!(!grade(&falling, false).1);
        assert!(grade(&steady, false).1);
    }
}
