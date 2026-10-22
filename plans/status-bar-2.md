# Status bar 2: GPU, temperatures, and colors past a threshold

Status: planned 2026-10-22, revised the same day with the user's answers. Against `main` at `74ffb38`, where device-ui S1 to S5 are merged. Device-ui S6 is committed on its branch (`917fb61`), and S7 is being built with the compact bar described below. Starts after `plans/device-ui.md` S8 passes. No slice started.

**Device-ui S7 builds the bar's final layout now, not this plan.** S7 builds the compact bar: CPU and MEM on the left, the date and time in the center of the screen, GPU and network on the right. It has no daemon name, shows `MEM 45%` and rates as `▼1.2M ▲340k`, and its GPU slot reads `GPU --`. S7 sizes every slot for the strings this plan will put in it, so this plan moves nothing on the board. B7 only adds GPU busy, temperatures and colors to slots that already exist.

After this plan:

- The bar shows the desktop's CPU use and temperature, memory, the time and date, GPU use and temperature, and network rates.
- Each value can change color when it passes a threshold, for example memory at 90% in red. The thresholds and colors live in `config.toml` and are edited in the Layout tab.
- The GPU is the NVIDIA RTX 4070 on this desktop by default, read through NVML. The AMD Radeon 610M iGPU on the same host can be picked instead. On a host with several GPUs, the bar shows one, picked automatically or by PCI address.

## Decisions 2026-10-22

The user answered the open questions of the first draft:

- **Daemon name** (was Q1): not in the bar. It is in settings. Built by device-ui S7, which also decides what the bar shows while the link is down. This plan doesn't change that.
- **Rates** (was Q2): `▼1.2M ▲340k`, bytes per second with SI prefixes and no `B/s`. Built by S7.
- **Memory** (was Q3): `MEM 45%`, which is also the unit its threshold uses. Built by S7.
- **Order** (the user's): CPU and MEM on the left, the date and time centered on the screen, GPU and network on the right. Built by S7 to the budget below.
- **No GPU** (was Q4): with `off`, or none found, the slot shows `GPU --`. Fixed slots.
- **Default steps** (was Q5): as in the Config table. GPU busy and network aren't colored by default.
- **NVIDIA source** (was Q6): **NVML through the `nvml-wrapper` crate**, loaded at runtime from `libnvidia-ml.so.1`. A host without the NVIDIA driver still runs and shows `GPU --`. The GPU is chosen by PCI address through NVML. A trait seam keeps CI, which has no NVIDIA, testable. The crates are vetted first (V0). The `nvidia-smi` loop is rejected.
- **`auto`** (was Q7): the `boot_vga` GPU, else the lowest PCI address. That is the RTX 4070 here.
- **CPU temperature** (was Q8): `Tdie` if present, else `Tctl`, or the hottest `coretemp` package. No key to pick another sensor.

## Goal and done

- **The bar**, in the slots S7 built, shows `CPU 12% 64°`, `MEM 45%`, `Wed 22 Oct 14:32`, `GPU 23% 41°` and `▼1.2M ▲340k`. On this desktop the numbers agree with `nvidia-smi`, `sensors` and `top` (B8).
- **GPU**: NVIDIA through NVML (`nvml-wrapper`, opened at runtime), AMD through sysfs. `[status_bar] gpu` is `"auto"` (the default), `"off"`, or a PCI address. `auto` picks the RTX 4070 here.
- **No NVIDIA driver**: the daemon starts, runs and shows `GPU --` (or the AMD) on a host without `libnvidia-ml.so.1`, and the binary doesn't link the library (B4 checks with `ldd`).
- **Temperatures**: the CPU's from hwmon (`k10temp` or `coretemp`), found by name and not by hwmon number. The GPU's from NVML or from the card's own hwmon.
- **Thresholds**: `[status_bar.thresholds]` in `config.toml` gives each metric up to 3 steps of `{ at, color }`. The daemon works out each value's color, with hysteresis, and sends it in `stats`. The board draws that value in that color. An edit applies within 2 s and pushes no layout.
- **Config UI**: a Status bar form in the Layout tab edits the GPU choice and every metric's steps, and saves with the existing Save.
- **Lifecycle**: NVML is open only while a device is connected and the selected GPU is an NVIDIA. The app holds no `/dev/nvidia*` file within 2 s of the last session ending, or of a switch to another GPU.
- `cargo test`, `pnpm test` and `mise run fw:test` cover the new rules without a GPU (CI has none). The hardware run (B8) passes on the board with serial-log evidence.
- `site/src/protocol.md`, `site/src/config.md` and `CLAUDE.md` are updated.

## Assumptions

- **device-ui S7 builds the bar to this plan's budget**: the positions and widths below, the formats for CPU %, `MEM %`, rates, the clock and `GPU --`, a 1 s timer, `on_stats` storing each frame, and `hoststat.c` with host tests (`device-ui.md:125-138`, `:314-325`). B7 adapts to S7 as merged. If S7 sized a slot smaller than the budget, B7 fixes the width and says so.
- **One host, Linux.** As for device-ui. Thresholds are per host, not per device.
- **Whole units.** Percent and °C on the wire and in thresholds are whole numbers. Net thresholds are whole MB/s (10^6 bytes per second).
- **Montserrat 24 for the bar**, with `°` (U+00B0) and `LV_SYMBOL_UP`/`DOWN`, all in the built-in font (F15). `fonts:gen` is ASCII-only, but the bar uses the built-in Montserrat, not a generated font.
- **NVML in the app process.** A crash inside `libnvidia-ml` takes the app down with it. The user chose NVML knowing this.
- **Development only.** Board and daemon come from the same commit. Old-firmware compatibility isn't needed (device-ui decision A), though the extension happens to be harmless to an older board (F4).

## Findings

Checked against `74ffb38` and this desktop on 2026-10-22.

| # | Where | What |
|---|---|---|
| F1 | `hoststats.rs:20-27`, `:152`, `:223`, `:236-268` | `HostStats` is `Copy` with CPU, memory and net. `Sampler::new(root)` reads under one root (`/proc`). `spawn(polling)` takes no config. The loop samples while `polling > 0` and publishes `None` at 0 |
| F2 | `server.rs:142-144`, `:780-830` | `hoststats::spawn(polling.clone())` beside `tiles::spawn(actions, config.clone(), polling)`. The session copies each reading out of the watch and stamps `time` at send |
| F3 | `server.rs:974-993` | `stats_frame` leaves out any field outside its wire range, so the device never refuses a frame. New fields need the same guard |
| F4 | `protocol.rs:12`, `:175`; `protocol.md:431-449` | `CAPS` already holds `status_bar`. The board ignores unknown fields, and drops the whole frame if any field has the wrong type or is out of range |
| F5 | `proto.c:278`, `:285`, `:423-465` | `get_whole` takes signed bounds (used for `utc_offset`). `get_pair` makes two fields both-or-neither, which suits `mem_*` and `net_*` but not GPU busy and temperature: a GPU without a temperature sensor must still show its busy share |
| F6 | `config.rs:73-83`, `:268-274`, `:328-382`, `:428` | `Config` refuses unknown fields. `color()` parses `#rrggbb` to `0xRRGGBB`, the form `layout` sends `bg`/`fg` in. `Live::layout` reads only the grid and buttons, so a new table can't change a layout. `check()` returns every broken rule as a string, and validated strings (`bg`, `font`, `url`) are plain `String` fields |
| F7 | `server.rs:794-812` | A config change pushes only when the session's view changed, so a thresholds-only edit pushes nothing |
| F8 | `edit.ts:220-243`, `ConfigPage.tsx:515-520` | `viewKey` ignores keys outside the grid and buttons. So a thresholds-only save has no targets, and the save bar would wrongly say "the layout is unchanged, so devices keep it" |
| F9 | `errors.ts:204`, `:222-246` | Only `layout.*`, `version` and the page and tile counts go to the grid bucket, so `status_bar.*` messages would land in `general`. `numberErrors` catches numbers that aren't whole before the call, since Rust would answer those with a decode error |
| F10 | `Cargo.toml:20-70` | No settings or store plugin. `libloading` 0.7.4 is in the lock through `libappindicator-sys`. There is no `unsafe` in `app/src-tauri/src`. No `cargo-audit` or `cargo-deny` in `mise.toml` |
| F11 | `lspci`, `/sys/class/drm` | `01:00.0` NVIDIA AD104 (RTX 4070), driver `nvidia` (open kernel module 615.78.08), `boot_vga` 1. `75:00.0` AMD Granite Ridge (Radeon 610M), driver `amdgpu`, `boot_vga` 0. `card0` is the AMD and `card1` the NVIDIA, so card numbers say nothing about which GPU is which |
| F12 | `card1/device`, `/sys/class/hwmon/*/name` | The NVIDIA driver exposes no `gpu_busy_percent` and no hwmon. Busy and temperature on NVIDIA need NVML |
| F13 | `card0/device` | `gpu_busy_percent` (0 at idle) and `device/hwmon/hwmon8` (`amdgpu`, `temp1_label` `edge`, 40000). Each sysfs read takes about 8 µs |
| F14 | `/sys/class/hwmon` | `hwmon4` is `k10temp`: `Tctl` 64.0, `Tccd1` 50.9, `Tccd2` 37.5, no `Tdie`. Others: `nvme` ×3, `spd5118` ×2, `r8169`, `ath12k`, `prom21_xhci`. The numbers are assigned at boot |
| F15 | `lv_font_montserrat_24.c:4`, `:3490`, `:3522-3523`; `lv_symbol_def.h:40-276` | The built-in font holds 0x20-0x7F, `°` (advance 161/16 ≈ 10.1 px), `•`, and the LVGL symbols. `LV_SYMBOL_UP`/`DOWN` are 21 px. No symbol for CPU, GPU, memory or temperature |
| F16 | `lv_label.c:273`, `sdkconfig:5353` | `lv_label_set_recolor` exists in LVGL 9.6 with `CONFIG_LV_TXT_COLOR_CMD="#"`: `CPU #e53935 97%# 64°` colors one value in one label |
| F17 | `nvidia-smi` 615.78.08 | One query: 13 ms wall and about 8 ms CPU (20 runs: 273 ms wall, 164 ms CPU), since each run starts NVML. The loop (`-l 1`) used 15 ms CPU over 10 s. NVML prints the bus id as `00000000:01:00.0` and the first busy reading after it starts as 0 |
| F18 | `nvidia-smi -q -d TEMPERATURE`, `/proc/driver/nvidia/gpus/0000:01:00.0/information` | Target temperature 84 °C. Persistence mode off, runtime D3 "Disabled by default". The `information` file gives `Model: NVIDIA GeForce RTX 4070` without starting NVML |
| F19 | crates.io, `Cargo.lock` | `nvml-wrapper` 0.13.0 (2026-08-31, MIT or Apache-2.0) needs `nvml-wrapper-sys` ^0.10 (`libloading` ^0.8.1), `wrapcenum-derive` ^0.4.1 (`darling` ^0.20, a proc macro), `static_assertions` ^1.1, `thiserror` ^1 and `bitflags` ^2, with an optional `serde`. New in the lock: `nvml-wrapper`, `nvml-wrapper-sys`, `wrapcenum-derive`, `static_assertions`, a second `libloading` (0.8 beside 0.7.4) and a second `darling` (0.20 beside 0.24.1, with `darling_core` and `darling_macro`). `thiserror` 1.0.69 and `bitflags` 2.13.2 are already there |
| F20 | `device-ui.md:134`, `montserrat.ts:14-25` | device-ui sizes the clock for `Wed 30 Sep 23:59` (214.6 px). The widest real clock is `Wed 24 May 04:44` (227.6 px): `Wed` is the widest day (58.1), `May` the widest month (50.8), `4` the widest digit (16.1) |
| F21 | `/proc/meminfo` | 96,186,728 kB (91.7 GiB), 45% used when checked |
| F22 | `device-ui.md:38` | The LVGL pool is about 59,800 bytes usable, with about 49,070 settled on the heaviest pages fixture |
| F23 | docs.rs `nvml-wrapper` 0.13.0 | `Nvml::init()` loads `libnvidia-ml.so`. `Nvml::builder().lib_path(&OsStr)` names another file, resolved as `libloading` resolves it. `device_by_pci_bus_id(&str)`, `device_count()`, and on `Device`: `utilization_rates`, `temperature`, `pci_info`, `name`. `Device<'nvml>` borrows the `Nvml`. `Nvml` and `Device` are `Send` and `Sync`. `Drop` shuts NVML down and ignores errors |
| F24 | `/usr/lib/libnvidia-ml.so*` | Here both `libnvidia-ml.so` and `libnvidia-ml.so.1` exist. Many distributions ship the unversioned name only in a development package, so the daemon asks for `libnvidia-ml.so.1` by name |

## Approach

### Bar budget (built by device-ui S7)

Widths are from Montserrat 24's advance table (`montserrat.ts`, plus F15 for `°` and the arrows), without kerning (about 1 px a string). The bar is 1024 px, and the clock is centered on the screen at x 512. Temperatures on the wire are at most 150 °C, so `144°` is the widest temperature (`4` is the widest digit).

| Slot | x | Width | Align | Worst string, at the end of this plan | Worst px | What S7 shows |
|---|---|---|---|---|---|---|
| CPU | 4 | 184 | left | `CPU 100% 144°` | 178.8 | `CPU 12%` |
| MEM | 204 | 136 | left | `MEM 100%` | 129.5 | `MEM 45%` |
| Clock | 396 | 232 | center | `Wed 24 May 04:44` | 227.6 | `Thu 22 Oct 17:05` |
| GPU | 636 | 184 | right | `GPU 100% 144°` | 179.9 | `GPU --` |
| NET | 828 | 192 | right | `▼444M ▲444M` | 190.7 | `▼1.2M ▲340k` |

- **Left half**: 4 + 184 + 16 + 136 = 340, leaving 56 px before the clock.
- **Right half is full**: from the center, 116 (half the clock) + 8 + 184 + 8 + 192 + 4 = 512, so 0 px spare. The worst strings leave 4.4 px in the clock, 4.1 px in GPU and 1.3 px in NET. If the overflow check reports NET or GPU on the board, take the 4 px from the right margin, and then the GPU-to-NET gap goes from 8 to 6 px. Nothing on the right can grow, which is why the right side has no room for `°C`, a second GPU or `MB`.
- The clock is centered on the screen, not in the space between the groups, as the user asked.
- **Formats** (in `hoststat.c`, host-tested):
  - S7 builds: `CPU 12%`; `MEM 45%`, where the percent is `mem_used * 100 / mem_total` rounded down; rates under 1000 B/s as the bare number (`512`), then `k`, `M`, `G` with one decimal below 10 (`1.2M`) and whole numbers from 10 (`12M`, `999M`), always truncated; the clock `Wed 22 Oct 14:32`; `GPU --`.
  - B7 adds: `CPU 12% 64°`. Without the temperature, `CPU 12%`. Without the busy share, `CPU --% 64°`. Without either, `CPU --`. The same for GPU.
- **Link down and stale frames**: S7 decides what shows while the link is down, and that stays. While the link is up but the last frame is over 5 s old, the slots show `--` in grey, and B7 adds that they have no colors.
- **The overflow check at creation** (`device-ui.md:134`) measures the worst strings in the table, the clock's included (F20). S7 measures the final strings, not only what it shows itself. B7 adds color markup around every value and sets the recolor flag, so the markup is measured as LVGL draws it. The slots are sized so a label never reaches its dots, since a cut inside `#rrggbb ...#` would show a stray `#`.
- **Pool**: B7 adds no widget. It turns on recolor for four labels.

### Color past a threshold

- **What "red" means on the board**: the value itself is drawn in the step's color, through LVGL's recolor markup in the slot's one label (F16): `CPU #e53935 97%# 64°`. The prefix (`CPU`, `MEM`, the arrows) stays grey, so each of the two values in a slot can have its own color. No extra widgets. The markup comes from `hoststat.c`, so host tests check colors as strings.
- **Who decides**: the daemon. It knows the thresholds, keeps the hysteresis state in one place, and sends each color with the value it belongs to. The board only draws.
- **Steps**: each metric has 0 to 3 steps `{ at, color }`, `at` rising. A value at or past a step's `at` takes that step's color. The highest step reached wins.
- **Hysteresis**: a step reached stays until the value falls below `at - h`, where `h` is 2 for percent and °C and `at / 10` for rates. Each sample, the new step is whichever is higher: the highest step the value reaches, or the highest step at or below the current one whose `at - h` the value still reaches. Without this, a CPU at 89, 90, 91 would flash every second. A missing value clears its step. A config change starts every metric afresh.
- **Units**: memory percent is rounded down, as the board shows it. Temperatures are rounded to the nearest °C before the threshold and the wire, so the board shows the number that was judged. Rates compare bytes per second with `at × 1,000,000`, and the board's truncated format never shows a step the value hasn't reached.

### Config

In `config.toml`, so it gets the existing validation, last-good config, the save flow with its revision check and backups, and the editor's error mapping (F6), and stays hand-editable:

```toml
[status_bar]
gpu = "auto"        # "auto", "off", or a PCI address such as "0000:01:00.0"

[status_bar.thresholds]
mem = [{ at = 80, color = "#ffb300" }, { at = 90, color = "#e53935" }]
cpu_temp = [{ at = 85, color = "#ffb300" }, { at = 95, color = "#e53935" }]
net_rx = []         # never colored
```

| Key | Unit | `at` range | Default steps |
|---|---|---|---|
| `cpu` | % busy | 1 to 100 | 90 amber |
| `mem` | % used | 1 to 100 | 80 amber, 90 red |
| `gpu` | % busy | 1 to 100 | none (a busy GPU in a game isn't a problem) |
| `cpu_temp` | °C | 1 to 150 | 85 amber, 95 red (Zen 4 and 5 run to 95 °C by design) |
| `gpu_temp` | °C | 1 to 150 | 84 amber, 90 red (this card's target is 84 °C, F18) |
| `net_rx`, `net_tx` | MB/s | 1 to 100000 | none |

- Amber is `#ffb300` and red `#e53935`.
- With no `[status_bar]` table, or no key for a metric, the defaults apply. `[]` means never colored. The defaults live in Rust and reach the editor through `get_config` (`ConfigView.status_bar_defaults`), so TypeScript holds no copy.
- `gpu` is a plain string, as `bg`, `font` and `url` are (F6), so a bad value gets the rule's message from `check()` and not a TOML decode error.
- **Rules**: a config that breaks one is refused and the last good config stays live. The editor maps each message to its field:
  - `status_bar.gpu "x" is not auto, off or a PCI address such as 0000:01:00.0` (the address as `dddd:bb:dd.f`, lowercase hex).
  - `status_bar.thresholds.mem: 4 steps, at most 3`.
  - `status_bar.thresholds.mem: at 120 is outside 1 to 100`.
  - `status_bar.thresholds.mem: steps must rise, 90 then 80`.
  - `status_bar.thresholds.mem: color "red" is not #rrggbb`.
  - Unknown keys are refused, as everywhere in the file.
- A PCI address that names no GPU, or one that can't be read, isn't a config error, since an eGPU can be unplugged. The bar shows `GPU --`, the daemon logs it once per selection, and the editor marks it "(not found)".

### Wire

`stats` gains optional fields under the same `status_bar` capability:

```json
{"t":"stats","time":1792692433,"utc_offset":-14400,"cpu":97,"cpu_temp":71,"gpu":23,"gpu_temp":41,"mem_used":43218907136,"mem_total":98495209472,"net_rx":1250000,"net_tx":340000,"colors":{"cpu":16757504}}
```

| Field | Type | Meaning |
|---|---|---|
| `gpu` | int | Optional. Busy share of the selected GPU, 0 to 100 |
| `cpu_temp` | int | Optional. CPU temperature in whole °C, -40 to 150 |
| `gpu_temp` | int | Optional. The selected GPU's temperature in whole °C, -40 to 150 |
| `colors` | object | Optional. One entry for each value past a threshold: its field name (`cpu`, `mem`, `gpu`, `cpu_temp`, `gpu_temp`, `net_rx`, `net_tx`), and the color to draw it in as `0xRRGGBB`, 0 to 16777215. Absent: every value is drawn in the bar's normal color |

- `gpu`, `cpu_temp` and `gpu_temp` are independent, not a pair (F5).
- **Refused** (the frame is dropped and the link stays up, as for every `stats`): a new field of the wrong type or out of range; `colors` that isn't an object; a known key in `colors` whose value isn't a whole number in range.
- **Ignored**: unknown keys in `colors`, and a `colors` key whose value is absent from the frame.
- **Daemon guard** (F3): `stats_frame` leaves out a temperature outside -40 to 150 (a sensor reading 511 °C or -273 °C), a `gpu` over 100, and the color of any field it leaves out. A bad sensor then costs one value and never the frame. It omits an empty `colors`.
- **No new capability**: the fields are optional and an older board ignores them (F4).

### GPU sources

| Source | Covers | Cost | Verdict |
|---|---|---|---|
| NVML through `nvml-wrapper` 0.13, loaded at runtime from `libnvidia-ml.so.1` | NVIDIA | about 8 ms to start NVML once a period (F17), then microseconds a read. 8 new packages (F19), vetted in V0 | **chosen for NVIDIA** (user decision) |
| sysfs and hwmon | AMD (`gpu_busy_percent`, `device/hwmon/*/temp*_input`) | about 8 µs a file (F13) | **chosen for AMD**. NVIDIA has neither (F12) |
| Intel (`i915`, `xe`) | — | busy share needs perf counters | out of scope: listed, not selectable |

- **Loading**: `Nvml::builder().lib_path("libnvidia-ml.so.1").init()` (F23, F24). `nvml-wrapper-sys` opens the library through `libloading` at runtime, so the binary doesn't link it. That way CI builds without NVIDIA, and a host without the driver gets an error at `init`, not at start. V0 confirms there is no `#[link]` to `nvidia-ml`, and B4 confirms it with `ldd`.
- **The seam**: our code calls NVML only through two traits, which tests replace with a fake:

  ```rust
  /// Opens NVML, or says why it can't (no library, driver not loaded, version mismatch).
  pub trait NvidiaOpener: Send + Sync {
      fn open(&self) -> Result<Box<dyn Nvidia>, NvidiaError>;
  }
  /// An open NVML. Dropping it shuts NVML down.
  pub trait Nvidia: Send {
      /// Every GPU NVML sees: PCI address (`dddd:bb:dd.f`, lowercase) and name.
      fn list(&self) -> Result<Vec<(String, String)>, NvidiaError>;
      /// Busy share and temperature of the GPU at `pci`. Either part may be `None` (not supported).
      fn read(&self, pci: &str) -> Result<Gpu, NvidiaError>;
  }
  ```

  The real implementation looks the device up with `device_by_pci_bus_id` on each read. That is cheap, and it avoids holding a `Device<'nvml>` borrow across calls. It normalizes NVML's `00000000:01:00.0` to `0000:01:00.0` (F17).
- **Its own thread**: NVML calls are blocking ioctls. Starting NVML takes milliseconds, and a wedged GPU can stall a call for seconds. So one `std::thread` named `nvml` owns the open handle for a sampling period. It reads the selected GPU once a second and publishes `(Instant, Gpu)` on a watch. The sampler uses a reading less than 3 s old. CPU, memory and net never wait on NVML. There is at most one such thread: a new period starts its thread only after the old one has finished. If the old one is stuck in a call, the new period runs without NVIDIA values and logs that once.
- **When it is open**: from the start of a period whose selection resolves to an NVIDIA GPU, until the period ends or the selection moves off it. Then the handle is dropped (NVML shut down), so the app holds no `/dev/nvidia*` file and a laptop's dGPU can sleep.
- **Faults**, with one log line per change of state, not per retry:
  - Library missing: "no NVIDIA driver" at info, no NVIDIA values, no retry this period.
  - Driver not loaded, or a library and driver of different versions after an update without a reboot: no values, and `init` is tried again every 60 s.
  - A read error, such as a lost GPU or a timeout: no value for that read. After a lost GPU, the handle is dropped and NVML reopened 60 s later.
  - Temperature not supported: busy only.
  - Busy over 100: none.
- **Timings** (1 s reads, 3 s freshness, 60 s retry) are constructor arguments, so tests run with milliseconds.
- **Laptops**: while NVML is open it keeps an NVIDIA dGPU awake. Here runtime D3 is off anyway (F18). On a hybrid laptop `auto` picks the iGPU, and NVML is open only while a device is connected. `config.md` says so.

### Which GPU

- **Enumeration**: every `/sys/bus/pci/devices/*` whose `class` starts with `0x03`, with its PCI address, its driver (the `driver` link's name), and `boot_vga`. Card numbers are never used (F11).
- **Readable**: `nvidia` (whether NVML opens is only known in the sampler, which shows `--` and logs why if it doesn't), and `amdgpu` when the device has `gpu_busy_percent`. Others are listed but can't be picked.
- **`auto`**: the readable GPU with `boot_vga` 1, else the readable GPU with the lowest PCI address. Here that is the RTX 4070. On a hybrid laptop it is usually the iGPU.
- **`off`**: no GPU values. The slot shows `GPU --`.
- **The selection is resolved** at the start of each sampling period, and again when `status_bar.gpu` changes.
- **Names for the editor**: an NVIDIA's from `/proc/driver/nvidia/gpus/<pci>/information` (`Model:`), so the editor's list never starts NVML. An AMD's from `device/product_name` when present, else `AMD GPU`. Names are for the editor only.
- One GPU in the bar. A second wouldn't fit (the right half is full).

### Temperatures

- **hwmon by name**: each sampling period looks up hwmon directories by their `name` file, never by number (F14), and looks again after a read fails.
- **CPU**: `k10temp` or `zenpower`: `Tdie` if present (older Zen parts add an offset to `Tctl`), else `Tctl`. `coretemp`: the highest `Package id N`. Here that gives `Tctl`, 64 °C, while the two CCDs read 51 and 38. `Tctl` is what AMD's tools and `btop` show. Without any of these sensors there is no `cpu_temp`.
- **AMD GPU**: the selected card's own `device/hwmon/*`, `junction` if present, else `edge`, else `temp1`. It doesn't scan all hwmon, so an iGPU and a dGPU never mix.
- **NVIDIA GPU**: NVML's GPU temperature sensor.
- Millidegrees are rounded to the nearest whole °C.

### Daemon

- **`hwmon.rs`** (new): pure parsers and the lookup above, under a sysfs root.
- **`gpu.rs`** (new): enumeration, readability, `auto`, the AMD reader.
- **`gpu/nvidia.rs`** (new): the two traits, the `nvml-wrapper` implementation, the fake for tests, and the `nvml` thread.
- **`hoststats.rs`**:
  - `Sampler::new(proc_root, sys_root)`.
  - `HostStats` gains `cpu_temp`, `gpu: Option<Gpu { busy, temp }>` and `colors`, and stays `Copy`.
  - `spawn(polling, config, sources)` takes the config receiver as `tiles::spawn` does (F2). At each tick it reads the GPU selection and the steps from the live config.
- **`alerts.rs`** (new): steps, hysteresis and colors, pure.
- **`server.rs`**:
  - `stats_frame` adds the new fields and their guards.
  - `Server::bind` takes a `HostSources { proc_root, sys_root, nvidia: Arc<dyn NvidiaOpener> }`. `lib.rs` sets it to `/proc`, `/sys` and the real opener, so `device_sim` can pass fixture trees and a fake opener. Without that, `device_sim` would open the real NVML on this desktop through `auto`, and in CI would read whatever the container's `/sys` holds.
  - The session code doesn't change otherwise.
- **`commands.rs`**: `async fn list_gpus() -> Vec<GpuInfo { id, name, driver, readable, boot_vga }>`, plus which one `auto` picks now. It is async, like `list_icons`, so the sysfs walk never runs on the main thread.

### Config UI

- **`StatusBarForm.tsx`** (new), in the Layout tab, opened from a "Status bar" button in the grid panel's head (`GridEditor.tsx:204`). While it shows, no button is selected. It is part of the same draft and the same Save.
  - **GPU**: a select of "Auto (currently: NVIDIA GeForce RTX 4070)", "Off", and each GPU from `list_gpus` with its address. Unreadable ones are disabled with the reason. A configured address that isn't present shows as "0000:xx:xx.x (not found)".
  - **Metrics**: one row each for CPU, Memory, GPU, CPU temperature, GPU temperature, Download and Upload, with the unit. Each row has up to 3 steps (a number, and the color field from `ButtonForm.tsx:37`, moved to `fields.tsx`), Add step, Remove, and "Use defaults". "Use defaults" removes the key and shows the default steps read-only.
- **`errors.ts`**: a `statusBar` bucket keyed by metric (and `gpu`), from `^status_bar\.`, shown next to its row (F9). `numberErrors` also covers each step's `at`.
- **`edit.ts`**: `barChanged(before, after)`. After a save that changed the status bar settings, the save bar says "status bar settings apply within 2 s", beside any layout lines (F8). The 2 s are the 500 ms config poll plus the 1 s tick.
- **`config.ts`**: the `status_bar` types.

## Rejected alternatives

| Option | Why not |
|---|---|
| `nvidia-smi --query-gpu=... -lms 1000 -i <pci>` as one long-running child | User chose NVML. It had no new crate, no closed library in the app, and cost 15 ms CPU per 10 s (F17) |
| `nvidia-smi` started once a second | About 8 ms CPU and a process start every second (F17) |
| Our own NVML `dlopen` | The daemon's first `unsafe`, with ABI structs written by hand. `nvml-wrapper` holds that code, and V0 vets it |
| Linking `libnvidia-ml` at build time | The app wouldn't start on a host without the driver, and CI couldn't build it |
| sysfs for NVIDIA | Not there (F12) |
| `sysinfo` or libsensors for temperatures | A crate or a C library for a few hwmon files, as device-ui decided for `/proc`. Neither reads GPU busy |
| Thresholds in app settings (a store plugin) | A new plugin and a second place for settings, with no validation, last-good config or hand editing (F10) |
| The board gets the thresholds and decides | Hysteresis and units in both C and Rust, a new message, and thresholds kept on the board across reconnects. With colors in `stats`, a color always comes with the value it describes |
| Levels (`warn`, `crit`) on the wire with a palette sent separately | The user sets colors. Sending the color is one mapping fewer, like `bg` and `fg` in `layout` (F6) |
| Fixed warn and crit per metric | Steps of `{ at, color }` say "memory at 90% red" directly, and allow one, two or three |
| A new capability | The fields are optional and an older board ignores them (F4) |
| An icon font for CPU, GPU, memory and temperature | A new generated font with its own license entry, while the text prefixes fit. A follow-up if wanted |
| A colored background per slot | One slot holds two values, which can't have two backgrounds |
| A label per value instead of recolor | About twice the labels, in a pool with about 10 KB free (F22) |
| A hold time instead of hysteresis | It delays a real spike's color. Hysteresis shows the color at once and only delays clearing it |
| Every GPU in the bar | The right half is full |
| `°C` | 17 px per temperature, with the right half full, and no Fahrenheit option it would tell apart |
| GPU memory | Not asked, and no room |

## Slices

| Slice | Kind | Depends on | Board check |
|---|---|---|---|
| B1 Wire contract | mechanical, both parsers | device-ui done | no |
| B2 Config schema | mechanical, schema | device-ui done | no |
| B3 sysfs sensors | design judgment, Rust | B2 (`GpuChoice::parse`) | no |
| V0 Vet the NVML crates | reviewer, supply chain | — | no |
| B4 NVML reader | design judgment, Rust, threads | V0 passed | no |
| B5 Sampler, colors and sessions | design judgment, daemon | B1, B2, B3, B4 | via B7 |
| B6 Status bar form | design judgment, app | B2, B3 | no, window check |
| B7 Values and colors on the board | design judgment, firmware, small | B1, device-ui S7. Its board check needs B5 merged | yes |
| B8 Hardware run and docs | operator plus scribe | B5, B6, B7 | yes |

**Waves**, each slice in its own worktree:

| Wave | Slices | Shared files |
|---|---|---|
| 1 | B1, B2, B3, V0, then B4 once V0 passes | B2 defines `GpuChoice::parse` in `config.rs`, the one thing B3 needs from it; merge B2 first and rebase B3. B2 and B3 both touch `commands.rs` (`status_bar_defaults`, `list_gpus`). B3 and B4 both add a module (one line each). B3 and B4 agree on the PCI address form (`dddd:bb:dd.f`, lowercase) in this plan |
| 2 | B5, B6, B7 | None |
| 3 | B8 | — |

Every flash needs the user's yes, and nobody runs `erase_flash`. The pairing survives, since NVS doesn't change.

### B1 Wire contract

- **Files**: `protocol.rs`, `tests/fixtures/stats.json` (every field) and a new `stats_colors.json`; `proto.h`/`proto.c`; `test_proto.c`; `site/src/protocol.md` (the fields, refusals and the example).
  - `protocol.rs`: `Message::Stats` gains `gpu`, `cpu_temp`, `gpu_temp`, and `colors: Option<Colors>`, a struct of seven optional `u32`, each skipped when `None`.
  - `proto.h`/`proto.c`: `proto_stats_t` gains `has_gpu`, `gpu`, `has_cpu_temp`, `cpu_temp` as `int16_t`, the GPU temperature likewise, and `colors[7]` with a `colors_mask`. `parse_stats` reads them.
- **Host cases**: the full fixture; `gpu` 101, -1 and 23.5; `cpu_temp` -41, 151 and -40; `gpu_temp` without `gpu` and the reverse (both accepted); `colors` as an array, as a string, empty, with an unknown key (ignored), with `cpu` 16777216, -1 or a string (refused); a `colors.gpu` with no `gpu` field (accepted, ignored).
- **Acceptance**: both fixtures round-trip on the daemon. The board parses both and refuses each malformed case. The existing `stats` and `stats_min` cases pass unchanged.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml protocol` and `mise run fw:test`.

### B2 Config schema

- **Files**: `config.rs`, `commands.rs` (`ConfigView.status_bar_defaults`), `app/src/config.ts` (the types), `site/src/config.md` (a Status bar section: the keys, units, defaults, hysteresis, what `auto` picks, and the laptop note).
  - `config.rs`: `StatusBarConfig`, `Thresholds` and `Step`, with `gpu: Option<String>`; `GpuChoice::parse(&str) -> Option<GpuChoice>` (`Auto`, `Off`, `Pci(String)`), which `check()` and the sampler both call; the defaults; the rules in `check()`.
- **Tests**: the example and the default file still pass; each rule's message above, word for word; `[]` and an absent key differ; 3 steps pass and 4 don't; an unknown key under `[status_bar]` or in a step is refused; a thresholds-only change leaves `Live::layout()` equal (F6).
- **Round trip**: the Layout tab's save rewrites the whole file from the draft (`config.md:71`), so a `[status_bar]` the form doesn't know about must survive it.
  - A Rust test saves a config with every `status_bar` key through `Configs::save` and reads back an equal config.
  - An `edit.test.ts` case checks that a button edit keeps `status_bar` in the draft.
  - The `toml` serializer may write the steps as `[[status_bar.thresholds.mem]]` tables rather than the inline form above. Both parse, and `config.md` shows both.
- **Acceptance**: `cargo test` and clippy clean. A `config.toml` without `[status_bar]` loads as before.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml config::`.

### B3 sysfs sensors

- **Files**: `daemon/hwmon.rs`, `daemon/gpu.rs` (enumeration, readability, `auto`, the AMD reader), `daemon/mod.rs`, `commands.rs` and `lib.rs` (`list_gpus`), `app/src/api.ts` (the call).
- **Fixtures**: each test builds a fake sysfs tree in a temp dir, with real `driver` symlinks (`std::os::unix::fs::symlink`):
  - this desktop's layout: NVIDIA at `0000:01:00.0` with `boot_vga` 1, no hwmon, and a `/proc/driver/nvidia/gpus/.../information`; AMD at `0000:75:00.0` with `gpu_busy_percent` and `hwmon8`; `k10temp` at `hwmon4` among the `nvme` and `spd5118` entries;
  - an Intel `coretemp` host with two packages;
  - an older Zen with `Tdie`;
  - a host with no GPU.
- **Tests**:
  - CPU: `Tdie` beats `Tctl`; `coretemp` gives the higher package; no sensor gives none; renumbered hwmon directories still find the sensor; 64875 m°C gives 65.
  - GPU: enumeration finds both; `auto` picks the `boot_vga` one, and without one the lowest address; Intel is listed but not readable; an AMD dGPU's `junction` beats `edge`; the iGPU reads its own hwmon, not another card's; the NVIDIA name comes from the `information` file.
  - Bad values (511000, -273000) come back as they are; the guard is B5's.
- **Acceptance**: `cargo test` and clippy clean, no new crate in `Cargo.lock`.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml hwmon gpu::`.

### V0 Vet the NVML crates

Before B4 starts, in a scratch worktree that is thrown away afterwards. The `dependency-vetting` skill covers npm and PyPI only, so this is done by hand. No new tool goes into `mise.toml` for it unless the user asks.

1. **The exact set**: run `cargo add nvml-wrapper@=0.13.0 --no-default-features` in `app/src-tauri`, so the optional `serde` stays off, then `cargo tree -i <each>`. The `Cargo.lock` diff must add exactly these: `nvml-wrapper` 0.13.0, `nvml-wrapper-sys` 0.10.x, `wrapcenum-derive` 0.4.1, `static_assertions` 1.1.x, `libloading` 0.8.x (beside 0.7.4), `darling`, `darling_core` and `darling_macro` 0.20.x (beside 0.24.1). It may also add any small helper they pull in (such as `ident_case` or a second `strsim`). Record every package and version.
2. **Each crate on crates.io**: the owners and publisher, the repository link, not yanked, the last release date, and the license (MIT, Apache-2.0 or ISC are fine).
3. **Advisories**: query osv.dev (`https://api.osv.dev/v1/query`, ecosystem `crates.io`) for each name and version, and read the RustSec advisory list for them.
4. **Source matches the repo**: for `nvml-wrapper`, `nvml-wrapper-sys` and `wrapcenum-derive`, download the `.crate` from crates.io and diff it against the repository's tag for that version. Generated bindings may differ only as the repo's own generator would produce them.
5. **Build-time code**: no `build.rs` in `nvml-wrapper` or `nvml-wrapper-sys` that touches the network, the file system outside `OUT_DIR`, or the environment. Read the proc macros (`wrapcenum-derive`, `darling_macro`) for anything other than token generation.
6. **Runtime loading**: `nvml-wrapper-sys` opens the library only through `libloading::Library::new(path)`. It has no `#[link(name = "nvidia-ml")]` and no `extern` block that the linker would resolve. Grep for `#[link` and `extern "C"`.
7. **`unsafe`**: skim where `nvml-wrapper` calls the bindings. It is expected there, behind a safe API. Our code adds none.
8. **Size**: record the release binary's size before and after, built on this desktop.

- **Stop** and go back to the user if: an advisory applies; a published crate doesn't match its repository; a build script has side effects; or a package outside step 1's set appears. The rejected `nvidia-smi` loop is the documented fallback.
- **Output**: a short vetting record (each package, version, license, advisories none, source checked, build script none or what it does), put in this plan's results and in B4's commit message.

### B4 NVML reader

- **Files**: `Cargo.toml` (`nvml-wrapper = { version = "=0.13.0", default-features = false }`, with a comment that V0 vetted it and that it is opened at runtime), `Cargo.lock`, `daemon/gpu/nvidia.rs`, `daemon/mod.rs`.
- **Changes**: the traits, the `nvml-wrapper` implementation (`lib_path("libnvidia-ml.so.1")`, `device_by_pci_bus_id`, `utilization_rates`, `temperature` with the GPU sensor, `pci_info` and `name` for `list`), the fake, and the `nvml` thread with its faults handling. No wiring.
- **Tests**:
  - With the fake opener: the reading for the selected address; addresses normalized from `00000000:01:00.0`; an address NVML doesn't know gives no value and logs once; temperature not supported gives busy only; busy 101 gives none; library missing gives no values and no retry; driver not loaded retries at the given interval; a lost GPU drops the handle and reopens; dropping the period's handle shuts the fake down (it counts shutdowns); a fake whose `read` blocks leaves the published reading stale, and no second thread starts.
  - With the real opener and a library name that doesn't exist: `open` fails with "library missing". This runs in CI and exercises the no-driver path for real.
  - An `#[ignore]` test reads `0000:01:00.0` through the real NVML. On this desktop, `cargo test -- --ignored nvml` gives busy 0 to 100 and a temperature of 20 to 100 °C.
- **Acceptance**:
  - `cargo test` and clippy clean, with no `unsafe` in our code.
  - `ldd target/debug/watchdeck` names no `libnvidia-ml`.
  - The CI app job builds and tests green without NVIDIA.
  - The ignored test passes on this desktop.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml nvidia`, then the ignored test here.

### B5 Sampler, colors and sessions

- **Files**: `daemon/alerts.rs` (new), `hoststats.rs`, `server.rs` (`spawn(polling, config, sources)` at `:143`, `HostSources` in `bind`, `stats_frame`), `lib.rs` (the system sources), `tests/device_sim.rs` (fixture sources and the fake opener for every test).
- **Tests**:
  - `alerts`:
    - each default;
    - a value at `at` colors, and one below doesn't;
    - 91, 89, 88, 87 with `at` 90 stays colored until 87 (`h` 2);
    - a jump from none to the third step;
    - a fall across two steps at once;
    - a rate at `at × 10^6` colors, and 10% below clears;
    - a missing value clears its step;
    - a config change starts afresh;
    - `[]` never colors.
  - `stats_frame`: `cpu_temp` 151 and -41 are left out with their colors; `gpu` 101 likewise; an empty `colors` is left out.
  - Sampler, with the B3 fixtures, the fake opener and a paused clock: the GPU resolves at the start of a period; `gpu = "off"` opens nothing; switching from the NVIDIA to the AMD shuts the fake down; a period without sessions opens nothing.
  - `device_sim`: a `status_bar` session gets a `stats` frame with `cpu_temp` and `gpu` from the fixtures. With a thresholds config whose `mem` step is at 1, the frame's `colors.mem` is that step's color. A thresholds-only edit then sends no `layout` within 2 s, and the next frames carry the new color.
- **Acceptance**: every existing test passes unchanged. On this desktop, with the app running and a device connected, `ls -l /proc/<app pid>/fd` shows `/dev/nvidia*` files, and none 2 s after the device disconnects.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml alerts hoststats` and `--test device_sim`.

### B6 Status bar form

- **Files**: `StatusBarForm.tsx` (new), `ConfigPage.tsx`, `GridEditor.tsx` (the head button), `fields.tsx` (`ColorField` moved from `ButtonForm.tsx`), `errors.ts`, `edit.ts`, `config.ts`, `api.ts`, `app/tests/edit.test.ts`, and new `errors.ts` cases.
- **Tests**:
  - every rule message from B2 lands in its metric's bucket;
  - `numberErrors` (`errors.ts:222-246`) catches a step's `at` that isn't a whole number before the call;
  - `barChanged` is true for a thresholds-only edit and false for a button edit;
  - `layoutTargets` is empty for a thresholds-only edit.
- **Acceptance**: in the window:
  - The GPU select lists both GPUs here, with "Auto (currently: NVIDIA GeForce RTX 4070)".
  - Setting memory's steps and saving writes a `config.toml` that parses back to the same steps.
  - "Use defaults" removes the key.
  - A refused value shows next to its row.
  - After a thresholds-only save, the save bar says the status bar applies within 2 s and names no layout push.
- **Cheapest check**: `pnpm test`, `pnpm build`, Biome from `app/`. The window check needs the user.

### B7 Values and colors on the board

S7 has built the slots, their positions and widths, and the formats for what it shows. This slice only fills them in.

- **Files**: `ui.c` (recolor on the CPU, MEM, GPU and NET labels; the new fields in the stored frame; the overflow strings with markup), `hoststat.c/.h` (temperatures, GPU busy, color markup), `test_hoststat.c`.
- **Host cases**:
  - every CPU and GPU row of the format rules, with `144°` and `100%` worst digits;
  - color markup around exactly the colored value (`CPU #e53935 97%# 64°`), and around each of the two net values separately;
  - a `colors` entry for an absent value adds nothing;
  - memory at 89.99% shows `89%` and isn't colored at 90;
  - stale (over 5 s) gives `--` with no markup.
- **Acceptance (board, against the B5 daemon)**:
  - The five slots are where S7 put them, with no `bar slot overflow` warning for the final strings with markup. `°` draws as a glyph, not a placeholder box. No `#` is ever visible.
  - With `mem = [{ at = 1, color = "#e53935" }]` saved, the memory value turns red within 2 s and the daemon log has no `pushing layout rev`. With the key removed it is grey within 2 s.
  - The `LVGL heap` line at "page built" is within 300 bytes of S7's number.
- **Cheapest check**: `mise run fw:test`, `mise run fw:build`, then the board. Needs the user for every flash.

### B8 Hardware run and docs

Run on the board and this desktop, the daemon and firmware from the same commit.

1. **NVIDIA**: put a load on the RTX 4070 (`vkcube` or a game). For 30 s, compare the bar's GPU busy and temperature with `nvidia-smi --query-gpu=utilization.gpu,temperature.gpu --format=csv -lms 1000` in a terminal: the same trend, and each temperature within 2 °C.
2. **AMD iGPU**: pick `0000:75:00.0` in the form. Within 2 s the app holds no `/dev/nvidia*` file. Load it with `DRI_PRIME=pci-0000_75_00_0 vkcube`: busy rises as `gpu_busy_percent` does, and the temperature matches `sensors amdgpu-pci-7500` within 2 °C.
3. **CPU temperature**: under `cargo build` load, compare with `sensors k10temp-pci-00c3` `Tctl`: within 2 °C.
4. **Lifecycle**: with the daemon running and no device connected, the app holds no `/dev/nvidia*` file. A device connects: it does. `gpu = "off"`: the slot reads `GPU --` and no NVIDIA file is held.
5. **No driver**: B4's CI test (the real opener with a missing library) is the evidence. If a host or VM without the NVIDIA driver is at hand, also start the app there: it starts, logs "no NVIDIA driver" once, and an NVIDIA selection reads `GPU --`. Record which evidence was used.
6. **Colors**: `cpu = [{ at = 30, color = "#e53935" }]` during a build: the CPU value turns red, and goes back to grey only after the load drops more than 2 points below 30. Record whether it flickers.
7. **Memory**: the pages fixtures as in device-ui S8. Settled pool, largest free block and minimum internal heap within 300 bytes of device-ui S8's results.

- **Docs**:
  - `site/src/protocol.md` final.
  - `site/src/config.md`: the Status bar section and the bar's formats.
  - `CLAUDE.md` Daemon facts: GPU sources, NVML opened at runtime from `libnvidia-ml.so.1` only while a device is connected, its own thread, hwmon found by name, `[status_bar]`, colors decided by the daemon.
  - `CLAUDE.md` Hardware facts: the recolor markup.
  - A results section in this plan, with V0's vetting record.
- **Acceptance**: every step passes or its failure is recorded with a decision.

## Risks

| Risk | Mitigation |
|---|---|
| A crate in the NVML tree is malicious, abandoned or vulnerable | V0 before B4, with stop conditions and the `nvidia-smi` loop as the fallback. `=0.13.0` is pinned |
| A crash inside `libnvidia-ml` takes down the app, with the daemon in it | Accepted by the user's decision. NVML runs only while a device is connected, on its own thread. Nothing persistent is at risk, and the app starts again |
| No NVIDIA driver, or a library and driver of different versions after an update | `init` fails cleanly (B4 CI test). The slot shows `GPU --`, there is one log line, and `init` is retried every 60 s for a mismatch |
| An NVML call blocks for seconds on a wedged GPU | The `nvml` thread. CPU, memory and net keep coming, GPU goes `--` after 3 s, and no second thread starts (B4 test) |
| NVML keeps a laptop's dGPU awake | `auto` prefers `boot_vga`, usually the iGPU there. NVML is closed outside periods and when another GPU is picked. `off` exists. Documented |
| A sensor reports nonsense (511 °C, -273 °C) and the board drops every frame (F4) | `stats_frame` leaves out the value and its color (B5 test). The rest of the bar keeps working |
| hwmon numbers change between boots | Lookup by name each period and after a failed read (B3 test) |
| `Tctl` carries an offset on older Ryzen | `Tdie` wins when present (B3 test) |
| Colors flicker near a threshold | Hysteresis (B5 tests), B8 step 6 |
| The board and the daemon judge a different number (rounding) | One rule each, pinned on both sides: memory rounded down, rates truncated, temperatures rounded before both |
| The right half of the bar overflows | 0 px spare by the table. The overflow check measures the final strings with markup, and the fallback takes the 4 px margin, then 2 px of the GPU-to-NET gap |
| S7 sizes a slot for what it shows today rather than the final strings | The budget is in this plan and in the S7 hand-off. B7 fixes any width and says so |
| PCI address of a GPU changes (a card moved to another slot) | The editor shows "(not found)"; pick it again |
| A save from the Layout tab drops a hand-written `[status_bar]` | B2's round-trip tests on both sides |
| `device_sim` behaves differently on this desktop (real NVML) and in CI (the container's `/sys`) | `HostSources` injected, with fixture trees and the fake opener in every `device_sim` test |
| A runtime-suspended AMD dGPU, picked by hand, is woken by the reads or refuses them | `auto` doesn't pick it on a hybrid laptop. A refused read shows `--`. Documented |
| A thresholds-only edit takes longer than 2 s, because the reload also checks icons | B7 times it on the board. If it is over 2 s, the save bar's wording changes, not the design |

**Rollback.**

- Each slice is its own commit, so `git revert` undoes one.
- The firmware rolls back by flashing the build before B7, with the daemon from the same commit. NVS doesn't change, so the pairing survives.
- The riskiest slice is B4 (a closed library in the app process). Reverting B4 and B5 removes the crate tree and leaves the bar without GPU values. Everything else keeps working, and the S7 slot keeps reading `GPU --`.
- A daemon from before B2 refuses `[status_bar]` in a user's `config.toml`. Remove the table before going back.

## Open questions

None. The user settled every question on 2026-10-22 (Decisions).

## Critique log

No outside review. The consult tool refused both reviewers, as it did for device-ui: the codex app server isn't locked down (`web_search` unset, `features.plugins` missing), and the agy reviewer's hook blocked its own `read_file`. So this is a self-review that argued against each slice and the approach, with each point checked against `74ffb38` or this desktop. Points R5, R6 and R13 were about the `nvidia-smi` loop, and the NVML decision superseded them.

| # | Point | Outcome |
|---|---|---|
| R1 | `device_sim` builds the real server, so `auto` would use the real NVIDIA on this desktop and read the container's `/sys` in CI | adopted: `HostSources` in `Server::bind`, fixture trees and the fake opener in `device_sim` (Daemon, B5) |
| R2 | The Layout tab's save rewrites the whole file (`config.md:71`), so a `[status_bar]` the draft lost would be erased | adopted: round-trip tests on both sides in B2 |
| R3 | The `toml` serializer may write steps as `[[status_bar.thresholds.mem]]`, so "the file reads as the example" would fail | adopted: B6 checks the parsed steps, and `config.md` shows both forms |
| R4 | Recolor markup cut by the label's dots would show a stray `#` | adopted: slots sized so a label is never cut, and the overflow check measures with markup and the recolor flag (Bar budget) |
| R5 | `kill_on_drop` doesn't run if the app is killed with SIGKILL | superseded: no child process with NVML |
| R6 | A killed child is reaped asynchronously, so a "pid gone" check can race | superseded: no child process with NVML |
| R7 | B7's board check reads GPU and colors, which only the B5 daemon sends | adopted: the slice table says the board check waits for B5 |
| R8 | `list_gpus` as a sync command would walk sysfs on the main thread | adopted: async, like `list_icons`. It also never starts NVML (names from `/proc/driver/nvidia`) |
| R9 | A thresholds-only edit reloads the whole config, icons included, so "within 2 s" may not hold with many icons | adopted as a B7 measurement. Only the save bar's wording depends on it |
| R10 | A runtime-suspended AMD dGPU may wake on the reads or refuse them | adopted as a risk: `auto` doesn't pick it on a hybrid laptop, and a refused read shows `--` |
| R11 | A background thread or task on Tauri's runtime might stall the sampler | adopted in a new form: NVML gets its own `std::thread`, so no blocking call runs on a tokio worker |
| R12 | A step at `at` 1 with `h` 2 never clears | rejected: a 1% step means "always", which is what it says |
| R13 | `nvidia-smi`'s CSV could change between drivers | superseded: NVML has a typed API |
| R14 | Colors from the daemon don't work offline | rejected: offline the values show `--`, so there is nothing to color |
| R15 | Hysteresis state is shared by all sessions | rejected as a defect: one host, so every device shows the same color for the same value |
| R16 | The first frame of a period has no CPU share, so no CPU color for a second | accepted as it is, as with device-ui's first frame |
| R17 | Without the name, a user with two desktops can't tell from the bar which one a board talks to | settled by the user: the name is in settings |
| R18 | `gpu` as a serde enum would fail in the TOML decoder, so the user would never see the rule's message | adopted: `gpu` stays a string judged in `check()`, with `GpuChoice::parse` shared with the sampler (B2) |
| R19 | A fractional `at` from the form would come back as a decode error, not a rule | adopted: `numberErrors` covers `at` (B6) |
| R20 | Real-time tests at production timings would take tens of seconds in every CI run | adopted: timings are constructor arguments, and tests use milliseconds (B4) |
| R21 | `Nvml::init()` loads `libnvidia-ml.so`, which many distributions ship only in a development package (F23, F24) | adopted: `lib_path("libnvidia-ml.so.1")` |
| R22 | NVML calls can block for seconds, and starting NVML takes milliseconds, on the sampler's task | adopted: one `nvml` thread per period, at most one alive, publishing readings with their time |
| R23 | `Device<'nvml>` borrows the `Nvml`, which fights a handle kept across ticks | adopted: the trait hides it, and the implementation looks the device up by address on each read |
| R24 | A test that needs a real NVIDIA can't run in CI | adopted: the fake opener for logic, a CI test of the real opener with a missing library, and an `#[ignore]` test for this desktop |
| R25 | With the clock centered, the right half has no spare pixels | adopted: stated in the budget, with a measured fallback (the margin, then 2 px of a gap) and the overflow check on the final strings |
