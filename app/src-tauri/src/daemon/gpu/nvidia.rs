//! NVIDIA GPU busy share and temperature through NVML.
//!
//! Our code reaches NVML only through [`NvidiaOpener`] and [`Nvidia`], so tests (and CI, which has
//! no NVIDIA) replace it with [`fake::FakeNvidia`]. [`SystemNvml`] is the real one: `nvml-wrapper`
//! opens `libnvidia-ml.so.1` with `dlopen` at runtime, so the binary links nothing and a host
//! without the driver starts and shows `GPU --`.
//!
//! NVML calls are blocking ioctls. Starting it takes milliseconds and a wedged GPU can stall a
//! call for seconds, so [`NvidiaReader::start`] runs one `nvml` thread per sampling period. The
//! thread owns the open handle, reads once a second and publishes each reading with its time.
//! [`Period::latest`] gives a reading less than `fresh_for` old, so the sampler never waits on
//! NVML. Dropping the [`Period`] stops the thread, which drops the handle and so shuts NVML down.

use std::ffi::OsString;
use std::fmt;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, RecvTimeoutError};
use std::sync::Arc;
use std::time::{Duration, Instant};

use nvml_wrapper::enum_wrappers::device::TemperatureSensor;
use nvml_wrapper::error::NvmlError;
use nvml_wrapper::Nvml;
use tokio::sync::watch;

/// The name the dynamic loader is asked for. Many distributions ship the unversioned
/// `libnvidia-ml.so` only in a development package.
const LIBRARY: &str = "libnvidia-ml.so.1";

/// Busy share (0 to 100) and temperature in whole °C of one GPU. Either part is `None` when the
/// GPU doesn't support it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Gpu {
    pub busy: Option<u32>,
    pub temp: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NvidiaError {
    /// `libnvidia-ml.so.1` isn't installed. Trying again won't help.
    LibraryMissing,
    /// The library is there but the kernel driver isn't loaded.
    DriverNotLoaded,
    /// Library and driver are of different versions (a driver update without a reboot).
    VersionMismatch,
    /// NVML doesn't know the PCI address.
    NotFound,
    /// The GPU fell off the bus.
    Lost,
    Other(String),
}

impl fmt::Display for NvidiaError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::LibraryMissing => f.write_str("library missing"),
            Self::DriverNotLoaded => f.write_str("driver not loaded"),
            Self::VersionMismatch => f.write_str("library and driver versions differ"),
            Self::NotFound => f.write_str("no GPU at that address"),
            Self::Lost => f.write_str("GPU lost"),
            Self::Other(e) => f.write_str(e),
        }
    }
}

/// Opens NVML, or says why it can't (no library, driver not loaded, version mismatch).
pub trait NvidiaOpener: Send + Sync {
    fn open(&self) -> Result<Box<dyn Nvidia>, NvidiaError>;
}

/// An open NVML. Dropping it shuts NVML down.
pub trait Nvidia: Send {
    /// Every GPU NVML sees: PCI address (`dddd:bb:dd.f`, lowercase) and name.
    fn list(&self) -> Result<Vec<(String, String)>, NvidiaError>;
    /// Busy share and temperature of the GPU at `pci`.
    fn read(&self, pci: &str) -> Result<Gpu, NvidiaError>;
}

/// `dddd:bb:dd.f` in lowercase. NVML spells the domain with eight digits (`00000000:01:00.0`).
pub fn normalize_pci(raw: &str) -> String {
    let lower = raw.trim().to_ascii_lowercase();
    if let Some((domain, rest)) = lower.split_once(':') {
        if domain.len() > 4 && domain.is_ascii() {
            return format!("{}:{rest}", &domain[domain.len() - 4..]);
        }
    }
    lower
}

/// The real NVML, loaded from `library` at runtime.
pub struct SystemNvml {
    library: OsString,
}

impl SystemNvml {
    pub fn new() -> Self {
        Self::with_library(LIBRARY)
    }

    pub fn with_library(library: impl Into<OsString>) -> Self {
        Self {
            library: library.into(),
        }
    }
}

impl Default for SystemNvml {
    fn default() -> Self {
        Self::new()
    }
}

impl NvidiaOpener for SystemNvml {
    fn open(&self) -> Result<Box<dyn Nvidia>, NvidiaError> {
        let mut builder = Nvml::builder();
        builder.lib_path(&self.library);
        let nvml = builder.init().map_err(|e| match e {
            NvmlError::LibloadingError(_) | NvmlError::LibraryNotFound => {
                NvidiaError::LibraryMissing
            }
            NvmlError::DriverNotLoaded => NvidiaError::DriverNotLoaded,
            NvmlError::LibRmVersionMismatch => NvidiaError::VersionMismatch,
            e => NvidiaError::Other(e.to_string()),
        })?;
        Ok(Box::new(Open { nvml }))
    }
}

struct Open {
    nvml: Nvml,
}

fn read_error(e: NvmlError) -> NvidiaError {
    match e {
        NvmlError::GpuLost => NvidiaError::Lost,
        e => NvidiaError::Other(e.to_string()),
    }
}

/// A part the GPU doesn't support is `None`, any other error is the read's.
fn optional(r: Result<u32, NvmlError>) -> Result<Option<u32>, NvidiaError> {
    match r {
        Ok(v) => Ok(Some(v)),
        Err(NvmlError::NotSupported) => Ok(None),
        Err(e) => Err(read_error(e)),
    }
}

impl Nvidia for Open {
    fn list(&self) -> Result<Vec<(String, String)>, NvidiaError> {
        let count = self.nvml.device_count().map_err(read_error)?;
        (0..count)
            .map(|i| {
                let device = self.nvml.device_by_index(i).map_err(read_error)?;
                let pci = device.pci_info().map_err(read_error)?.bus_id;
                let name = device.name().map_err(read_error)?;
                Ok((normalize_pci(&pci), name))
            })
            .collect()
    }

    // The device is looked up on each read: it is cheap, and a `Device` borrows the `Nvml`,
    // which can't be kept beside it in one struct.
    fn read(&self, pci: &str) -> Result<Gpu, NvidiaError> {
        let device = self.nvml.device_by_pci_bus_id(pci).map_err(|e| match e {
            NvmlError::InvalidArg | NvmlError::NotFound => NvidiaError::NotFound,
            e => read_error(e),
        })?;
        Ok(Gpu {
            busy: optional(device.utilization_rates().map(|u| u.gpu))?,
            temp: optional(device.temperature(TemperatureSensor::Gpu))?,
        })
    }
}

/// How often the thread reads, how old a reading may be and how long it waits before it opens
/// NVML again. The sampler passes seconds, tests milliseconds.
#[derive(Debug, Clone, Copy)]
pub struct Timings {
    pub read_every: Duration,
    pub fresh_for: Duration,
    pub retry_every: Duration,
}

impl Default for Timings {
    fn default() -> Self {
        Self {
            read_every: Duration::from_secs(1),
            fresh_for: Duration::from_secs(3),
            retry_every: Duration::from_secs(60),
        }
    }
}

type Reading = (Instant, Gpu);

/// Starts the `nvml` thread for each sampling period, and never more than one at a time.
pub struct NvidiaReader {
    opener: Arc<dyn NvidiaOpener>,
    timings: Timings,
    /// True while a thread is running, including one whose period has already ended.
    alive: Arc<AtomicBool>,
    warned_busy: AtomicBool,
}

impl NvidiaReader {
    pub fn new(opener: Arc<dyn NvidiaOpener>, timings: Timings) -> Self {
        Self {
            opener,
            timings,
            alive: Arc::new(AtomicBool::new(false)),
            warned_busy: AtomicBool::new(false),
        }
    }

    /// Reads the GPU at `pci` until the returned [`Period`] is dropped. If the thread of an
    /// earlier period is still running (stuck in a call), this period has no NVIDIA values.
    pub fn start(&self, pci: &str) -> Period {
        let (tx, rx) = watch::channel(None);
        let (stop, stopped) = mpsc::channel();
        let period = Period {
            latest: rx,
            fresh_for: self.timings.fresh_for,
            _stop: stop,
        };

        if self.alive.swap(true, Ordering::SeqCst) {
            if !self.warned_busy.swap(true, Ordering::Relaxed) {
                log::warn!("nvml: the previous reader is still stuck in a call, no NVIDIA values this period");
            }
            return period;
        }
        self.warned_busy.store(false, Ordering::Relaxed);

        let worker = Worker {
            opener: self.opener.clone(),
            pci: normalize_pci(pci),
            timings: self.timings,
            stopped,
            out: tx,
            _alive: AliveGuard(self.alive.clone()),
        };
        if let Err(e) = std::thread::Builder::new()
            .name("nvml".into())
            .spawn(move || worker.run())
        {
            // The closure, and with it the guard, is dropped, which clears `alive`.
            log::error!("nvml: starting the reader thread failed: {e}");
        }
        period
    }
}

/// One sampling period's view of the reader. Dropping it stops the thread and so shuts NVML down.
pub struct Period {
    latest: watch::Receiver<Option<Reading>>,
    fresh_for: Duration,
    /// Dropping the sender wakes the thread's wait, and the thread exits.
    _stop: mpsc::Sender<()>,
}

impl Period {
    /// The newest reading, if it is less than `fresh_for` old.
    pub fn latest(&self) -> Option<Gpu> {
        let (at, gpu) = (*self.latest.borrow())?;
        (at.elapsed() < self.fresh_for).then_some(gpu)
    }
}

struct AliveGuard(Arc<AtomicBool>);

impl Drop for AliveGuard {
    fn drop(&mut self) {
        self.0.store(false, Ordering::SeqCst);
    }
}

/// Tells when the thread's state changes, so a fault is logged once and not on every retry.
#[derive(Default)]
struct Logged(Option<String>);

impl Logged {
    /// True when `state` differs from the last fault.
    fn fault(&mut self, state: String) -> bool {
        if self.0.as_deref() == Some(&state) {
            return false;
        }
        self.0 = Some(state);
        true
    }

    /// True when a fault was showing and reads work again.
    fn recovered(&mut self) -> bool {
        self.0.take().is_some()
    }
}

struct Worker {
    opener: Arc<dyn NvidiaOpener>,
    pci: String,
    timings: Timings,
    stopped: mpsc::Receiver<()>,
    out: watch::Sender<Option<Reading>>,
    _alive: AliveGuard,
}

impl Worker {
    fn run(self) {
        let mut handle: Option<Box<dyn Nvidia>> = None;
        let mut reopen_at = Instant::now();
        let mut logged = Logged::default();

        loop {
            if handle.is_none() && Instant::now() >= reopen_at {
                match self.opener.open() {
                    Ok(h) => {
                        log::debug!("nvml: open, reading {}", self.pci);
                        handle = Some(h);
                    }
                    Err(NvidiaError::LibraryMissing) => {
                        log::info!("nvml: no NVIDIA driver (libnvidia-ml.so.1 is missing)");
                        return;
                    }
                    Err(e) => {
                        if logged.fault(format!("open: {e}")) {
                            log::warn!(
                                "nvml: can't open NVML ({e}), trying again every {:?}",
                                self.timings.retry_every
                            );
                        }
                        reopen_at = Instant::now() + self.timings.retry_every;
                    }
                }
            }

            if let Some(h) = &handle {
                match h.read(&self.pci) {
                    Ok(gpu) => {
                        if logged.recovered() {
                            log::info!("nvml: reading {} again", self.pci);
                        }
                        // NVML's busy share is 0 to 100, anything else is a bad reading.
                        let busy = gpu.busy.filter(|b| *b <= 100);
                        self.out
                            .send_replace(Some((Instant::now(), Gpu { busy, ..gpu })));
                    }
                    Err(NvidiaError::Lost) => {
                        if logged.fault("lost".into()) {
                            log::warn!("nvml: GPU {} lost, opening NVML again later", self.pci);
                        }
                        handle = None; // shuts NVML down
                        reopen_at = Instant::now() + self.timings.retry_every;
                    }
                    Err(e) => {
                        if logged.fault(format!("read: {e}")) {
                            log::warn!("nvml: reading {}: {e}", self.pci);
                        }
                    }
                }
            }

            match self.stopped.recv_timeout(self.timings.read_every) {
                Err(RecvTimeoutError::Timeout) => {}
                Ok(()) | Err(RecvTimeoutError::Disconnected) => return,
            }
        }
    }
}

/// A scripted NVML for tests, here and in `device_sim`. It counts opens and shutdowns.
pub mod fake {
    use super::*;
    use std::sync::atomic::AtomicUsize;
    use std::sync::{Condvar, Mutex};

    struct Script {
        open: Result<(), NvidiaError>,
        gpus: Vec<(String, String, Gpu)>,
        read: Option<NvidiaError>,
        block: bool,
    }

    struct Shared {
        script: Mutex<Script>,
        unblocked: Condvar,
        opens: AtomicUsize,
        shutdowns: AtomicUsize,
    }

    /// Both the opener and the knobs: clone it, hand one clone to the reader and keep the other.
    #[derive(Clone)]
    pub struct FakeNvidia(Arc<Shared>);

    impl FakeNvidia {
        pub fn new() -> Self {
            Self(Arc::new(Shared {
                script: Mutex::new(Script {
                    open: Ok(()),
                    gpus: Vec::new(),
                    read: None,
                    block: false,
                }),
                unblocked: Condvar::new(),
                opens: AtomicUsize::new(0),
                shutdowns: AtomicUsize::new(0),
            }))
        }

        fn script(&self) -> std::sync::MutexGuard<'_, Script> {
            self.0.script.lock().unwrap_or_else(|e| e.into_inner())
        }

        /// A GPU NVML sees. `pci` is in the form `list` gives.
        pub fn with_gpu(self, pci: &str, name: &str, gpu: Gpu) -> Self {
            self.script().gpus.push((pci.into(), name.into(), gpu));
            self
        }

        /// What `open` answers from now on.
        pub fn set_open(&self, result: Result<(), NvidiaError>) {
            self.script().open = result;
        }

        /// An error every `read` gives from now on, or `None` for readings again.
        pub fn set_read_error(&self, error: Option<NvidiaError>) {
            self.script().read = error;
        }

        /// While true, `read` doesn't return, as on a wedged GPU.
        pub fn set_block(&self, block: bool) {
            self.script().block = block;
            self.0.unblocked.notify_all();
        }

        pub fn opens(&self) -> usize {
            self.0.opens.load(Ordering::SeqCst)
        }

        pub fn shutdowns(&self) -> usize {
            self.0.shutdowns.load(Ordering::SeqCst)
        }
    }

    impl Default for FakeNvidia {
        fn default() -> Self {
            Self::new()
        }
    }

    impl NvidiaOpener for FakeNvidia {
        fn open(&self) -> Result<Box<dyn Nvidia>, NvidiaError> {
            self.0.opens.fetch_add(1, Ordering::SeqCst);
            self.script().open.clone()?;
            Ok(Box::new(Handle(self.0.clone())))
        }
    }

    struct Handle(Arc<Shared>);

    impl Drop for Handle {
        fn drop(&mut self) {
            self.0.shutdowns.fetch_add(1, Ordering::SeqCst);
        }
    }

    impl Nvidia for Handle {
        fn list(&self) -> Result<Vec<(String, String)>, NvidiaError> {
            let script = self.0.script.lock().unwrap_or_else(|e| e.into_inner());
            Ok(script
                .gpus
                .iter()
                .map(|(pci, name, _)| (pci.clone(), name.clone()))
                .collect())
        }

        fn read(&self, pci: &str) -> Result<Gpu, NvidiaError> {
            let mut script = self.0.script.lock().unwrap_or_else(|e| e.into_inner());
            while script.block {
                script = self
                    .0
                    .unblocked
                    .wait(script)
                    .unwrap_or_else(|e| e.into_inner());
            }
            if let Some(e) = &script.read {
                return Err(e.clone());
            }
            script
                .gpus
                .iter()
                .find(|(p, _, _)| p == pci)
                .map(|(_, _, gpu)| *gpu)
                .ok_or(NvidiaError::NotFound)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::fake::FakeNvidia;
    use super::*;

    const PCI: &str = "0000:01:00.0";

    fn timings() -> Timings {
        Timings {
            read_every: Duration::from_millis(5),
            fresh_for: Duration::from_millis(100),
            retry_every: Duration::from_millis(30),
        }
    }

    fn card(busy: Option<u32>, temp: Option<u32>) -> FakeNvidia {
        FakeNvidia::new().with_gpu(PCI, "NVIDIA GeForce RTX 4070", Gpu { busy, temp })
    }

    fn reader(fake: &FakeNvidia) -> NvidiaReader {
        NvidiaReader::new(Arc::new(fake.clone()), timings())
    }

    /// Polls `cond` for up to 5 s.
    fn eventually(what: &str, mut cond: impl FnMut() -> bool) {
        let end = Instant::now() + Duration::from_secs(5);
        while !cond() {
            assert!(Instant::now() < end, "timed out waiting for {what}");
            std::thread::sleep(Duration::from_millis(2));
        }
    }

    #[test]
    fn reads_the_selected_address() {
        let fake = card(Some(23), Some(41));
        let r = reader(&fake);
        let period = r.start(PCI);
        eventually("a reading", || period.latest().is_some());
        assert_eq!(
            period.latest(),
            Some(Gpu {
                busy: Some(23),
                temp: Some(41)
            })
        );
    }

    #[test]
    fn normalizes_addresses() {
        assert_eq!(normalize_pci("00000000:01:00.0"), "0000:01:00.0");
        assert_eq!(normalize_pci("0000:0A:00.0"), "0000:0a:00.0");
        assert_eq!(normalize_pci("00000000:0A:00.1"), "0000:0a:00.1");
        assert_eq!(normalize_pci(" 0000:75:00.0 "), "0000:75:00.0");
        assert_eq!(normalize_pci("garbage"), "garbage");
        assert_eq!(normalize_pci("ééééé:01:00.0"), "ééééé:01:00.0");
    }

    #[test]
    fn a_selection_in_nvmls_spelling_still_reads() {
        let fake = card(Some(5), Some(30));
        let r = reader(&fake);
        let period = r.start("00000000:01:00.0");
        eventually("a reading", || period.latest().is_some());
    }

    #[test]
    fn an_unknown_address_gives_no_value_and_keeps_trying() {
        let fake = card(Some(5), Some(30));
        let r = reader(&fake);
        let period = r.start("0000:99:00.0");
        std::thread::sleep(Duration::from_millis(60));
        assert_eq!(period.latest(), None);
        assert!(r.alive.load(Ordering::SeqCst));
        assert_eq!(fake.opens(), 1);
    }

    #[test]
    fn a_fault_is_logged_once_until_reads_recover() {
        let mut logged = Logged::default();
        assert!(logged.fault("read: x".into()));
        assert!(!logged.fault("read: x".into()));
        assert!(logged.fault("read: y".into()));
        assert!(logged.recovered());
        assert!(!logged.recovered());
        assert!(logged.fault("read: y".into()));
    }

    #[test]
    fn unsupported_temperature_gives_busy_only() {
        let fake = card(Some(40), None);
        let r = reader(&fake);
        let period = r.start(PCI);
        eventually("a reading", || period.latest().is_some());
        assert_eq!(
            period.latest(),
            Some(Gpu {
                busy: Some(40),
                temp: None
            })
        );
    }

    #[test]
    fn busy_over_100_gives_none() {
        let fake = card(Some(101), Some(50));
        let r = reader(&fake);
        let period = r.start(PCI);
        eventually("a reading", || period.latest().is_some());
        assert_eq!(
            period.latest(),
            Some(Gpu {
                busy: None,
                temp: Some(50)
            })
        );
    }

    #[test]
    fn a_missing_library_gives_no_values_and_no_retry() {
        let fake = card(Some(1), Some(1));
        fake.set_open(Err(NvidiaError::LibraryMissing));
        let r = reader(&fake);
        let period = r.start(PCI);
        eventually("the thread to give up", || !r.alive.load(Ordering::SeqCst));
        std::thread::sleep(Duration::from_millis(120)); // four retry intervals
        assert_eq!(fake.opens(), 1);
        assert_eq!(period.latest(), None);
    }

    #[test]
    fn a_driver_not_loaded_is_retried_at_the_interval() {
        let fake = card(Some(7), Some(33));
        fake.set_open(Err(NvidiaError::DriverNotLoaded));
        let r = reader(&fake);
        let period = r.start(PCI);
        eventually("three opens", || fake.opens() >= 3);
        assert_eq!(period.latest(), None);
        // A version mismatch is retried the same way.
        fake.set_open(Err(NvidiaError::VersionMismatch));
        let before = fake.opens();
        eventually("another open", || fake.opens() > before);
        // The driver comes up.
        fake.set_open(Ok(()));
        eventually("a reading", || period.latest().is_some());
    }

    #[test]
    fn a_lost_gpu_drops_the_handle_and_reopens() {
        let fake = card(Some(7), Some(33));
        let r = reader(&fake);
        let period = r.start(PCI);
        eventually("a reading", || period.latest().is_some());
        assert_eq!((fake.opens(), fake.shutdowns()), (1, 0));

        fake.set_read_error(Some(NvidiaError::Lost));
        eventually("the handle to drop", || fake.shutdowns() >= 1);
        eventually("the reading to go stale", || period.latest().is_none());

        fake.set_read_error(None);
        eventually("a reopen", || fake.opens() >= 2);
        eventually("a reading again", || period.latest().is_some());
    }

    #[test]
    fn dropping_the_period_shuts_nvml_down() {
        let fake = card(Some(7), Some(33));
        let r = reader(&fake);
        let period = r.start(PCI);
        eventually("a reading", || period.latest().is_some());
        assert_eq!(fake.shutdowns(), 0);
        drop(period);
        eventually("the shutdown", || fake.shutdowns() == 1);
        eventually("the thread to end", || !r.alive.load(Ordering::SeqCst));
        // The next period starts a thread again.
        let again = r.start(PCI);
        eventually("a reading", || again.latest().is_some());
        assert_eq!(fake.opens(), 2);
    }

    #[test]
    fn a_blocked_read_leaves_the_reading_stale_and_starts_no_second_thread() {
        let fake = card(Some(7), Some(33));
        let r = reader(&fake);
        let period = r.start(PCI);
        eventually("a reading", || period.latest().is_some());

        fake.set_block(true);
        eventually("the reading to go stale", || period.latest().is_none());

        // The period ends, but the thread is stuck in its call. The next period gets no thread.
        drop(period);
        let next = r.start(PCI);
        std::thread::sleep(Duration::from_millis(50));
        assert_eq!(next.latest(), None);
        assert_eq!(fake.opens(), 1);
        assert!(r.warned_busy.load(Ordering::Relaxed));

        // The call returns, the old thread ends, and a later period reads again.
        fake.set_block(false);
        eventually("the old thread to end", || !r.alive.load(Ordering::SeqCst));
        assert_eq!(fake.shutdowns(), 1);
        let later = r.start(PCI);
        eventually("a reading", || later.latest().is_some());
        assert!(!r.warned_busy.load(Ordering::Relaxed));
    }

    #[test]
    fn the_fake_lists_its_gpus() {
        let fake = card(Some(1), Some(1));
        let handle = fake.open().expect("open");
        assert_eq!(
            handle.list().expect("list"),
            vec![(PCI.to_string(), "NVIDIA GeForce RTX 4070".to_string())]
        );
    }

    // Runs in CI: dlopen of a name that doesn't exist is the no-driver path for real.
    #[test]
    fn the_real_opener_reports_a_missing_library() {
        let opener = SystemNvml::with_library("libwatchdeck-no-such-nvml.so.1");
        assert!(matches!(opener.open(), Err(NvidiaError::LibraryMissing)));
    }

    // Needs the NVIDIA driver and the RTX 4070 at 0000:01:00.0, so it only runs on the
    // desktop: `cargo test -- --ignored nvml`.
    #[test]
    #[ignore = "needs the NVIDIA driver and a GPU at 0000:01:00.0"]
    fn nvml_reads_the_rtx_4070() {
        let opener = SystemNvml::new();
        let nvml = opener.open().expect("open NVML");
        let listed = nvml.list().expect("list GPUs");
        eprintln!("nvml GPUs: {listed:?}");
        assert!(listed.iter().any(|(pci, _)| pci == PCI), "{listed:?}");

        let gpu = nvml.read(PCI).expect("read 0000:01:00.0");
        eprintln!("nvml reading: {gpu:?}");
        assert!(gpu.busy.expect("busy") <= 100);
        assert!((20..=100).contains(&gpu.temp.expect("temperature")));
        drop(nvml);

        // Through the thread as well, with the production timings' shape.
        let r = NvidiaReader::new(Arc::new(opener), Timings::default());
        let period = r.start("00000000:01:00.0");
        eventually("a reading through the thread", || period.latest().is_some());
        eprintln!("nvml thread reading: {:?}", period.latest());
    }
}
