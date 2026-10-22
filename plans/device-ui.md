# Device UI: a status bar, and settings behind a long swipe up

Status: planned 2026-10-22, revised with user decisions. Against `main` at `aad6e3f`. No slice started.

Gives the paired screen's top 204 px back to the buttons. Today a paired board shows five status lines (Wi-Fi, SD, USB, Touch, Daemon), a Licenses button and a Forget pairing button above the grid (`ui.c:443-462`). After this plan:

- The grid starts 44 px from the top, under a 36 px bar that shows the desktop's CPU and memory use, its network rates, and the time and date.
- A long swipe up opens a settings page with the device's status and info, Licenses, and Forget pairing. It is built when it opens and deleted when it closes.
- Until the first layout arrives, and whenever the board is unpaired, the screen is today's status screen, so Wi-Fi and pairing problems stay visible without a gesture.

## Decisions 2026-10-22

The user answered the open questions of the first draft:

- **No backward compatibility** (was A): "this is still development, no need to worry about old firmware version support." `GRID_TOP` moves to 44 everywhere, with no geometry flag, no icon stripping, no Devices-tab note and no old-firmware log line. Today's firmware against the new daemon gets icons rendered for the new, bigger boxes. That combination is unsupported.
- **Network traffic** (was B): the interface of the desktop's default route.
- **Capability** (was C): `status_bar` stays, but only to gate `stats` (Wire). It no longer says anything about geometry.
- **Clock** (was D): 24-hour, shown as `Wed 22 Oct 14:32`.
- **Long swipe** (was E): 200 px up, more than twice as far up as sideways, anywhere on the grid, tuned on the board in S6.
- **Auto-close** (was F): settings closes after 60 s without a touch.
- **Rates** (was G): bytes per second with SI prefixes (`1.2 MB/s`).
- **Daemon name in the bar** (was H): yes, while the link is up.

## Goal and done

- **Grid mode** (paired, and a layout set applied since boot): the bar on top, then the grid from y 44. No status lines, Licenses or Forget pairing on screen.
- **Status mode** (booting, unpaired, or paired with no set yet): today's screen. The five status lines at y 4 to 156, Licenses on the USB line, Forget pairing on the Touch line while paired, and the pairing views from y 204. A board that can't reach its daemon (wrong cert, daemon gone) stays here and can still be unpaired.
- **Settings** opens on a long swipe up in grid mode: at least 200 px up, more than twice as far up as sideways, starting on the grid. It never presses a button or turns a page. It closes with Close, a swipe down, 60 s without a touch, or an unpair. While it is open the grid's widgets are deleted; closing rebuilds the page from the model. Opening and closing it 10 times leaves the LVGL pool where it was.
- **The bar** shows the daemon's host name (or the link status while down), CPU %, memory used of total, receive and send rates, and `Wed 22 Oct 14:32`. Host values come from the daemon once a second. The clock keeps ticking on the board between frames and while the link is down. Without stats (never connected since boot, or more than 5 s since the last frame) the host values show `--`, and the clock shows `--:--` until the first frame since boot.
- **Contract**: `GRID_TOP` is 44 on the board, in the daemon's `box_px` and in the preview. A session whose hello names `status_bar` gets a `stats` frame once a second.
- `cargo test`, `pnpm test` and `mise run fw:test` cover the new rules. The hardware run (S8) passes on the board with serial-log evidence.
- `site/src/protocol.md`, `site/src/config.md`, `site/src/wifi.md`, `README.md` and `CLAUDE.md` are updated.

## Assumptions

- **Which host.** The stats describe the machine the daemon runs on, the one the board is paired with. The desktop is Linux (`plans/desktop-app-v1.md:57`), and `actions/shell.rs:238-244` already reads `/proc`.
- **Time.** The board has no SNTP and no time zone. The daemon sends Unix seconds and its UTC offset in seconds, so the board shows the desktop's wall clock, and DST changes arrive with the next frame. A time zone change on the desktop needs a daemon restart (the `time` crate reads TZ once).
- **LVGL pool.** 64 KB internal (`CONFIG_LV_MEM_SIZE=65536`, `sdkconfig:4965`), about 59,800 bytes usable. Pages S8 measured 49,052 to 49,072 bytes settled with 4 pages of 8×5 and 160 icons, and a 52,196-byte peak with a 1 KB result screen (`plans/pages.md`, S5 baseline and S8 results). Keeping the grid alive under a settings page of about 4 to 6 KB would leave about 2 KB, so the grid goes while settings is open.
- **Bigger cells don't add widgets.** The pool cost of a page depends on its widget count, not its pixel size. Icons live in PSRAM (`icons.c`), so the bigger boxes cost PSRAM and transfer time, not pool.
- **Montserrat 24 for the bar.** Chrome stays Montserrat, as fonts decided (`site/src/config.md:212`). The built-in font carries `LV_SYMBOL_UP`/`DOWN` (`lv_symbol_def.h:160`, `:164`) for the rate arrows. A Unicode arrow (U+2193) is not in the font.
- **Development only.** Every board runs the firmware built from the same commit as the daemon. Mixed versions are not tested or documented.

## Findings

Checked against `aad6e3f`.

| # | Where | What |
|---|---|---|
| F1 | `ui.c:443-462`, `ui.c:25-32`, `:267-280`, `:331-342` | `ui_init` builds the five status lines at y 4, 42, 80, 118, 156 on the screen, Forget pairing (300×40 at y 114, hidden until paired) and Licenses (220×34 at y 72). All live for the board's whole uptime |
| F2 | `ui.c:863`, `:889`, `:906`, `:921` | Every pairing entry point uses `s_forget` as its "ui_init succeeded" guard. Once Forget is created on demand, that guard needs its own flag |
| F3 | `ui.c:930-970`, `main.c:70-106`, `wifi.c:27-38`, `link.c:165-176` | Status texts arrive through `ui_set_*_text` callbacks and are written straight into the labels. Nothing keeps them, so a page built later can't show them. Each is at most 95 characters |
| F4 | `ui.c:759-771` | The pairing views start at `GRID_TOP` (204). They need their own constant once `GRID_TOP` moves |
| F5 | `ui.c:284-329` | The license viewer is the model: created on `lv_layer_top()` when opened, `lv_obj_delete_async` on Close |
| F6 | `ui.c:125-139`, `:180-207` | The toast and the result modal are children of `lv_layer_top()`. A page on the active screen stays under both, so Forget's "hold for 2 s" toast and a late result show over it. A page on the top layer created after them would cover them |
| F7 | `ui.c:466-475` | `grid_gesture` calls `lv_indev_wait_release` for every gesture direction, at LVGL's 50 px. After that the press handler returns early (`lv_indev.c:1293`) so no `PRESSING` arrives, and the release sends only `PRESS_LOST` (`lv_indev.c:1518-1531`). A "long" swipe can't be measured after an upward gesture unless `LV_DIR_TOP` stops calling it |
| F8 | `ui.c:365-379`, `:393-408`, `lv_indev.c:1440`, `:1566-1575` | `touch_track` sees `PRESSED` and every `PRESSING` on `s_grid` and on each cell. Without a wait, the release sends `CLICKED` when nothing scrolled, and `drag_consumed` keeps any touch that moved over 20 px from pressing. A vertical drag of any length already presses nothing |
| F9 | `lv_indev.c:1857-1873`, `sdkconfig:5247-5249` | LVGL classifies a gesture once, at 50 px of summed movement on the larger axis, and resets the sum on any read under 3 px. A slow drag gets no gesture at all |
| F10 | `lv_display.h:642` | `lv_display_get_inactive_time` gives the time since the last touch, which an idle close can poll |
| F11 | `grid.c:5-22`, `grid.h:13`, `icons.rs:161-202`, `preview.ts:17-24`, `:46-95` | `GRID_TOP` 204 is mirrored three times and sets every cell and icon box. `test_grid.c:14-17` and `:103`, `icons.rs:1225-1237` and the bound test near `:1270`, and `preview.test.ts` pin the table and the 3,625,216-byte bound. `config.md:134`, `:172-173` and `icons.h:15` quote them |
| F12 | `server.rs:94-105`, `:118-119`, `:136-137`, `:649-705`, `:800-806` | Tiles poll only while a session is open (`connected` watch), publish to a `watch` channel, and each session reads it in its `select!`. A stats sampler fits the same shape |
| F13 | `server.rs:596-600`, `protocol.rs:11-17` | The session keeps only names in `CAPS`. `CAPS` is `[fonts, pages]` |
| F14 | `link.c:729-734`, `test_proto.c:762` | The board names `{"fonts","pages"}` in a 256-byte hello buffer. With `status_bar` added and a 32-character `fw` the hello is 219 bytes |
| F15 | `link.c:1128` | `handle_msg` logs every received message at INFO before parsing. At one `stats` a second that is 86,400 lines a day at 115200 baud, on the link task |
| F16 | `proto.c:264`, `proto.c:452-489` | `get_int` takes `double` bounds, so it reads integers up to 2^53. Dispatch is by `t`, and an unknown `t` is logged at WARN and ignored |
| F17 | `tests/device_sim.rs:255`, `:2124`, `:2443` | The 65 `device_sim` tests read each session's messages in strict order with `recv`. An ungated `stats` frame every second would land in any test that runs past a second |
| F18 | `link.c:1330-1342` | The link task wakes at least once a second, so it can refresh a cached RSSI. It can block up to 13 s in inline mDNS discovery (`plans/pages.md` F28), which only makes the value stale |
| F19 | `esp_wifi_remote/idf_v6.1/esp_wifi_with_remote.c:400`, `esp_hosted/host/wifi_remote_glue/src/esp_wifi_remote_extended.c:114` | `esp_wifi_sta_get_rssi` exists for IDF 6.1 and is an RPC to the C6 over SDIO. It must not run on the LVGL task |
| F20 | `wifi.c:23`, `:132` | The SSID is kept in `s_ssid`. The IP is only in the status text |
| F21 | `Cargo.toml:43`, `time-0.3.55/src/sys/local_offset_at/unix.rs:104-107` | `time` 0.3.55 is a direct dependency. `local-offset` is on only through feature unification (tauri's `cookie`). Its Unix `local_offset_at` is a plain `localtime_r`, with no single-thread check since 0.3.37, so it works in the multithreaded daemon. No `sysinfo`, `chrono` or `procfs` in the build |
| F22 | `edit.ts:237-240` | `viewKey` counts the paged flag only for a view that names an icon, because the flag changes icon boxes. That stays true with the new top, so `viewKey` needs no change, only its comment's numbers |
| F23 | `GridEditor.tsx:235`, `:243`, `:296` | The editor's stage is `GRID_H` tall, from `preview.ts:22`. A new `GRID_TOP` changes its height |
| F24 | `README.md:83`, `config.md:218`, `protocol.md:272`, `config.md:212`, `wifi.md` | The docs place Forget pairing and Licenses on the main screen ("right of the USB line") and name the status lines as Montserrat chrome |

## Approach

### Screen modes

| Mode | When | On screen |
|---|---|---|
| Status | From boot until the first set applies. Again on every unpair | Today's screen: the five status lines, Licenses, Forget pairing while paired, and the pairing views from y 204 (`PAIR_TOP`) while unpaired |
| Grid | From the first applied set until an unpair. A dropped link keeps it (dimmed grid, as today) | The bar (y 0 to 36), then the grid from y 44 |
| Settings | Opened from grid mode | The bar stays. The page covers y 44 to 600. The grid's widgets are gone until it closes |

- The status texts move into `ui.c` buffers (five of 96 bytes). The setters do nothing before `ui_init` succeeds (as the `NULL`-label check does today), then store the text under the LVGL lock and update a label only if one exists: the status line in status mode, the bar's left slot for the daemon text while the link is down. Status mode builds its labels from the buffers. Settings reads them each second.
- A 2 s hold on Forget pairing in status mode is cut short if the first set applies during it, since the button goes with status mode. The user holds it again in settings. LVGL resets the press when a pressed widget is deleted.
- Status mode's widgets (five labels, Licenses, Forget) are created when it starts and deleted when the first set applies, so grid mode doesn't carry them in the pool.
- A board that loses its daemon in grid mode stays in grid mode. The bar's left slot shows the link status ("searching", "retry in 8 s") and settings has the full texts and Forget pairing. A board that boots paired and never connects stays in status mode, as today, with Forget on screen.
- `s_ready` replaces the `s_forget` guard (F2).

### Long swipe up

- **Trigger**: during one touch that began on `s_grid` or a cell, the net movement reaches 200 px up (`SETTINGS_SWIPE_PX`) while `|dy| > 2 × |dx|` (`DRAG_SWIPE_RATIO`, as for pages). It fires in the `PRESSING` handler, the moment it crosses, so the user sees the page before lifting. The handler calls `lv_indev_wait_release` (the release then presses nothing, F7) and opens settings through `lv_async_call`, so no widget is deleted inside its own event.
- **`LV_DIR_TOP` stops calling `lv_indev_wait_release`** in `grid_gesture`, so `PRESSING` keeps coming after LVGL's 50 px gesture (F7). The release then reaches `drag_consumed`, which already refuses to press after 20 px (F8). Left, right and down keep the wait.
- **Cells stop being scrollable.** `build_grid` clears `LV_OBJ_FLAG_SCROLLABLE` on each cell as it already does on `s_grid` (`ui.c:494`). `lv_button_create` leaves cells scrollable, and with the new boxes a one-pixel overflow of icon plus label would let a vertical drag scroll the cell instead of reaching `CLICKED` (`lv_indev.c:1570-1580`). The trigger itself still fires, since `PRESSING` is sent before the scroll handler (`:1440`, `:1450`).
- Opening settings from the `PRESSING` handler follows the pages swipe pattern that S8 proved on the board: wait for release first, then delete the grid from `lv_async_call`, so LVGL's indev reset handles the deleted active widget.
- **Why 200 px**: a third of the screen, 4× `DRAG_SWIPE_PX` and 4× LVGL's gesture distance, and about 1.8 rows of the tallest 5-row cell (103 + 8 px). A flick that turns a page, a slide that LVGL misses, and a tap all stay far below it. A slow drag counts too, since LVGL's gesture isn't needed (F9). Tuned on the board in S6, as `DRAG_SWIPE_PX` was.
- **No collisions**:
  - A page turn needs `|dx| > 2|dy|`, this needs `|dy| > 2|dx|`, and LVGL decides a fast diagonal at 50 px on the larger axis, so one touch does one or the other.
  - A press needs a touch under 20 px.
  - The result modal is a clickable full-screen backdrop on the top layer (`ui.c:182-188`), so a touch while it shows never reaches the grid. The toast isn't clickable, so it doesn't block. The license viewer covers the screen.
  - Status mode has no grid, so the swipe doesn't exist there (settings isn't reachable unpaired; status mode shows what it would).
- **Close**: a Close button; a swipe down (`LV_EVENT_GESTURE` `LV_DIR_BOTTOM` on the page, which clears `gesture_bubble` like `s_grid`, and waits for release so a swipe that starts on Forget never forgets); 60 s without a touch (`lv_display_get_inactive_time`, F10); and an unpair, which deletes it before status mode returns.

### Settings page

- Created on `lv_screen_active()` at (0, 44), 1024 × 556, when it opens, after `s_grid` is deleted. When it closes, `build_page()` rebuilds the shown page from the model. That reuses the swipe path (pages S8: 60 to 105 ms).
- **Never both at once.** Opening and closing each run from `lv_async_call`, outside any event of the widgets they delete, and delete synchronously before they build: open deletes `s_grid`, then creates the page; close deletes the page, then builds the grid. A `lv_obj_delete_async` followed by a build in the same callback would hold both in the pool for a moment, which is the peak this design avoids.
- On the active screen, not the top layer, so the toast and the result modal show over it (F6). The license viewer, on the top layer, covers it.
- Not scrollable: every row fits 556 px. Two columns of rows in Montserrat 24 (27 px a line), then a row of buttons.

| Left: this device | Right: connections |
|---|---|
| Firmware version and IDF version (`esp_app_get_description()`) | Wi-Fi status text, SSID (`wifi_ssid()`), RSSI in dBm (cached, with its age) |
| Device id (`wd-` and the MAC, from the link) | Daemon status text and the daemon's name |
| Uptime (`esp_timer_get_time`) | SD, USB and Touch texts |
| Internal heap free and minimum, PSRAM free and minimum | Layout `rev`, page x of y |
| LVGL pool used of total, largest free block | |
| Icons cached and their bytes (a new `icons_usage()`, under the model lock) | |

- Buttons: Licenses (opens the existing viewer), Forget pairing (the same 2 s hold and toast), Close.
- An `lv_timer` created with the page and deleted with it refreshes every row once a second. It runs in the LVGL task, takes the model lock after the LVGL lock (the pages lock order), and calls nothing that blocks.
- **RSSI** is read off the LVGL task (F19). The link task refreshes a cached value at most every 5 s while settings is open, from its loop that wakes every second (F18). `wifi.c` keeps the value and its time. The page shows `RSSI -61 dBm` or `RSSI --` with no fresh value.
- **While open**: `ui_show_set` stores nothing new (the model already has the set) and marks the page to rebuild on close. `ui_set_icon` and `ui_set_tile` find no buttons, which is fine since `build_page` reads both from the model (`ui.c:603-619`). `ui_link_down` clears `s_online`, and the rebuild dims the page. `ui_set_paired(false)` deletes settings first.

### Bar

- A container at (0, 0), 1024 × 36, created when grid mode starts and deleted when status mode returns. Black, with grey Montserrat 24 text at y 4 to 31. The grid starts 8 px under it (`GRID_TOP` = `BAR_H` 36 + `GRID_GAP` 8 = 44).
- Slots, left to right, each a label of fixed width that ends in dots:
  - the daemon's name from `welcome` while up, else the link status text
  - `CPU 12%`
  - `MEM 9.8/31.1 GiB`
  - `▼ 1.2 MB/s ▲ 340 kB/s` (with `LV_SYMBOL_DOWN`/`UP`)
  - `Wed 22 Oct 14:32` on the right
- Slot widths come from Montserrat 24's advance widths for the worst cases (`CPU 100%`, `MEM 999.9/999.9 GiB`, `▼ 999.9 MB/s ▲ 999.9 MB/s`, `Wed 30 Sep 23:59`). At creation the bar measures those strings with `lv_text_get_size` and logs a warning for any slot they overflow, so a font or format change can't silently clip.
- One `lv_timer` of 1 s, owned by the bar, ticks the clock and blanks stale host values. A `stats` frame updates the labels at once.
- `on_stats` stores every frame in `ui.c` whether or not the bar exists. The daemon sends `stats` from the first `select!` pass after `welcome`, which can be while a set is still arriving and the board is in status mode. The bar reads the stored values when it is created.
- **Clock**: each frame stores `time + utc_offset` and `esp_timer_get_time()`. The shown time is that plus the elapsed time. The frames correct it every second while up, and the crystal holds it well enough between them for an offline evening. 24-hour, as `Wed 22 Oct 14:32`.
- **Formatting** lives in a new `firmware/main/hoststat.c/.h` with no LVGL (rates in bytes per second with SI prefixes, memory in GiB, the clock from `gmtime_r` with its own day and month names), so `fw:test` covers it, like `pageset.c`.

### Geometry

`GRID_TOP` goes from 204 to 44 in all three mirrors. The table, from the same arithmetic (`grid.c:5-22`) with 556 px full and 525 px paged:

| Rows | Cell height | Label lines | Icon box | Paged cell height | Paged lines | Paged box | Today (full / paged) |
|---|---|---|---|---|---|---|---|
| 1 | 548 | 2 | 256 | 517 | 2 | 256 | 256 / 256 |
| 2 | 270 | 2 | 190 | 254 | 2 | 174 | 110 / 94 |
| 3 | 177 | 2 | 97 | 167 | 2 | 87 | 44 / 33 |
| 4 | 131 | 2 | 51 | 123 | 2 | 43 | 46 / 38 |
| 5 | 103 | 1 | 58 | 97 | 1 | 52 | 26 / 20 |

- Width caps don't change (1000, 492, 322, 238, 187, 153, 129, 111 px for 1 to 8 columns), so a single row of 4 or more columns is still bound by width.
- **Why 44 and not 40.** At `GRID_TOP` 40 a full 5-row cell is 104 px, which crosses the two-line threshold (`room >= 96`), so the label gets two lines and the icon drops to 24 px, smaller than today's 26. 44 keeps one line and gives 58 px. 48 works too but gives every row less.
- Every slot still fits every cell (the `test_grid.c` sums pass for all 80 grids at 44).
- **Icon cache bound**: the worst set becomes 4 pages of 5×2 at 174 px, 4,844,160 bytes (4.62 MiB), under the 8 MiB cap. `ICONS_MAX` stays 160. Transfer of that worst set takes about 11 s at the measured rate (pages S5: about 450 KB/s), within the per-frame 5 s timeout.
- Every icon id changes once with the new boxes, so the board fetches every icon on its first connect after the change.

### Wire

- **Capability `status_bar`**: the board names it in `hello.caps` beside `fonts` and `pages`. It means only "reads `stats`". It stays because it costs one constant on each side and keeps the frame away from sessions that don't read it: today's firmware would log every frame at WARN (F16), and the 65 strict-order `device_sim` tests would each need to skip it (F17). `v`, TXT `v` and the pairing label don't change.
- **`stats` (daemon → device)**, once a second, only to a session whose hello names `status_bar`, and only after `welcome`:

  ```json
  {"t":"stats","time":1792692433,"utc_offset":-14400,"cpu":12,"mem_used":10522394624,"mem_total":33395638272,"net_rx":125000,"net_tx":34000}
  ```

| Field | Type | Meaning |
|---|---|---|
| `time` | int | Required. The daemon host's clock when the frame was sent, Unix seconds, 0 to 2^53 - 1 |
| `utc_offset` | int | Optional. The host's local offset from UTC in seconds, -64800 to 64800. Absent: the device shows UTC |
| `cpu` | int | Optional. Busy share of all CPUs since the last frame, 0 to 100. Absent on the first frame |
| `mem_used`, `mem_total` | int | Optional, both or neither. Bytes, used = total minus available. `mem_used <= mem_total` |
| `net_rx`, `net_tx` | int | Optional, both or neither. Bytes per second received and sent on the host's default-route interface since the last frame |

- The device refuses a `stats` whose fields are out of range or of the wrong type, as it does any malformed message: logged and dropped. Unknown fields are ignored. The daemon never sends `stats` between the pages of a set, since a set goes out from one `select!` branch.
- The board logs `stats` at debug level, not with the INFO `rx:` line (F15).

### Daemon

- **`hoststats.rs`** (new): pure parsers for `/proc/stat` (the `cpu` line), `/proc/meminfo` (`MemTotal`, `MemAvailable`), `/proc/net/route` (the default route's interface: destination `00000000`, flag `RTF_UP`, lowest metric) and `/proc/net/dev` (that interface's byte counters). A `Sampler` keeps the previous counters and turns them into rates. A counter that went backwards (a reset or a new interface) gives no rate for that frame.
- `spawn(polling)` runs a 1 s `tokio::time::interval` only while `connected > 0` (F12), and publishes `Option<HostStats>` (CPU, memory and rates only) on a `watch` channel. It publishes `None` when `connected` returns to 0, so a session that opens later never gets a reading from an earlier period. It reads the files with `std::fs`. Each read is a few hundred bytes, so it doesn't need `spawn_blocking`. The `polling` receiver is a clone of the one `tiles::spawn` takes (`server.rs:136-137`).
- **Time**: stamped by the session at send, not by the sampler, with `time::OffsetDateTime::now_utc()` and `UtcOffset::local_offset_at`, so a frame never carries an old clock. `Cargo.toml` names the `local-offset` feature on `time` directly, rather than relying on unification (F21). An error leaves `utc_offset` out and logs once.
- **Sessions**: `CAPS` gains `status_bar`. `serve` adds a `select!` branch on `stats.changed()`, guarded by the cap, that sends a frame for each new `Some` value. It sends nothing at session start, and waits for the next sample (at most 1 s).
- **Icons**: `box_px` uses the new `GRID_TOP` (S2), so `Icons::load` needs no change.

### Config UI

- `preview.ts` and `GridEditor.tsx` show the new geometry: a taller stage (556 px at scale 1) and bigger icon boxes. The preview doesn't draw the bar.
- `viewKey` and `layoutTargets` don't change (F22), and nor does the Devices tab.

## Rejected alternatives

| Option | Why not |
|---|---|
| Keep the status lines and shrink them to one line | Still about 40 px of rarely read text on every screen, and the buttons under them still take space. The user asked for the space back |
| Settings built at boot and hidden | The user asked that it load only when shown. It would sit in the pool all the time |
| Settings as a separate LVGL screen (`lv_screen_load`) | The grid's widgets stay alive on the old screen, so it saves no pool. The overlay on the active screen does the same job and keeps the toast and modal above it |
| Keep the grid alive under settings | A full 8×5 page with a result screen already peaks at 52,196 of about 59,800 bytes. Settings on top would leave about 2 KB. Deleting and rebuilding costs 60 to 105 ms on close |
| Settings on the top layer | Created after the toast and the result modal, it would cover them, and Forget's "hold for 2 s" toast would be hidden (F6) |
| LVGL's `LV_DIR_TOP` gesture as the trigger | Fires at 50 px, so any upward flick would open settings. Not "long" |
| Swipe down from the bar | The user asked for a swipe up |
| Settings reachable while unpaired | Status mode already shows every status line and Licenses there. A swipe on the pairing views would need drag guards on Yes and No |
| `sysinfo` crate | A new dependency to vet for a Linux-only desktop. Three `/proc` files are about 100 lines with fixture tests |
| SNTP on the board | Needs internet access and a time zone database on the board, and would show the board's idea of time, not the host's |
| The daemon sends formatted time strings | The clock would freeze when the link drops. Formatting is a board concern, testable in `hoststat.c` |
| Stats as tiles | Tiles belong to buttons and the config. The bar is fixed chrome |
| `GRID_TOP` 40 | The 5-row label flips to two lines and icons shrink to 24 px (Geometry) |
| `stats` to every session, no capability | Every `device_sim` test that runs past a second would see it (F17), and a board between S6 and S7 would log it at WARN every second (F16) |
| RSSI read in the settings timer | An SDIO RPC on the LVGL task under the LVGL lock (F19) |

## Slices

| Slice | Kind | Depends on | Board check |
|---|---|---|---|
| S1 Wire contract | mechanical, both parsers | — | no |
| S2 Geometry to `GRID_TOP` 44 | mechanical, three mirrors | — | no (S7 shows it) |
| S3 Host stats sampler | design judgment, daemon | — | no |
| S4 Stats in sessions | mechanical, daemon | S1, S3 | via S7 |
| S5 Config UI | mechanical, app | S2 | no, window check |
| S6 Screen modes and settings | high complexity, firmware LVGL | — | yes |
| S7 Bar and stats on the board | design judgment, firmware | S1, S2, S6 | yes |
| S8 Hardware run and docs | operator plus scribe | S4, S5, S7 | yes |

**Waves**, each slice in its own worktree:

| Wave | Slices | Shared files |
|---|---|---|
| 1 | S1, S2, S3, S6 | S1 and S2 both add `RUN_TEST` lines to `test_proto.c` (a one-line merge). S2 and S6 both touch `ui.c`: S2 only where the grid and the pairing views are placed, so merge S2 first and S6 rebases onto it |
| 2 | S4, S5, S7 | None |
| 3 | S8 | — |

Between S2 and S7, the board on `main` puts its grid at y 44 under today's status lines, which overlap the top row, and once S6 lands the band above the grid is empty until S7 brings the bar. Both are expected; merge S6 and S7 close together. Every flash needs the user's yes, and nobody runs `erase_flash`.

### S1 Wire contract

- **Files**
  - `site/src/protocol.md`: the `status_bar` row in the capability table (`:199-202`), the `stats` section, a line in the session diagram (`:156-168`), and the debug-level logging note.
  - `app/src-tauri/src/daemon/protocol.rs`: `CAP_STATUS_BAR` (not yet in `CAPS`; S4 adds it). `Message::Stats { time, utc_offset, cpu, mem_used, mem_total, net_rx, net_tx }`, the optional fields skipped when `None`.
  - `app/src-tauri/tests/fixtures/stats.json` and `stats_min.json` (time only).
  - `firmware/main/proto.h`, `proto.c`: `PROTO_MSG_STATS`, `proto_stats_t` with `has_` flags, `parse_stats`.
  - `firmware/test/host_proto/main/test_proto.c`: new cases.
  - `link.c` doesn't name the cap yet.
- **Host cases**: full; time only; `cpu` 101 and -1; `time` as a string, negative, and 2^53; `utc_offset` 64801; `mem_used` without `mem_total`; `mem_used > mem_total`; `net_rx` without `net_tx`; a fraction in an int field; unknown fields ignored; the hello with `{"fonts","pages","status_bar"}` and a 32-character `fw` fits 256 bytes (219).
- **Acceptance**: both fixtures round-trip on the daemon. The board accepts both and refuses each malformed case. Existing tests pass unchanged. clippy and CI green.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml protocol` and `mise run fw:test`.
- **Owner**: a mechanical worker who knows both parsers.

### S2 Geometry to `GRID_TOP` 44

- **Files**: `firmware/main/grid.h` (`BAR_H` 36, `GRID_TOP` 44), `ui.c` (the pairing views move to their own `PAIR_TOP` 204, F4), `icons.h:15` (the bound in the comment), `test_grid.c`; `icons.rs` (`GRID_TOP`, the comments at `:156-160` and `:190-195`, the table and bound tests); `preview.ts`, `preview.test.ts`, the numbers in the `edit.ts:237-240` comment; `site/src/config.md` (`:134`, `:172-173`).
- **Tests**
  - The table above, both `paged` values, row for row, on all three sides.
  - The slot-fit sums for all 80 grids.
  - The cache bound at most 8 MiB, worst 4,844,160 (4 pages of 5×2 at 174 px), printed.
  - `BAR_H + GRID_GAP == GRID_TOP`.
- **Acceptance**: `mise run fw:test`, `cargo test`, `pnpm test` and rumdl pass. `mise run fw:build` logs `icon box 97 px` for a 5×3 grid.
- **Cheapest check**: `mise run fw:test`, `cargo test --manifest-path app/src-tauri/Cargo.toml icons::tests`, `pnpm test` in `app/`.
- **Owner**: mechanical, three languages.

### S3 Host stats sampler

- **Files**: `app/src-tauri/src/daemon/hoststats.rs` (new), `daemon/mod.rs`, `Cargo.toml` (`time` with `local-offset` named), `tests/fixtures/proc/` (captured `stat`, `meminfo`, `net/route`, `net/dev` samples, including one with Docker bridges and veths and one with no default route).
- **Changes**: the parsers, `Sampler` with a root path (so tests point it at fixtures), and `spawn(polling) -> watch::Receiver<Option<HostStats>>`. Not wired to sessions yet.
- **Tests**
  - CPU %: two `stat` samples with known deltas give the expected integer. Equal samples give no CPU. A counter that went backwards gives none.
  - Memory: `MemTotal - MemAvailable`, in bytes. A file without `MemAvailable` gives none.
  - Default route: the lowest metric wins; a down route is skipped; no default route gives no rates.
  - Rates: bytes over elapsed seconds, rounded down. A wrapped or reset counter gives none for that frame.
  - Offset: `local_offset_at(now)` is `Ok` on this machine and in CI.
  - `spawn` with `tokio::time::pause`: nothing while `connected` is 0; one value a second after it turns 1; `None` and no more values once it is 0 again.
- **Acceptance**: `cargo test` and clippy clean. No new crate in `Cargo.lock`.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml hoststats`.
- **Owner**: design judgment, Rust.

### S4 Stats in sessions

- **Files**: `server.rs`, `protocol.rs` (`CAPS` gains `CAP_STATUS_BAR`), `tests/device_sim.rs`.
- **Changes**: `Shared` holds the stats receiver from `hoststats::spawn(polling)`. `serve` adds the `stats` branch for `status_bar` sessions, stamping `time` and `utc_offset` at send.
- **`device_sim` tests**
  - A hello with `["fonts","pages","status_bar"]` gets a `stats` within 2 s with `time` within 5 s of now, then one about every second.
  - A hello without `status_bar` gets no `stats` in 3 s.
  - A session that opens after every other session has closed doesn't get a reading from before.
- **Acceptance**: every existing `device_sim` test passes unchanged. The new tests fail on the S3 build.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml --test device_sim`.
- **Owner**: mechanical, daemon.

### S5 Config UI

- **Files**: `GridEditor.tsx` (the stage height), `app/tests/*.test.ts` if a snapshot pins the stage.
- **Acceptance**: the 5×3 preview shows 97 px icon boxes, 87 with 2 pages, on a stage 556 px tall at scale 1. Labels still break where the board breaks them.
- **Cheapest check**: `pnpm test`, `pnpm build`. The window check needs the user.
- **Owner**: mechanical, TS. Small enough to fold into S2 if one worker takes both.

### S6 Screen modes and settings

- **Files**: `firmware/main/ui.c`, `ui.h`, `link.c`, `link.h` (an `on_welcome(name)` callback, `link_device_id()`, the RSSI refresh while asked), `wifi.c`, `wifi.h` (`wifi_ssid()`, `wifi_refresh_rssi()`, `wifi_rssi(int *dbm, uint32_t *age_ms)`), `icons.c`, `icons.h` (`icons_usage`), `main.c` (the new callback).
- **Changes**
  - Status texts kept in `ui.c` buffers (F3). Status mode built from them and torn down when the first set applies. `s_ready` for the guards (F2).
  - Grid mode: no status widgets.
  - The long swipe in `touch_track`'s `PRESSING` path, `LV_DIR_TOP` without the wait (F7), cells not scrollable, and the settings page with its timer, its four ways to close, and the rebuild on close.
- **Acceptance (board, against the current daemon)**
  - Boot paired: status mode, then grid mode when the set applies, with no status lines left.
  - Unplug the desktop's network and reboot the board: status mode stays, and Forget pairing works from it.
  - 20 long swipes up from the bottom two rows of 5×3 and 8×5 grids, half fast and half slow: each opens settings, and neither log shows a `press` or a page turn.
  - 20 upward flicks of 50 to 150 px, 20 page-turn flicks, 20 taps: no settings. Flicks press nothing, taps press.
  - With a result modal up, a long swipe up opens nothing, and the modal behaves as today (a drag on its card scrolls the output).
  - Settings closes by Close, by a swipe down that starts on Forget pairing (no forget), and after 60 s idle. A 2 s hold on Forget unpairs and shows status mode with the pairing list.
  - A layout push while settings is open: closing shows the new layout. A link drop while it is open: closing shows the page dimmed.
  - Memory: the `LVGL heap` line at "page built" before opening and after the 10th close are within 200 bytes, and the largest free block doesn't shrink by more than that. Each open logs the pool in use with settings up.
  - RSSI appears within 6 s of opening. The refresh logs its duration once at debug level; if it is over 20 ms, it moves to its own timer task (record the number).
- **Cheapest check**: `mise run fw:build`, then the board with `mise run fw:monitor`.
- **Owner**: high complexity, firmware LVGL. Needs the user for every flash.

### S7 Bar and stats on the board

- **Files**: `firmware/main/ui.c` (the bar, the clock), `hoststat.c/.h` (new), `link.c` (dispatch `stats` to an `on_stats` callback, log it at debug, hello names `status_bar` in the same commit), `link.h`, `main.c`, `CMakeLists.txt`, the host test build and `test_hoststat.c` (new).
- **Host cases** (`hoststat`): rates 0, 999, 1000, 999,999, 1,000,000, 12,345,678 and 2.5 GB/s; memory 9.8 of 31.1 GiB and a total over 1 TiB; the clock at 0, at a leap day, a negative offset across midnight, and a time after 2038; the format `Wed 22 Oct 14:32`.
- **Acceptance (board, against the S4 daemon)**
  - The hello names `{"fonts","pages","status_bar"}`. The layout gives `icon box 97 px` on 5×3 and the icons fill it.
  - The rate arrows draw as glyphs, not the placeholder box `CONFIG_LV_USE_FONT_PLACEHOLDER` would show for a missing one.
  - The bar shows the host name, CPU, memory and rates that agree with `top` and `ip -s link` on the desktop within a few percent, and the time and date matching the desktop's clock.
  - Stop the daemon: within 6 s the host values go to `--`, the left slot shows the link status, and the clock keeps the right time for 5 minutes.
  - No `bar slot overflow` warning at creation. The serial log has no `rx:` line per second.
- **Cheapest check**: `mise run fw:test`, `mise run fw:build`, then the board.
- **Owner**: design judgment, firmware. Needs the user for every flash.

### S8 Hardware run and docs

Run on the board with the dev daemon, both built from the same commit. Every flash needs the user's yes. The pairing survives all flashes, since NVS doesn't change.

1. **The whole flow**, a 3-page 5×3 config with icons and tiles: grid mode with the bar, page swipes as in pages S8, the long swipe up, settings and its closes, and Forget pairing from settings, then pair again.
2. **Memory** with the pages fixtures (4 pages of 8×5 with 160 icons and 40 tiles; 4 pages of 4×1): settled pool in use, largest free block and minimum internal heap, compared with pages S8 (49,072; 10.5 KB; 273,816). The pool may differ by the bar and status-widget delta; the internal minimum stays within 10 KB. Ten settings open and close rounds with no drift. PSRAM free after the worst icon set (4 pages of 5×2, expected about 4.6 MiB of icons), and how long it took to arrive.
3. **Page switch time** with the bigger icons: record against pages S8's 60 to 105 ms. Over 150 ms is a finding.
4. **Boot without Wi-Fi** (wrong `wifi.txt` copied via USB MSC, then restored): status mode with the Wi-Fi line, and Forget reachable.
5. **Clock**: compare with the desktop at a minute boundary. Change the desktop's time zone and restart the daemon: the board follows within 2 s.

- **Docs**: `site/src/protocol.md` finished; `site/src/config.md` (the bar, the long swipe, where Licenses lives now, `:212` and `:218`); `site/src/wifi.md` (the status lines show until the first layout and while unpaired, then in settings); `README.md:83` (Forget pairing is in settings: swipe up); `CLAUDE.md` (Hardware facts: the screen modes, the long swipe, the settings page deleting the grid; Daemon facts: `status_bar`, `stats` once a second while a session is open, `/proc` sources, the local offset); a results section in this plan.
- **Acceptance**: every step passes or its failure is recorded with a decision.

## Risks

| Risk | Mitigation |
|---|---|
| The long swipe opens settings by accident, or a deliberate one presses a button | 200 px and the 2:1 ratio, fired from `PRESSING` with a wait for release. S6 runs 60 negative touches and 20 positive ones on the board |
| Opening settings while the grid holds the pool runs LVGL out of memory | The grid is deleted first. S6 and S8 measure the pool with settings up |
| A rebuild on close is slow on the 40-tile page | Pages S8 measured 95 to 105 ms for it. S8 step 3 records it with the bigger icons |
| The board, the daemon and the preview disagree on the new table | One table pinned on all three sides in S2 |
| A board or daemon from before this plan meets one from after it | Unsupported by decision. Old firmware gets icons bigger than its boxes. Flash both from the same commit |
| The bigger worst-case icon set (4.62 MiB) takes longer to arrive | About 11 s at the measured rate, within the per-frame timeout. S8 step 2 records it |
| RSSI RPC stalls a task | Off the LVGL task, refreshed at most every 5 s and only while settings is open. S6 measures it |
| `stats` floods the serial log | Logged at debug (F15). S7 checks the log |
| `local_offset_at` fails on some desktop | `utc_offset` is optional and the board shows UTC. S3 tests it, and the log says so once |
| A daemon that sends broken `stats` | The board drops each one with a log line and shows `--` after 5 s. Nothing else depends on it. One that arrived between the pages of a set would break the set and reconnect (`link.c:1130-1136`), but the daemon sends a set from one `select!` branch, so `stats` can't land there. Keep `stats` inside `proto_parse`; don't route around that rule |
| Between S2 and S7 the board on `main` looks wrong (status lines over the top row, then an empty band) | Merge S2, S6 and S7 close together. Each is still revertable on its own |
| Docs still send people to a Forget button on the main screen | S8 rewrites `README.md:83` and `config.md:218` |

**Rollback.**

- Each slice is its own commit, so `git revert` undoes one.
- The firmware rolls back by flashing the previous build, together with the daemon from the same commit. The partition table and NVS don't change, so the pairing survives.
- The riskiest slice is S6 (the `ui.c` restructure). Its rollback is a revert plus a flash of the build before S6. S7 reverts with it, since it depends on S6.

## Open questions

None. The user settled every question on 2026-10-22 (Decisions).

## Critique log

No outside review. The consult tool refused both reviewers, as it did for the pages revision: the codex app server isn't locked down (`web_search` and `features.plugins` unset), and the agy reviewer's hook blocked its own `read_file`. So this is a self-review that argued against each slice and the approach, with each point checked against `aad6e3f`.

| # | Point | Outcome |
|---|---|---|
| R1 | Close as `lv_obj_delete_async(settings)` then `build_page()` in the same callback holds the page and the grid in the pool at once, the peak the design exists to avoid | adopted: open and close run from `lv_async_call` and delete synchronously before building (Settings page) |
| R2 | The sampler's `watch` keeps its last value after sessions drop to 0, so a later session could send hours-old numbers and an old `time` | adopted: the sampler publishes `None` at 0 sessions, and the session stamps `time` and `utc_offset` at send |
| R3 | Cells are scrollable by default (`ui.c:526-539` never clears it). With the new boxes, a one-pixel overflow turns a vertical drag into a cell scroll, so `CLICKED` never comes | adopted: cells clear `LV_OBJ_FLAG_SCROLLABLE`. The trigger itself survives, since `PRESSING` precedes the scroll handler (`lv_indev.c:1440-1450`) |
| R4 | Deleting the grid from a `PRESSING` handler of one of its cells | rejected as a defect: wait for release, then `lv_async_call`, is the path `swipe_page` uses (`ui.c:466-475`, `:623-640`), proven in pages S8 |
| R5 | Setters called before `ui_init` succeeds would lock a port that may not exist | adopted: they stay no-ops until `s_ready`, as the `NULL`-label check makes them today (`ui.c:930-938`) |
| R6 | The first set arriving mid-hold on status mode's Forget cuts the hold | adopted as documented behavior (Screen modes). Rare, and the hold works again from settings |
| R7 | The S6 acceptance said a long swipe closes a result modal. A drag on the card scrolls it and sends no `CLICKED` | adopted: the case now says the swipe opens nothing and the modal behaves as today |
| R8 | The Montserrat 24 build may lack the arrow glyphs | adopted as an S7 board check. `LV_SYMBOL_UP`/`DOWN` are in LVGL's built-in symbol range (`lv_symbol_def.h:160-164`), and a missing glyph would show the placeholder box (`sdkconfig.defaults`, `CONFIG_LV_USE_FONT_PLACEHOLDER=y`) |
| R9 | Stripping icons for old firmware gives a worse old-board experience than today's | superseded by the user's decision: no old-firmware support at all |
| R10 | S6 then S7 leaves `main` with an empty band, and S6's swipe acceptance runs on its own build | rejected as a blocker: merging them makes one slice too big for one coder. S8 step 1 reruns the swipe with the bar |
| R11 | Once geometry is unconditional, the capability may not earn its place | kept: it gates `stats` from sessions that don't read it, which spares the 65 strict-order `device_sim` tests (F17) and the WARN line on a board between S6 and S7 (F16), for one constant per side |
| R12 | The rollback text named the wrong build | adopted: "the build before S6" |
| R13 | Against the trigger: a "long swipe up" might be expected from the bottom edge only | settled by the user: 200 px anywhere on the grid |
