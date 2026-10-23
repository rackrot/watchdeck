# Status bar 2: GPU, temperatures, units, and colors past a threshold

Status: planned 2026-10-22, revised the same day with the user's answers and the unit settings. Against `main` at `0ddf91d`, where device-ui S1 to S6 are merged. Device-ui S7 is built on its branch, first as `3e30f9a` and since revised to the layout below. Starts after `plans/device-ui.md` S8 passes. No slice started.

**Device-ui S7 has built the bar's layout. This plan doesn't move it.** Left to right: CPU, RAM, GPU and NET groups, spaced equally, then the date and time flush right in an area reserved for `Wed 24 May 04:44`. The bar is drawn in Roboto Mono 24, with Montserrat 24 as the fallback for `°` and the arrows. Each value is right-aligned in a fixed number of cells, and each net rate gets 6 cells, sized for `999.9M`. The GPU group reads `GPU --`. S7 measures each group's worst string when the bar is created and works out the gaps from them (`ui.c` `bar_create` on S7's branch). So B7 changes the worst strings, the groups get their new widths, and the gaps shrink to fit. B7 only adds GPU busy, temperatures, the unit forms and colors to groups that already exist.

After this plan:

- The bar shows the desktop's CPU use and temperature, RAM, GPU use and temperature, network rates, and the date and time.
- The app's Status bar settings choose the units: temperature in °C or °F, and rates in SI bytes (`1.2M`), binary bytes (`1.1Mi`) or bits (`9.6Mb`, pending Q2).
- Each value can change color when it passes a threshold, for example RAM at 90% in red. The thresholds are entered in the chosen units and stay right when the units change.
- The GPU is the NVIDIA RTX 4070 on this desktop by default, read through NVML. The AMD Radeon 610M iGPU on the same host can be picked instead. On a host with several GPUs, the bar shows one, picked automatically or by PCI address.

## Decisions 2026-10-22

The user answered the open questions of the first draft:

- **Daemon name** (was Q1): not in the bar. It is in settings. Built by device-ui S7, which also decides what the bar shows while the link is down. This plan doesn't change that.
- **Rates** (was Q2): bytes per second with SI prefixes and no `B/s` (`▼1.2M ▲340k`). Built by S7. This plan adds binary and bits forms (Units).
- **Memory** (was Q3): a percent, labelled `RAM` (S7 renamed it from `MEM`), which is also the unit of its threshold. Built by S7.
- **Order and type** (the user's, built by S7): CPU, RAM, GPU, NET spaced equally, the clock flush right, Roboto Mono 24, values right-aligned in fixed cells.
- **No GPU** (was Q4): with `off`, or none found, the group shows `GPU --`.
- **Default steps** (was Q5): as in the Config table. GPU busy and network aren't colored by default.
- **NVIDIA source** (was Q6): NVML through the `nvml-wrapper` crate, loaded at runtime from `libnvidia-ml.so.1`. A host without the NVIDIA driver still runs and shows `GPU --`. The GPU is chosen by PCI address through NVML. A trait seam keeps CI, which has no NVIDIA, testable. The crates are vetted first (V0). The `nvidia-smi` loop is rejected.
- **`auto`** (was Q7): the `boot_vga` GPU, else the lowest PCI address. That is the RTX 4070 here.
- **CPU temperature** (was Q8): `Tdie` if present, else `Tctl`, or the hottest `coretemp` package. No key to pick another sensor.
- **Units** (the user's request): °C or °F for temperatures, and SI or binary prefixes for rates, chosen in the app's Status bar form and kept in `[status_bar]`. This plan decides where the conversion happens (on the board, Units). Bits per second is open as Q2.

## Goal and done

- **The bar**, in the groups S7 built, shows CPU busy and temperature (`CPU 12% 64°C`), RAM (`RAM 45%`), GPU busy and temperature (`GPU 23% 41°C`), both rates (`▼1.2M ▲340k`) and `Wed 22 Oct 14:32`. Values are right-aligned in their cells, with the padding S7 uses. On this desktop the numbers agree with `nvidia-smi`, `sensors` and `top` (B8).
- **Units**: `temp_unit` (`c` or `f`) and `net_unit` (`si`, `binary`, and `bits` if Q2 says so) in `[status_bar]`, set in the Status bar form. A change shows on the board within 2 s, without a layout push and without moving any group.
- **Thresholds in the chosen units**: entered and stored in the units the bar shows. A value is colored exactly when the number on the bar has reached the step. Switching units in the form converts the steps.
- **GPU**: NVIDIA through NVML (`nvml-wrapper`, opened at runtime), AMD through sysfs. `[status_bar] gpu` is `"auto"` (the default), `"off"`, or a PCI address. `auto` picks the RTX 4070 here.
- **No NVIDIA driver**: the daemon starts, runs and shows `GPU --` (or the AMD) on a host without `libnvidia-ml.so.1`. The binary doesn't link the library (B4 checks with `ldd`).
- **Temperatures**: the CPU's from hwmon (`k10temp` or `coretemp`), found by name and not by hwmon number. The GPU's from NVML or from the card's own hwmon.
- **Colors**: `[status_bar.thresholds]` gives each metric up to 3 steps of `{ at, color }`. The daemon works out each value's color, with hysteresis, and sends it in `stats`. The board draws that value in that color. An edit applies within 2 s and pushes no layout.
- **Lifecycle**: NVML is open only while a device is connected and the selected GPU is an NVIDIA. The app holds no `/dev/nvidia*` file within 2 s of the last session ending, or of a switch to another GPU.
- `cargo test`, `pnpm test` and `mise run fw:test` cover the new rules without a GPU (CI has none). The hardware run (B8) passes on the board with serial-log evidence.
- `site/src/protocol.md`, `site/src/config.md` and `CLAUDE.md` are updated.

## Assumptions

- **device-ui S7 as described above** is merged before B7: the groups, the right-aligned value cells, the runtime measurement of worst strings, `on_stats` storing each frame, and `hoststat.c` with host tests. B7 adapts to S7 as merged. If S7's gap rule differs from "equal gaps across the rest", the budget's totals still hold and only the gap figure changes.
- **One host, Linux.** As for device-ui. Thresholds and units are per host, not per device.
- **Whole units.** Percent and °C on the wire are whole numbers. Every threshold `at` is a whole number in its display unit.
- **Canonical wire units.** `stats` keeps °C and bytes per second. The units only say how the board shows them.
- **NVML in the app process.** A crash inside `libnvidia-ml` takes the app down with it. The user chose NVML knowing this.
- **Development only.** Board and daemon come from the same commit. Old-firmware compatibility isn't needed (device-ui decision A).

## Findings

Checked against `0ddf91d`, S7's branch, and this desktop on 2026-10-22.

| # | Where | What |
|---|---|---|
| F1 | `hoststats.rs:20-27`, `:152`, `:223`, `:236-268` | `HostStats` is `Copy` with CPU, memory and net. `Sampler::new(root)` reads under one root (`/proc`). `spawn(polling)` takes no config. The loop samples while `polling > 0` and publishes `None` at 0 |
| F2 | `server.rs:142-144`, `:780-830` | `hoststats::spawn(polling.clone())` beside `tiles::spawn(actions, config.clone(), polling)`. The session copies each reading out of the watch and stamps `time` at send |
| F3 | `server.rs:974-993` | `stats_frame` leaves out any field outside its wire range, so the device never refuses a frame. New fields need the same guard |
| F4 | `protocol.rs:12`, `:175`; `protocol.md:431-449` | `CAPS` already holds `status_bar`. The board ignores unknown fields, and drops the whole frame if any field has the wrong type or is out of range |
| F5 | `proto.c:278`, `:285`, `:423-465` | `get_whole` takes signed bounds (used for `utc_offset`). `get_pair` makes two fields both-or-neither, which suits `mem_*` and `net_*` but not GPU busy and temperature: a GPU without a temperature sensor must still show its busy share |
| F6 | `config.rs:73-83`, `:268-274`, `:328-382`, `:428` | `Config` refuses unknown fields. `color()` parses `#rrggbb` to `0xRRGGBB`, the form `layout` sends `bg`/`fg` in. `Live::layout` reads only the grid and buttons, so a new table can't change a layout. `check()` returns every broken rule as a string, and validated strings (`bg`, `font`, `url`) are plain `String` fields |
| F7 | `server.rs:794-812` | A config change pushes only when the session's view changed, so a `[status_bar]`-only edit pushes nothing |
| F8 | `edit.ts:220-243`, `ConfigPage.tsx:515-520` | `viewKey` ignores keys outside the grid and buttons. So a `[status_bar]`-only save has no targets, and the save bar would wrongly say "the layout is unchanged, so devices keep it" |
| F9 | `errors.ts:204`, `:222-246` | Only `layout.*`, `version` and the page and tile counts go to the grid bucket, so `status_bar.*` messages would land in `general`. `numberErrors` catches numbers that aren't whole before the call, since Rust would answer those with a decode error |
| F10 | `Cargo.toml:20-70` | No settings or store plugin. `libloading` 0.7.4 is in the lock through `libappindicator-sys`. There is no `unsafe` in `app/src-tauri/src`. No `cargo-audit` or `cargo-deny` in `mise.toml` |
| F11 | `lspci`, `/sys/class/drm` | `01:00.0` NVIDIA AD104 (RTX 4070), driver `nvidia` (open kernel module 615.78.08), `boot_vga` 1. `75:00.0` AMD Granite Ridge (Radeon 610M), driver `amdgpu`, `boot_vga` 0. `card0` is the AMD and `card1` the NVIDIA, so card numbers say nothing about which GPU is which |
| F12 | `card1/device`, `/sys/class/hwmon/*/name` | The NVIDIA driver exposes no `gpu_busy_percent` and no hwmon. Busy and temperature on NVIDIA need NVML |
| F13 | `card0/device` | `gpu_busy_percent` (0 at idle) and `device/hwmon/hwmon8` (`amdgpu`, `temp1_label` `edge`, 40000). Each sysfs read takes about 8 µs |
| F14 | `/sys/class/hwmon` | `hwmon4` is `k10temp`: `Tctl` 64.0, `Tccd1` 50.9, `Tccd2` 37.5, no `Tdie`. Others: `nvme` ×3, `spd5118` ×2, `r8169`, `ath12k`, `prom21_xhci`. The numbers are assigned at boot |
| F15 | `lv_font_montserrat_24.c:4`, `:3490`, `:3522-3523` | The built-in Montserrat 24 holds `°` (advance 161/16) and `LV_SYMBOL_UP`/`DOWN` (336/16) |
| F16 | `lv_label.c:273`, `sdkconfig:5353` | `lv_label_set_recolor` exists in LVGL 9.6 with `CONFIG_LV_TXT_COLOR_CMD="#"`: `CPU #e53935 97%# 64°C` colors one value in one label |
| F17 | `nvidia-smi` 615.78.08 | One query: 13 ms wall and about 8 ms CPU, since each run starts NVML. The loop (`-l 1`) used 15 ms CPU over 10 s. NVML prints the bus id as `00000000:01:00.0` and the first busy reading after it starts as 0 |
| F18 | `nvidia-smi -q -d TEMPERATURE`, `/proc/driver/nvidia/gpus/0000:01:00.0/information` | Target temperature 84 °C. Persistence mode off, runtime D3 "Disabled by default". The `information` file gives `Model: NVIDIA GeForce RTX 4070` without starting NVML |
| F19 | crates.io, `Cargo.lock` | `nvml-wrapper` 0.13.0 (2026-08-31, MIT or Apache-2.0) needs `nvml-wrapper-sys` ^0.10 (`libloading` ^0.8.1), `wrapcenum-derive` ^0.4.1 (`darling` ^0.20, a proc macro), `static_assertions` ^1.1, `thiserror` ^1 and `bitflags` ^2, with an optional `serde`. New in the lock: `nvml-wrapper`, `nvml-wrapper-sys`, `wrapcenum-derive`, `static_assertions`, a second `libloading` (0.8 beside 0.7.4) and a second `darling` (0.20 beside 0.24.1, with `darling_core` and `darling_macro`) |
| F20 | `montserrat.ts`, `fonts.gen.ts:108-117` | The widest clock is `Wed 24 May 04:44` (S7 reserves its area). In Roboto Mono every character is as wide as any other, so it is 16 cells |
| F21 | `/proc/meminfo` | 96,186,728 kB (91.7 GiB), 45% used when checked |
| F22 | `device-ui.md:38` | The LVGL pool is about 59,800 bytes usable, with about 49,070 settled on the heaviest pages fixture |
| F23 | docs.rs `nvml-wrapper` 0.13.0 | `Nvml::init()` loads `libnvidia-ml.so`. `Nvml::builder().lib_path(&OsStr)` names another file, resolved as `libloading` resolves it. `device_by_pci_bus_id(&str)`, `device_count()`, and on `Device`: `utilization_rates`, `temperature`, `pci_info`, `name`. `Device<'nvml>` borrows the `Nvml`. `Nvml` and `Device` are `Send` and `Sync`. `Drop` shuts NVML down and ignores errors |
| F24 | `/usr/lib/libnvidia-ml.so*` | Here both `libnvidia-ml.so` and `libnvidia-ml.so.1` exist. Many distributions ship the unversioned name only in a development package, so the daemon asks for `libnvidia-ml.so.1` by name |
| F25 | `fonts.gen.ts:113-117`, `lv_font_fmt_txt.c:252-258` | Roboto Mono 24's advance is 230/16 for every ASCII character. LVGL rounds each glyph's advance to whole pixels (`(adv_w + 8) >> 4`), so a cell is 14 px, Montserrat's `°` 10 px and each arrow 21 px |
| F26 | S7's `ui.c` `bar_create` (`3e30f9a`) | The bar measures each group's worst string with `lv_text_get_size` when it is created, logs `bar slot overflow` if they don't fit, and spreads what is left as equal gaps. Values are right-aligned in fixed cells (`HOSTSTAT_VALUE_W`), so the `%` and the units stay put |

## Approach

### Units

- **Choices**, in `[status_bar]`:
  - `temp_unit`: `"c"` (default) or `"f"`. The bar shows `64°C` or `147°F`. The letter is always shown, since a bare `°` doesn't say which.
  - `net_unit`: `"si"` (default, bytes with 1000-based prefixes, `1.2M`), `"binary"` (bytes with 1024-based prefixes, `1.1Mi`), or `"bits"` (bits with 1000-based prefixes, `9.6Mb`, if Q2 includes it). Bytes have no `B`, as decided for SI (device-ui G). Bits always carry their `b`, so the two never look alike.
- **Where the conversion happens: on the board, in `hoststat.c`**, while it formats. The daemon keeps sending °C and bytes per second, and adds the two preferences to each `stats` frame (Wire). The reasons, against the fixed cells:
  - The cells belong to the board. It measures the worst string of every unit form when the bar is created (F26) and right-aligns each value in its cells. The function that turns a number into text in those cells has to know the unit form anyway: the prefix, the `i`, the `b`, the `°F`, and when to switch prefix so the number stays within its cells. Converting in that same function keeps the arithmetic and the width rule together, where one host test pins both.
  - If the daemon converted and sent a unit flag, the board would still need the flag to pick the suffix and the width rule, so the units would live in two places. The wire's numbers would also change meaning per session, and `stats_frame`'s range guards (F3) would need a range per unit.
  - A unit change is then only different text in the same cells. Nothing is laid out again, because the cells were measured for the widest form.
  - The preferences ride in every frame, so a change applies at the next frame and the board stores nothing across reboots. Until the first frame there are no values to show.
- **Conversion rules** (one table of test vectors, the same in `test_hoststat.c` and in the daemon's `alerts` tests):
  - °F from the whole °C on the wire: the nearest whole number to `c × 9 / 5 + 32`. `18c + 320` is even, so its tenths digit is never 5, and no tie can occur. Examples: -40 → -40, 0 → 32, 37 → 99, 64 → 147, 100 → 212, 150 → 302.
  - Rates: the value in the unit (bytes, or bits = bytes × 8), divided by 1000 (SI, bits) or 1024 (binary) until it is under 1000. Then one decimal, truncated, with the prefix: `k`/`M`/`G` (SI), `Ki`/`Mi`/`Gi` (binary), `kb`/`Mb`/`Gb` (bits). Under 1000 of the base unit it is the bare number in bytes, and `b` added in bits. Binary switches prefix at 1000, not 1024, so `1000 KiB/s` shows `0.9Mi`, the number never needs a fourth integer digit, and the cells stay 7. S7's SI form (`999.9M` in 6 cells) is unchanged.
  - Percent: unchanged.

### Bar budget (the layout is S7's, the widths B7's)

In Roboto Mono 24 a cell is 14 px, Montserrat's `°` 10 px and each arrow 21 px (F25). Temperatures on the wire are -40 to 150 °C, so 3 cells hold every °C and °F value (`-40`, `150`, `212`, `302`).

| Group | Worst string after B7 | Width | As S7 built it | Width |
|---|---|---|---|---|
| CPU | `CPU 100% 302°F` (13 cells, `°`) | 192 | `CPU 100% 100°` (12 cells, `°`) | 178 |
| RAM | `RAM 100%` (8 cells) | 112 | the same | 112 |
| GPU | `GPU 100% 302°F` | 192 | `GPU 100% 100°` | 178 |
| NET | `▼999.9Mi ▲999.9Mi` (15 cells, 2 arrows) | 252 | `▼999.9M ▲999.9M` (13 cells, 2 arrows) | 224 |
| Clock | `Wed 24 May 04:44` (16 cells) plus 4 px | 228, flush right at x 792 with a 4 px margin | the same | 228 |

- **The rest of the bar** is x 0 to 792. After B7 the four groups take 192 + 112 + 192 + 252 = 748 px, leaving 44. With equal gaps at the left edge, between the groups and before the clock (5 gaps), that is **8 px each**. As S7 built it the groups take 692 px, and the gaps are 20 px. The units cost 56 px: 14 for each `°C`/`°F` letter, and 14 for each rate's seventh cell.
- **8 px is under one cell**, so groups whose values fill every cell (`CPU 100% 302°F` next to `RAM 100%`) will read close together. Q1 asks whether to buy space back. The cheapest options are to drop the space between the two rates (the `▲` already separates them; gaps go to 11 px), and to drop the temperature letter (gaps go to 14 px, or 17 px with both).
- **Overflow**: S7's check (F26) measures the worst strings above, with color markup around every value and the recolor flag set, so markup is measured as LVGL draws it. A label never reaches its dots, since a cut inside `#rrggbb ...#` would show a stray `#`. If the check fires on the board, the cells are wrong, not the gaps, and B7 stops.
- **Pool**: B7 adds no widget. It turns on recolor for four labels.

### Color past a threshold

- **What "red" means on the board**: the value itself is drawn in the step's color, through LVGL's recolor markup in the group's one label (F16): `CPU #e53935 97%#  64°C`. The prefixes (`CPU`, `RAM`, the arrows) stay grey, so each of the two values in a group can have its own color. No extra widgets. The markup comes from `hoststat.c`, so host tests check colors as strings.
- **Who decides**: the daemon. It knows the thresholds, keeps the hysteresis state in one place, and sends each color with the value it belongs to. The board only draws.
- **Steps**: each metric has 0 to 3 steps `{ at, color }`, `at` rising. A value at or past a step's `at` takes that step's color. The highest step reached wins.
- **Thresholds are in the display units.** `at` for `cpu_temp` and `gpu_temp` is in °C or °F as `temp_unit` says, and `at` for `net_rx` and `net_tx` is in MB/s, MiB/s or Mb/s as `net_unit` says. The daemon judges the number the board shows:
  - Temperatures: it converts the whole °C to °F by the same rule as `hoststat.c`, then compares with `at`. "190 °F" turns red exactly when the bar shows 190°F or more.
  - Rates: bytes per second against `at` × 10^6 (SI), × 2^20 (binary) or × 125,000 (bits, 10^6 bits). The board truncates, so it shows `99.9Mi` for a value under 100 MiB/s and never shows a step the value hasn't reached.
  - Percent: as before, with RAM rounded down as the board shows it.
- **Hysteresis**: a step reached stays until the value falls below `at - h`. `h` is 2 for percent and °C, 4 for °F (about 2 °C, since °F moves in steps of about 1.8), and `at / 10` for rates. Each sample, the new step is whichever is higher: the highest step the value reaches, or the highest step at or below the current one whose `at - h` the value still reaches. A missing value clears its step. A config change, the units included, starts every metric afresh.
- **Switching units in the form converts the steps** in the draft, through a Rust command so the arithmetic lives once (Config UI). °C to °F is `round(at × 9/5 + 32)` and back `round((at - 32) × 5/9)`. Rates go by the ratio of the factors, rounded, at least 1. If two steps land on the same value, the higher one moves up by 1. The form says the steps were converted.
- **Hand edits**: someone who changes `temp_unit` in the file without changing the steps changes what they mean. The rules catch some of it, since the ranges differ per unit (a 190 left behind after a switch to `c` is refused, outside 1 to 150), but not all (85 is valid in both). `config.md` says so next to the key.

### Config

In `config.toml`, so it gets the existing validation, last-good config, the save flow with its revision check and backups, and the editor's error mapping (F6), and stays hand-editable:

```toml
[status_bar]
gpu = "auto"        # "auto", "off", or a PCI address such as "0000:01:00.0"
temp_unit = "c"     # "c" or "f". The temperature steps below are in this unit
net_unit = "si"     # "si" (1.2M), "binary" (1.1Mi) or "bits" (9.6Mb). The rate steps are in MB/s, MiB/s or Mb/s

[status_bar.thresholds]
mem = [{ at = 80, color = "#ffb300" }, { at = 90, color = "#e53935" }]
cpu_temp = [{ at = 85, color = "#ffb300" }, { at = 95, color = "#e53935" }]
net_rx = []         # never colored
```

| Key | Unit | `at` range | Default steps |
|---|---|---|---|
| `cpu` | % busy | 1 to 100 | 90 amber |
| `mem` (RAM on the bar and in the form) | % used | 1 to 100 | 80 amber, 90 red |
| `gpu` | % busy | 1 to 100 | none (a busy GPU in a game isn't a problem) |
| `cpu_temp` | °C / °F | 1 to 150 / 34 to 302 | 85 amber, 95 red / 185, 203 (Zen 4 and 5 run to 95 °C by design) |
| `gpu_temp` | °C / °F | 1 to 150 / 34 to 302 | 84 amber, 90 red / 183, 194 (this card's target is 84 °C, F18) |
| `net_rx`, `net_tx` | MB/s, MiB/s / Mb/s | 1 to 100000 / 1 to 800000 | none |

- The metric keys stay the wire's names (`mem`, as in `mem_used`). The form and the bar say RAM.
- Amber is `#ffb300` and red `#e53935`.
- With no `[status_bar]` table, or no key for a metric, the defaults for the configured unit apply. `[]` means never colored. The defaults live in Rust and reach the editor through `get_config` (`ConfigView.status_bar_defaults`, given for both temperature units), so TypeScript holds no copy.
- `gpu`, `temp_unit` and `net_unit` are plain strings, as `bg`, `font` and `url` are (F6). A bad value then gets the rule's message from `check()`, not a TOML decode error.
- **Rules**: a config that breaks one is refused and the last good config stays live. The editor maps each message to its field:
  - `status_bar.gpu "x" is not auto, off or a PCI address such as 0000:01:00.0` (the address as `dddd:bb:dd.f`, lowercase hex).
  - `status_bar.temp_unit "k" is not c or f`.
  - `status_bar.net_unit "x" is not si, binary or bits`.
  - `status_bar.thresholds.mem: 4 steps, at most 3`.
  - `status_bar.thresholds.cpu_temp: at 400 is outside 34 to 302 °F` (the range of the configured unit).
  - `status_bar.thresholds.mem: steps must rise, 90 then 80`.
  - `status_bar.thresholds.mem: color "red" is not #rrggbb`.
  - Unknown keys are refused, as everywhere in the file.
- A PCI address that names no GPU, or one that can't be read, isn't a config error, since an eGPU can be unplugged. The bar shows `GPU --`, the daemon logs it once per selection, and the editor marks it "(not found)".

### Wire

`stats` gains optional fields under the same `status_bar` capability:

```json
{"t":"stats","time":1792692433,"utc_offset":-14400,"cpu":97,"cpu_temp":71,"gpu":23,"gpu_temp":41,"mem_used":43218907136,"mem_total":98495209472,"net_rx":1250000,"net_tx":340000,"temp_unit":"c","net_unit":"si","colors":{"cpu":16757504}}
```

| Field | Type | Meaning |
|---|---|---|
| `gpu` | int | Optional. Busy share of the selected GPU, 0 to 100 |
| `cpu_temp` | int | Optional. CPU temperature in whole °C, -40 to 150 |
| `gpu_temp` | int | Optional. The selected GPU's temperature in whole °C, -40 to 150 |
| `temp_unit` | string | Optional. `"c"` or `"f"`: how the device shows temperatures. Absent: `c`. The values stay °C |
| `net_unit` | string | Optional. `"si"`, `"binary"` or `"bits"`: how the device shows rates. Absent: `si`. The values stay bytes per second |
| `colors` | object | Optional. One entry for each value past a threshold: its field name (`cpu`, `mem`, `gpu`, `cpu_temp`, `gpu_temp`, `net_rx`, `net_tx`), and the color to draw it in as `0xRRGGBB`, 0 to 16777215. Absent: every value is drawn in the bar's normal color |

- `gpu`, `cpu_temp` and `gpu_temp` are independent, not a pair (F5).
- The daemon sends both units in every frame.
- **Refused** (the frame is dropped and the link stays up, as for every `stats`): a new field of the wrong type or out of range; a unit string not in its list; `colors` that isn't an object; a known key in `colors` whose value isn't a whole number in range.
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
- **Readable**: `nvidia`, and `amdgpu` when the device has `gpu_busy_percent`. Whether NVML opens is only known in the sampler, which shows `--` and logs why if it doesn't. Others are listed but can't be picked.
- **`auto`**: the readable GPU with `boot_vga` 1, else the readable GPU with the lowest PCI address. Here that is the RTX 4070. On a hybrid laptop it is usually the iGPU.
- **`off`**: no GPU values. The group shows `GPU --`.
- **The selection is resolved** at the start of each sampling period, and again when `status_bar.gpu` changes.
- **Names for the editor**: an NVIDIA's from `/proc/driver/nvidia/gpus/<pci>/information` (`Model:`), so the editor's list never starts NVML. An AMD's from `device/product_name` when present, else `AMD GPU`. Names are for the editor only.
- One GPU in the bar. There is no room for a second.

### Temperatures

- **hwmon by name**: each sampling period looks up hwmon directories by their `name` file, never by number (F14), and looks again after a read fails.
- **CPU**: `k10temp` or `zenpower`: `Tdie` if present (older Zen parts add an offset to `Tctl`), else `Tctl`. `coretemp`: the highest `Package id N`. Here that gives `Tctl`, 64 °C, while the two CCDs read 51 and 38. `Tctl` is what AMD's tools and `btop` show. Without any of these sensors there is no `cpu_temp`.
- **AMD GPU**: the selected card's own `device/hwmon/*`, `junction` if present, else `edge`, else `temp1`. It doesn't scan all hwmon, so an iGPU and a dGPU never mix.
- **NVIDIA GPU**: NVML's GPU temperature sensor.
- Millidegrees are rounded to the nearest whole °C. °F happens only on the board, and in the daemon's threshold check.

### Daemon

- **`hwmon.rs`** (new): pure parsers and the lookup above, under a sysfs root.
- **`gpu.rs`** (new): enumeration, readability, `auto`, the AMD reader.
- **`gpu/nvidia.rs`** (new): the two traits, the `nvml-wrapper` implementation, the fake for tests, and the `nvml` thread.
- **`units.rs`** (new): `TempUnit` and `NetUnit` with `parse`, °C to °F, rate factors, the per-unit ranges, and the step conversion for the form. Pure.
- **`hoststats.rs`**:
  - `Sampler::new(proc_root, sys_root)`.
  - `HostStats` gains `cpu_temp`, `gpu: Option<Gpu { busy, temp }>`, `colors` and the two units, and stays `Copy`.
  - `spawn(polling, config, sources)` takes the config receiver as `tiles::spawn` does (F2). At each tick it reads the GPU selection, the units and the steps from the live config.
- **`alerts.rs`** (new): steps, hysteresis and colors in the display units, pure.
- **`server.rs`**:
  - `stats_frame` adds the new fields and their guards.
  - `Server::bind` takes a `HostSources { proc_root, sys_root, nvidia: Arc<dyn NvidiaOpener> }`. `lib.rs` sets it to `/proc`, `/sys` and the real opener, so `device_sim` can pass fixture trees and a fake opener. Without that, `device_sim` would open the real NVML on this desktop through `auto`, and in CI would read whatever the container's `/sys` holds.
  - The session code doesn't change otherwise.
- **`commands.rs`**:
  - `async fn list_gpus() -> Vec<GpuInfo { id, name, driver, readable, boot_vga }>`, plus which one `auto` picks now. It is async, like `list_icons`, so the sysfs walk never runs on the main thread.
  - `convert_status_bar(bar, temp_unit, net_unit) -> StatusBarConfig`, which converts the steps for the form.

### Config UI

- **`StatusBarForm.tsx`** (new), in the Layout tab, opened from a "Status bar" button in the grid panel's head (`GridEditor.tsx:204`). While it shows, no button is selected. It is part of the same draft and the same Save.
  - **Units**, at the top: Temperature (°C, °F) and Network rates (SI, `1.2M`; Binary, `1.1Mi`; Bits, `9.6Mb` if Q2 includes it), each with a short example of what the bar will show. Changing either calls `convert_status_bar` on the draft's steps and says "Thresholds converted to °F" under it.
  - **GPU**: a select of "Auto (currently: NVIDIA GeForce RTX 4070)", "Off", and each GPU from `list_gpus` with its address. Unreadable ones are disabled with the reason. A configured address that isn't present shows as "0000:xx:xx.x (not found)".
  - **Metrics**: one row each for CPU, RAM, GPU, CPU temperature, GPU temperature, Download and Upload, with the unit the steps are in (°F, MiB/s). Each row has up to 3 steps (a number, and the color field from `ButtonForm.tsx:37`, moved to `fields.tsx`), Add step, Remove, and "Use defaults". "Use defaults" removes the key and shows the default steps for the current unit read-only.
- **`errors.ts`**: a `statusBar` bucket keyed by metric (and `gpu`, `temp_unit`, `net_unit`), from `^status_bar\.`, shown next to its row (F9). `numberErrors` also covers each step's `at`.
- **`edit.ts`**: `barChanged(before, after)`. After a save that changed `[status_bar]`, the save bar says "status bar settings apply within 2 s", beside any layout lines (F8). The 2 s are the 500 ms config poll plus the 1 s tick.
- **`config.ts`**: the `status_bar` types.

## Rejected alternatives

| Option | Why not |
|---|---|
| The daemon converts the values and sends a unit flag | The board still needs the flag to pick the suffix and keep the number in its cells, so the units would live in two places. The wire's numbers would mean different things per session, and every range guard would need a range per unit (Units) |
| The board gets the units once, in `welcome` or its own message, and keeps them | More protocol for about 35 bytes a frame, and state to keep across reconnects. In every frame, a change applies at the next one |
| Thresholds stored in °C and bytes, converted only for the form | A step entered as 191 °F is stored as 88 °C and shows as 190 °F afterwards, and colors at a number the user didn't type |
| Binary prefixes switching at 1024 | `1023.9Ki` needs an eighth cell in each rate, 28 px the bar doesn't have |
| A `B` on bytes (`1.2MB`, `1.1MiB`) | One more cell per rate. Bits carry their `b`, so bytes don't need one |
| A bare `°` without the letter | With a unit choice, `100°` no longer says which unit. Q1 keeps it as a way to buy back 28 px |
| `nvidia-smi --query-gpu=... -lms 1000 -i <pci>` as one long-running child | User chose NVML. It had no new crate, no closed library in the app, and cost 15 ms CPU per 10 s (F17) |
| `nvidia-smi` started once a second | About 8 ms CPU and a process start every second (F17) |
| Our own NVML `dlopen` | The daemon's first `unsafe`, with ABI structs written by hand. `nvml-wrapper` holds that code, and V0 vets it |
| Linking `libnvidia-ml` at build time | The app wouldn't start on a host without the driver, and CI couldn't build it |
| sysfs for NVIDIA | Not there (F12) |
| `sysinfo` or libsensors for temperatures | A crate or a C library for a few hwmon files, as device-ui decided for `/proc`. Neither reads GPU busy |
| Thresholds or units in app settings (a store plugin) | A new plugin and a second place for settings, with no validation, last-good config or hand editing (F10) |
| The board gets the thresholds and decides | Hysteresis and units in both C and Rust, a new message, and thresholds kept on the board across reconnects. With colors in `stats`, a color always comes with the value it describes |
| Levels (`warn`, `crit`) on the wire with a palette sent separately | The user sets colors. Sending the color is one mapping fewer, like `bg` and `fg` in `layout` (F6) |
| Fixed warn and crit per metric | Steps of `{ at, color }` say "RAM at 90% red" directly, and allow one, two or three |
| A new capability | The fields are optional and an older board ignores them (F4) |
| An icon font for CPU, GPU, RAM and temperature | A new generated font with its own license entry, while the text prefixes fit. A follow-up if wanted |
| A colored background per group | One group holds two values, which can't have two backgrounds |
| A label per value instead of recolor | About twice the labels, in a pool with about 10 KB free (F22) |
| A hold time instead of hysteresis | It delays a real spike's color. Hysteresis shows the color at once and only delays clearing it |
| Every GPU in the bar, or GPU memory | No room |

## Slices

| Slice | Kind | Depends on | Board check |
|---|---|---|---|
| B1 Wire contract | mechanical, both parsers | device-ui done | no |
| B2 Config schema and units | mechanical, schema | device-ui done | no |
| B3 sysfs sensors | design judgment, Rust | B2 (`GpuChoice::parse`) | no |
| V0 Vet the NVML crates | reviewer, supply chain | — | no |
| B4 NVML reader | design judgment, Rust, threads | V0 passed | no |
| B5 Sampler, colors and sessions | design judgment, daemon | B1, B2, B3, B4 | via B7 |
| B6 Status bar form | design judgment, app | B2, B3 | no, window check |
| B7 Values, units and colors on the board | design judgment, firmware | B1, device-ui S7. Its board check needs B5 merged | yes |
| B8 Hardware run and docs | operator plus scribe | B5, B6, B7 | yes |

**Waves**, each slice in its own worktree:

| Wave | Slices | Shared files |
|---|---|---|
| 1 | B1, B2, B3, V0, then B4 once V0 passes | B2 defines `GpuChoice::parse` in `config.rs` and the `units.rs` module; B3 needs only the first. Merge B2 first and rebase B3. B2 and B3 both touch `commands.rs` (`status_bar_defaults`, `convert_status_bar`, `list_gpus`). B3 and B4 both add a module (one line each). B3 and B4 agree on the PCI address form (`dddd:bb:dd.f`, lowercase) in this plan |
| 2 | B5, B6, B7 | B5 and B7 hold the same conversion vectors (Units); B7 copies them from B2's `units.rs` tests |
| 3 | B8 | — |

Every flash needs the user's yes, and nobody runs `erase_flash`. The pairing survives, since NVS doesn't change.

### B1 Wire contract

- **Files**: `protocol.rs`, `tests/fixtures/stats.json` (every field) and a new `stats_colors.json`; `proto.h`/`proto.c`; `test_proto.c`; `site/src/protocol.md` (the fields, refusals and the example).
  - `protocol.rs`: `Message::Stats` gains `gpu`, `cpu_temp`, `gpu_temp`, `temp_unit` and `net_unit` (enums serialized as their strings), and `colors: Option<Colors>`, a struct of seven optional `u32`, each skipped when `None`.
  - `proto.h`/`proto.c`: `proto_stats_t` gains `has_gpu`, `gpu`, `has_cpu_temp`, `cpu_temp` as `int16_t`, the GPU temperature likewise, `temp_unit` and `net_unit` as enums (defaults when absent), and `colors[7]` with a `colors_mask`. `parse_stats` reads them.
- **Host cases**: the full fixture; `gpu` 101, -1 and 23.5; `cpu_temp` -41, 151 and -40; `gpu_temp` without `gpu` and the reverse (both accepted); `temp_unit` `"f"`, `"F"`, `"k"` and `1` (only the first accepted); `net_unit` each of the three, `"mib"` refused; absent units give `c` and `si`; `colors` as an array, as a string, empty, with an unknown key (ignored), with `cpu` 16777216, -1 or a string (refused); a `colors.gpu` with no `gpu` field (accepted, ignored).
- **Acceptance**: both fixtures round-trip on the daemon. The board parses both and refuses each malformed case. The existing `stats` and `stats_min` cases pass unchanged.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml protocol` and `mise run fw:test`.

### B2 Config schema and units

- **Files**: `config.rs`, `daemon/units.rs` (new), `commands.rs` (`ConfigView.status_bar_defaults` for both temperature units, `convert_status_bar`), `app/src/config.ts` (the types), `site/src/config.md` (a Status bar section: the keys, the units and what each shows, the per-unit ranges, the warning about hand-editing a unit, the defaults, hysteresis, what `auto` picks, and the laptop note).
  - `config.rs`: `StatusBarConfig`, `Thresholds` and `Step`, with `gpu`, `temp_unit` and `net_unit` as `Option<String>`; `GpuChoice::parse(&str) -> Option<GpuChoice>` (`Auto`, `Off`, `Pci(String)`), which `check()` and the sampler both call; the rules in `check()`, with ranges from `units.rs`.
  - `units.rs`: the conversion vectors of Units as a test table.
- **Tests**:
  - The example and the default file still pass; each rule's message above, word for word; `[]` and an absent key differ; 3 steps pass and 4 don't; an unknown key under `[status_bar]` or in a step is refused; a `[status_bar]`-only change leaves `Live::layout()` equal (F6).
  - `cpu_temp` at 190 passes with `f` and is refused with `c`; `net_rx` at 500000 passes with `bits` and is refused with `si`.
  - The defaults for `f` are 185, 203, 183 and 194.
  - `convert_status_bar`: 85 °C → 185 °F → 85 °C; 100 MB/s → 95 MiB/s → 100 MB/s; 100 MB/s → 800 Mb/s; 84 and 85 °F → 29 and 30 °C (the higher moved up); a converted step outside the new range is clamped to it.
- **Round trip**: the Layout tab's save rewrites the whole file from the draft (`config.md:71`), so a `[status_bar]` the form doesn't know about must survive it.
  - A Rust test saves a config with every `status_bar` key through `Configs::save` and reads back an equal config.
  - An `edit.test.ts` case checks that a button edit keeps `status_bar` in the draft.
  - The `toml` serializer may write the steps as `[[status_bar.thresholds.mem]]` tables rather than the inline form above. Both parse, and `config.md` shows both.
- **Acceptance**: `cargo test` and clippy clean. A `config.toml` without `[status_bar]` loads as before.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml config:: units`.

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
    - each default, in both temperature units;
    - a value at `at` colors, and one below doesn't;
    - 91, 89, 88, 87 with `at` 90 stays colored until 87 (`h` 2);
    - with `f` and `at` 190: 88 °C (190 °F) colors and 87 °C (189 °F) doesn't; once colored it clears below 186 °F (`h` 4);
    - a rate at `at × 10^6` colors in `si`, at `at × 2^20` in `binary`, at `at × 125000` in `bits`, and each clears 10% below;
    - a jump from none to the third step; a fall across two steps at once;
    - a missing value clears its step; a config or unit change starts afresh; `[]` never colors.
  - `stats_frame`: `cpu_temp` 151 and -41 are left out with their colors; `gpu` 101 likewise; an empty `colors` is left out; both units always present.
  - Sampler, with the B3 fixtures, the fake opener and a paused clock: the GPU resolves at the start of a period; `gpu = "off"` opens nothing; switching from the NVIDIA to the AMD shuts the fake down; a period without sessions opens nothing.
  - `device_sim`:
    - A `status_bar` session gets a `stats` frame with `cpu_temp`, `gpu` and the units from the fixtures and config.
    - With a thresholds config whose `mem` step is at 1, the frame's `colors.mem` is that step's color.
    - A `[status_bar]`-only edit, steps or units, then sends no `layout` within 2 s, and the next frames carry the change.
- **Acceptance**: every existing test passes unchanged. On this desktop, with the app running and a device connected, `ls -l /proc/<app pid>/fd` shows `/dev/nvidia*` files, and none 2 s after the device disconnects.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml alerts hoststats` and `--test device_sim`.

### B6 Status bar form

- **Files**: `StatusBarForm.tsx` (new), `ConfigPage.tsx`, `GridEditor.tsx` (the head button), `fields.tsx` (`ColorField` moved from `ButtonForm.tsx`), `errors.ts`, `edit.ts`, `config.ts`, `api.ts` (`list_gpus`, `convert_status_bar`), `app/tests/edit.test.ts`, and new `errors.ts` cases.
- **Tests**:
  - every rule message from B2 lands in its bucket, the unit ones included;
  - `numberErrors` (`errors.ts:222-246`) catches a step's `at` that isn't a whole number before the call;
  - `barChanged` is true for a `[status_bar]`-only edit and false for a button edit;
  - `layoutTargets` is empty for a `[status_bar]`-only edit.
- **Acceptance**: in the window:
  - Switching Temperature to °F turns the CPU temperature steps 85 and 95 into 185 and 203, labels them °F, and says they were converted. Switching back gives 85 and 95.
  - Switching rates to Binary relabels the rate steps MiB/s and converts any.
  - The GPU select lists both GPUs here, with "Auto (currently: NVIDIA GeForce RTX 4070)".
  - Setting RAM's steps and saving writes a `config.toml` that parses back to the same steps and units.
  - "Use defaults" removes the key and shows the defaults in the current unit.
  - A refused value shows next to its row.
  - After a `[status_bar]`-only save, the save bar says the status bar applies within 2 s and names no layout push.
- **Cheapest check**: `pnpm test`, `pnpm build`, Biome from `app/`. The window check needs the user.

### B7 Values, units and colors on the board

S7 built the groups, their layout, the measurement of worst strings, and the formats for what it shows. This slice fills the groups in and widens them.

- **Files**:
  - `ui.c`: the worst strings in `bar_create` become those in the Bar budget, with markup; recolor on the CPU, RAM, GPU and NET labels; the new fields and units in the stored frame.
  - `hoststat.c/.h`: temperatures with `°C`/`°F` in 3 cells, GPU busy, binary and bits rates in 7 cells, color markup. The SI rate and the percent formats stay S7's.
  - `test_hoststat.c`.
- **Host cases**:
  - the conversion vectors of Units, the same table as B2's: °F for -40, 0, 37, 64, 100 and 150 °C; rates in all three units at 999, 1000, 1023, 1024, 999,999, 1,000,000, 1,048,575, 1,048,576, 999,999,999 and 2.5 × 10^9 bytes per second, among them `1000 KiB/s` → `0.9Mi`, and nothing ever wider than its cells;
  - every CPU and GPU row of the format rules (`CPU --`, busy only, temperature only, both), right-aligned in their cells;
  - color markup around exactly the colored value (`CPU #e53935 97%#  64°C`), and around each of the two rates separately;
  - a `colors` entry for an absent value adds nothing;
  - RAM at 89.99% shows `89%`;
  - stale (over 5 s) gives `--` with no markup.
- **Acceptance (board, against the B5 daemon)**:
  - `bar_create` logs the new group widths (192, 112, 192, 252, clock 228) and 8 px gaps, or the gaps Q1 buys back, and no `bar slot overflow`. `°` draws as a glyph, not a placeholder box. No `#` is ever visible.
  - Switching to °F and Binary in the form: within 2 s the bar shows `147°F`-style temperatures and `Mi` rates, with no group moving and no `pushing layout rev` in the daemon log.
  - With `mem = [{ at = 1, color = "#e53935" }]` saved, the RAM value turns red within 2 s. With the key removed it is grey within 2 s.
  - The `LVGL heap` line at "page built" is within 300 bytes of S7's number.
- **Cheapest check**: `mise run fw:test`, `mise run fw:build`, then the board. Needs the user for every flash.

### B8 Hardware run and docs

Run on the board and this desktop, the daemon and firmware from the same commit.

1. **NVIDIA**: put a load on the RTX 4070 (`vkcube` or a game). For 30 s, compare the bar's GPU busy and temperature with `nvidia-smi --query-gpu=utilization.gpu,temperature.gpu --format=csv -lms 1000` in a terminal: the same trend, and each temperature within 2 °C.
2. **AMD iGPU**: pick `0000:75:00.0` in the form. Within 2 s the app holds no `/dev/nvidia*` file. Load it with `DRI_PRIME=pci-0000_75_00_0 vkcube`: busy rises as `gpu_busy_percent` does, and the temperature matches `sensors amdgpu-pci-7500` within 2 °C.
3. **CPU temperature**: under `cargo build` load, compare with `sensors k10temp-pci-00c3` `Tctl`: within 2 °C, and within 4 °F after switching to °F.
4. **Rates in each unit**: copy a large file to another machine. Compare the bar with `ip -s link` deltas over 10 s in SI, Binary and (if Q2 includes it) Bits.
5. **Lifecycle**: with the daemon running and no device connected, the app holds no `/dev/nvidia*` file. A device connects: it does. `gpu = "off"`: the group reads `GPU --` and no NVIDIA file is held.
6. **No driver**: B4's CI test (the real opener with a missing library) is the evidence. If a host or VM without the NVIDIA driver is at hand, also start the app there: it starts and logs "no NVIDIA driver" once. Record which evidence was used.
7. **Colors and units**: `cpu = [{ at = 30, color = "#e53935" }]` during a build: the CPU value turns red, and goes back to grey only after the load drops more than 2 points below 30. Record whether it flickers. Then set `temp_unit = "f"` in the form with a `cpu_temp` step at the current reading in °F: the temperature turns red exactly when the bar shows that number.
8. **Look**: with every value at its widest (a full CPU and GPU, °F, Binary), photograph the bar and record whether the 8 px gaps read as separate groups (Q1).
9. **Memory**: the pages fixtures as in device-ui S8. Settled pool, largest free block and minimum internal heap within 300 bytes of device-ui S8's results.

- **Docs**:
  - `site/src/protocol.md` final.
  - `site/src/config.md`: the Status bar section, the units, and the bar's formats.
  - `CLAUDE.md` Daemon facts: GPU sources, NVML opened at runtime from `libnvidia-ml.so.1` only while a device is connected, its own thread, hwmon found by name, `[status_bar]`, colors decided by the daemon in display units, units converted on the board.
  - `CLAUDE.md` Hardware facts: the recolor markup, the bar's cells.
  - A results section in this plan, with V0's vetting record.
- **Acceptance**: every step passes or its failure is recorded with a decision.

## Risks

| Risk | Mitigation |
|---|---|
| The board and the daemon convert units differently, so a color comes at a number the bar doesn't show | One rule, simple integer arithmetic, and one table of vectors in both test suites (B2, B5, B7). B8 step 7 checks it on the board |
| Someone changes `temp_unit` or `net_unit` by hand and leaves the steps in the old unit | Per-unit ranges catch some of it. The form converts. `config.md` warns next to the keys |
| The 8 px gaps make the bar hard to read | Q1, and B8 step 8 photographs the worst case. Dropping the space between the rates or the temperature letter buys back 3 to 9 px a gap |
| A crate in the NVML tree is malicious, abandoned or vulnerable | V0 before B4, with stop conditions and the `nvidia-smi` loop as the fallback. `=0.13.0` is pinned |
| A crash inside `libnvidia-ml` takes down the app, with the daemon in it | Accepted by the user's decision. NVML runs only while a device is connected, on its own thread. Nothing persistent is at risk, and the app starts again |
| No NVIDIA driver, or a library and driver of different versions after an update | `init` fails cleanly (B4 CI test). The group shows `GPU --`, there is one log line, and `init` is retried every 60 s for a mismatch |
| An NVML call blocks for seconds on a wedged GPU | The `nvml` thread. CPU, RAM and net keep coming, GPU goes `--` after 3 s, and no second thread starts (B4 test) |
| NVML keeps a laptop's dGPU awake | `auto` prefers `boot_vga`, usually the iGPU there. NVML is closed outside periods and when another GPU is picked. `off` exists. Documented |
| A sensor reports nonsense (511 °C, -273 °C) and the board drops every frame (F4) | `stats_frame` leaves out the value and its color (B5 test). The rest of the bar keeps working |
| hwmon numbers change between boots | Lookup by name each period and after a failed read (B3 test) |
| `Tctl` carries an offset on older Ryzen | `Tdie` wins when present (B3 test) |
| Colors flicker near a threshold | Hysteresis (B5 tests), B8 step 7 |
| A group overflows its cells | Every unit form is in the worst strings S7's check measures, with markup. B7 checks the log, and stops if it fires |
| S7 lands with a different gap rule | The group widths still hold. B7 records the gaps it logs |
| PCI address of a GPU changes (a card moved to another slot) | The editor shows "(not found)"; pick it again |
| A save from the Layout tab drops a hand-written `[status_bar]` | B2's round-trip tests on both sides |
| `device_sim` behaves differently on this desktop (real NVML) and in CI (the container's `/sys`) | `HostSources` injected, with fixture trees and the fake opener in every `device_sim` test |
| A runtime-suspended AMD dGPU, picked by hand, is woken by the reads or refuses them | `auto` doesn't pick it on a hybrid laptop. A refused read shows `--`. Documented |
| A `[status_bar]`-only edit takes longer than 2 s, because the reload also checks icons | B7 times it on the board. If it is over 2 s, the save bar's wording changes, not the design |

**Rollback.**

- Each slice is its own commit, so `git revert` undoes one.
- The firmware rolls back by flashing the build before B7, with the daemon from the same commit. NVS doesn't change, so the pairing survives.
- The riskiest slice is B4 (a closed library in the app process). Reverting B4 and B5 removes the crate tree and leaves the bar without GPU values. Everything else keeps working, and the S7 group keeps reading `GPU --`.
- A daemon from before B2 refuses `[status_bar]` in a user's `config.toml`. Remove the table before going back.

## Open questions

**Settled 2026-10-22, when the plan started.** Q2: yes, bits are the third rate unit (the recommendation). Q1 is moot. S7 moved the bar to Roboto Mono Regular 20 (12 px cells, with `°` in the font, 18 px arrows), 12 px margins and a 192 px clock. B7's worst groups become CPU and GPU 168 (`CPU 100% 302°F`, 14 cells), RAM 96, and NET 216 (`▼999.9Mi ▲999.9Mi`). That is 648 px of the 808 left of the clock, so the 4 gaps are 40 px each and the space between the rates stays. B7 takes these numbers over the Bar budget table above, which assumed the 24 px font.

- **Q1** With every unit form sized in, the gaps between groups shrink from S7's 20 px to 8 px, under one cell. Accept that, or buy space back? **Recommended: drop the space between the two rates** (`▼999.9Mi▲999.9Mi`; the `▲` separates them), which gives 11 px gaps. Keep the `°C`/`°F` letter, since a bare `°` no longer says which unit. Revisit after B8 step 8's photo.
- **Q2** Offer bits per second (`9.6Mb`, 1000-based) as a third rate unit? **Recommended: yes.** It costs no width, since `999.9Mb` fits the same 7 cells as binary's `999.9Mi`, and link speeds are quoted in bits. The cost is one more case in the formatter, the threshold factor (125,000 bytes per Mb) and the form. Binary bits (`Mib`) aren't offered. If no, `net_unit` is `si` or `binary`, and nothing else changes.

## Critique log

No outside review. The consult tool refused both reviewers, as it did for device-ui: the codex app server isn't locked down (`web_search` unset, `features.plugins` missing), and the agy reviewer's hook blocked its own `read_file`. So this is a self-review that argued against each slice and the approach, with each point checked against the code or this desktop. Points R5, R6 and R13 were about the `nvidia-smi` loop, and the NVML decision superseded them.

| # | Point | Outcome |
|---|---|---|
| R1 | `device_sim` builds the real server, so `auto` would use the real NVIDIA on this desktop and read the container's `/sys` in CI | adopted: `HostSources` in `Server::bind`, fixture trees and the fake opener in `device_sim` (Daemon, B5) |
| R2 | The Layout tab's save rewrites the whole file (`config.md:71`), so a `[status_bar]` the draft lost would be erased | adopted: round-trip tests on both sides in B2 |
| R3 | The `toml` serializer may write steps as `[[status_bar.thresholds.mem]]`, so "the file reads as the example" would fail | adopted: B6 checks the parsed steps, and `config.md` shows both forms |
| R4 | Recolor markup cut by the label's dots would show a stray `#` | adopted: cells sized so a label is never cut, and the overflow check measures with markup and the recolor flag (Bar budget) |
| R5 | `kill_on_drop` doesn't run if the app is killed with SIGKILL | superseded: no child process with NVML |
| R6 | A killed child is reaped asynchronously, so a "pid gone" check can race | superseded: no child process with NVML |
| R7 | B7's board check reads GPU and colors, which only the B5 daemon sends | adopted: the slice table says the board check waits for B5 |
| R8 | `list_gpus` as a sync command would walk sysfs on the main thread | adopted: async, like `list_icons`. It also never starts NVML (names from `/proc/driver/nvidia`) |
| R9 | A `[status_bar]`-only edit reloads the whole config, icons included, so "within 2 s" may not hold with many icons | adopted as a B7 measurement. Only the save bar's wording depends on it |
| R10 | A runtime-suspended AMD dGPU may wake on the reads or refuse them | adopted as a risk: `auto` doesn't pick it on a hybrid laptop, and a refused read shows `--` |
| R11 | A background thread or task on Tauri's runtime might stall the sampler | adopted in a new form: NVML gets its own `std::thread`, so no blocking call runs on a tokio worker |
| R12 | A step at `at` 1 with `h` 2 never clears | rejected: a 1% step means "always", which is what it says |
| R13 | `nvidia-smi`'s CSV could change between drivers | superseded: NVML has a typed API |
| R14 | Colors from the daemon don't work offline | rejected: offline the values show `--`, so there is nothing to color |
| R15 | Hysteresis state is shared by all sessions | rejected as a defect: one host and one set of units, so every device shows the same color for the same value |
| R16 | The first frame of a period has no CPU share, so no CPU color for a second | accepted as it is, as with device-ui's first frame |
| R17 | Without the name, a user with two desktops can't tell from the bar which one a board talks to | settled by the user: the name is in settings |
| R18 | A string enum as a serde type would fail in the TOML decoder, so the user would never see the rule's message | adopted: `gpu`, `temp_unit` and `net_unit` stay strings judged in `check()` (B2) |
| R19 | A fractional `at` from the form would come back as a decode error, not a rule | adopted: `numberErrors` covers `at` (B6) |
| R20 | Real-time tests at production timings would take tens of seconds in every CI run | adopted: timings are constructor arguments, and tests use milliseconds (B4) |
| R21 | `Nvml::init()` loads `libnvidia-ml.so`, which many distributions ship only in a development package (F23, F24) | adopted: `lib_path("libnvidia-ml.so.1")` |
| R22 | NVML calls can block for seconds, and starting NVML takes milliseconds, on the sampler's task | adopted: one `nvml` thread per period, at most one alive, publishing readings with their time |
| R23 | `Device<'nvml>` borrows the `Nvml`, which fights a handle kept across ticks | adopted: the trait hides it, and the implementation looks the device up by address on each read |
| R24 | A test that needs a real NVIDIA can't run in CI | adopted: the fake opener for logic, a CI test of the real opener with a missing library, and an `#[ignore]` test for this desktop |
| R25 | The first budget assumed Montserrat's fractional widths | superseded: S7 uses Roboto Mono, whose glyphs LVGL rounds to 14 px cells (F25), and the budget is in cells |
| R26 | Converting on the board and judging thresholds in the daemon puts the °F rule in two languages | adopted as a known cost: the rule is one line of integer arithmetic, with the same vector table in both test suites. The alternative (the daemon converts) puts the units in two places anyway (Rejected) |
| R27 | °F from a whole °C moves in steps of about 1.8, so a °F threshold can be skipped over | accepted: the step is judged on the shown number, so it colors at the first shown value at or above it. `h` is 4 for °F so it doesn't clear on one step |
| R28 | Binary prefixes switching at 1024 need an eighth cell | adopted: switch at 1000, so `1000 KiB/s` shows `0.9Mi` (Units) |
| R29 | Thresholds stored in canonical units would show back a number the user didn't type | adopted: thresholds in display units, converted by the form on a switch (Rejected) |
