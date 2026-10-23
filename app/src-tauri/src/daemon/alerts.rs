//! The color of each status bar value past a threshold. Steps are counted in the units the bar
//! shows (°C or °F, MB/s, MiB/s or Mb/s), so a value is colored exactly when the number on the bar
//! has reached the step. Each metric keeps its level between samples, which gives hysteresis: a step
//! reached stays until the value falls below `at` minus 2 points (4 in °F, 10% for a rate). A
//! change of rules, the units included, starts every metric afresh. Pure: no I/O.

use super::config::{color, StatusBarConfig, Step as ConfigStep, Thresholds};
use super::hoststats::HostStats;
use super::protocol::Colors;
use super::units::{celsius_to_fahrenheit, Metric, NetUnit, TempUnit, Units};

/// One step in the unit the metric is judged in (percent, display degrees or bytes per second).
#[derive(Debug, Clone, PartialEq, Eq)]
struct Step {
    /// The value that reaches the step.
    on: i64,
    /// A reached step is kept until the value falls below this.
    off: i64,
    color: u32,
}

/// The steps of every metric, resolved from a config: the units, and for each metric its own
/// steps or the defaults for the temperature unit.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Rules {
    units: Units,
    /// Indexed by `Metric as usize`.
    steps: [Vec<Step>; 7],
}

impl Rules {
    /// `None`, or a table without a key, takes the defaults. A step that fails the config's own
    /// rules can't be live; it is skipped here rather than trusted.
    pub fn new(bar: Option<&StatusBarConfig>) -> Self {
        let units = Units {
            temp: bar
                .and_then(|b| b.temp_unit.as_deref())
                .and_then(TempUnit::parse)
                .unwrap_or_default(),
            net: bar
                .and_then(|b| b.net_unit.as_deref())
                .and_then(NetUnit::parse)
                .unwrap_or_default(),
        };
        let defaults = Thresholds::defaults(units.temp);
        let set = bar.and_then(|b| b.thresholds.as_ref());
        let steps = Metric::ALL.map(|metric| {
            let chosen = set.and_then(|t| t.get(metric)).or(defaults.get(metric));
            chosen
                .map(|steps| {
                    steps
                        .iter()
                        .filter_map(|s| step(metric, units, s))
                        .collect()
                })
                .unwrap_or_default()
        });
        Self { units, steps }
    }

    pub fn units(&self) -> Units {
        self.units
    }
}

fn step(metric: Metric, units: Units, step: &ConfigStep) -> Option<Step> {
    let at = step.at;
    let (on, off) = match metric {
        Metric::Cpu | Metric::Mem | Metric::Gpu => (at, at - 2),
        Metric::CpuTemp | Metric::GpuTemp => (
            at,
            at - match units.temp {
                TempUnit::C => 2,
                TempUnit::F => 4,
            },
        ),
        Metric::NetRx | Metric::NetTx => {
            let on = at.saturating_mul(units.net.bytes_per_at());
            (on, on - on / 10)
        }
    };
    Some(Step {
        on,
        off,
        color: color(&step.color)?,
    })
}

/// The number the bar shows for `metric`, in the unit its steps are in (bytes per second for a
/// rate, which the steps are scaled to).
fn value(metric: Metric, stats: &HostStats, units: Units) -> Option<i64> {
    let degrees = |c: i32| match units.temp {
        TempUnit::C => i64::from(c),
        TempUnit::F => celsius_to_fahrenheit(i64::from(c)),
    };
    let rate = |bytes: u64| i64::try_from(bytes).unwrap_or(i64::MAX);
    match metric {
        Metric::Cpu => stats.cpu.map(i64::from),
        // Rounded down, as the board shows it.
        Metric::Mem => stats
            .mem
            .filter(|m| m.total > 0)
            .map(|m| (u128::from(m.used) * 100 / u128::from(m.total)) as i64),
        Metric::Gpu => stats.gpu.and_then(|g| g.busy).map(i64::from),
        Metric::CpuTemp => stats.cpu_temp.map(degrees),
        Metric::GpuTemp => stats.gpu.and_then(|g| g.temp).map(degrees),
        Metric::NetRx => stats.net.map(|n| rate(n.rx)),
        Metric::NetTx => stats.net.map(|n| rate(n.tx)),
    }
}

/// Each metric's level between samples: 0 is no color, `k` is the k-th step.
#[derive(Debug, Default)]
pub struct Alerts {
    rules: Rules,
    levels: [usize; 7],
}

impl Alerts {
    /// The colors for one reading under `rules`. Rules that differ from the last call's reset every
    /// level first.
    pub fn colors(&mut self, rules: &Rules, stats: &HostStats) -> Colors {
        if self.rules != *rules {
            self.rules = rules.clone();
            self.levels = [0; 7];
        }
        let mut colors = Colors::default();
        for metric in Metric::ALL {
            let i = metric as usize;
            let steps = &rules.steps[i];
            // A missing value clears its step.
            let level = match value(metric, stats, rules.units) {
                Some(v) => next_level(steps, self.levels[i], v),
                None => 0,
            };
            self.levels[i] = level;
            let slot = match metric {
                Metric::Cpu => &mut colors.cpu,
                Metric::Mem => &mut colors.mem,
                Metric::Gpu => &mut colors.gpu,
                Metric::CpuTemp => &mut colors.cpu_temp,
                Metric::GpuTemp => &mut colors.gpu_temp,
                Metric::NetRx => &mut colors.net_rx,
                Metric::NetTx => &mut colors.net_tx,
            };
            *slot = level.checked_sub(1).map(|k| steps[k].color);
        }
        colors
    }
}

/// The higher of the highest step `v` reaches and the highest step at or below `current` that `v`
/// still holds.
fn next_level(steps: &[Step], current: usize, v: i64) -> usize {
    let reached = steps.iter().rposition(|s| v >= s.on).map_or(0, |k| k + 1);
    let held = steps[..current.min(steps.len())]
        .iter()
        .rposition(|s| v >= s.off)
        .map_or(0, |k| k + 1);
    reached.max(held)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::daemon::config::Thresholds;
    use crate::daemon::gpu::GpuReading;
    use crate::daemon::hoststats::{Mem, Net};

    const AMBER: u32 = 0xffb300;
    const RED: u32 = 0xe53935;

    fn bar(temp: &str, net: &str, set: impl FnOnce(&mut Thresholds)) -> StatusBarConfig {
        let mut thresholds = Thresholds::default();
        set(&mut thresholds);
        StatusBarConfig {
            temp_unit: Some(temp.into()),
            net_unit: Some(net.into()),
            thresholds: Some(thresholds),
            ..StatusBarConfig::default()
        }
    }

    fn steps(list: &[(i64, &str)]) -> Option<Vec<ConfigStep>> {
        Some(
            list.iter()
                .map(|(at, c)| ConfigStep {
                    at: *at,
                    color: (*c).into(),
                })
                .collect(),
        )
    }

    fn cpu(v: u8) -> HostStats {
        HostStats {
            cpu: Some(v),
            ..HostStats::default()
        }
    }

    fn cpu_temp(c: i32) -> HostStats {
        HostStats {
            cpu_temp: Some(c),
            ..HostStats::default()
        }
    }

    fn rx(bytes: u64) -> HostStats {
        HostStats {
            net: Some(Net { rx: bytes, tx: 0 }),
            ..HostStats::default()
        }
    }

    /// Feeds `values` one after another and returns the `cpu` color of each.
    fn run_cpu(rules: &Rules, values: &[u8]) -> Vec<Option<u32>> {
        let mut alerts = Alerts::default();
        values
            .iter()
            .map(|v| alerts.colors(rules, &cpu(*v)).cpu)
            .collect()
    }

    fn once(rules: &Rules, stats: &HostStats) -> Colors {
        Alerts::default().colors(rules, stats)
    }

    #[test]
    fn defaults_in_celsius() {
        let rules = Rules::new(None);
        assert_eq!(once(&rules, &cpu(89)).cpu, None);
        assert_eq!(once(&rules, &cpu(90)).cpu, Some(AMBER));
        let mem = |used| HostStats {
            mem: Some(Mem { used, total: 100 }),
            ..HostStats::default()
        };
        assert_eq!(once(&rules, &mem(79)).mem, None);
        assert_eq!(once(&rules, &mem(80)).mem, Some(AMBER));
        assert_eq!(once(&rules, &mem(90)).mem, Some(RED));
        let gpu = |busy, temp| HostStats {
            gpu: Some(GpuReading { busy, temp }),
            ..HostStats::default()
        };
        assert_eq!(once(&rules, &gpu(Some(100), None)).gpu, None);
        assert_eq!(once(&rules, &gpu(None, Some(83))).gpu_temp, None);
        assert_eq!(once(&rules, &gpu(None, Some(84))).gpu_temp, Some(AMBER));
        assert_eq!(once(&rules, &gpu(None, Some(90))).gpu_temp, Some(RED));
        assert_eq!(once(&rules, &cpu_temp(84)).cpu_temp, None);
        assert_eq!(once(&rules, &cpu_temp(85)).cpu_temp, Some(AMBER));
        assert_eq!(once(&rules, &cpu_temp(95)).cpu_temp, Some(RED));
        assert_eq!(once(&rules, &rx(u64::MAX / 2)).net_rx, None);
    }

    #[test]
    fn defaults_in_fahrenheit() {
        let rules = Rules::new(Some(&bar("f", "si", |_| {})));
        let temps = |c: i32| {
            let s = HostStats {
                cpu_temp: Some(c),
                gpu: Some(GpuReading {
                    busy: None,
                    temp: Some(c),
                }),
                ..HostStats::default()
            };
            let colors = once(&rules, &s);
            (colors.cpu_temp, colors.gpu_temp)
        };
        // 84 C is 183 F and 85 C is 185 F; 90 C is 194 F and 95 C is 203 F.
        assert_eq!(temps(83), (None, None));
        assert_eq!(temps(84), (None, Some(AMBER)));
        assert_eq!(temps(85), (Some(AMBER), Some(AMBER)));
        assert_eq!(temps(89), (Some(AMBER), Some(AMBER)));
        assert_eq!(temps(90), (Some(AMBER), Some(RED)));
        assert_eq!(temps(94), (Some(AMBER), Some(RED)));
        assert_eq!(temps(95), (Some(RED), Some(RED)));
    }

    #[test]
    fn a_value_at_the_step_colors_and_one_below_does_not() {
        let rules = Rules::new(Some(&bar("c", "si", |t| t.cpu = steps(&[(50, "#112233")]))));
        assert_eq!(run_cpu(&rules, &[49, 50]), [None, Some(0x112233)]);
    }

    #[test]
    fn hysteresis_keeps_a_step_until_two_points_below() {
        let rules = Rules::new(Some(&bar("c", "si", |t| t.cpu = steps(&[(90, "#112233")]))));
        let c = Some(0x112233);
        assert_eq!(run_cpu(&rules, &[91, 89, 88, 87]), [c, c, c, None]);
        // Not colored yet: 89 alone does nothing.
        assert_eq!(run_cpu(&rules, &[89, 88]), [None, None]);
    }

    #[test]
    fn fahrenheit_steps_are_judged_on_the_number_the_bar_shows() {
        let rules = Rules::new(Some(&bar("f", "si", |t| {
            t.cpu_temp = steps(&[(190, "#112233")])
        })));
        let c = Some(0x112233);
        let mut alerts = Alerts::default();
        let mut at = |deg| alerts.colors(&rules, &cpu_temp(deg)).cpu_temp;
        // 88 C is 190 F and 87 C is 189 F.
        assert_eq!(at(87), None);
        assert_eq!(at(88), c);
        // 86 C is 187 F, 85 C is 185 F: kept down to 186 F (h 4), gone below.
        assert_eq!(at(86), c);
        assert_eq!(at(85), None);
        // 87 C is 189 F: a fresh value under `at` doesn't color again.
        assert_eq!(at(87), None);
    }

    #[test]
    fn rates_color_at_the_step_in_their_unit_and_clear_ten_percent_below() {
        for (unit, per) in [("si", 1_000_000u64), ("binary", 1 << 20), ("bits", 125_000)] {
            let rules = Rules::new(Some(&bar("c", unit, |t| {
                t.net_rx = steps(&[(100, "#112233")])
            })));
            let c = Some(0x112233);
            let mut alerts = Alerts::default();
            let mut at = |bytes| alerts.colors(&rules, &rx(bytes)).net_rx;
            assert_eq!(at(100 * per - 1), None, "{unit}");
            assert_eq!(at(100 * per), c, "{unit}");
            assert_eq!(at(90 * per), c, "{unit}");
            assert_eq!(at(90 * per - 1), None, "{unit}");
        }
    }

    #[test]
    fn a_jump_to_the_top_step_and_a_fall_across_two() {
        let rules = Rules::new(Some(&bar("c", "si", |t| {
            t.cpu = steps(&[(20, "#000001"), (50, "#000002"), (80, "#000003")])
        })));
        let got = run_cpu(&rules, &[0, 90, 10]);
        assert_eq!(got, [None, Some(3), None]);
        // From the third step to under the first in one sample, and to between the two.
        let got = run_cpu(&rules, &[90, 30]);
        assert_eq!(got, [Some(3), Some(1)]);
    }

    #[test]
    fn a_missing_value_clears_its_step() {
        let rules = Rules::new(Some(&bar("c", "si", |t| t.cpu = steps(&[(90, "#112233")]))));
        let mut alerts = Alerts::default();
        assert!(alerts.colors(&rules, &cpu(95)).cpu.is_some());
        assert_eq!(alerts.colors(&rules, &HostStats::default()).cpu, None);
        // The step was cleared, so 89 doesn't hold anything.
        assert_eq!(alerts.colors(&rules, &cpu(89)).cpu, None);
    }

    #[test]
    fn changed_rules_or_units_start_afresh() {
        let a = Rules::new(Some(&bar("c", "si", |t| t.cpu = steps(&[(90, "#112233")]))));
        let b = Rules::new(Some(&bar("c", "si", |t| t.cpu = steps(&[(91, "#112233")]))));
        let mut alerts = Alerts::default();
        assert!(alerts.colors(&a, &cpu(95)).cpu.is_some());
        // 89 would be held by `a`'s step, but the rules changed.
        assert_eq!(alerts.colors(&b, &cpu(89)).cpu, None);

        let c = Rules::new(Some(&bar("c", "si", |t| {
            t.cpu_temp = steps(&[(85, "#112233")])
        })));
        let f = Rules::new(Some(&bar("f", "si", |t| {
            t.cpu_temp = steps(&[(185, "#112233")])
        })));
        let mut alerts = Alerts::default();
        assert!(alerts.colors(&c, &cpu_temp(90)).cpu_temp.is_some());
        // 84 C would be held (hysteresis 2); 183 F is held by nothing after the switch.
        assert_eq!(alerts.colors(&f, &cpu_temp(84)).cpu_temp, None);
        assert_eq!(f.units().temp, TempUnit::F);
    }

    #[test]
    fn an_empty_list_never_colors_even_where_defaults_would() {
        let rules = Rules::new(Some(&bar("c", "si", |t| {
            t.cpu = Some(Vec::new());
            t.cpu_temp = Some(Vec::new());
        })));
        let both = HostStats {
            cpu: Some(100),
            cpu_temp: Some(150),
            ..HostStats::default()
        };
        assert_eq!(once(&rules, &both), Colors::default());
    }
}
