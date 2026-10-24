# Settings: tabs for status, Wi-Fi, storage, logs and errors

Part of [`00-index.md`](00-index.md). Findings F1 to F44 are there. Needs G1 to G3 from [`01-groundwork.md`](01-groundwork.md).

Today settings is one page: device rows on the left, connection rows on the right, and Licenses, Forget pairing and Close along the bottom (F16). It is reachable only from grid mode, by the long swipe up (F15, F18). After this plan:

- Settings has a row of tabs under the bar: **Status, Wi-Fi, Storage, Logs, Errors**, with Close at the right end of the row.
- Each tab is built when shown and deleted when left. The page as a whole is still built on open and deleted on close (device-ui).
- Status mode gets a **Settings** button, so a board that can't join Wi-Fi or reach its daemon can be fixed from the screen.
- The Wi-Fi tab edits `wifi.txt` on the screen: scan or type, enter the password on an on-screen keyboard, try it, and save it.

## Decisions

### Tabs on top

The row sits at y 44 to 108, directly under the bar, and Close moves into it. Reasons, from the theme and the board:

- **The chrome is already on top.** In grid mode the bar is the top 36 px. Tabs under it read as one header band, with content below, which is the layout every other screen here uses.
- **The finger is already there.** Settings opens with a 200 px swipe up, so the finger ends in the upper half of the screen, next to the tabs.
- **The bottom is taken.** The Wi-Fi tab's keyboard needs the bottom 264 px. Bottom tabs would sit under it or push it up into the fields. The Status tab's Licenses and Forget pairing also stay at the bottom of their tab, where device-ui put them.
- **On a desk, the bottom is nearer the hand**, which is the one argument for bottom tabs. A 10.1" panel on a desk is reached at arm's length either way, and at 64 px (about 14 mm) the targets are large enough anywhere.
- **The close gesture doesn't decide it.** A swipe down closes from anywhere on the page except a scrolling area, so neither position blocks it.

### Lazy tabs

Each tab has `build(parent)`, `tick()` (once a second) and `leave()`. A switch runs from `lv_async_call`, deletes the current tab's content synchronously, then builds the next one, so two tabs are never in the pool together (the same rule as the grid and settings, F17). The last tab shown is remembered until reboot.

### Not `lv_tabview`

`lv_tabview` builds every tab's container at once and switches by scrolling. That holds all tabs in the pool and turns horizontal drags into scrolls that fight the swipe gestures. A button row plus one content object does the same job.

### Settings in status mode

Device-ui rejected this because status mode then showed everything settings did (F18). That's no longer true: Wi-Fi, logs and errors are most needed exactly when the board can't get past status mode. A Settings button (220 x 34) goes at the right of the Wi-Fi line, like Licenses on the USB line. It is hidden while a pairing code or the "Paired?" question is on screen, so the pairing views are never covered mid-attempt. In status mode the page uses the same geometry (from y 44), and the band above shows "Settings" in place of the bar. Closing rebuilds status mode and, if unpaired, the daemon list.

### Wi-Fi: try now, save on success

- **Wi-Fi always starts** (F43). Today `wifi_start` returns before `esp_netif_create_default_wifi_sta`, `esp_wifi_init`, the retry timer and the event handlers when there are no credentials or the Kconfig ones are too long (`wifi.c:150-165`), so a board with no `wifi.txt`, the case this editor matters most for, couldn't scan or connect. T3 splits it: `wifi_init()` always brings up the netif, mDNS, the driver, the timer and the handlers and starts STA mode with no config; `wifi_apply(ssid, pass)` sets a config and connects. Boot calls `wifi_init`, then `wifi_apply` only with valid credentials; otherwise the status says "no Wi-Fi credentials: set them in Settings > Wi-Fi".
- **One owner for every connect.** A small state machine in `wifi.c` (`IDLE`, `CONNECTING`, `CONNECTED`, `BACKOFF`, `TRIAL`, `RESTORING`), run from the Wi-Fi event handler, the retry timer's callback and the requests svc posts, all serialized through one mutex. Only it calls `esp_wifi_connect` and `esp_wifi_disconnect`. Entering `TRIAL` stops the retry timer (`esp_timer_stop`, and a stale callback that fires anyway sees the state and returns); a `GOT_IP` counts for the trial only if `esp_wifi_sta_get_ap_info` names the trial's SSID; disconnect events during the trial record their reason and never arm the normal retry.
- **Connect and save** starts a trial: disconnect, set the new credentials, connect, and finish on the matching `GOT_IP`, on 20 s (a one-shot timer), or on a second "wrong password" or "not found". On success, a svc job writes `wifi.txt` (`cardfs_write_small`, G1's format) and the tab says "Saved to wifi.txt". On failure, `RESTORING` sets the old credentials and connects, the reason shows ("wrong password?", "network not found", "timed out", from G3's names) and nothing is written. If the restore itself fails to connect, the normal retry takes over with the old credentials, which is where the board was before the trial. With no old credentials, it goes back to `IDLE`.
- **Save without connecting** writes `wifi.txt` without a trial, for a network that is out of range now. It asks first and says it applies at the next boot.
- If the card is shared or missing, a successful trial still connects, and the tab says "Connected, but not saved: the card is shared over USB" (or missing). Nothing is queued: a reboot goes back to the old file.
- **RSSI moves to svc.** `link.c:1376` reads RSSI on the link task today; a scan's RPCs through ESP-Hosted would block it there. The 1 s RSSI read becomes a svc job posted by a timer, skipped while a scan runs, and the link task only reads the cached value (`wifi_rssi`, already lock-free).
- Why not "save, then reboot": a typo then costs a trip back through status mode, and the board shows nothing until it boots. The trial costs the state machine, which the split needs anyway.

### The keyboard

- `lv_keyboard` is one buttonmatrix, not a widget per key (a few KB of pool, measured in T3). Its keys are drawn in Montserrat 24, which is built in and has printable ASCII and the `LV_SYMBOL_*` glyphs the keyboard uses (backspace, OK, the arrows, the keyboard symbol). No new font and no flash cost.
- LVGL's default maps lack `~`, `^`, `|` and the backtick, which a password can hold. T3 replaces the special-character map. The maps live in a plain C file (`kbmap.c`) so a host test checks that the three modes together hold all 95 printable characters.
- The SSID field and the password field are `lv_textarea`s in Montserrat 32. The password is in password mode with a Show toggle.
- A scanned SSID that isn't printable ASCII shows with `?` but is kept as its raw bytes, which is what `wifi.txt` and the driver need.

### Logs: pages of lines, not a scrolling label

- The tab shows 15 lines at a time, one `LONG_CLIP` label each, in Montserrat 20, with Newest, Older and Newer, and an "Errors only" switch (levels E and W). On the newest page it follows new lines once a second unless paused.
- A tap on a line opens it whole (wrapped) in a modal, with its level and time since boot.
- Each label points at a PSRAM copy with `lv_label_set_text_static`, as the license viewer does with flash text, so log text costs the pool nothing.
- Why pages: a 16 KB label would scroll slowly (LVGL measures lines from the top on every draw) and a scrolling area swallows the swipe down that closes settings.

### Errors

From the top: the pending crash or reset report (F40: kind, reason, task, pc, cause), the reset reason of this boot, the counters with their last reason (Wi-Fi drops, link drops, broken sets, icon timeouts, card errors, from G3's `diag.c`), and the last 12 E and W lines from the ring, each opening whole on a tap.

## Goal and done

- A long swipe up in grid mode, or Settings in status mode, opens the page on its last tab. The five tabs and Close are in the top row, 64 px tall. Close, a swipe down outside a scrolling area, 60 s idle (not while the license viewer or the keyboard is up), and an unpair close it.
- Opening and closing settings 10 times while visiting every tab moves the pool at "page built" by under 200 bytes, and the largest free block by no more.
- The Status tab shows today's rows, Licenses and Forget pairing (the same 2 s hold).
- The Wi-Fi tab scans in under 5 s, lists up to 15 networks with their signal, takes a typed SSID, and connects and saves as above. The written `wifi.txt` reads back at the next boot as `source: sd`.
- The Storage tab shows the card, its owner, `wifi.txt` and the sleep files, shares the card, and takes it back by a restart (G2).
- Logs shows the log since boot, and Errors the reports and counters. No token or Wi-Fi password is on screen.

## Slices

### T1 UI modes and settings shell

- **Files**: `firmware/main/ui.c` (modes, the status-mode button, hooks), `ui_priv.h` (new: the helpers settings, sleep and the feed page share: `set_label`, `log_heap`, the mode switch, the chrome fonts), `settings.c/.h` (new: the page, the tab row, the tab table, the switch, the idle close), `tab_status.c` (new: today's rows and buttons, moved out of `ui.c`), `CMakeLists.txt`.
- **Changes**:
  - A mode enum in `ui.c`: `MODE_STATUS`, `MODE_GRID`, `MODE_SETTINGS`, and room for `MODE_SLEEP` (P1) and the feed sub-state of grid mode (N3). `settings_open(from)` and `settings_close()` remember where settings came from and rebuild that mode.
  - Settings moves out of `ui.c` whole, with no behavior change beyond the tab row: Close moves to the row, Licenses and Forget pairing stay at the bottom of the Status tab.
  - The tab table: `{name, build, tick, leave}` for Status only. T2, T3 and T4 each add one row.
  - The Settings button in status mode, hidden during a pairing attempt.
- **Acceptance (board, batch A)**:
  - Device-ui S6's settings cases still pass: open from 5x3 and 8x5 grids, closes by Close, by a swipe down that starts on Forget pairing (no forget), and by 60 s idle; a layout push while open shows on close; a link drop while open shows the page dimmed on close.
  - Settings from status mode, paired and unpaired: open, close, and the status lines and daemon list come back. During a pairing code the button is hidden.
  - Memory: 10 open and close rounds within 200 bytes at "page built"; the pool with settings open is logged.
- **Cheapest check**: `mise run fw:build`, then batch A.
- **Owner**: high complexity, firmware LVGL. Merge first in wave 1, since P1, N3 and T2 to T4 hook into it.

### T2 Logs and Errors tabs

- **Files**: `tab_logs.c`, `tab_errors.c` (new), the tab table, `settings.c` (the line modal, shared by both).
- **Changes**: as in Decisions. Reads the ring through G3's paging call, copying a page into a PSRAM buffer the tab owns. Errors reads `crash_pending()`, `esp_reset_reason()` and `diag.c`.
- **Acceptance (batch A)**:
  - The boot's first lines (`watchdeck starting: chip rev ...`) are on the oldest page.
  - With the daemon stopped, Errors shows the link-drop count rising and the last reason. With `WATCHDECK_TEST_CRASH`, the next boot shows the pending crash report until the daemon acks it.
  - Paging while lines arrive keeps the page's lines (sequence numbers). "Errors only" shows only E and W.
  - A tap opens a long line whole.
  - Pool with the Logs tab up is logged; the line labels' text isn't in it (compare with and without the text: within a few hundred bytes).
- **Cheapest check**: `mise run fw:build`, then batch A.
- **Owner**: design judgment, firmware LVGL.

### T3 Wi-Fi tab and editor

- **Files**: `tab_wifi.c`, `kbmap.c/.h`, `wifistate.c/.h` (new, pure: the connect state machine's transitions as a function of state and event, host-tested); `wifi.c/.h` (`wifi_init`, `wifi_apply`, `wifi_scan`, `wifi_try`, the machine's glue, the current password kept in a static for the restore and handed to `logcap`); `link.c` (the RSSI read leaves the link task); `main.c` (`wifi_init` then `wifi_apply`); the tab table; host `test_kbmap.c`, `test_wifistate.c`.
- **Changes**:
  - The split and the state machine (Decisions).
  - The tab: the Wi-Fi status text, SSID, IP, RSSI, the credential source (`sd`, `config` or `none`), whether `wifi.txt` exists, and Change Wi-Fi.
  - The editor replaces the tab's content: the network (a scan list, or "Other network" to type one), the password, Connect and save, Save without connecting, Cancel, and the keyboard along the bottom.
  - `wifi_scan` (svc): `esp_wifi_scan_start` blocking, up to 20 records, de-duplicated by SSID, sorted by RSSI. Refused while a trial runs.
  - The link drops and comes back by itself during a trial, as for any Wi-Fi change (F9, F10).
- **Host cases**: `kbmap`: the three modes together contain every printable ASCII character; no map has an empty or over-long label. `wifistate`: a trial from `BACKOFF` cancels the retry, and a retry tick in `TRIAL` does nothing; a `GOT_IP` for another SSID doesn't end the trial; two wrong-password disconnects end it; the 20 s timeout restores; a restore failure falls to `BACKOFF` with the old credentials; a trial with no old credentials ends in `IDLE`; Save without connecting changes no state.
- **Acceptance (batch A)**:
  - Boot with no `wifi.txt` and empty Kconfig credentials: the status says "no Wi-Fi credentials", Settings > Wi-Fi scans, and Connect and save joins and writes `wifi.txt`; the next boot says `source: sd`.
  - A scan with settings open returns in under 5 s; the RSSI row keeps refreshing outside the scan; the link task logs no frame handled more than 500 ms late during it (a temporary log of the queue's oldest age).
  - A trial started while the board is in its retry backoff (daemon side irrelevant: switch the access point off for 10 s first) ends on the new network, with no `wifi_retry` connect in the log during the trial.
  - Connect and save with the right password: connected within 20 s, `wifi.txt` written, and after a reboot the log says `source: sd` with the new SSID.
  - With a wrong password: "wrong password?" within 20 s, the board back on the old network, and `wifi.txt` unchanged (its bytes compared with the backup's).
  - Save without connecting to an absent SSID, then reboot: `connecting to <ssid> (retry: network not found)`; then fix it from Settings in status mode.
  - With the card shared: connects, says not saved, writes nothing.
  - A password with `~ ^ | \`` typed on the keyboard round-trips through `wifi.txt`.
  - Neither the screen nor the serial log shows the password at any point (search the captured log for it).
  - Pool with the editor and the keyboard up is logged.
- **Cheapest check**: `mise run fw:test`, `mise run fw:build`, then batch A.
- **Owner**: high complexity, firmware. Needs the user at the panel, and a second network or a phone hotspot for the "network not found" and recovery cases.

### T4 Storage tab

- **Files**: `tab_storage.c` (new), the tab table, `sdcard.h`/`usb.h` (read-only getters for the tab).
- **Changes**: rows for the card (name, size), the owner (`on the device`, `shared over USB`, `shared, no computer attached`, `no card`, `unreadable: <reason>`), `wifi.txt` (present, and its SSID, never the password), and the sleep files (`sleep.cfg`, the image and its size, or "none"). Buttons: Share over USB (a dialog: "The device can't save the sleep screen or Wi-Fi while the card is shared. To take it back, the board restarts"), and while shared, Stop sharing (a dialog: "Eject the drive on your computer first. The board restarts to take the card back (about 8 s); the pairing stays", then "I ejected it, restart", which reboots, G2). "Check free space" runs `f_getfree` on the svc task with "this can take 10 s on a large card".
- **Acceptance (batch A)**: G2's share and Stop sharing cases, run from this tab instead of the self-test, with the restart showing the boot view and the grid again within 15 s; the rows follow each state within a second; a Wi-Fi save while shared says not saved.
- **Cheapest check**: `mise run fw:build`, then batch A.
- **Owner**: mechanical, firmware LVGL.

## Rejected alternatives

| Option | Why not |
|---|---|
| Tabs along the bottom | Under the keyboard, and away from the bar's header band (Decisions) |
| A side rail of tabs on the left | Takes 160 px of width from the two-column Status tab and the log lines |
| `lv_tabview` | All tabs in the pool at once, and swipe-to-switch fights the page gestures |
| Swiping sideways between tabs | Horizontal swipes turn pages on the grid; in settings they'd be a second meaning for the same gesture |
| The `wifi.txt` editor as a popup over the grid | The keyboard and the scan list need most of the screen, and the grid would stay in the pool under them |
| Save, then reboot to apply Wi-Fi | A typo locks the board out until the next trip through status mode (Decisions) |
| Scan on the link task, where RSSI is read today | Blocks the link task for seconds while frames arrive, against the 500 ms drop (F10), and waits behind up to 13 s of mDNS |
| A scrolling log label | Slow to draw at 16 KB, and it swallows the close gesture |
| Settings unreachable from status mode (device-ui) | The new tabs matter most when the board can't leave status mode (F18) |
| Showing the Wi-Fi password in Storage or Logs | It's on the card in clear already, but the screen is visible to anyone in the room |

## Risks

| Risk | Mitigation |
|---|---|
| T1's move of settings out of `ui.c` breaks a close path | T1's acceptance reruns device-ui S6's settings cases |
| Scans or trials through ESP-Hosted stall the link task | The RSSI read moves to svc and skips during a scan; T3 measures the link queue's age during a scan |
| The retry timer and the trial both call connect | One state machine owns every connect and stops the timer on entering a trial; `test_wifistate.c` and T3's backoff case |
| A trial leaves the board on no network | It always restores the old config on failure or timeout, and a failed restore falls back to the normal retry. T3 tests the wrong-password case |
| The `wifi_init`/`wifi_apply` split changes boot behavior | Boot with valid credentials must log the same lines as today; T3's no-credentials case is new behavior on purpose |
| The keyboard's pool cost is higher than estimated | Measured in T3 with the editor up. The budget with the grid deleted is about 40 KB |
| Opening settings in status mode covers a pairing attempt | The button is hidden during one (T1 test) |
| A long log line or a crash line with no spaces clips | `LONG_CLIP` on the list, and the modal shows it whole |

## Memory estimates (pool, to be measured)

| Screen | Estimate |
|---|---|
| Settings shell (tab row, Close, header) | about 2.5 KB |
| Status tab (today's rows and buttons) | about 10 KB |
| Wi-Fi tab, rows | about 2 KB |
| Wi-Fi editor: two text areas, the keyboard, a list of 15 networks | about 11 KB |
| Logs or Errors tab: 15 line labels with static text, buttons | about 5 KB |
| Storage tab | about 3 KB |
| Worst case open: bar plus shell plus editor | about 17 KB of the 59,800, with the grid deleted |
