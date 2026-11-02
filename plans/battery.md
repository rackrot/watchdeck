# Battery level: the bar, Settings > Status, and the app

Status: built 2026-11-01 (slices B0 to B8) on `main`; planned against `a1cd882`. C1, C5 and C6 passed on the board; C4 passed; C2 was skipped by choice; C3 is running and C4b was not run (see [Results](#results)). One second-opinion pass is folded in (see the [Critique log](#critique-log)).

The user put a LiPo in the CrowPanel. The ESP32-P4 has no battery line. An STC8H1K08 helper MCU reads VBAT and the TP4059 charger's CHRG and STDBY pins, and answers as an I2C slave at 0x2F on the touch bus. `site/src/device-spec.md` "Battery monitoring" has the probe results. After this plan:

1. The bar shows a battery glyph, the percent and a charging mark.
2. Settings > Status has a row with the voltage, the percent and the charge state.
3. The board sends the reading to the daemon under a new `battery` capability, and the app's Devices tab shows it.

## Goal and done

- **With the STC8.** A board whose STC8 answers shows `<charge mark><battery glyph> 53%` between NET and the clock, offline too. The glyph steps with the percent, and the charge mark shows only while charging. A reading older than 15 s shows `--`.
- **Without the STC8.** A board whose STC8 doesn't answer (an older revision, or `touch_init` failed) has today's bar: four groups, 40 px gaps, and no battery slot. Its Status row says `Battery --`, and it sends no `battery` frames. A NACK at boot logs one info line and nothing in the Errors tab.
- **Status row.** Settings > Status has `Battery 3.87 V, 53%, discharging` (or `charging`, `full`, `no battery`, `--`), refreshed every second from the cached reading.
- **Timing.** Unplugging or plugging USB changes the bar's charge mark and the Status row within about 6 s: one 5 s poll plus one 1 s bar tick. This doesn't hold while svc runs a long job such as Format card, when the bar shows `--` after 15 s.
- **The wire.**
  - The hello names `battery`.
  - The board sends a `battery` frame after each welcome, when the charge state changes, when the percent moves 2 points or more from the last frame sent, when a reading becomes unavailable, and when it comes back.
  - The daemon keeps the latest frame per session and drops it when the session ends or is replaced. `DeviceInfo.battery` carries it, and the Devices tab shows it for a connected device that has one.
  - A `battery` frame that fails to parse or check is logged and ignored. It never ends the session.
- **A failed touch read.** One failed GT911 read no longer reboots the board (Q5, slice B0).
- **Tests.**
  - Host tests cover decoding, the plausibility check, the charge-state mapping, freshness and the send rule (`test_batfmt.c`, `test_proto.c`).
  - Daemon tests cover the frame, the field and state rules, the cap gate, malformed frames (wrong types, a missing field, negative and overflowing numbers) followed by a working press, replacement, and disconnect (`protocol.rs`, `tests/device_sim.rs`).
- **On-board checks.** The user's checks C1 to C6 are recorded in [Results](#results), including the battery-unplugged and full-charge readings that are untested today.
- **Docs.** `CLAUDE.md`, `site/src/protocol.md` and `site/src/device-spec.md` describe what was built.

## Assumptions

| # | Assumption | Least evidence that updates it | Trigger |
|---|---|---|---|
| A1 | One `i2c_master_transmit_receive` of register 0x00 and 11 bytes returns 0x00 to 0x0A in order, so the register pointer auto-increments. The u32 values in the probe suggest it does | B2's first step compares the 11-byte read with single-byte reads of each register | If they differ, read each field with its own transfer (0x00, 0x04, 0x08, 0x09). `batfmt_decode` still takes the same 11-byte buffer |
| A2 | The battery mV is exactly the pin mV × 139 / 100, truncated. Both probe rows match: 2948 → 4097 and 2787 → 3873. A junk read won't match | C1 readings | A real reading that fails the check is investigated (byte order, a different register, a different STC8 firmware) before anything changes. The tolerance isn't widened just to let it through |
| A3 | Status 0x09: bit 0 is STDBY and bit 1 is CHRG, both active low. 0x01 means charging, 0x03 discharging, 0x02 full (untested), and 0x00 (both low) unknown. 0x0A repeats 0x09 and is only logged | C3 (full charge) | If a full battery reads something other than 0x02, change the table in `batfmt.c` and its test |
| A4 | With no battery on USB power, the TP4059's BAT pin floats near 4.2 V and STDBY may assert, so the registers may look like "full" or toggle (Q2) | C2, with every sample logged | If the reading can be told apart, a `BAT_NONE` rule goes in: a single-sample rule in `batfmt_decode`, or a rule over the last few samples in `battery.c`, with its test. Otherwise the board shows what the STC8 says, and the docs say so |
| A5 | The STC8 is fine at 100 kHz beside the GT911's 400 kHz device. `i2c_master` sets the speed per device, and the user's probe ran at 100 kHz | C1, the soak in C3, and the touch feel | If touch reads start failing (B0's counter), poll less often first, then look at the bus |
| A6 | The `i2c_master` driver serializes transfers from several tasks with its bus mutex (IDF 6.1 `i2c.rst` "Thread Safety"). `i2c_master_bus_add_device` inserts under that mutex. A NACK returns `ESP_ERR_INVALID_RESPONSE` and logs only at debug level (`i2c_master.c:157, 726`). Timeouts and bus-clear failures log E lines from several paths (`:93`, `:544`, `:553`, `:596`). The transfer timeout applies to the mutex wait and the transfer separately, so it isn't a strict end-to-end deadline | Read of the installed IDF (done) | None. This is verified. The failure latency is measured in C4b |
| A7 | Montserrat 20 has the battery and charge glyphs: U+F240 to U+F244 (adv 25 px each) and U+F0E7 (adv 12.5 px) are in `lv_font_montserrat_20.c`, and `LV_SYMBOL_BATTERY_FULL` to `_EMPTY` and `LV_SYMBOL_CHARGE` are in `lvgl/font/lv_symbol_def.h`. It is already the bar font's fallback, so no `fonts:gen` and no sdkconfig change | Read of the managed component (done) | None |
| A8 | The STC8's percent is its own voltage lookup. Under charge current the terminal voltage reads high, so the percent may jump when USB is plugged in (85 % charging against 53 % on battery in the probe) | C1: plug and unplug USB and watch the percent | Shown as the STC8 reports it. Q3 asks whether to do more |

## Findings

| # | Where | What |
|---|---|---|
| F1 | `touch.c` `touch_init` | Creates the I2C0 master bus as a local and deletes it on failure. `i2c_master_get_bus_handle(I2C_NUM_0, &bus)` (IDF 6.1 `i2c_master.h:365`) gets it back, so `touch_init` needn't change its signature |
| F2 | `esp_lvgl_port_touch.c:127`, `esp_lcd_touch_gt911.c:231` | esp_lvgl_port wraps the touch read in `ESP_ERROR_CHECK`, and the GT911 driver returns any I2C error. So one failed GT911 read aborts and reboots the board today. A second device on the bus makes that likelier |
| F3 | `i2c_master.c:1215` | `i2c_master_bus_rm_device` checks the bus status before taking the bus mutex, so a removal during a touch read can fail with `ESP_ERR_INVALID_STATE` and an E line |
| F4 | `wifi.c` `rssi_job`, `post_rssi`, `rssi_timer_cb`, `wifi_rssi` | The repo's pattern for a periodic device read. An esp_timer posts one svc job at a time, guarded by a `queued` flag. The job reads with no lock held and stores a snapshot under a spinlock, and the UI's 1 s ticks read the snapshot |
| F5 | `svc.h` | svc runs the slow device jobs and is never the link or LVGL task. A Format card blocks it for the whole format |
| F6 | `ui.c` bar | Several places assume every slot from `BAR_CPU` to `BAR_COUNT` is a host group: the gap math (`/ 4`, `% 4`, `(0 < spare)` to `(2 < spare)`), `bar_refresh`'s offline blanking and early return, `bar_group_text`'s `default:` (NET), `bar_group_w`, and the label loop that turns recolor on for `slot >= BAR_CPU`. `w[BAR_STATUS]` spans `clock_x - 2 * BAR_MARGIN` |
| F7 | `plans/status-bar-2.md` "Board geometry" | Measured on the board: `cpu 12/168 ram 220/96 gpu 356/168 net 564/216 clock 820/192, gap 40`. That leaves 160 px for gaps |
| F8 | `tab_status.c` | The left column ("this device") has 8 rows and the right has 9. Rows are 31 px, there is room for about 13, and the left column is 412 px wide |
| F9 | `link.c` `send_hello`, `post` | The cap list is static, and `test_proto.c` pins the hello's size. Four real caps and a 32-character fw make 226 bytes. `"battery"` makes 236, under the 256-byte buffer. `post` never waits and logs an E line when the 16-slot queue is full |
| F10 | `server.rs` `incoming` (`:527`), session loop | A text frame that fails serde becomes `Incoming::BadRequest`, which sends `bad_request` and ends the session. The one exception is `unparsable_report_id` (`:599`), which turns a mistyped `report` into one that `check` rejects. `FeedDismiss` and `AssetGet` are gated on their caps. An unknown `t` is ignored (`unknown_message_type_does_not_end_the_session` in `device_sim.rs`) |
| F11 | `lib.rs` event loop | Any `DaemonEvent` other than the two layout events is logged at info, emits `devices-changed` and rewrites the tray text |
| F12 | `server.rs` `DeviceInfo`, `api.ts:12`, `tray.rs:110` | `DeviceInfo` is mirrored in TS and built in a tray test fixture. All three change together |
| F13 | `logring.c:210, 282` | The Errors tab shows W and E lines both |

## Approach

### Firmware: touch tolerance (B0)

`touch_init` keeps the GT911 driver's `read_data` pointer and installs a wrapper in the handle (`esp_lcd_touch_t` is public in `esp_lcd_touch.h`).

1. Before each read, the wrapper copies `tp->data.points` and `tp->data.coords` under `tp->data.lock`. It copies only those fields, never the struct, because the struct holds the lock.
2. On an error it restores them, counts the error, logs at most one W line a minute (`touch read failed N times: <err>`), and returns `ESP_OK`.

The restore matters because the GT911 driver sets `points = 0` (`esp_lcd_touch_gt911.c:262`) before its second I2C read. A failure there would otherwise read as a release, and a false release can land as a tap. With the restore, a finger that was down stays down for one more 10 ms read. Its `get_xy` doesn't invalidate the points, so the restored ones are what esp_lvgl_port gets. This removes the F2 abort without vendoring esp_lvgl_port.

### Firmware: read

**`batfmt.c` and `.h`** are pure, with no LVGL and no driver, for the host tests (like `feedfmt.c` and `storagefmt.c`). They hold:

- `bat_reading_t`: the pin mV, the battery mV, the percent, the raw status bytes, and `bat_charge_t` (`BAT_UNKNOWN`, `BAT_CHARGING`, `BAT_FULL`, `BAT_DISCHARGING`, `BAT_NONE`).
- `batfmt_decode(const uint8_t regs[11], bat_reading_t *out)` decodes the little-endian u32s at 0x00 and 0x04, the percent, and the charge from 0x09 (A3). It returns false, meaning junk and not a reading, if any of these hold:
  - the pin mV is 0, so an all-zero buffer fails;
  - the battery mV differs from pin × 139 / 100 by more than 2 % + 20 mV (computed in 64 bits);
  - the battery mV is over 6000;
  - the percent is over 100 (refused, not clamped);
  - the status byte is over 0x03.
- `batfmt_fresh(have, age_us)`: false after `BAT_STALE_US`, 15 s (three polls).
- `batfmt_level(percent)`: 0 to 4, for the glyph.
- `batfmt_should_send(sent, now)`. Each argument is "unavailable" or a reading. It returns true when:
  - nothing was sent this session;
  - one is unavailable and the other isn't;
  - the charge state differs;
  - the percent is 2 or more from the last sent.
- The bar group text and the Status row text.

**`battery.c` and `.h`** are the driver.

- **Start.** `battery_start()` runs from `app_main` right after `touch_init` succeeds. It gets the bus (F1) and adds device 0x2F at 100 kHz. The add is safe beside the touch read, since the insert is under the bus mutex. It then reads synchronously up to 3 times, with a 50 ms transfer timeout.
  - **Present:** one decoded read makes the battery present, and an esp_timer then posts `battery_job` to svc every `BATTERY_POLL_MS` (5000).
  - **Absent:** with no decoded read the battery is absent. It never polls, and logs one info line (`battery: no monitor at 0x2F (<err or "implausible">)`). The device handle stays on the bus unused, never removed, which avoids F3.
  - If touch failed, the battery is absent.
- **The poll job.** `battery_job` copies `rssi_job`'s shape: a `queued` flag, the read, and the snapshot under a spinlock.
  - It logs the first read's duration once.
  - It logs a reading with its raw bytes whenever the charge state changes. For C1 to C3 a local, uncommitted one-line change logs every poll at info.
  - **Failure.** After 3 failed reads in a row it logs one W line and moves the timer to 60 s. The job reads the current period itself, so a job that was already queued can't undo the backoff. The first good read after that logs an info line and goes back to 5 s. The promise is the attempt rate: at most one failing transfer a minute. A NACK is silent (A6). A timeout can add a few IDF E lines per attempt, and C4b measures how many and how long it takes.
- **The link.** Every poll, good or failed, ends with `link_battery_poll()`, and the link decides what goes on the wire.
  - A one-pending flag holds the queue to one slot. The flag is set on a successful post and cleared when the link task takes the event, and no post is made while it is set.
  - A refused post is retried at the next poll.
  - If the link task is wedged (CLAUDE.md's mid-frame stall), its 16-slot queue still doesn't fill with battery events, so `post` logs no E line every 5 s.
  - `link_battery_poll()` returns silently when `link_start` never ran, unlike `link_press`'s `ESP_RETURN_ON_FALSE`, so a board with no link logs nothing every 5 s.
- **The snapshot.** `battery_snapshot(bat_snapshot_t *out)` is safe from any task. It holds present, have-reading, the reading, its age and the last error.

**Pull, not push.** `bar_refresh` (1 s) and `tab_status_tick` (1 s) read `battery_snapshot()`, as `tab_status_tick` already reads `wifi_rssi()`. There's no new callback into `ui.c` and no `uimode.c` change, and the sleep screen needs nothing.

### Firmware: show

- **The bar slot.** A `BAR_BATT` slot sits between NET and the clock. It exists only when `battery_present()` was true at `bar_create`, which is known at boot, long before grid mode. The host groups get an explicit range (`BAR_CPU` to `BAR_NET`) wherever F6's loops assume "every slot after `BAR_CPU`", so the battery has its own text, width, alignment and recolor branch.
- **The label.** It is one right-aligned label with recolor off: `[LV_SYMBOL_CHARGE]<battery glyph by level> <percent in 4 cells>`. The charge mark is left out unless charging, and right alignment keeps the glyph and the percent from moving.
  - It shows `--` when stale or after the read fails.
  - It shows nothing for `BAT_NONE`.
  - `BAT_UNKNOWN` shows the glyph and percent with no mark.
- **Width.** The worst case, measured with `bar_text_w`, is the charge mark + `LV_SYMBOL_BATTERY_FULL` + `100%`: about 13 + 25 + 12 + 48 = 98 px. That leaves 62 px for five gaps, about 12 px each.
- **Gaps.** `bar_create`'s gap math works over the groups present, so a board without a battery keeps four 40 px gaps exactly.
- **Offline.** `bar_refresh` updates the battery slot before its offline early return, so the slot stays while offline (it is a local value). `w[BAR_STATUS]` becomes `x[BAR_BATT] - gap - BAR_MARGIN` when the slot exists, and stays as it is when it doesn't.
- **Log.** The `bar groups (x/width)` line adds `batt x/w`.

**Settings > Status.** `ROW_BATTERY` is a 9th left-column row, after `ROW_UPTIME`. The longest form is `Battery 4.10 V, 100%, discharging`. The implementer measures it in Montserrat 24 against `LEFT_W` (412). If it doesn't fit, the state words get shorter first (`on battery`), then the row moves to the right column. The tab comment that lists the widest rows is updated.

### Wire

- **The cap.** `hello.caps` gains `battery`. It is static in `send_hello`, meaning "this build reads the STC8 and sends `battery`". A board without the STC8 sends no frames.
- **The frame.** `{"t":"battery","mv":3873,"percent":53,"charge":"discharging"}` goes device → daemon.
  - `charge` is one of `charging`, `full`, `discharging`, `none` and `unknown`.
  - `charging`, `full` and `discharging` need `mv` and `percent`.
  - `none` and `unknown` may leave both out. `unknown` with no numbers means "the reading is unavailable".
  - `mv` and `percent` are present together or not at all.
  - An unknown future `charge` string is read as `unknown` and keeps any numbers.
- **When it's sent.**
  - The link task keeps the last-sent value per session and resets it at welcome.
  - After each welcome it sends the snapshot, or `unknown` with no numbers if there's no fresh reading. It sends nothing if the battery is absent.
  - On each `EV_BATTERY` it sends when `batfmt_should_send` says so.
  - While the link is down, `EV_BATTERY` is dropped silently. A W line every 5 s offline would fill the Errors tab.
  - A stale snapshot caused by a blocked svc (no poll ran) isn't seen by the link, so the daemon may show a reading as old as the svc job. This is documented, not fixed.
- **The daemon's parse.**
  - It adds `CAP_BATTERY` to `CAPS` and `Message::Battery(Battery)`.
  - `Battery` uses unsigned fields with `charge` as a serde enum with `#[serde(other)] Unknown`, and a `check()` that enforces the combinations above, `mv` ≤ 10 000 and `percent` ≤ 100.
  - `incoming()` gains a fallback beside `unparsable_report_id`: a text frame whose JSON `t` is `"battery"` but which fails serde becomes `Incoming::Msg(Message::Battery(..))` with a marker that `check()` rejects. Wrong types, a missing `charge`, negative and overflowing numbers all then reach the session arm instead of `BadRequest`. Text that isn't JSON at all can't be identified as a battery frame and keeps today's `bad_request`.
- **The session arm.** It is gated on the cap. A failed `check()` logs one W line per session and is ignored. It is never `bad_request` and close: the board resends at every welcome, so a close would loop it through reconnects over a status nicety (F10's `report` precedent).
- **Storage.**
  - `SessionEntry` gains `battery: Option<Battery>`.
  - `Shared::set_battery(device, session_id, battery)` updates only the current session, so an old session's late frame can't overwrite its replacement's. It emits `DaemonEvent::BatteryChanged(device)` only when the value changed.
  - `DeviceInfo` gains `battery: Option<Battery>`, which is `None` while disconnected.
- **`lib.rs`.** An arm for `BatteryChanged` emits `devices-changed` with no info log and no tray rewrite (F11).
- **The app.** `api.ts`'s `DeviceInfo` gains `battery`. `Devices.tsx` shows `53% · 3.87 V · discharging` beside `connected`, or the charge word alone when there are no numbers.

### Rejected alternatives

| Option | Why not |
|---|---|
| Poll from an LVGL timer, in the same task as the GT911 read | No bus contention, but it does I/O under the LVGL lock. A timeout would freeze the UI, and it breaks `svc.h`'s rule. svc costs one GT911 read delayed by about 1.5 ms every 5 s, and `--` during a format, both accepted |
| A dedicated battery task | About 3 KB of internal stack for a 1 ms read every 5 s, when svc and the RSSI pattern already exist |
| Change `touch_init` to return the bus, or move the bus to its own module | API churn. `i2c_master_get_bus_handle` already gives the bus back |
| `i2c_master_probe` for detection | The real register read was verified on the board, and it also runs the plausibility check, so junk at 0x2F counts as absent. A probe adds a zero-length write that the STC8's slave firmware has never seen |
| Remove the device handle when absent | F3: the removal races the touch read. An unused handle costs a few bytes |
| Vendor esp_lvgl_port to drop its `ESP_ERROR_CHECK` | A vendored component to maintain. Wrapping `read_data` in the handle does the same from `touch.c` |
| Push the reading into `ui.c` with a callback | Needs the LVGL lock from svc and a `uimode.c` entry for sleep. Pull fits the existing 1 s ticks |
| Hide or show the bar slot at run time as the battery comes and goes | A relayout at run time. The slot is reserved whenever the STC8 answered at boot, and shows nothing for `BAT_NONE` |
| Name `battery` in the hello only when the STC8 answered | A dynamic cap list for no gain. A board with no STC8 sends no frames |
| Post `EV_BATTERY` only on a change | A post refused by a full queue would lose a charge change for good. Posting every poll (one pending at most) retries for free |
| Send every poll, or every mV change | The mV is noisy, and every frame rewrites the app's list. The send rule is enough, and the mV in the app is "as of the last frame" |
| A daemon-side timeout for a silent battery | Healthy readings that don't change send nothing, so silence isn't staleness. The board sends `unknown` instead |
| Compute our own percent from the mV | No calibrated curve for this cell, and the STC8 already has one (Q3) |
| `bad_request` and close on a bad frame, like `feed_dismiss` | A reconnect loop, see above |

## Reuse analysis

- **Patterns reviewed:**
  - `wifi.c`'s RSSI job and snapshot (F4).
  - `feedfmt.c`, `storagefmt.c`, `wifistate.c` and `hoststat.c`, as pure modules with host tests.
  - `bar_group_w` and `bar_text_w`, for the worst-case widths.
  - `link_feed_dismiss` and `EV_FEED_DISMISS`, for a device → daemon frame.
  - `send_report`, at welcome.
  - `protocol.rs`'s `check()` methods, and `unparsable_report_id` with the `Report` leniency.
  - `SessionEntry.caps` flowing to `DeviceInfo`.
  - `tests/device_sim.rs`'s `hello_with_caps`, `caps_of` and `unknown_message_type_does_not_end_the_session`.
- **Chosen:** reuse throughout. There are two extensions:
  - `bar_create` and `bar_refresh`'s "every slot after `BAR_CPU`" become an explicit host range.
  - `incoming()`'s report fallback gains a battery sibling.

  Nothing is extracted. `batfmt.c`, `battery.c` and the touch read wrapper are new.
- **Duplication risk:** low. The poll copies the RSSI shape (a timer, a flag, one svc post), with nothing worth sharing. The parse fallback could become a table keyed by `t` if a third message needs one, but not for two.

## Slices

| Slice | Kind / agent | Depends on |
|---|---|---|
| B0 Touch read survives a failed read | coder (small, needs care) | — |
| B1 `batfmt` and host tests | coder (mechanical, from this spec) | — |
| B2 `battery.c`, boot wiring, logs | coder-high (driver and concurrency judgment) | B0, B1 |
| C1–C4 On-board reads | operator (the user) | B2 |
| B3 Settings > Status row | coder | B2 |
| B4 Bar slot | coder (design judgment: layout) | B2 |
| C5 Bar on the panel | operator | B4 |
| B5 Wire contract and daemon | coder (Rust) | — (contract first) |
| B6 Firmware frame | coder | B2, B5 |
| B7 App Devices tab | coder (mechanical) | B5 |
| C6 End to end | operator | B6, B7 |
| B8 Docs and results | scribe | all |

B0, B1 and B5 can start at once. B3, B4 and B6 are independent once B2 lands. Firmware-only value (the log, the row and the bar) lands with B2 to B4, and doesn't wait for the daemon.

### B0 Touch read survives a failed read

- **Files:** `firmware/main/touch.c`.
- **Acceptance:**
  - A failed GT911 read returns `ESP_OK` to esp_lvgl_port and is counted.
  - A failure in either of the driver's reads leaves the points as they were before the read: a held finger stays pressed.
  - It logs at most one W line a minute.
  - Normal reads are unchanged: the same flick and tap feel as device-ui S8.
- **Cheapest check:** build and flash. Force one failure with a local, uncommitted hack (for example, failing every 1000th read in the wrapper) and see the W line and no reboot. Revert the hack.
- **Rollback:** remove the wrapper install, one line.

### B1 `batfmt` and host tests

- **Files:**
  - New `firmware/main/batfmt.c` and `.h`.
  - New `firmware/test/host_proto/main/test_batfmt.c`.
  - Add both to `firmware/test/host_proto/main/CMakeLists.txt`, and `batfmt.c` to `firmware/main/CMakeLists.txt`.
- **Acceptance:**
  - **Decoding.** The two probe rows decode to the table's values. Each status byte 0x00 to 0x03 maps per A3.
  - **Refusals.** These are refused: an all-zero buffer, an all-0xFF buffer, a mismatched pin/battery pair, a percent of 101, and a status of 0x04. The pin × 139 check doesn't overflow at a pin mV of 0xFFFFFFFF.
  - **Freshness.** `batfmt_fresh` is true at exactly 15 s and false just after.
  - **`batfmt_should_send`** is:
    - true for the first frame;
    - true for a charge change alone;
    - false for a 1-point move;
    - true for a 2-point move measured from the last sent, not the last seen;
    - true for reading → unavailable and unavailable → reading, even when the reading equals the last one sent before it went unavailable.
  - No test asserts on glyphs, words or spacing.
- **Cheapest check:** `mise run fw:test:unit`. Also `prek run --files <new files>`, since `--all-files` skips untracked files (CLAUDE.md). The same applies to B2's new files.

### B2 `battery.c`, boot wiring, logs

- **Files:**
  - New `firmware/main/battery.c` and `.h`.
  - `main.c`: call `battery_start()` after `touch_init` returns OK.
  - `firmware/main/CMakeLists.txt`, which already requires `esp_driver_i2c`.
  - `svc.h`'s comment adds the battery read to its job list.
  - `link.h` and `link.c`: `link_battery_poll()` (silent before `link_start`, and the one-pending flag) and an `EV_BATTERY` that does nothing until B6, and drops silently.
- **First step:** confirm A1 by logging one 11-byte read beside single-byte reads of 0x00 to 0x0A, once, then delete that code.
- **Acceptance:**
  - The boot log has the first reading and the read time.
  - The state changes are logged within 5 s of plugging and unplugging USB.
  - B0's counter stays at 0 over C1's session.
  - The board still boots, connects and shows its grid, and the `stack bytes free:` line about 60 s in shows svc's margin.
- **Cheapest check:** `mise run fw:build`, flash, read the boot log with pyserial (CLAUDE.md), and plug and unplug USB.
- **Rollback:** remove the one `battery_start()` call.

### C1–C4 On-board reads (the user)

Run C1 to C3 with the local "log every poll" change, and paste the lines into Results.

- **C1.** On USB with the battery, on battery only, and the transitions both ways. Record the percent jump (A8) and the read time.
- **C2.** The battery unplugged, on USB power: every sample over a minute. This decides A4, and the presence rule depends on it. If the reading can be told apart, add the `BAT_NONE` rule with its test before B4's acceptance.
- **C3.** Charge to the end, overnight: the status byte at full, which confirms or changes A3. The overnight run doubles as a soak. Check B0's counter, any reboot, and the crash report count. This doesn't block the other slices.
- **C4.** A local, uncommitted build with the address set to 0x2E (the absent path). It should log one info line, add no Errors-tab lines, have no bar slot (`gap 40` in the log), show `Battery --` in Settings, and send no `battery` frames once B6 is in. Revert after.
- **C4b, the lost path.** Optional, if a cheap way exists: briefly short the STC8's supply, or point the poll (not the boot read) at 0x2E. Expect the W line after 3 failures, the 60 s backoff, `--` in the bar, an `unknown` frame once B6 is in, and recovery. Record the failure latency and the E lines per attempt.

### B3 Settings > Status row

- **Files:** `firmware/main/tab_status.c`.
- **Acceptance:**
  - The row shows each state from the snapshot.
  - It shows `--` when absent or stale.
  - It doesn't end in dots at the longest form, or the fallback (shorter words) is applied.
  - The tab still doesn't scroll.
- **Cheapest check:** build, flash, and open Settings > Status on and off USB. No unit test, because it is presentation.

### B4 Bar slot

- **Files:** `firmware/main/ui.c`: `bar_slot_t`, the host-group range in `bar_group_text`, `bar_group_w`, `bar_refresh` and the label loop, a battery text, width and alignment branch, `bar_create`'s gap math and `w[BAR_STATUS]`, and the bar comment block.
- **Acceptance:**
  - **With the STC8:** `bar groups` lists `batt`, the gaps are equal within 1 px, and there is no `bar slot overflow`.
  - **Offline:** the slot shows beside the status text, and the status text doesn't overlap it.
  - **Stale:** `--` after 15 s with no fresh reading.
  - **Without the STC8 (C4's build):** the log line has today's numbers exactly.
- **Cheapest check:** the boot log line, then C5.

### C5 Bar on the panel (the user)

- The glyph, the percent and the charge mark read clearly at about 12 px gaps.
- If the bar still looks crowded with the `HH:MM` clock, say so; there is no further fallback planned.

### B5 Wire contract and daemon

- **Files:**
  - `site/src/protocol.md`: a `battery` (device → daemon) section with the field and state rules, and the `battery` cap in `hello`.
  - `app/src-tauri/src/daemon/protocol.rs`: `CAP_BATTERY`, `CAPS` grows to 6, `Battery`, `Charge`, `check`, and unit tests for every combination rule and an unknown charge string.
  - `server.rs`: the `incoming()` fallback, the session arm, `SessionEntry`, `set_battery`, `DeviceInfo` and `DaemonEvent::BatteryChanged`.
  - `lib.rs`: the event arm.
  - `tray.rs`: the fixture.
  - `tests/device_sim.rs`.
- **Acceptance (`device_sim`):**
  - A session with the cap sends a frame, and `list()` shows it.
  - A second frame with the same value emits no event.
  - A session without the cap has its frame ignored.
  - Each of `percent: 101`, `percent: "53"`, `mv: -1`, `mv: 1e30`, a missing `charge`, and `charging` with no numbers is ignored, and a following press still gets its result.
  - After a replacement session connects, a frame from the old one doesn't change `list()`.
  - Disconnecting clears it.
- **Cheapest check:** `cargo test` in `app/src-tauri`, and clippy (the app job, or `mise run lint` with clippy).

### B6 Firmware frame

- **Files:**
  - `firmware/main/proto.c` and `.h`: `proto_build_battery`.
  - `firmware/main/link.c`: the cap in `send_hello` and its comment, the send at welcome, the `EV_BATTERY` handling, and the per-session last-sent value.
  - `firmware/test/host_proto/main/test_proto.c`:
    - The built frame parses back to its fields for a reading, for `none`, and for `unknown` with no `mv` or `percent`.
    - The pinned hello size becomes 236 for the five real caps with a 32-character fw.
- **Acceptance:** host tests pass. Old-daemon compatibility is already covered by `unknown_message_type_does_not_end_the_session`, so there is no separate board run against the old daemon.
- **Cheapest check:** `mise run fw:test:unit`. The board check is C6.

### B7 App Devices tab

- **Files:** `app/src/api.ts` (`DeviceInfo.battery`) and `app/src/Devices.tsx`.
- **Acceptance:** the type checks, biome is clean, and a connected device with a reading shows it. No TS test, because it is presentation.
- **Cheapest check:** `mise run app:build` (tsc), and biome from `app/`.

### C6 End to end (the user)

- With `mise run app:dev` and the board connected, the Devices tab shows the battery.
- Unplugging USB changes it within about 6 s.
- Forgetting or disconnecting the board removes the line.

### B8 Docs and results

- **`CLAUDE.md`:**
  - A Hardware facts bullet on the STC8 at 0x2F: the poll on svc every 5 s, the absent, stale and lost rules, the A1 to A4 outcomes, and "only 0x2F is read; nothing is written but the register pointer".
  - The touch read wrapper and why it exists (F2).
  - Boot order step 4 gains "then the battery monitor, which needs touch's bus".
  - A Daemon facts line for the `battery` cap and the parse fallback.
- **`site/src/device-spec.md`:** the untested cells, filled from C2 and C3. This builds on the uncommitted battery section, which the user commits first.
- **`site/src/protocol.md`:** B5's section, checked against the code.
- **This file:** Results and the Status line.
- **Cheapest check:** `mise run lint`, and `mise run site:build`.

## Verification

| Condition or risk | Cheapest reliable evidence | Rerun when |
|---|---|---|
| Decoding, plausibility, the charge map, freshness, the send rule | `test_batfmt.c` in `fw:test:unit` | `batfmt.*` changes |
| The frame and the hello size | `test_proto.c` | `proto.c`, or the cap list |
| The daemon accepts, gates, ignores bad frames, guards replacement, clears | `device_sim.rs` and `protocol.rs` tests | `server.rs` or `protocol.rs` changes |
| One bad touch read doesn't reboot | B0's forced failure | `touch.c` changes, or an esp_lvgl_port bump |
| Sharing the bus doesn't hurt touch | B0's counter at 0 over C1 and the C3 soak | `battery.c`'s timing or speed changes |
| The absent path is silent and unchanged | C4 | `battery_start` or `bar_create` changes |
| The lost path backs off and recovers | C4b, if run. Otherwise the code review of the period logic | `battery_job`'s failure handling changes |
| The bar fits | `bar groups` line, no overflow, C5 | `bar_create`, the battery text, or the bar font changes |
| The register meanings hold for states not yet seen | C2, C3 | Never, once recorded |

A failure is diagnosed before anything is rerun. It may be a firmware defect, the STC8 behaving differently from A1 to A4, or a bus problem: look for `i2c` E lines and B0's counter first.

## Risks

| Risk | Impact | Mitigation | Rollback |
|---|---|---|---|
| The STC8 holds SDA low or stretches the clock forever | Touch reads fail with it. Without B0 the board would reboot in a loop | B0 turns a failed touch read into a counted miss. A 50 ms transfer timeout, the 60 s backoff after 3 failures, and a 100 kHz device clock. The IDF clears the bus on a timeout | Remove `battery_start()` |
| Five groups make the bar crowded (gaps about 12 px) | Looks cramped | Measured layout, `HH:MM` clock (Q1), C5 | Drop the slot (B4 only) |
| The percent jumps on plug and unplug (A8) | It looks wrong to a user | Documented, and Q3 | — |
| No battery on USB reads as full (A4) | The bar shows full with no cell | C2 decides. The docs say so if it can't be told apart | — |
| svc is blocked by a Format card | `--` on the board, and the daemon keeps the last reading meanwhile | Accepted. Formats are rare, and the user is watching Settings | — |
| Deep discharge browns out the P4 | Reboot loops at the end of the charge | Out of scope. A `brownout` report already reaches the daemon (Q4) | — |
| B0 hides a GT911 that is really gone | Touch stops with no reboot to reset it | The W line names the failure count. An RTS pulse or the reset button recovers it | Remove the wrapper |

## Open questions

Decided by the user on 2026-11-01:

- **Q1. Bar spacing with five groups.** Decided: the clock drops to `HH:MM`, 24 h, with no weekday and no date. B4 changes the clock format and its worst-case measurement in `bar_create`; C5 still judges the spacing on the panel.
- **Q2. No battery attached, on USB.** Decided without C2 (the user chose not to test it): no `BAT_NONE` rule; the board shows what the STC8 reports, which may look like a charging or full battery.
- **Q3. The percent while charging.** Decided: shown as the STC8 reports it.
- **Q4. Low-battery behavior.** Decided: display only.
- **Q5. B0.** Decided: included.

## Results

Built as planned. Recorded by B8 from the code and the user's board checks.

### On-board checks

- **C1 passed.** The boot read took about 1.9 ms. Unplugging and replugging USB logged `charging -> discharging` and back, with no reset.
- **C5 and C6 passed.** The user saw the bar slot with the `HH:MM` clock and judged the spacing fine. The Devices tab shows the percent, the voltage and the charge state, and switches to discharging on unplug.
- **Still open.**
  - **C2:** not run, and the user chose not to run it (2026-11-01). What the STC8 reports with no battery on USB is unknown; the board shows whatever it reports, and `batfmt_decode` never gives `BAT_NONE`.
  - **C3:** charge to full overnight. The status byte at full (A3's 0x02) is untested, and so is the soak.
  - **C4 (2026-11-01), passed:** a local build at 0x2E logged one info line, `battery: no monitor at 0x2E (ESP_ERR_INVALID_RESPONSE)`, added no E lines, laid the bar out as four groups (`net 663/216 clock 952/60, gap 73`), sent no `battery` frame, and connected normally.
  - **C4b:** the lost path (an STC8 that stops answering mid-run) wasn't run; the backoff is checked by code review only.

### Measured on the board

- svc stack: 4968 bytes free (the `stack bytes free:` line).
- B0's touch failure counter was 0 in the boot logs.

### Risks to record

- **Log flood on a stuck bus.** The 10 ms touch reads can produce unthrottled IDF `I2C transaction timeout` E lines. B0's W line is limited to one a minute, but the IDF's lines are not.
- **Late unavailable frame.** The daemon's view goes stale only when a poll arrives. A long svc block (Format card) or the 60 s backoff delays the `unknown` frame.
- **Status row fallback.** The row falls back to "draining" when the "discharging" text is wider than the column. That fallback is unverified on the panel.
- **A NACK and a stuck bus look alike.** Both come back from the IDF as `ESP_ERR_INVALID_RESPONSE`, so `last_err` can't tell them apart.
- **Daemon event throttle.** Battery events reach the app at most once a second per session (`BATTERY_EVENT_GAP`), added after the plan.

### Note

A panic report pending on the board came from a temporary probe build (ELF `7985206...`), not from shipped code.

## Critique log

One adversarial pass, codex `gpt-6.1-sol` at high effort, in plan mode, on the first draft. Each point was checked against the code before it was logged.

| # | Point | Severity | Outcome |
|---|---|---|---|
| 1 | esp_lvgl_port `ESP_ERROR_CHECK`s the touch read (`esp_lvgl_port_touch.c:127`), so a stuck bus reboots the board, not just touch | High | Adopted: verified. Added B0 (the touch read wrapper), Q5, F2, and the C3 soak check. A follow-up check found the GT911 driver zeroes the points before its second read (`:262`), so the wrapper saves and restores them |
| 2 | A frame that fails serde becomes `BadRequest` and closes the session before `check()` runs (`server.rs:527`) | High | Adopted: verified. Added an `incoming()` fallback beside `unparsable_report_id`, and malformed-frame tests followed by a press |
| 3 | Plausibility accepts an all-zero buffer, clamping hides junk, and "widen the tolerance" weakens the filter | Medium | Adopted: pin mV > 0, a percent over 100 refused, status ≤ 0x03, 64-bit math, and A2's trigger now says investigate first |
| 4 | A lost monitor never updates the daemon, so the app shows a stale reading | Medium | Adopted: an `unknown` frame with no numbers when the reading becomes unavailable, and a forced resend on recovery, in `batfmt_should_send` with tests. A stale reading from a blocked svc is documented, not fixed |
| 5 | `i2c_master_bus_rm_device` checks status before the mutex (`i2c_master.c:1215`), so removing during a touch read can fail | Medium | Adopted: verified. The absent path keeps the handle and never removes it |
| 6 | The timeout and log-volume promises are too strong. Several IDF E paths, the timeout isn't end to end, and W lines reach the Errors tab | Medium | Adopted: the promise is now an attempt rate, the job reads the current period, and C4b measures latency and lines. The offline `EV_BATTERY` drop is silent |
| 7 | A notification refused by a full link queue is lost for good if the reading then stays put | Medium | Adopted: `link_battery_poll()` posts every poll with at most one pending, and the link decides. A failed post is retried 5 s later. A pre-handback review added the one-pending flag (so a wedged link task can't fill the queue) and silence before `link_start` |
| 8 | Logging only on state change can't give C2 its samples, and a toggling-status rule needs history | Medium | Adopted: a local log-every-poll change for C1 to C3. A history rule, if needed, lives in `battery.c`. C2 gates the presence rule before B4's acceptance |
| 9 | `ui.c`'s "every slot after `BAR_CPU`" loops (`default:` is NET, recolor on, offline early return) would trap the battery slot | Medium | Adopted: verified. An explicit host range, a battery branch, a refresh before the offline return, and the `w[BAR_STATUS]` formula |
| 10 | The validator doesn't enforce which states need numbers, and the replacement race needs a test | Medium | Adopted: the combination rules are in the contract, `check()` and its tests, plus a replacement test |
| 11 | Firmware-against-old-daemon board check is redundant with `unknown_message_type_does_not_end_the_session`. Do the contract and daemon first | Low | Adopted: B5 is the contract and daemon, B6 the firmware frame, and the old-daemon board run is dropped |
