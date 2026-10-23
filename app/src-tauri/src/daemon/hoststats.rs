//! Host load for the device's status bar: CPU, memory and network rates, read from `/proc`, and
//! the CPU and GPU temperatures, the GPU's busy share, the units and the threshold colors.
//!
//! The parsers are pure. A [`Sampler`] reads the files under a root (`/proc`, or a fixture
//! directory in tests) and turns the counters of two reads into rates. [`spawn`] samples once a
//! second, only while a session is open. A [`Bar`] adds the rest to each sample: it reads sysfs
//! off the async runtime, asks the NVML thread for the NVIDIA reading, and works out the colors.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

use time::{OffsetDateTime, UtcOffset};
use tokio::sync::watch;
use tokio::time::{interval_at, Instant, MissedTickBehavior};

use super::alerts::{Alerts, Rules};
use super::config::{GpuChoice, Live};
use super::gpu::nvidia::{NvidiaOpener, NvidiaReader, Period, SystemNvml, Timings};
use super::gpu::{self, GpuReading};
use super::hwmon;
use super::protocol::Colors;
use super::units::Units;

const PERIOD: Duration = Duration::from_secs(1);
/// `RTF_UP` in the flags column of `/proc/net/route`.
const RTF_UP: u32 = 0x1;

/// One reading. Each part is `None` when it could not be read or needs an earlier sample.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct HostStats {
    /// Busy share of all CPUs since the last sample, 0 to 100.
    pub cpu: Option<u8>,
    pub mem: Option<Mem>,
    /// Bytes per second on the default-route interface since the last sample.
    pub net: Option<Net>,
    /// The CPU's temperature in whole °C, as the sensor gave it.
    pub cpu_temp: Option<i32>,
    /// The selected GPU, as its source gave it. `None` with `gpu = "off"` or no readable GPU.
    pub gpu: Option<GpuReading>,
    /// How the board shows temperatures and rates, from the config at this sample.
    pub units: Units,
    /// The color of each value past a threshold.
    pub colors: Colors,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Mem {
    pub used: u64,
    pub total: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Net {
    pub rx: u64,
    pub tx: u64,
}

/// The jiffy counters of `/proc/stat`'s `cpu` line.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CpuTimes {
    idle: u64,
    total: u64,
}

/// The `cpu` line of `/proc/stat`: user, nice, system, idle, iowait, irq, softirq, steal. Guest
/// time is already inside user and nice. Older kernels stop after iowait, so those count as 0.
pub fn parse_cpu(stat: &str) -> Option<CpuTimes> {
    let line = stat
        .lines()
        .find(|l| l.split_whitespace().next() == Some("cpu"))?;
    let mut fields = [0u64; 8];
    let mut count = 0;
    for (slot, value) in fields.iter_mut().zip(line.split_whitespace().skip(1)) {
        *slot = value.parse().ok()?;
        count += 1;
    }
    if count < 4 {
        return None;
    }
    let total = fields.iter().try_fold(0u64, |sum, v| sum.checked_add(*v))?;
    Some(CpuTimes {
        idle: fields[3].checked_add(fields[4])?,
        total,
    })
}

/// The busy share between two reads, rounded to the nearest percent. `None` when time did not move
/// forward or a counter went backwards.
pub fn cpu_percent(prev: CpuTimes, cur: CpuTimes) -> Option<u8> {
    let total = cur.total.checked_sub(prev.total).filter(|t| *t > 0)?;
    let idle = cur.idle.checked_sub(prev.idle)?;
    let busy = u128::from(total.saturating_sub(idle));
    let pct = (busy * 100 + u128::from(total) / 2) / u128::from(total);
    Some(pct.min(100) as u8)
}

/// `MemTotal` and `MemAvailable` of `/proc/meminfo`, in bytes. `None` without either.
pub fn parse_meminfo(meminfo: &str) -> Option<Mem> {
    let kib = |key: &str| -> Option<u64> {
        let line = meminfo.lines().find_map(|l| l.strip_prefix(key))?;
        line.strip_prefix(':')?
            .split_whitespace()
            .next()?
            .parse::<u64>()
            .ok()?
            .checked_mul(1024)
    };
    let total = kib("MemTotal")?;
    let available = kib("MemAvailable")?;
    Some(Mem {
        used: total.saturating_sub(available),
        total,
    })
}

/// The interface of the default route in `/proc/net/route`: destination `00000000`, `RTF_UP`, the
/// lowest metric (the first of equals).
pub fn parse_default_iface(route: &str) -> Option<&str> {
    route
        .lines()
        .skip(1)
        .filter_map(|line| {
            let mut f = line.split_whitespace();
            let iface = f.next()?;
            let dest = f.next()?;
            let flags = u32::from_str_radix(f.nth(1)?, 16).ok()?; // after the gateway
            let metric = f.nth(2).and_then(|m| m.parse::<u32>().ok())?; // after RefCnt, Use
            (dest == "00000000" && flags & RTF_UP != 0).then_some((metric, iface))
        })
        .min_by_key(|(metric, _)| *metric)
        .map(|(_, iface)| iface)
}

/// The receive and transmit byte counters of `iface` in `/proc/net/dev`.
pub fn parse_net_dev(net_dev: &str, iface: &str) -> Option<(u64, u64)> {
    let line = net_dev.lines().skip(2).find_map(|l| {
        let (name, rest) = l.split_once(':')?;
        (name.trim() == iface).then_some(rest)
    })?;
    let mut f = line.split_whitespace();
    let rx = f.next()?.parse().ok()?;
    let tx = f.nth(7)?.parse().ok()?;
    Some((rx, tx))
}

/// Bytes per second between two counter reads, rounded down. `None` for a counter that went
/// backwards (a wrap or a reset) or no time between the reads.
fn rate(prev: u64, cur: u64, elapsed: Duration) -> Option<u64> {
    let delta = cur.checked_sub(prev)?;
    let ms = elapsed.as_millis().max(1);
    u64::try_from(u128::from(delta) * 1000 / ms).ok()
}

struct Previous {
    at: Instant,
    cpu: Option<CpuTimes>,
    /// The interface and its counters.
    net: Option<(String, u64, u64)>,
}

/// Reads host counters under a root and keeps the previous read, so each call gives rates.
pub struct Sampler {
    root: PathBuf,
    prev: Option<Previous>,
}

impl Sampler {
    /// Reads the files under `root`: `stat`, `meminfo`, `net/route` and `net/dev`.
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self {
            root: root.into(),
            prev: None,
        }
    }

    fn read(&self, name: &str) -> Option<String> {
        std::fs::read_to_string(self.root.join(name))
            .inspect_err(|e| log::debug!("host stats: reading {name}: {e}"))
            .ok()
    }

    /// Reads the counters now (`at` is the time of the read). The first call has no CPU or
    /// network rates, since those need an earlier read.
    pub fn sample(&mut self, at: Instant) -> HostStats {
        let cpu = self.read("stat").as_deref().and_then(parse_cpu);
        let mem = self.read("meminfo").as_deref().and_then(parse_meminfo);
        let net = self
            .read("net/route")
            .zip(self.read("net/dev"))
            .and_then(|(route, dev)| {
                let iface = parse_default_iface(&route)?;
                let (rx, tx) = parse_net_dev(&dev, iface)?;
                Some((iface.to_owned(), rx, tx))
            });

        let mut stats = HostStats {
            mem,
            ..HostStats::default()
        };
        if let Some(prev) = &self.prev {
            let elapsed = at.saturating_duration_since(prev.at);
            stats.cpu = prev.cpu.zip(cpu).and_then(|(p, c)| cpu_percent(p, c));
            stats.net = match (&prev.net, &net) {
                (Some((pi, prx, ptx)), Some((ci, crx, ctx))) if pi == ci => {
                    rate(*prx, *crx, elapsed)
                        .zip(rate(*ptx, *ctx, elapsed))
                        .map(|(rx, tx)| Net { rx, tx })
                }
                _ => None,
            };
        }
        self.prev = Some(Previous { at, cpu, net });
        stats
    }
}

/// The host's UTC offset in seconds at `now`, for the `stats` frame's `utc_offset`. `None` when
/// the platform can't tell; that is logged once, and the board then shows UTC.
pub fn utc_offset_seconds(now: OffsetDateTime) -> Option<i32> {
    static WARNED: AtomicBool = AtomicBool::new(false);
    match UtcOffset::local_offset_at(now) {
        Ok(offset) => Some(offset.whole_seconds()),
        Err(e) => {
            if !WARNED.swap(true, Ordering::Relaxed) {
                log::warn!("host stats: no local UTC offset, the device will show UTC: {e}");
            }
            None
        }
    }
}

/// Where the sampler reads from. The system's in the app, fixture trees and a fake NVML in tests.
pub struct HostSources {
    pub proc_root: PathBuf,
    pub sys_root: PathBuf,
    pub nvidia: Arc<dyn NvidiaOpener>,
}

impl HostSources {
    /// `/proc`, `/sys` and the real NVML, which is loaded only when a period needs it.
    pub fn system() -> Self {
        Self {
            proc_root: "/proc".into(),
            sys_root: "/sys".into(),
            nvidia: Arc::new(SystemNvml::new()),
        }
    }
}

/// The GPU the status bar reads in one sampling period.
enum Selected {
    Nothing,
    /// The PCI address of an AMD GPU, read from sysfs.
    Amd(String),
    /// An NVIDIA GPU, read by the `nvml` thread for as long as the period lives.
    Nvidia(Period),
}

/// Everything beyond `/proc` in a sample: temperatures, the GPU, units and colors.
struct Bar {
    config: watch::Receiver<Arc<Live>>,
    proc_root: Arc<PathBuf>,
    sys_root: Arc<PathBuf>,
    nvidia: NvidiaReader,
    /// What `selected` was resolved for. `None` after a period ends.
    resolved_for: Option<GpuChoice>,
    selected: Selected,
    /// The sysfs read of an earlier tick that hasn't finished.
    sysfs: Option<tokio::task::JoinHandle<SysfsRead>>,
    /// Whether the stall of `sysfs` has been logged.
    sysfs_stuck: bool,
    alerts: Alerts,
}

/// The CPU temperature and, for an AMD selection, the GPU's reading.
type SysfsRead = (Option<i32>, Option<GpuReading>);
/// How long a tick waits for its sysfs read before it publishes without it.
const SYSFS_WAIT: Duration = Duration::from_millis(500);

impl Bar {
    fn new(config: watch::Receiver<Arc<Live>>, sources: HostSources, timings: Timings) -> Self {
        Self {
            config,
            proc_root: Arc::new(sources.proc_root),
            sys_root: Arc::new(sources.sys_root),
            nvidia: NvidiaReader::new(sources.nvidia, timings),
            resolved_for: None,
            selected: Selected::Nothing,
            sysfs: None,
            sysfs_stuck: false,
            alerts: Alerts::default(),
        }
    }

    fn choice(live: &Live) -> GpuChoice {
        (live.status_bar.as_ref())
            .and_then(|bar| bar.gpu.as_deref())
            .and_then(GpuChoice::parse)
            .unwrap_or(GpuChoice::Auto)
    }

    /// A period starts: resolves the GPU now, so an NVIDIA's reader is up before the first tick.
    async fn begin(&mut self) {
        let choice = Self::choice(&self.config.borrow());
        self.resolve(choice).await;
    }

    /// A period ends: drops the NVML handle (NVML shuts down, `/dev/nvidia*` closes) and the colors'
    /// state.
    fn end(&mut self) {
        self.selected = Selected::Nothing;
        self.resolved_for = None;
        // A read still blocked is left to finish on its own; the next period starts a new one.
        self.sysfs = None;
        self.alerts = Alerts::default();
    }

    async fn resolve(&mut self, choice: GpuChoice) {
        // The old one goes first, so a switch away from NVIDIA shuts NVML down. A read in flight
        // was for the old selection.
        self.selected = Selected::Nothing;
        self.sysfs = None;
        self.resolved_for = Some(choice.clone());
        if choice == GpuChoice::Off {
            return;
        }
        let (sys, proc_root) = (self.sys_root.clone(), self.proc_root.clone());
        let gpus = tokio::task::spawn_blocking(move || gpu::enumerate(&sys, &proc_root))
            .await
            .inspect_err(|e| log::warn!("host stats: listing GPUs: {e}"))
            .unwrap_or_default();
        let pick = match &choice {
            GpuChoice::Auto => gpu::auto_pick(&gpus),
            GpuChoice::Pci(id) => gpus.iter().find(|g| g.id == *id),
            GpuChoice::Off => None,
        };
        self.selected = match pick {
            None => {
                log::info!("host stats: no GPU for status_bar.gpu {choice:?}");
                Selected::Nothing
            }
            Some(g) if !g.readable => {
                log::info!("host stats: GPU {} can't be read ({:?})", g.id, g.driver);
                Selected::Nothing
            }
            Some(g) => match g.driver.as_deref() {
                Some("nvidia") => Selected::Nvidia(self.nvidia.start(&g.id)),
                Some("amdgpu") => Selected::Amd(g.id.clone()),
                _ => Selected::Nothing,
            },
        };
    }

    /// Adds the temperatures, the GPU, the units and the colors to `stats`.
    async fn sample(&mut self, mut stats: HostStats) -> HostStats {
        let live = self.config.borrow().clone();
        let choice = Self::choice(&live);
        if self.resolved_for.as_ref() != Some(&choice) {
            self.resolve(choice).await;
        }

        // sysfs reads block, so they run on the blocking pool. A read that takes longer than
        // `SYSFS_WAIT` (a GPU waking from runtime suspend) gives no values this tick and is
        // awaited again on the next, and no second one starts meanwhile.
        let sys = self.sys_root.clone();
        let amd = match &self.selected {
            Selected::Amd(pci) => Some(pci.clone()),
            _ => None,
        };
        let read = self.sysfs.get_or_insert_with(|| {
            tokio::task::spawn_blocking(move || {
                (
                    hwmon::cpu_temp(&sys),
                    amd.map(|pci| gpu::read_amd(&sys, &pci)),
                )
            })
        });
        let (cpu_temp, amd) = match tokio::time::timeout(SYSFS_WAIT, read).await {
            Ok(joined) => {
                self.sysfs = None;
                self.sysfs_stuck = false;
                joined
                    .inspect_err(|e| log::warn!("host stats: reading sysfs: {e}"))
                    .unwrap_or_default()
            }
            Err(_) => {
                if !std::mem::replace(&mut self.sysfs_stuck, true) {
                    log::warn!("host stats: a sysfs read is taking over {SYSFS_WAIT:?}");
                }
                (None, None)
            }
        };

        // Asked on every tick, since that is what starts a thread that waited for the last one.
        let gpu = match &self.selected {
            Selected::Nvidia(period) => period.latest(),
            Selected::Amd(_) => amd,
            Selected::Nothing => None,
        };
        stats.cpu_temp = cpu_temp;
        stats.gpu = gpu.filter(|g| g.busy.is_some() || g.temp.is_some());
        let rules = Rules::new(live.status_bar.as_ref());
        stats.units = rules.units();
        stats.colors = self.alerts.colors(&rules, &stats);
        stats
    }
}

/// Samples `/proc` and `sources` every second while `polling` (the number of open sessions) is
/// above 0, and publishes each reading. Publishes `None` when the count returns to 0, so a session
/// that opens later never gets a reading from an earlier period. The GPU, units and thresholds
/// come from the live `config`. Stops when `polling`'s sender is dropped. Must run inside a tokio
/// runtime.
pub fn spawn(
    polling: watch::Receiver<usize>,
    config: watch::Receiver<Arc<Live>>,
    sources: HostSources,
) -> watch::Receiver<Option<HostStats>> {
    let sampler = Sampler::new(sources.proc_root.clone());
    let bar = Bar::new(config, sources, Timings::default());
    spawn_task(sampler, Some(bar), polling)
}

/// Only `/proc`, no sysfs or GPU.
#[cfg(test)]
fn spawn_with(
    sampler: Sampler,
    polling: watch::Receiver<usize>,
) -> watch::Receiver<Option<HostStats>> {
    spawn_task(sampler, None, polling)
}

fn spawn_task(
    sampler: Sampler,
    bar: Option<Bar>,
    polling: watch::Receiver<usize>,
) -> watch::Receiver<Option<HostStats>> {
    let (tx, rx) = watch::channel(None);
    tokio::spawn(run(sampler, bar, polling, tx));
    rx
}

async fn run(
    mut sampler: Sampler,
    mut bar: Option<Bar>,
    mut polling: watch::Receiver<usize>,
    tx: watch::Sender<Option<HostStats>>,
) {
    loop {
        while *polling.borrow_and_update() == 0 {
            if polling.changed().await.is_err() {
                return;
            }
        }
        let mut tick = interval_at(Instant::now() + PERIOD, PERIOD);
        tick.set_missed_tick_behavior(MissedTickBehavior::Skip);
        if let Some(bar) = &mut bar {
            bar.begin().await;
        }
        loop {
            tokio::select! {
                _ = tick.tick() => {
                    let mut stats = sampler.sample(Instant::now());
                    if let Some(bar) = &mut bar {
                        stats = bar.sample(stats).await;
                    }
                    tx.send_replace(Some(stats));
                }
                changed = polling.changed() => {
                    if changed.is_err() {
                        return;
                    }
                    if *polling.borrow_and_update() == 0 {
                        break;
                    }
                }
            }
        }
        // The next period starts from scratch: no rate may span the gap.
        sampler.prev = None;
        if let Some(bar) = &mut bar {
            bar.end();
        }
        tx.send_replace(None);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const PROC: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/fixtures/proc");

    fn fixture(dir: &str, file: &str) -> String {
        std::fs::read_to_string(format!("{PROC}/{dir}/{file}")).unwrap()
    }

    fn stat(user: u64, idle: u64) -> String {
        format!("cpu  {user} 0 0 {idle} 0 0 0 0 0 0\ncpu0 1 2 3 4 5 6 7 8 9 10\n")
    }

    fn pct(a: &str, b: &str) -> Option<u8> {
        cpu_percent(parse_cpu(a)?, parse_cpu(b)?)
    }

    #[test]
    fn cpu_percent_from_known_deltas() {
        // 25 busy of 100 jiffies.
        assert_eq!(pct(&stat(100, 900), &stat(125, 975)), Some(25));
        // Rounds to nearest: 1 of 3.
        assert_eq!(pct(&stat(0, 0), &stat(1, 2)), Some(33));
        assert_eq!(pct(&stat(0, 0), &stat(100, 0)), Some(100));
        assert_eq!(pct(&stat(0, 0), &stat(0, 100)), Some(0));
    }

    #[test]
    fn cpu_counts_every_state_and_ignores_guest() {
        // user nice system idle iowait irq softirq steal guest guest_nice
        let a = parse_cpu("cpu  1 2 3 4 5 6 7 8 9 10\n").unwrap();
        assert_eq!((a.idle, a.total), (9, 36));
        // An old kernel's line stops after iowait.
        let old = parse_cpu("cpu 1 2 3 4 5\n").unwrap();
        assert_eq!((old.idle, old.total), (9, 15));
    }

    #[test]
    fn cpu_is_none_without_progress_or_after_a_backwards_counter() {
        let s = stat(100, 900);
        assert_eq!(pct(&s, &s), None);
        assert_eq!(pct(&stat(200, 900), &stat(100, 900)), None);
        // Total moves forward but idle went backwards.
        assert_eq!(pct(&stat(0, 100), &stat(200, 50)), None);
    }

    #[test]
    fn cpu_parse_refuses_malformed_lines() {
        assert!(parse_cpu("").is_none());
        assert!(parse_cpu("cpu0 1 2 3 4\n").is_none());
        assert!(parse_cpu("cpu 1 2 3\n").is_none());
        assert!(parse_cpu("cpu 1 2 x 4 5\n").is_none());
    }

    #[test]
    fn memory_is_total_minus_available_in_bytes() {
        let m = parse_meminfo(&fixture("docker", "meminfo")).unwrap();
        assert_eq!(m.total, 96_186_728 * 1024);
        assert_eq!(m.used, (96_186_728 - 48_245_376) * 1024);
        let m = parse_meminfo("MemTotal: 100 kB\nMemAvailable: 30 kB\n").unwrap();
        assert_eq!((m.used, m.total), (70 * 1024, 100 * 1024));
    }

    #[test]
    fn memory_is_none_without_memavailable_or_memtotal() {
        assert!(parse_meminfo("MemTotal: 100 kB\nMemFree: 5 kB\n").is_none());
        assert!(parse_meminfo("MemAvailable: 100 kB\n").is_none());
        assert!(parse_meminfo("MemTotal: lots kB\nMemAvailable: 1 kB\n").is_none());
        assert!(parse_meminfo("").is_none());
    }

    const HEADER: &str =
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n";

    fn route(rows: &[&str]) -> String {
        format!("{HEADER}{}\n", rows.join("\n"))
    }

    #[test]
    fn default_route_among_docker_bridges() {
        let r = fixture("docker", "net/route");
        assert_eq!(parse_default_iface(&r), Some("enp11s0"));
    }

    #[test]
    fn default_route_lowest_metric_wins() {
        let r = route(&[
            "wlan0\t00000000\t0100A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0",
            "enp1\t00000000\t0100A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0",
            "tun0\t00000000\t0100A8C0\t0003\t0\t0\t300\t00000000\t0\t0\t0",
        ]);
        assert_eq!(parse_default_iface(&r), Some("enp1"));
    }

    #[test]
    fn default_route_skips_a_down_route() {
        let r = route(&[
            "enp1\t00000000\t0100A8C0\t0002\t0\t0\t100\t00000000\t0\t0\t0",
            "wlan0\t00000000\t0100A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0",
        ]);
        assert_eq!(parse_default_iface(&r), Some("wlan0"));
    }

    #[test]
    fn no_default_route_gives_none() {
        assert_eq!(
            parse_default_iface(&fixture("nodefault", "net/route")),
            None
        );
        assert_eq!(parse_default_iface(HEADER), None);
        assert_eq!(parse_default_iface(""), None);
    }

    #[test]
    fn net_dev_counters_of_the_named_interface() {
        let d = fixture("docker", "net/dev");
        assert_eq!(
            parse_net_dev(&d, "enp11s0"),
            Some((37_537_039_336, 6_854_946_498))
        );
        // A veth next to its bridge, neither a prefix match of the other.
        assert_eq!(
            parse_net_dev(&d, "docker0"),
            Some((7_100_367, 2_690_665_666))
        );
        assert_eq!(parse_net_dev(&d, "enp11"), None);
        assert_eq!(parse_net_dev(&d, "missing0"), None);
    }

    #[test]
    fn rates_are_bytes_over_seconds_rounded_down() {
        assert_eq!(rate(1000, 4000, Duration::from_secs(1)), Some(3000));
        assert_eq!(rate(1000, 4000, Duration::from_secs(2)), Some(1500));
        assert_eq!(rate(0, 10, Duration::from_secs(3)), Some(3));
        assert_eq!(rate(0, 5, Duration::from_millis(1500)), Some(3));
        // A wrap or a reset.
        assert_eq!(rate(4000, 1000, Duration::from_secs(1)), None);
    }

    /// A copy of a fixture directory whose files a test can overwrite between samples.
    fn scratch(dir: &str) -> tempfile::TempDir {
        let tmp = tempfile::tempdir().unwrap();
        std::fs::create_dir(tmp.path().join("net")).unwrap();
        for f in ["stat", "meminfo", "net/route", "net/dev"] {
            std::fs::write(tmp.path().join(f), fixture(dir, f)).unwrap();
        }
        tmp
    }

    fn net_dev(iface: &str, rx: u64, tx: u64) -> String {
        format!(
            "Inter-|   Receive\n face |bytes\n  lo: 1 0 0 0 0 0 0 0 1 0\n{iface}: {rx} 1 0 0 0 0 0 0 {tx} 1 0 0 0 0 0 0\n"
        )
    }

    #[test]
    fn sampler_turns_two_reads_into_rates() {
        let tmp = scratch("docker");
        let mut s = Sampler::new(tmp.path());
        let t0 = Instant::now();

        let first = s.sample(t0);
        assert_eq!(first.cpu, None);
        assert_eq!(first.net, None);
        assert_eq!(first.mem.unwrap().total, 96_186_728 * 1024);

        std::fs::write(tmp.path().join("stat"), stat(0, 0)).unwrap();
        std::fs::write(
            tmp.path().join("net/dev"),
            net_dev("enp11s0", 37_537_039_336 + 2000, 6_854_946_498 + 500),
        )
        .unwrap();
        // The fixture's own counters moved on by a known amount.
        let second = s.sample(t0 + Duration::from_secs(2));
        // The first read's counters are far larger than the rewritten stat: the CPU went backwards.
        assert_eq!(second.cpu, None);
        assert_eq!(second.net, Some(Net { rx: 1000, tx: 250 }));
    }

    #[test]
    fn sampler_cpu_across_reads() {
        let tmp = scratch("docker");
        std::fs::write(tmp.path().join("stat"), stat(100, 900)).unwrap();
        let mut s = Sampler::new(tmp.path());
        let t0 = Instant::now();
        s.sample(t0);
        std::fs::write(tmp.path().join("stat"), stat(150, 950)).unwrap();
        assert_eq!(s.sample(t0 + PERIOD).cpu, Some(50));
    }

    #[test]
    fn sampler_gives_no_rate_when_the_interface_changes_or_counters_reset() {
        let tmp = scratch("docker");
        let mut s = Sampler::new(tmp.path());
        let t0 = Instant::now();
        s.sample(t0);

        // The counters went backwards.
        std::fs::write(tmp.path().join("net/dev"), net_dev("enp11s0", 5, 5)).unwrap();
        assert_eq!(s.sample(t0 + PERIOD).net, None);
        // And the next frame has a rate again.
        std::fs::write(tmp.path().join("net/dev"), net_dev("enp11s0", 105, 55)).unwrap();
        assert_eq!(s.sample(t0 + 2 * PERIOD).net, Some(Net { rx: 100, tx: 50 }));

        // The default route moves to another interface: its counters are not comparable.
        std::fs::write(
            tmp.path().join("net/route"),
            route(&["wlan0\t00000000\t0100A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0"]),
        )
        .unwrap();
        std::fs::write(tmp.path().join("net/dev"), net_dev("wlan0", 9000, 9000)).unwrap();
        assert_eq!(s.sample(t0 + 3 * PERIOD).net, None);
        std::fs::write(tmp.path().join("net/dev"), net_dev("wlan0", 9100, 9200)).unwrap();
        assert_eq!(
            s.sample(t0 + 4 * PERIOD).net,
            Some(Net { rx: 100, tx: 200 })
        );
    }

    #[test]
    fn sampler_without_a_default_route_still_gives_cpu_and_memory() {
        let tmp = scratch("nodefault");
        std::fs::write(tmp.path().join("stat"), stat(0, 0)).unwrap();
        let mut s = Sampler::new(tmp.path());
        let t0 = Instant::now();
        s.sample(t0);
        std::fs::write(tmp.path().join("stat"), stat(10, 90)).unwrap();
        let got = s.sample(t0 + PERIOD);
        assert_eq!(got.net, None);
        assert_eq!(got.cpu, Some(10));
        assert!(got.mem.is_some());
    }

    #[test]
    fn sampler_survives_missing_files() {
        let tmp = tempfile::tempdir().unwrap();
        let mut s = Sampler::new(tmp.path());
        let t0 = Instant::now();
        assert_eq!(s.sample(t0), HostStats::default());
        assert_eq!(s.sample(t0 + PERIOD), HostStats::default());
    }

    #[test]
    fn the_local_utc_offset_is_known_here() {
        // `UtcOffset::local_offset_at` fails on a platform without a usable localtime; the
        // plan's fallback is to show UTC, so this must hold on the desktop and in CI.
        let offset = utc_offset_seconds(OffsetDateTime::now_utc());
        let secs = offset.expect("local_offset_at failed on this machine");
        assert!((-64_800..=64_800).contains(&secs));
    }

    // `spawn` under a paused clock. The sampler reads a fixture directory whose `stat` the test
    // advances, so every published value is distinguishable.

    async fn settle() {
        // Lets the spawned task run to its next await without moving the clock.
        for _ in 0..5 {
            tokio::task::yield_now().await;
        }
    }

    #[tokio::test(start_paused = true)]
    async fn spawn_samples_only_while_sessions_are_connected() {
        let tmp = scratch("docker");
        let (count, polling) = watch::channel(0usize);
        let mut out = spawn_with(Sampler::new(tmp.path()), polling);

        // Nothing while the count is 0, however long it takes.
        tokio::time::advance(Duration::from_secs(5)).await;
        settle().await;
        assert!(!out.has_changed().unwrap());
        assert_eq!(*out.borrow(), None);

        // One value a second after the count turns 1.
        count.send_replace(1);
        settle().await;
        tokio::time::advance(Duration::from_millis(999)).await;
        settle().await;
        assert!(!out.has_changed().unwrap());
        tokio::time::advance(Duration::from_millis(1)).await;
        settle().await;
        assert!(out.has_changed().unwrap());
        let first = out.borrow_and_update().unwrap();
        assert_eq!(first.cpu, None);
        assert!(first.mem.is_some());

        // The next one comes a second later and has rates.
        std::fs::write(tmp.path().join("stat"), stat(1, 1)).unwrap();
        tokio::time::advance(PERIOD).await;
        settle().await;
        assert!(out.has_changed().unwrap());
        assert!(out.borrow_and_update().is_some());

        // A second session joining changes nothing about the cadence.
        count.send_replace(2);
        settle().await;
        assert!(!out.has_changed().unwrap());

        // Back to 0: None at once, then no more values.
        count.send_replace(0);
        settle().await;
        assert!(out.has_changed().unwrap());
        assert_eq!(*out.borrow_and_update(), None);
        tokio::time::advance(Duration::from_secs(5)).await;
        settle().await;
        assert!(!out.has_changed().unwrap());

        // A later period starts clean: its first value has no CPU from the earlier one.
        count.send_replace(1);
        settle().await;
        tokio::time::advance(PERIOD).await;
        settle().await;
        let again = out.borrow_and_update().unwrap();
        assert_eq!(again.cpu, None);
        assert_eq!(again.net, None);
    }

    #[tokio::test(start_paused = true)]
    async fn spawn_stops_when_the_counter_is_dropped() {
        let tmp = scratch("docker");
        let (count, polling) = watch::channel(1usize);
        let out = spawn_with(Sampler::new(tmp.path()), polling);
        settle().await;
        drop(count);
        settle().await;
        // The task ended and dropped its sender.
        assert!(out.has_changed().is_err());
    }

    // The bar's sysfs, GPU and color layer, on fixture trees and a fake NVML.

    use crate::daemon::config::{StatusBarConfig, Thresholds};
    use crate::daemon::gpu::nvidia::fake::FakeNvidia;
    use crate::daemon::gpu::testing::put;

    const NVIDIA: &str = "0000:01:00.0";
    const AMD: &str = "0000:75:00.0";

    /// This desktop in miniature: an NVIDIA boot GPU, an AMD iGPU with its sensors, and k10temp.
    fn sys_tree() -> tempfile::TempDir {
        let tmp = tempfile::tempdir().unwrap();
        let sys = tmp.path();
        for (id, driver, boot) in [(NVIDIA, "nvidia", "1"), (AMD, "amdgpu", "0")] {
            let dev = format!("bus/pci/devices/{id}");
            put(sys, &format!("{dev}/class"), "0x030000\n");
            put(sys, &format!("{dev}/boot_vga"), &format!("{boot}\n"));
            std::os::unix::fs::symlink(
                format!("../../../../bus/pci/drivers/{driver}"),
                sys.join(&dev).join("driver"),
            )
            .unwrap();
        }
        put(
            sys,
            &format!("bus/pci/devices/{AMD}/gpu_busy_percent"),
            "7\n",
        );
        let hwmon = format!("bus/pci/devices/{AMD}/hwmon/hwmon8");
        put(sys, &format!("{hwmon}/name"), "amdgpu\n");
        put(sys, &format!("{hwmon}/temp1_label"), "edge\n");
        put(sys, &format!("{hwmon}/temp1_input"), "39000\n");
        put(sys, "class/hwmon/hwmon4/name", "k10temp\n");
        put(sys, "class/hwmon/hwmon4/temp1_label", "Tctl\n");
        put(sys, "class/hwmon/hwmon4/temp1_input", "64000\n");
        tmp
    }

    fn fake_nvidia() -> FakeNvidia {
        FakeNvidia::new().with_gpu(
            NVIDIA,
            "NVIDIA GeForce RTX 4070",
            GpuReading {
                busy: Some(23),
                temp: Some(41),
            },
        )
    }

    fn timings() -> Timings {
        Timings {
            read_every: Duration::from_millis(10),
            fresh_for: Duration::from_secs(2),
            retry_every: Duration::from_millis(50),
        }
    }

    fn live_with(bar: StatusBarConfig) -> Arc<Live> {
        let mut live = Live::empty();
        live.config.status_bar = Some(bar);
        Arc::new(live)
    }

    fn gpu_config(gpu: &str) -> StatusBarConfig {
        StatusBarConfig {
            gpu: Some(gpu.into()),
            ..StatusBarConfig::default()
        }
    }

    fn bar_on(
        sys: &tempfile::TempDir,
        fake: &FakeNvidia,
        live: Arc<Live>,
    ) -> (Bar, watch::Sender<Arc<Live>>) {
        let (config_tx, config) = watch::channel(live);
        let sources = HostSources {
            proc_root: PROC.into(),
            sys_root: sys.path().into(),
            nvidia: Arc::new(fake.clone()),
        };
        (Bar::new(config, sources, timings()), config_tx)
    }

    async fn until(what: &str, mut done: impl FnMut() -> bool) {
        for _ in 0..300 {
            if done() {
                return;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        panic!("timed out waiting for {what}");
    }

    /// Samples until the GPU shows, since the NVML thread needs a moment for its first read.
    async fn sample_with_gpu(bar: &mut Bar) -> HostStats {
        for _ in 0..300 {
            let stats = bar.sample(HostStats::default()).await;
            if stats.gpu.is_some() {
                return stats;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        panic!("no GPU reading");
    }

    #[tokio::test]
    async fn auto_resolves_at_the_start_of_a_period_to_the_nvidia_and_adds_its_readings() {
        let (sys, fake) = (sys_tree(), fake_nvidia());
        let (mut bar, _config) = bar_on(&sys, &fake, Arc::new(Live::empty()));
        bar.begin().await;
        // Open before any tick, so the first sample already has a reading to wait for.
        until("the NVML thread to open", || fake.opens() == 1).await;

        let stats = sample_with_gpu(&mut bar).await;
        assert_eq!(stats.cpu_temp, Some(64));
        assert_eq!(
            stats.gpu,
            Some(GpuReading {
                busy: Some(23),
                temp: Some(41)
            })
        );
        assert_eq!(stats.units, Units::default());
        assert_eq!(fake.opens(), 1);
    }

    #[tokio::test]
    async fn off_opens_nothing_and_a_pci_address_reads_the_amd_from_sysfs() {
        let (sys, fake) = (sys_tree(), fake_nvidia());
        let (mut bar, config) = bar_on(&sys, &fake, live_with(gpu_config("off")));
        bar.begin().await;
        let stats = bar.sample(HostStats::default()).await;
        assert_eq!((stats.gpu, stats.cpu_temp), (None, Some(64)));
        assert_eq!(fake.opens(), 0);

        config.send_replace(live_with(gpu_config(AMD)));
        let stats = bar.sample(HostStats::default()).await;
        assert_eq!(
            stats.gpu,
            Some(GpuReading {
                busy: Some(7),
                temp: Some(39)
            })
        );
        assert_eq!(fake.opens(), 0, "sysfs needs no NVML");
    }

    #[tokio::test]
    async fn switching_from_the_nvidia_to_the_amd_shuts_the_fake_down() {
        let (sys, fake) = (sys_tree(), fake_nvidia());
        let (mut bar, config) = bar_on(&sys, &fake, Arc::new(Live::empty()));
        bar.begin().await;
        sample_with_gpu(&mut bar).await;
        assert_eq!(fake.shutdowns(), 0);

        config.send_replace(live_with(gpu_config(AMD)));
        let stats = bar.sample(HostStats::default()).await;
        assert_eq!(stats.gpu.and_then(|g| g.busy), Some(7));
        until("NVML to shut down", || fake.shutdowns() == 1).await;
    }

    #[tokio::test]
    async fn the_end_of_a_period_shuts_nvml_down_and_the_next_one_opens_it_again() {
        let (sys, fake) = (sys_tree(), fake_nvidia());
        let (mut bar, _config) = bar_on(&sys, &fake, Arc::new(Live::empty()));
        bar.begin().await;
        sample_with_gpu(&mut bar).await;
        bar.end();
        until("NVML to shut down", || fake.shutdowns() == 1).await;

        bar.begin().await;
        sample_with_gpu(&mut bar).await;
        assert_eq!(fake.opens(), 2);
    }

    #[tokio::test]
    async fn the_units_and_colors_follow_the_live_config() {
        let (sys, fake) = (sys_tree(), fake_nvidia());
        let bar_config = StatusBarConfig {
            gpu: Some("off".into()),
            temp_unit: Some("f".into()),
            net_unit: Some("bits".into()),
            thresholds: Some(Thresholds {
                cpu_temp: Some(vec![crate::daemon::config::Step {
                    at: 147,
                    color: "#123456".into(),
                }]),
                ..Thresholds::default()
            }),
        };
        let (mut bar, _config) = bar_on(&sys, &fake, live_with(bar_config));
        bar.begin().await;
        let stats = bar.sample(HostStats::default()).await;
        // 64 C is 147 F.
        assert_eq!(stats.units.temp, crate::daemon::units::TempUnit::F);
        assert_eq!(stats.units.net, crate::daemon::units::NetUnit::Bits);
        assert_eq!(stats.colors.cpu_temp, Some(0x123456));
    }

    /// A sysfs read that blocks, as an AMD dGPU waking from runtime suspend does: `temp1_input`
    /// becomes a FIFO nobody writes to, so opening it for reading waits.
    #[tokio::test]
    async fn a_blocked_sysfs_read_costs_its_values_but_not_the_tick_or_the_end_of_the_period() {
        let (sys, fake) = (sys_tree(), fake_nvidia());
        let input = sys.path().join("devices/class/hwmon/hwmon4/temp1_input");
        std::fs::remove_file(&input).unwrap();
        assert!(std::process::Command::new("mkfifo")
            .arg(&input)
            .status()
            .unwrap()
            .success());
        let _release = Release(input);
        let (_config_tx, config) = watch::channel(Arc::new(Live::empty()));
        let (count, polling) = watch::channel(1usize);
        let sources = HostSources {
            proc_root: format!("{PROC}/docker").into(),
            sys_root: sys.path().into(),
            nvidia: Arc::new(fake.clone()),
        };
        let mut out = spawn(polling, config, sources);

        // Readings keep coming, without the temperature, one tick apart or a little more.
        for _ in 0..2 {
            tokio::time::timeout(Duration::from_secs(4), out.changed())
                .await
                .expect("no reading while a sysfs read is blocked")
                .unwrap();
            let stats = out.borrow_and_update().unwrap();
            assert!(stats.mem.is_some());
            assert_eq!(stats.cpu_temp, None);
        }
        until("NVML to open", || fake.opens() == 1).await;

        // The period still ends: NVML shuts down and the reading goes away.
        count.send_replace(0);
        until("NVML to shut down", || fake.shutdowns() == 1).await;
        until("the reading to clear", || out.borrow().is_none()).await;
    }

    /// Frees a reader blocked in `open` on a FIFO when dropped, also when the test panics, since
    /// the runtime waits for blocking threads when it shuts down. A write-only non-blocking open
    /// fails until a reader waits, so it is tried for up to a second.
    struct Release(std::path::PathBuf);

    impl Drop for Release {
        fn drop(&mut self) {
            use std::os::unix::fs::OpenOptionsExt;
            const O_NONBLOCK: i32 = 0o4000; // Linux
            for _ in 0..20 {
                let opened = std::fs::OpenOptions::new()
                    .write(true)
                    .custom_flags(O_NONBLOCK)
                    .open(&self.0);
                if opened.is_ok() {
                    return;
                }
                std::thread::sleep(Duration::from_millis(50));
            }
        }
    }

    #[tokio::test(start_paused = true)]
    async fn a_period_without_sessions_opens_nothing() {
        let (sys, fake) = (sys_tree(), fake_nvidia());
        let (_config_tx, config) = watch::channel(Arc::new(Live::empty()));
        let (count, polling) = watch::channel(0usize);
        let sources = HostSources {
            proc_root: PROC.into(),
            sys_root: sys.path().into(),
            nvidia: Arc::new(fake.clone()),
        };
        let _out = spawn(polling, config, sources);
        tokio::time::advance(Duration::from_secs(5)).await;
        settle().await;
        assert_eq!(fake.opens(), 0);
        drop(count);
    }
}
