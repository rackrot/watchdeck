//! Which GPUs this host has and how to read them, from sysfs and procfs. Both roots are
//! arguments (`/sys`, `/proc`), so tests build fake trees. Card numbers (`card0`) are never used:
//! they say nothing about which GPU is which. NVIDIA readings come from NVML, not from here.

pub mod nvidia;

use std::fs;
use std::path::Path;

use serde::Serialize;

use crate::daemon::config::GpuChoice;

/// One display controller on the PCI bus.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct GpuInfo {
    /// The PCI address as `dddd:bb:dd.f`, lowercase.
    pub id: String,
    /// For the editor only.
    pub name: String,
    /// The bound kernel driver, such as `nvidia` or `amdgpu`. `None` when none is bound.
    pub driver: Option<String>,
    /// Whether the daemon can read its busy share: `nvidia` (through NVML, which may still fail to
    /// open) and `amdgpu` with `gpu_busy_percent`. Others are listed but can't be picked.
    pub readable: bool,
    pub boot_vga: bool,
}

/// What `list_gpus` returns.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct GpuList {
    pub gpus: Vec<GpuInfo>,
    /// The id `auto` picks now, if any GPU is readable.
    pub auto: Option<String>,
}

/// A GPU's busy share and temperature as the source gave them: a bad value is not corrected here.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct GpuReading {
    pub busy: Option<u32>,
    /// Whole °C. Signed, so a sensor reading -273 is passed on as it is.
    pub temp: Option<i32>,
}

impl GpuList {
    pub fn scan(sys: &Path, proc_root: &Path) -> Self {
        let gpus = enumerate(sys, proc_root);
        let auto = auto_pick(&gpus).map(|g| g.id.clone());
        Self { gpus, auto }
    }
}

/// Every PCI device of class 0x03 (display controller), sorted by address.
pub fn enumerate(sys: &Path, proc_root: &Path) -> Vec<GpuInfo> {
    let devices = sys.join("bus/pci/devices");
    let Ok(entries) = fs::read_dir(&devices) else {
        return Vec::new();
    };
    let mut gpus: Vec<GpuInfo> = entries
        .flatten()
        .filter_map(|e| {
            let id = e.file_name().into_string().ok()?;
            let dir = e.path();
            let class = fs::read_to_string(dir.join("class")).ok()?;
            if !class.trim().starts_with("0x03") {
                return None;
            }
            let driver = fs::read_link(dir.join("driver"))
                .ok()
                .and_then(|t| Some(t.file_name()?.to_string_lossy().into_owned()));
            let boot_vga = fs::read_to_string(dir.join("boot_vga")).is_ok_and(|v| v.trim() == "1");
            let readable = match driver.as_deref() {
                Some("nvidia") => true,
                Some("amdgpu") => dir.join("gpu_busy_percent").is_file(),
                _ => false,
            };
            let name = name_of(&dir, &id, driver.as_deref(), proc_root);
            Some(GpuInfo {
                id,
                name,
                driver,
                readable,
                boot_vga,
            })
        })
        .collect();
    gpus.sort_by(|a, b| a.id.cmp(&b.id));
    gpus
}

/// The readable GPU with `boot_vga`, else the readable one with the lowest address.
pub fn auto_pick(gpus: &[GpuInfo]) -> Option<&GpuInfo> {
    let mut readable = gpus.iter().filter(|g| g.readable);
    let first = readable.next()?;
    Some(
        std::iter::once(first)
            .chain(readable)
            .find(|g| g.boot_vga)
            .unwrap_or(first),
    )
}

/// Busy share and temperature of the AMD GPU at `pci`, from its own `gpu_busy_percent` and hwmon.
/// Anything but a PCI address (`../..`, an absolute path) reads as nothing.
///
/// Blocking sysfs I/O: call it off the async runtime.
pub fn read_amd(sys: &Path, pci: &str) -> GpuReading {
    if !matches!(GpuChoice::parse(pci), Some(GpuChoice::Pci(_))) {
        return GpuReading::default();
    }
    let dir = sys.join("bus/pci/devices").join(pci);
    GpuReading {
        busy: fs::read_to_string(dir.join("gpu_busy_percent"))
            .ok()
            .and_then(|v| v.trim().parse().ok()),
        temp: crate::daemon::hwmon::gpu_temp(&dir),
    }
}

fn name_of(dir: &Path, id: &str, driver: Option<&str>, proc_root: &Path) -> String {
    let found = match driver {
        // The driver's own file, so listing never starts NVML.
        Some("nvidia") => {
            let info = proc_root
                .join("driver/nvidia/gpus")
                .join(id)
                .join("information");
            fs::read_to_string(info).ok().and_then(|text| {
                let model = text.lines().find_map(|l| l.strip_prefix("Model:"))?;
                Some(model.trim().to_string())
            })
        }
        Some("amdgpu") => fs::read_to_string(dir.join("product_name"))
            .ok()
            .map(|n| n.trim().to_string()),
        _ => None,
    };
    found.filter(|n| !n.is_empty()).unwrap_or_else(|| {
        match driver {
            Some("nvidia") => "NVIDIA GPU",
            Some("amdgpu") => "AMD GPU",
            Some("i915" | "xe") => "Intel GPU",
            _ => "GPU",
        }
        .to_string()
    })
}

#[cfg(test)]
pub(crate) mod testing {
    use std::fs;
    use std::path::Path;

    /// Writes `text` to `root/rel`, creating directories. As in the kernel, a device
    /// (`bus/pci/devices/<addr>`) or hwmon (`class/hwmon/<hwmonN>`) entry is a symlink into a
    /// sibling `devices/` tree, made on first use.
    pub fn put(root: &Path, rel: &str, text: &str) {
        let parts: Vec<&str> = rel.split('/').collect();
        let entry = match parts.as_slice() {
            ["bus", "pci", "devices", _, ..] => Some(4),
            ["class", "hwmon", _, ..] => Some(3),
            _ => None,
        };
        let path = match entry {
            Some(n) => {
                let link = root.join(parts[..n].join("/"));
                let real = root.join("devices").join(parts[..n].join("/"));
                if !link.exists() {
                    fs::create_dir_all(&real).unwrap();
                    fs::create_dir_all(link.parent().unwrap()).unwrap();
                    // Relative, from the link's directory back up to the root.
                    let up = "../".repeat(n - 1);
                    std::os::unix::fs::symlink(
                        format!("{up}devices/{}", parts[..n].join("/")),
                        &link,
                    )
                    .unwrap();
                }
                root.join("devices").join(rel)
            }
            None => root.join(rel),
        };
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, text).unwrap();
    }
}

#[cfg(test)]
mod tests {
    use super::testing::put;
    use super::*;
    use std::os::unix::fs::symlink;

    /// A PCI device with `class`, and a real `driver` symlink to `drivers/<driver>` when given.
    fn pci(sys: &Path, id: &str, class: &str, driver: Option<&str>, boot_vga: Option<&str>) {
        let dev = format!("bus/pci/devices/{id}");
        put(sys, &format!("{dev}/class"), &format!("{class}\n"));
        if let Some(v) = boot_vga {
            put(sys, &format!("{dev}/boot_vga"), &format!("{v}\n"));
        }
        if let Some(d) = driver {
            let drivers = sys.join("bus/pci/drivers").join(d);
            fs::create_dir_all(&drivers).unwrap();
            // The same relative form the kernel uses.
            symlink(
                format!("../../../../bus/pci/drivers/{d}"),
                sys.join(&dev).join("driver"),
            )
            .unwrap();
        }
    }

    fn amd_hwmon(sys: &Path, id: &str, hwmon: &str, temps: &[(&str, &str)]) {
        let dir = format!("bus/pci/devices/{id}/hwmon/{hwmon}");
        put(sys, &format!("{dir}/name"), "amdgpu\n");
        for (i, (label, milli)) in temps.iter().enumerate() {
            put(
                sys,
                &format!("{dir}/temp{}_label", i + 1),
                &format!("{label}\n"),
            );
            put(
                sys,
                &format!("{dir}/temp{}_input", i + 1),
                &format!("{milli}\n"),
            );
        }
    }

    /// This desktop: the NVIDIA boot GPU at 01:00.0 with no hwmon, an AMD iGPU at 75:00.0, and
    /// devices of other classes around them.
    fn desktop(sys: &Path, proc_root: &Path) {
        pci(sys, "0000:00:00.0", "0x060000", None, None);
        pci(sys, "0000:01:00.0", "0x030000", Some("nvidia"), Some("1"));
        pci(sys, "0000:01:00.1", "0x040300", Some("snd_audio"), None);
        pci(sys, "0000:75:00.0", "0x030000", Some("amdgpu"), Some("0"));
        put(sys, "bus/pci/devices/0000:75:00.0/gpu_busy_percent", "7\n");
        amd_hwmon(sys, "0000:75:00.0", "hwmon8", &[("edge", "39000")]);
        put(
            proc_root,
            "driver/nvidia/gpus/0000:01:00.0/information",
            "Model: \t\t NVIDIA GeForce RTX 4070\nIRQ:   \t\t 160\nGPU UUID: \t GPU-00000000-0000-0000-0000-000000000000\n",
        );
    }

    fn trees() -> (tempfile::TempDir, tempfile::TempDir) {
        (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap())
    }

    #[test]
    fn enumeration_finds_both_cards_and_nothing_else() {
        let (sys, proc_root) = trees();
        desktop(sys.path(), proc_root.path());
        let gpus = enumerate(sys.path(), proc_root.path());
        assert_eq!(
            gpus,
            vec![
                GpuInfo {
                    id: "0000:01:00.0".into(),
                    name: "NVIDIA GeForce RTX 4070".into(),
                    driver: Some("nvidia".into()),
                    readable: true,
                    boot_vga: true,
                },
                GpuInfo {
                    id: "0000:75:00.0".into(),
                    name: "AMD GPU".into(),
                    driver: Some("amdgpu".into()),
                    readable: true,
                    boot_vga: false,
                },
            ]
        );
    }

    #[test]
    fn auto_picks_the_boot_vga_gpu() {
        let (sys, proc_root) = trees();
        desktop(sys.path(), proc_root.path());
        let list = GpuList::scan(sys.path(), proc_root.path());
        assert_eq!(list.auto.as_deref(), Some("0000:01:00.0"));
    }

    #[test]
    fn auto_without_boot_vga_picks_the_lowest_address() {
        let (sys, proc_root) = trees();
        pci(
            sys.path(),
            "0000:75:00.0",
            "0x030000",
            Some("amdgpu"),
            Some("0"),
        );
        put(
            sys.path(),
            "bus/pci/devices/0000:75:00.0/gpu_busy_percent",
            "0\n",
        );
        pci(sys.path(), "0000:03:00.0", "0x030200", Some("amdgpu"), None);
        put(
            sys.path(),
            "bus/pci/devices/0000:03:00.0/gpu_busy_percent",
            "0\n",
        );
        let list = GpuList::scan(sys.path(), proc_root.path());
        assert_eq!(list.auto.as_deref(), Some("0000:03:00.0"));
    }

    #[test]
    fn auto_skips_an_unreadable_boot_vga_gpu() {
        let (sys, proc_root) = trees();
        pci(
            sys.path(),
            "0000:00:02.0",
            "0x030000",
            Some("i915"),
            Some("1"),
        );
        pci(
            sys.path(),
            "0000:01:00.0",
            "0x030000",
            Some("nvidia"),
            Some("0"),
        );
        let list = GpuList::scan(sys.path(), proc_root.path());
        assert_eq!(list.auto.as_deref(), Some("0000:01:00.0"));
    }

    #[test]
    fn intel_is_listed_but_not_readable() {
        let (sys, proc_root) = trees();
        pci(
            sys.path(),
            "0000:00:02.0",
            "0x030000",
            Some("i915"),
            Some("1"),
        );
        let list = GpuList::scan(sys.path(), proc_root.path());
        assert_eq!(list.gpus.len(), 1);
        assert_eq!(list.gpus[0].driver.as_deref(), Some("i915"));
        assert_eq!(list.gpus[0].name, "Intel GPU");
        assert!(!list.gpus[0].readable);
        assert_eq!(list.auto, None);
    }

    #[test]
    fn amdgpu_without_gpu_busy_percent_is_not_readable() {
        let (sys, proc_root) = trees();
        pci(sys.path(), "0000:75:00.0", "0x030000", Some("amdgpu"), None);
        let gpus = enumerate(sys.path(), proc_root.path());
        assert!(!gpus[0].readable);
    }

    #[test]
    fn a_device_with_no_driver_is_listed_unreadable() {
        let (sys, proc_root) = trees();
        pci(sys.path(), "0000:05:00.0", "0x030000", None, None);
        let gpus = enumerate(sys.path(), proc_root.path());
        assert_eq!(gpus[0].driver, None);
        assert_eq!(gpus[0].name, "GPU");
        assert!(!gpus[0].readable && !gpus[0].boot_vga);
    }

    #[test]
    fn no_gpu_gives_an_empty_list() {
        let (sys, proc_root) = trees();
        pci(sys.path(), "0000:00:00.0", "0x060000", None, None);
        let list = GpuList::scan(sys.path(), proc_root.path());
        assert_eq!(
            list,
            GpuList {
                gpus: vec![],
                auto: None
            }
        );
        // And a root with no PCI bus at all.
        let empty = GpuList::scan(Path::new("/nonexistent-sysfs"), proc_root.path());
        assert!(empty.gpus.is_empty());
    }

    #[test]
    fn the_nvidia_name_falls_back_without_the_information_file() {
        let (sys, proc_root) = trees();
        pci(
            sys.path(),
            "0000:01:00.0",
            "0x030000",
            Some("nvidia"),
            Some("1"),
        );
        let gpus = enumerate(sys.path(), proc_root.path());
        assert_eq!(gpus[0].name, "NVIDIA GPU");
    }

    #[test]
    fn an_amd_name_comes_from_product_name_when_there_is_one() {
        let (sys, proc_root) = trees();
        pci(sys.path(), "0000:75:00.0", "0x030000", Some("amdgpu"), None);
        put(
            sys.path(),
            "bus/pci/devices/0000:75:00.0/product_name",
            "AMD Radeon RX 7800 XT\n",
        );
        let gpus = enumerate(sys.path(), proc_root.path());
        assert_eq!(gpus[0].name, "AMD Radeon RX 7800 XT");
    }

    #[test]
    fn the_igpu_reads_its_own_hwmon_not_another_cards() {
        let (sys, proc_root) = trees();
        desktop(sys.path(), proc_root.path());
        // A dGPU with a hotter sensor, beside the iGPU.
        pci(sys.path(), "0000:03:00.0", "0x030000", Some("amdgpu"), None);
        put(
            sys.path(),
            "bus/pci/devices/0000:03:00.0/gpu_busy_percent",
            "88\n",
        );
        amd_hwmon(
            sys.path(),
            "0000:03:00.0",
            "hwmon3",
            &[("edge", "70000"), ("junction", "95000")],
        );
        assert_eq!(
            read_amd(sys.path(), "0000:75:00.0"),
            GpuReading {
                busy: Some(7),
                temp: Some(39)
            }
        );
        assert_eq!(
            read_amd(sys.path(), "0000:03:00.0"),
            GpuReading {
                busy: Some(88),
                temp: Some(95)
            }
        );
    }

    #[test]
    fn an_amd_dgpu_junction_beats_edge() {
        let (sys, _) = trees();
        pci(sys.path(), "0000:03:00.0", "0x030000", Some("amdgpu"), None);
        put(
            sys.path(),
            "bus/pci/devices/0000:03:00.0/gpu_busy_percent",
            "12\n",
        );
        amd_hwmon(
            sys.path(),
            "0000:03:00.0",
            "hwmon3",
            &[("edge", "52000"), ("junction", "61000")],
        );
        assert_eq!(read_amd(sys.path(), "0000:03:00.0").temp, Some(61));
    }

    #[test]
    fn bad_amd_values_come_back_as_they_are() {
        let (sys, _) = trees();
        pci(sys.path(), "0000:03:00.0", "0x030000", Some("amdgpu"), None);
        put(
            sys.path(),
            "bus/pci/devices/0000:03:00.0/gpu_busy_percent",
            "250\n",
        );
        amd_hwmon(sys.path(), "0000:03:00.0", "hwmon3", &[("edge", "-273000")]);
        assert_eq!(
            read_amd(sys.path(), "0000:03:00.0"),
            GpuReading {
                busy: Some(250),
                temp: Some(-273)
            }
        );
    }

    #[test]
    fn read_amd_refuses_anything_but_a_pci_address() {
        let (sys, other) = trees();
        // `bus/pci/devices/../../x` is `bus/x`, inside the root but not a device.
        put(sys.path(), "bus/x/gpu_busy_percent", "5\n");
        assert_eq!(read_amd(sys.path(), "../../x"), GpuReading::default());
        // An absolute path replaces the join's base entirely.
        put(other.path(), "gpu_busy_percent", "6\n");
        let abs = other.path().to_str().unwrap();
        assert_eq!(read_amd(sys.path(), abs), GpuReading::default());
    }

    #[test]
    fn a_missing_card_reads_as_nothing() {
        let (sys, _) = trees();
        assert_eq!(read_amd(sys.path(), "0000:99:00.0"), GpuReading::default());
    }
}
