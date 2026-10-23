//! The status bar's units: °C or °F, and rates in SI bytes, binary bytes or bits. The wire keeps
//! °C and bytes per second. The board converts for display (`hoststat.c`), and the daemon converts
//! here to judge a threshold the way the bar shows it and to convert the steps in the editor.
//! Everything here is pure.

use std::ops::RangeInclusive;

use serde::{Deserialize, Serialize};

/// Serialized as `"c"` and `"f"`, the strings B1's copy in `protocol.rs` uses.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum TempUnit {
    #[default]
    C,
    F,
}

impl TempUnit {
    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "c" => Some(Self::C),
            "f" => Some(Self::F),
            _ => None,
        }
    }

    pub fn key(self) -> &'static str {
        match self {
            Self::C => "c",
            Self::F => "f",
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Self::C => "°C",
            Self::F => "°F",
        }
    }
}

/// Serialized as `"si"`, `"binary"` and `"bits"`, as in `protocol.rs`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum NetUnit {
    #[default]
    Si,
    Binary,
    Bits,
}

impl NetUnit {
    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "si" => Some(Self::Si),
            "binary" => Some(Self::Binary),
            "bits" => Some(Self::Bits),
            _ => None,
        }
    }

    pub fn key(self) -> &'static str {
        match self {
            Self::Si => "si",
            Self::Binary => "binary",
            Self::Bits => "bits",
        }
    }

    /// What a rate step's `at` is counted in.
    pub fn label(self) -> &'static str {
        match self {
            Self::Si => "MB/s",
            Self::Binary => "MiB/s",
            Self::Bits => "Mb/s",
        }
    }

    /// Bytes per second in one unit of a rate step's `at`: 10^6 bytes, 2^20 bytes or 10^6 bits.
    pub fn bytes_per_at(self) -> i64 {
        match self {
            Self::Si => 1_000_000,
            Self::Binary => 1 << 20,
            Self::Bits => 125_000,
        }
    }
}

/// The two choices together.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Units {
    pub temp: TempUnit,
    pub net: NetUnit,
}

/// What a threshold is measured on. The keys are the wire's names (`mem`, as in `mem_used`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Metric {
    Cpu,
    Mem,
    Gpu,
    CpuTemp,
    GpuTemp,
    NetRx,
    NetTx,
}

impl Metric {
    pub const ALL: [Metric; 7] = [
        Metric::Cpu,
        Metric::Mem,
        Metric::Gpu,
        Metric::CpuTemp,
        Metric::GpuTemp,
        Metric::NetRx,
        Metric::NetTx,
    ];

    pub fn key(self) -> &'static str {
        match self {
            Self::Cpu => "cpu",
            Self::Mem => "mem",
            Self::Gpu => "gpu",
            Self::CpuTemp => "cpu_temp",
            Self::GpuTemp => "gpu_temp",
            Self::NetRx => "net_rx",
            Self::NetTx => "net_tx",
        }
    }

    /// The range of a step's `at` and the unit it is in. `None` when the unit this metric depends
    /// on is not known, because the config's choice of it is itself refused.
    pub fn range(
        self,
        temp: Option<TempUnit>,
        net: Option<NetUnit>,
    ) -> Option<(RangeInclusive<i64>, &'static str)> {
        match self {
            Self::Cpu | Self::Mem | Self::Gpu => Some((1..=100, "%")),
            Self::CpuTemp | Self::GpuTemp => temp.map(|t| match t {
                TempUnit::C => (1..=150, t.label()),
                TempUnit::F => (34..=302, t.label()),
            }),
            Self::NetRx | Self::NetTx => net.map(|n| match n {
                NetUnit::Si | NetUnit::Binary => (1..=100_000, n.label()),
                NetUnit::Bits => (1..=800_000, n.label()),
            }),
        }
    }

    /// `at` counted in `to` instead of `from`, or `None` when the change of units leaves this
    /// metric alone (percent, or the other kind of unit). Temperatures round to the nearest
    /// whole degree. Rates go by the ratio of the units, rounded, at least 1.
    pub fn convert(self, at: i64, from: Units, to: Units) -> Option<i64> {
        match self {
            Self::CpuTemp | Self::GpuTemp if from.temp != to.temp => Some(match to.temp {
                TempUnit::F => celsius_to_fahrenheit(at),
                TempUnit::C => fahrenheit_to_celsius(at),
            }),
            Self::NetRx | Self::NetTx if from.net != to.net => {
                Some(div_round(at * from.net.bytes_per_at(), to.net.bytes_per_at()).max(1))
            }
            _ => None,
        }
    }
}

/// `n / d` rounded to the nearest whole number, halves up. `d` is positive.
fn div_round(n: i64, d: i64) -> i64 {
    (2 * n + d).div_euclid(2 * d)
}

/// The nearest whole number to `c × 9 / 5 + 32`. `18c + 320` is even, so its tenths digit is never
/// 5 and no tie can occur.
pub fn celsius_to_fahrenheit(c: i64) -> i64 {
    div_round(c * 9, 5) + 32
}

/// The nearest whole number to `(f - 32) × 5 / 9`. A tie would need `10(f - 32)` to be an odd
/// multiple of 9, which is even, so none occurs.
pub fn fahrenheit_to_celsius(f: i64) -> i64 {
    div_round((f - 32) * 5, 9)
}

/// Brings converted steps back to a valid list: each clamped to `range`, then made to rise (a
/// step that lands on or under the one before moves up to one more than it), then clamped again,
/// so a list crowded against the top ends there and is refused by the rules.
pub fn fit_steps(ats: &mut [i64], range: &RangeInclusive<i64>) {
    let clamp = |at: i64| at.clamp(*range.start(), *range.end());
    let mut previous = None;
    for at in ats {
        let mut next = clamp(*at);
        if let Some(previous) = previous.filter(|p| next <= *p) {
            next = clamp(previous + 1);
        }
        *at = next;
        previous = Some(next);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Whole °C and the whole °F the bar shows for it. The board's `test_hoststat.c` holds the
    /// same rows.
    const TEMP_VECTORS: &[(i64, i64)] = &[
        (-40, -40),
        (0, 32),
        (37, 99),
        (64, 147),
        (84, 183),
        (85, 185),
        (95, 203),
        (100, 212),
        (150, 302),
    ];

    /// Bytes per second, the unit, and the text the bar shows. The board's `test_hoststat.c` holds
    /// the same rows.
    const RATE_VECTORS: &[(u64, NetUnit, &str)] = &[
        (0, NetUnit::Si, "0"),
        (999, NetUnit::Si, "999"),
        (1_000, NetUnit::Si, "1.0k"),
        (340_000, NetUnit::Si, "340.0k"),
        (1_250_000, NetUnit::Si, "1.2M"),
        (999_999_999, NetUnit::Si, "999.9M"),
        (1_000_000_000, NetUnit::Si, "1.0G"),
        (0, NetUnit::Binary, "0"),
        (999, NetUnit::Binary, "999"),
        (1_000, NetUnit::Binary, "0.9Ki"),
        (1_024, NetUnit::Binary, "1.0Ki"),
        (1_024_000, NetUnit::Binary, "0.9Mi"),
        (1_153_434, NetUnit::Binary, "1.1Mi"),
        (104_857_599, NetUnit::Binary, "99.9Mi"),
        (104_857_600, NetUnit::Binary, "100.0Mi"),
        (1_048_576_000, NetUnit::Binary, "0.9Gi"),
        (1_073_741_824, NetUnit::Binary, "1.0Gi"),
        (0, NetUnit::Bits, "0b"),
        (124, NetUnit::Bits, "992b"),
        (125, NetUnit::Bits, "1.0kb"),
        (1_200_000, NetUnit::Bits, "9.6Mb"),
        (125_000_000, NetUnit::Bits, "1.0Gb"),
        (12_499_999, NetUnit::Bits, "99.9Mb"),
    ];

    /// The rule of the plan's Units section, as the board applies it: the rate in its unit (bits
    /// are bytes × 8), divided by 1000 (SI, bits) or 1024 (binary) while it is 1000 or more, then
    /// one decimal, truncated, with the prefix. Binary switches prefix at 1000 and not 1024, so
    /// the number never needs a fourth integer digit.
    fn rate_text(bytes_per_s: u64, unit: NetUnit) -> String {
        let (value, step, prefixes, suffix) = match unit {
            NetUnit::Si => (bytes_per_s, 1000, ["", "k", "M", "G"], ""),
            NetUnit::Binary => (bytes_per_s, 1024, ["", "Ki", "Mi", "Gi"], ""),
            NetUnit::Bits => (bytes_per_s * 8, 1000, ["", "k", "M", "G"], "b"),
        };
        if value < 1000 {
            return format!("{value}{suffix}");
        }
        let (mut tenths, mut i) = (value * 10, 0);
        while tenths >= 10_000 && i < 3 {
            tenths /= step;
            i += 1;
        }
        format!("{}.{}{}{suffix}", tenths / 10, tenths % 10, prefixes[i])
    }

    #[test]
    fn fahrenheit_follows_the_vectors_and_converts_back() {
        for &(c, f) in TEMP_VECTORS {
            assert_eq!(celsius_to_fahrenheit(c), f, "{c} C");
            assert_eq!(fahrenheit_to_celsius(f), c, "{f} F");
        }
        // Every whole °C the wire carries comes back as itself.
        for c in -40..=150 {
            assert_eq!(fahrenheit_to_celsius(celsius_to_fahrenheit(c)), c, "{c}");
        }
    }

    #[test]
    fn rates_follow_the_vectors() {
        for &(bytes, unit, text) in RATE_VECTORS {
            assert_eq!(rate_text(bytes, unit), text, "{bytes} B/s as {unit:?}");
        }
    }

    #[test]
    fn a_rate_step_is_reached_exactly_when_the_bar_shows_it() {
        // 100 MiB/s is 104,857,600 B/s: one byte less still shows 99.9Mi.
        let at = 100 * NetUnit::Binary.bytes_per_at() as u64;
        assert_eq!(rate_text(at - 1, NetUnit::Binary), "99.9Mi");
        assert_eq!(rate_text(at, NetUnit::Binary), "100.0Mi");
        assert_eq!(NetUnit::Si.bytes_per_at(), 1_000_000);
        assert_eq!(NetUnit::Bits.bytes_per_at() * 8, 1_000_000);
    }

    #[test]
    fn units_parse_exactly_their_keys() {
        for unit in [TempUnit::C, TempUnit::F] {
            assert_eq!(TempUnit::parse(unit.key()), Some(unit));
        }
        for unit in [NetUnit::Si, NetUnit::Binary, NetUnit::Bits] {
            assert_eq!(NetUnit::parse(unit.key()), Some(unit));
        }
        for bad in ["", "C", "k", "celsius"] {
            assert_eq!(TempUnit::parse(bad), None, "{bad}");
        }
        for bad in ["", "SI", "bytes", "mib"] {
            assert_eq!(NetUnit::parse(bad), None, "{bad}");
        }
    }

    #[test]
    fn ranges_follow_the_units() {
        let range = |m: Metric, t, n| m.range(t, n).unwrap();
        assert_eq!(range(Metric::Mem, None, None), (1..=100, "%"));
        assert_eq!(
            range(Metric::CpuTemp, Some(TempUnit::C), None),
            (1..=150, "°C")
        );
        assert_eq!(
            range(Metric::GpuTemp, Some(TempUnit::F), None),
            (34..=302, "°F")
        );
        assert_eq!(
            range(Metric::NetRx, None, Some(NetUnit::Binary)),
            (1..=100_000, "MiB/s")
        );
        assert_eq!(
            range(Metric::NetTx, None, Some(NetUnit::Bits)),
            (1..=800_000, "Mb/s")
        );
        assert_eq!(Metric::CpuTemp.range(None, None), None);
        assert_eq!(Metric::NetRx.range(None, None), None);
    }

    #[test]
    fn steps_convert_by_the_unit_that_changed() {
        let si = Units::default();
        let f = Units {
            temp: TempUnit::F,
            ..si
        };
        let bin = Units {
            net: NetUnit::Binary,
            ..si
        };
        let bits = Units {
            net: NetUnit::Bits,
            ..si
        };
        assert_eq!(Metric::CpuTemp.convert(85, si, f), Some(185));
        assert_eq!(Metric::GpuTemp.convert(185, f, si), Some(85));
        assert_eq!(Metric::NetRx.convert(100, si, bin), Some(95));
        assert_eq!(Metric::NetRx.convert(95, bin, si), Some(100));
        assert_eq!(Metric::NetTx.convert(100, si, bits), Some(800));
        // 1 Mb/s is 0.125 MB/s, held at 1.
        assert_eq!(Metric::NetTx.convert(1, bits, si), Some(1));
        // A change of one unit leaves the other kind of metric, and percent, alone.
        assert_eq!(Metric::NetRx.convert(100, si, f), None);
        assert_eq!(Metric::CpuTemp.convert(85, si, bin), None);
        assert_eq!(Metric::Mem.convert(90, si, bin), None);
    }

    #[test]
    fn fitted_steps_are_in_range_and_rise() {
        let fit = |mut ats: Vec<i64>, range: RangeInclusive<i64>| {
            fit_steps(&mut ats, &range);
            ats
        };
        // 84 and 85 °F are 29 and 29 °C: the higher moves up.
        assert_eq!(fit(vec![29, 29], 1..=150), [29, 30]);
        assert_eq!(fit(vec![5, 5, 5], 1..=150), [5, 6, 7]);
        assert_eq!(fit(vec![0, 400], 34..=302), [34, 302]);
        assert_eq!(fit(vec![200, 100], 1..=150), [150, 150]);
        assert_eq!(fit(vec![], 1..=150), Vec::<i64>::new());
    }
}
