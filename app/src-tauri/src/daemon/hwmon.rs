//! Temperatures from hwmon. Directories are found by their `name` file, never by number, since
//! the numbers are assigned at boot. Values come back as the sensor gave them, in whole °C; a
//! sensor that reads nonsense (511 °C, -273 °C) is the caller's to guard.

use std::fs;
use std::path::{Path, PathBuf};

/// The CPU's temperature in whole °C, under the sysfs root `sys` (`/sys`). `k10temp` and
/// `zenpower`: `Tdie` if present (older Zen parts add an offset to `Tctl`), else `Tctl`.
/// `coretemp`: the hottest `Package id N`. `None` without any of these.
pub fn cpu_temp(sys: &Path) -> Option<i32> {
    let (mut tdie, mut tctl, mut package) = (None, None, None::<i32>);
    for dir in hwmon_dirs(&sys.join("class/hwmon")) {
        match read_trimmed(&dir.join("name")).as_deref() {
            Some("k10temp" | "zenpower") => {
                tdie = tdie.or(labeled(&dir, |l| l == "Tdie"));
                tctl = tctl.or(labeled(&dir, |l| l == "Tctl"));
            }
            Some("coretemp") => {
                for (_, c) in temps(&dir).filter(|(l, _)| l.starts_with("Package id")) {
                    package = package.max(Some(c));
                }
            }
            _ => {}
        }
    }
    tdie.or(tctl).or(package)
}

/// An AMD GPU's temperature in whole °C, from the hwmon directories under its own PCI device
/// directory `device` (`.../devices/0000:75:00.0`): `junction` if present, else `edge`, else
/// `temp1`. It never looks at other devices' sensors, so an iGPU and a dGPU don't mix.
pub fn gpu_temp(device: &Path) -> Option<i32> {
    let dirs = hwmon_dirs(&device.join("hwmon"));
    let find = |want: &dyn Fn(&str) -> bool| dirs.iter().find_map(|d| labeled(d, want));
    find(&|l| l == "junction")
        .or_else(|| find(&|l| l == "edge"))
        .or_else(|| dirs.iter().find_map(|d| temp_input(d, 1)))
}

/// The `hwmon*` subdirectories of `parent`, sorted so the choice among several is stable.
fn hwmon_dirs(parent: &Path) -> Vec<PathBuf> {
    let Ok(entries) = fs::read_dir(parent) else {
        return Vec::new();
    };
    let mut dirs: Vec<PathBuf> = entries
        .flatten()
        .filter(|e| e.file_name().to_string_lossy().starts_with("hwmon"))
        .map(|e| e.path())
        .collect();
    dirs.sort();
    dirs
}

/// Every `tempN_input` in `dir` that has a `tempN_label`, as (label, whole °C), in order of N.
fn temps(dir: &Path) -> impl Iterator<Item = (String, i32)> + '_ {
    (1..=64).filter_map(move |n| {
        let label = read_trimmed(&dir.join(format!("temp{n}_label")))?;
        Some((label, temp_input(dir, n)?))
    })
}

/// The first temperature in `dir` whose label passes `want`.
fn labeled(dir: &Path, want: impl Fn(&str) -> bool) -> Option<i32> {
    temps(dir).find(|(l, _)| want(l)).map(|(_, c)| c)
}

fn temp_input(dir: &Path, n: u32) -> Option<i32> {
    let milli: i64 = read_trimmed(&dir.join(format!("temp{n}_input")))?
        .parse()
        .ok()?;
    // Nearest whole °C, halves away from zero.
    let whole = (milli as f64 / 1000.0).round();
    Some(whole.clamp(i32::MIN as f64, i32::MAX as f64) as i32)
}

fn read_trimmed(path: &Path) -> Option<String> {
    Some(fs::read_to_string(path).ok()?.trim().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::daemon::gpu::testing::put;

    fn sensor(sys: &Path, hwmon: &str, name: &str, temps: &[(&str, &str)]) {
        let dir = format!("class/hwmon/{hwmon}");
        put(sys, &format!("{dir}/name"), name);
        for (i, (label, milli)) in temps.iter().enumerate() {
            put(sys, &format!("{dir}/temp{}_label", i + 1), label);
            put(sys, &format!("{dir}/temp{}_input", i + 1), milli);
        }
    }

    /// This desktop: k10temp among nvme, spd5118 and the rest, with no `Tdie`.
    fn desktop(sys: &Path) {
        sensor(sys, "hwmon0", "nvme", &[("Composite", "36850")]);
        sensor(sys, "hwmon3", "prom21_xhci", &[]);
        sensor(
            sys,
            "hwmon4",
            "k10temp",
            &[("Tctl", "64000"), ("Tccd1", "50900"), ("Tccd2", "37500")],
        );
        sensor(sys, "hwmon6", "spd5118", &[("temp1", "30250")]);
        sensor(sys, "hwmon8", "amdgpu", &[("edge", "39000")]);
    }

    #[test]
    fn k10temp_gives_tctl_when_there_is_no_tdie() {
        let sys = tempfile::tempdir().unwrap();
        desktop(sys.path());
        assert_eq!(cpu_temp(sys.path()), Some(64));
    }

    #[test]
    fn tdie_beats_tctl() {
        let sys = tempfile::tempdir().unwrap();
        sensor(
            sys.path(),
            "hwmon2",
            "k10temp",
            &[("Tctl", "75000"), ("Tdie", "48000")],
        );
        assert_eq!(cpu_temp(sys.path()), Some(48));
    }

    #[test]
    fn coretemp_gives_the_hotter_package() {
        let sys = tempfile::tempdir().unwrap();
        // Two packages, as two hwmon directories; a core hotter than both is not a package.
        sensor(
            sys.path(),
            "hwmon1",
            "coretemp",
            &[("Package id 0", "55000"), ("Core 0", "90000")],
        );
        sensor(
            sys.path(),
            "hwmon2",
            "coretemp",
            &[("Package id 1", "61500")],
        );
        assert_eq!(cpu_temp(sys.path()), Some(62));
    }

    #[test]
    fn no_sensor_gives_none() {
        let sys = tempfile::tempdir().unwrap();
        assert_eq!(cpu_temp(sys.path()), None);
        sensor(sys.path(), "hwmon0", "nvme", &[("Composite", "36850")]);
        assert_eq!(cpu_temp(sys.path()), None);
    }

    #[test]
    fn renumbered_directories_still_find_the_sensor() {
        for number in ["hwmon0", "hwmon4", "hwmon17"] {
            let sys = tempfile::tempdir().unwrap();
            sensor(sys.path(), "hwmon1", "nvme", &[("Composite", "30000")]);
            sensor(sys.path(), "hwmon9", "spd5118", &[("temp1", "31000")]);
            sensor(sys.path(), number, "k10temp", &[("Tctl", "70000")]);
            assert_eq!(cpu_temp(sys.path()), Some(70), "{number}");
        }
    }

    #[test]
    fn millidegrees_round_to_the_nearest_whole_degree() {
        let sys = tempfile::tempdir().unwrap();
        sensor(sys.path(), "hwmon0", "k10temp", &[("Tctl", "64875")]);
        assert_eq!(cpu_temp(sys.path()), Some(65));
    }

    #[test]
    fn bad_values_come_back_as_they_are() {
        let sys = tempfile::tempdir().unwrap();
        sensor(sys.path(), "hwmon0", "k10temp", &[("Tctl", "511000")]);
        assert_eq!(cpu_temp(sys.path()), Some(511));
        sensor(sys.path(), "hwmon0", "k10temp", &[("Tctl", "-273000")]);
        assert_eq!(cpu_temp(sys.path()), Some(-273));
    }

    #[test]
    fn an_unreadable_value_gives_none() {
        let sys = tempfile::tempdir().unwrap();
        sensor(sys.path(), "hwmon0", "k10temp", &[("Tctl", "n/a")]);
        assert_eq!(cpu_temp(sys.path()), None);
    }

    fn card(dev: &Path, hwmon: &str, temps: &[(&str, &str)]) {
        let dir = format!("hwmon/{hwmon}");
        put(dev, &format!("{dir}/name"), "amdgpu");
        for (i, (label, milli)) in temps.iter().enumerate() {
            put(dev, &format!("{dir}/temp{}_label", i + 1), label);
            put(dev, &format!("{dir}/temp{}_input", i + 1), milli);
        }
    }

    #[test]
    fn junction_beats_edge() {
        let dev = tempfile::tempdir().unwrap();
        card(
            dev.path(),
            "hwmon3",
            &[("edge", "52000"), ("junction", "61000"), ("mem", "58000")],
        );
        assert_eq!(gpu_temp(dev.path()), Some(61));
    }

    #[test]
    fn edge_alone_is_used_and_so_is_an_unlabeled_temp1() {
        let dev = tempfile::tempdir().unwrap();
        card(dev.path(), "hwmon8", &[("edge", "39000")]);
        assert_eq!(gpu_temp(dev.path()), Some(39));

        let bare = tempfile::tempdir().unwrap();
        put(bare.path(), "hwmon/hwmon1/temp1_input", "44000");
        assert_eq!(gpu_temp(bare.path()), Some(44));
    }

    #[test]
    fn a_card_without_hwmon_gives_none() {
        let dev = tempfile::tempdir().unwrap();
        assert_eq!(gpu_temp(dev.path()), None);
    }
}
