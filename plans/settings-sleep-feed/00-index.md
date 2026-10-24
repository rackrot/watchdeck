# Next round: settings diagnostics, sleep screen, the feed page, and a logo

Status: planned 2026-10-23 against `main` at `4d1fe96`. Open questions Q1 to Q4 decided as recommended. Wave 1 started 2026-10-23.

This round adds four things the user asked for:

1. **Settings sections** with tabs, for status, Wi-Fi (with a `wifi.txt` editor on the screen), storage, logs and errors.
2. **A sleep (privacy) screen** that shows when the link is offline or searching, or after a long swipe down. The app customizes it, and its image is stored on the TF card.
3. **A logo**, in the app (icons, header, splash) and on the device (boot screen, default sleep screen).
4. **A page at index -1** (a right swipe on page 1) that shows notifications and stats from integrations in the daemon.

The plan is one file per feature, plus groundwork that three of them share:

| File | What |
|---|---|
| [`01-groundwork.md`](01-groundwork.md) | TF card writes (G1, G2), the log ring (G3), the wire contract (G4), the daemon config schema (G5) |
| [`02-settings.md`](02-settings.md) | UI modes and the settings shell (T1), Logs and Errors (T2), Wi-Fi (T3), Storage (T4) |
| [`03-sleep.md`](03-sleep.md) | The backlight probe (P0), the sleep screen on the board (P1), the daemon side (P2), config and image on the board (P3), the app form (P4) |
| [`04-feed.md`](04-feed.md) | The daemon feed and the integration registry (N1), the push API and CLI (N2), the -1 page (N3), the app (N4), Prometheus and Grafana (N5) |
| [`05-logo.md`](05-logo.md) | Candidates (L1), the asset pipeline (L2), app branding and splash (L3), the device logo (L4) |

Each file follows `plans/device-ui.md`: decisions, goal and done, approach, rejected alternatives, slices with acceptance and the cheapest check, risks, and rollback. Findings (F1 to F44) are shared, in this file. The feed slices are N1 to N5, so a slice never reads as a finding.

## Decisions the user made

1. **TF card writes are allowed, scoped.** The firmware writes only `wifi.txt` and files under `watchdeck/`, never formats, refuses writes while the card is shared over USB, and writes atomically. The user backs up the card before the first board write test (an explicit gate in G2). CLAUDE.md changes to match.
2. **Integrations** come from three sources behind one extensible interface in the daemon: a local push API with a CLI, polled stats like tiles, and named connectors (Grafana and Prometheus first). Phasing is allowed.
3. **The logo goes on the app and the device.** 2 or 3 SVG candidates first, for the user to pick.

## Decisions this plan makes (each argued in its file)

| # | Decision | Where |
|---|---|---|
| D1 | The firmware owns the card from boot. Sharing over USB is something the user starts in Settings > Storage; Stop sharing reboots the board to take it back (runtime take-back races esp_tinyusb's callbacks, F41). While shared, writes are refused, and the device shows "no medium" to the host otherwise. This changes today's "card goes to USB at every boot" | G2, open question Q1 |
| D2 | No `rename` on the card. A small file (`wifi.txt`, `sleep.cfg`) is written as an intent record `watchdeck/<key>.new` (header with length and SHA-256, fsynced), then in place, then the record is emptied with `O_TRUNC` (never deleted, so routine writes never grow a directory). Recovery at every mount redoes a valid record and empties an invalid one, and a write recovers its own key's pending record first. Every entry creation is refused unless the directory already has room (G1 amendments, F45). Proven at every cut point by a FatFs-level host test with a RAM disk | G1 |
| D3 | Big images are write-once under content-addressed names (`watchdeck/<8hex>.img`), trusted only after a size and SHA-256 check, and deleted only after the `sleep.cfg` that stops naming them is committed | G1, P3 |
| D4 | A worker task (`svc`) runs every slow device job: card I/O, the Wi-Fi scan and the steps of a trial, the RSSI read, the share, image loads. No job waits on a network event; the trial is an event-driven state machine in `wifi.c`. Not the link task (inline mDNS for up to 13 s, a 16-slot queue that drops after 500 ms, F10) and not the LVGL task | G1, T3 |
| D5 | Settings tabs go on **top**, directly under the bar, with Close at the right end of the tab row. Each tab is built when shown and deleted when left | T1 |
| D6 | Wi-Fi always initializes, with or without credentials (F43). Changes apply now: a trial connect owned by one state machine, and `wifi.txt` is written only after the board gets an IP on the trial's SSID. On failure the old network comes back. "Save without connecting" is the escape for a network that isn't in range | T3 |
| D7 | The sleep screen is a full-screen mode. It deletes the grid, the bar and any settings page, so it hides host stats (privacy) and costs about 3 KB of pool. Every UI entry point is mode-aware: results, sets and tiles that arrive while asleep update the model and build nothing (F44). The image fills 1024 x 512 at the top, and the spinner strip is the 88 px below it | P1 |
| D8 | Two sleep reasons: offline (the link is down for `offline_after_s`, default 10 s) and manual (a long swipe down). A reconnect wakes only the offline one. Any touch wakes and is swallowed | P1, Q2 |
| D9 | Backlight dimming only if a board probe shows clean PWM on GPIO31 (P0). Otherwise on or off | P0 |
| D10 | Images travel as device-pulled chunks of 262,144 bytes (`asset_get`), each in a binary frame of exactly today's 262,168-byte cap, one request outstanding. RGB565, the panel's format | G4, P2, P3 |
| D11 | The -1 page is not a `[[page]]`. It exists on the board when the daemon has at least one integration. It changes no grid geometry, icon box or page label | N3 |
| D12 | One daemon interface for every source: a `Connector` trait (polled sources) and a push adapter (inbound requests), built from `[[integration]]` by one `match`. Prometheus is a polled HTTP query, and Grafana is its webhook contact point into the push API | N1, N5 |
| D13 | The push API listens on loopback over plain HTTP, and on the LAN only when `[push] lan = true`, then over HTTPS with the daemon's own certificate. Bearer tokens are `secrets.toml` entries, compared in constant time | N2, Q3 |
| D14 | The CLI is `watchdeck notify ...`, a subcommand of the app binary that runs before Tauri starts | N2 |
| D15 | Device logo: an A4 bitmap (or I4 if the chosen mark needs two colors), about 29 KB of flash at 240 px, generated from the SVG by a pinned tool | L2 |
| D16 | App splash: shown inside the main window while it loads, not a second window | L3 |

## Goal and done

- **Settings** (T1 to T4): a long swipe up in grid mode, or the new Settings button in status mode, opens a page with five tabs: Status, Wi-Fi, Storage, Logs, Errors. Each tab is built when shown. Opening and closing settings 10 times, visiting every tab, leaves the LVGL pool within 200 bytes. The Wi-Fi tab scans, takes a typed SSID and password on an on-screen keyboard, tries the network, and writes `wifi.txt` in the format `wifi.c` reads. Logs shows the device's log since boot, and Errors shows the crash report, the reset reason, counters and recent warnings and errors. No token or Wi-Fi password appears on screen.
- **Card** (G1, G2): writes only `wifi.txt` and `watchdeck/*`, recoverably, and only while the card isn't shared. Settings > Storage shares the card over USB, and Stop sharing restarts the board to take it back. The FatFs host test cuts power at every write of every operation and recovery leaves each file old or new; five power cuts on the board agree, and `fsck.vfat -n` on the partition shows nothing beyond the dirty flag, the free count or lost clusters.
- **Sleep** (P0 to P4): the screen shows after `offline_after_s` of lost link (with a spinner and the link status), or at once after a long swipe down (no spinner while the link is up). A touch wakes it and presses nothing. The app sets the image, the shown elements, the spinner style and color, the offline delay and the backlight level. The device stores the config and image on the card and shows them after a reboot with no daemon.
- **Feed** (N1 to N5): a right swipe on page 1 shows notifications and stat cards from `[[integration]]` sources. `watchdeck notify "Backup done"` and a Home Assistant `rest_command` both show a notification within 2 s. A polled stat, a Prometheus query and a Grafana alert each show up. A tap on a notification shows it whole with Dismiss, which removes it on every board.
- **Logo** (L1 to L4): the chosen mark is the app icon set, the tray icon, the favicon, in the header, on the in-window splash, on the board's boot screen and on the default sleep screen.
- **Contract**: two new capabilities, `sleep` and `feed`, with their frames in `site/src/protocol.md`. `cargo test`, `pnpm test` and `mise run fw:test` cover the new rules. The board runs (H1) pass with serial-log evidence.
- **Docs**: `site/src/protocol.md`, `config.md`, `wifi.md`, a new `site/src/integrations.md`, `README.md` and `CLAUDE.md`.

## Assumptions

- **Development only**, as in device-ui decision A: board and daemon come from the same commit, and old-firmware support isn't needed. New fields still go behind capabilities, as `fonts`, `pages` and `status_bar` do, so the 65+ strict-order `device_sim` tests don't see frames they don't expect (device-ui F17).
- **One desktop, Linux**, as before (`plans/desktop-app-v1.md:57`).
- **LVGL pool**: 64 KB internal, about 59,800 bytes usable. Device-ui S8 measured 19,088 used at "page built" on a 2-page 5x5 set and 16,192 with settings open; pages S8 measured 49,052 to 49,072 settled on 4 pages of 8x5 with 160 icons. Every new screen here replaces the grid, so its budget is the pool minus the bar (about 3 KB), not minus a page.
- **PSRAM**: 32 MB. Icons take at most 8 MiB. The new uses are 64 KiB of log ring, 1 MiB for a sleep image (2 MiB while a new one downloads), and about 12 KiB for the feed.
- **The card** is FAT32 in the first GPT partition (FatFs here has no exFAT and no 64-bit LBA, F6). Writes go at 20 MHz over a 1-bit bus that ESP-Hosted shares (CLAUDE.md, boot order).

## Findings

Checked against `4d1fe96` and the managed components in `firmware/managed_components/`.

| # | Where | What |
|---|---|---|
| F1 | `sdcard.c:31-52`, `:156` | The diskio is read-only by construction: `part_disk_init` returns `STA_PROTECT`, `part_disk_write` returns `RES_WRPRT`. `f_mount` with "never f_mkfs" |
| F2 | `wifi.c:158-159`, `main.c:107-108`, `usb.c:43-44` | `wifi_start` releases the FS after reading `wifi.txt`, and `usb_msc_start` hands the raw card to USB at every boot. Nothing in the firmware has the FS after boot |
| F3 | `esp_tinyusb/tinyusb_msc.c:462-554`, `:760-790`, `:1169-1191`; `usb.c:53` | `tinyusb_msc_set_storage_mount_point(APP)`, and an eject with `auto_mount_off` clear, run `msc_storage_mount`: it mounts the whole disk through its own medium and, on "no filesystem", formats it unless `do_not_format` is set (`:522-535`). The card is GPT and FatFs has `FF_LBA64 0`, so that path would format the card. `usb.c` sets `auto_mount_off` |
| F4 | `tinyusb_msc.c:1321-1336`, `:1103-1139`; `storage_blockdev.c:124-130`, `:185-204` | Test Unit Ready says "medium not present" unless a storage is mapped to USB. `tinyusb_msc_delete_storage` refuses while deferred writes are pending. The blockdev medium's `close` only forgets the handle: the caller releases the `esp_blockdev` and can open a new one |
| F5 | `tinyusb_msc.c:351-390` | USB writes are copied and deferred to the TinyUSB task, acked before they reach the card (CLAUDE.md says so too) |
| F6 | IDF `fatfs/vfs/vfs_fat.c:979-986`; `fatfs/src/ffconf.h:16`, `:266`, `:294`, `:331` | `rename` is `f_rename`, which refuses an existing destination. `FF_FS_READONLY 0` (writes are compiled in), `FF_LBA64 0`, no exFAT, `FF_FS_LOCK 0` (`sdkconfig:2937`) |
| F7 | `sdkconfig:2876-2945`; IDF `fatfs/Kconfig:301` | LFN on the heap, 4096-byte max sector, per-file cache, buffers in PSRAM. `CONFIG_FATFS_IMMEDIATE_FSYNC` exists and is off |
| F8 | `wifi.c:46-88`, `site/src/wifi.md:14-25` | `wifi.txt`: line 1 the SSID (at most 32), line 2 the password (at most 64), LF or CRLF, no quoting; no line 2 means an open network; a blank line 1 means "don't connect" |
| F9 | `wifi.c:120-139`, `:141-195` | Credentials are read once at boot. A disconnect reports its reason number and retries every 3 s. The password lives only in the driver config |
| F10 | `link.c:34`, `:186-198`, `:255`; `plans/pages.md` F28 | 16-slot queue; the websocket handler waits 500 ms for room, then drops the frame. The link task runs mDNS discovery inline for up to 13 s |
| F11 | `link.c:743-751`; `test_proto.c:773` | `hello` names `{"fonts","pages","status_bar"}` in a 256-byte buffer, pinned at 219 bytes with a 32-character `fw` |
| F12 | `link.c:751`, `:918`; `pairing.c:162-179`; `wifi.c:59` | Secrets are kept out of logs at the source: the hello is logged as `hello (token redacted)`, the token leaves only hashed (`pair_req`), pairing frames carry nothing secret, and `wifi.c` logs only lengths |
| F13 | `sdkconfig:3136-3166`; IDF `log/include/esp_log_write.h:33` | Log v1, maximum level INFO (debug is compiled out). `esp_log_set_vprintf` is available |
| F14 | `ui.c:622-634` | `grid_gesture` waits for release on every direction but `LV_DIR_TOP`. A long swipe down needs `LV_DIR_BOTTOM` exempted the same way, or `PRESSING` stops at 50 px (device-ui F7) |
| F15 | `ui.c:521-545` | `touch_track` fires the long swipe up at 200 px with `|dy| > 2|dx|`, from `PRESSING`, then waits for release and opens settings through `lv_async_call` |
| F16 | `ui.c:947-955`, `:979-1015` | Settings is one page: rows in two columns, then Licenses, Forget pairing and Close along the bottom. A swipe down anywhere on it closes it |
| F17 | `ui.c:919-937`, `:1017-1032` | Open deletes the grid, then builds; close deletes, then rebuilds with `build_page`. Never both in the pool |
| F18 | `ui.c:451-472`; `plans/device-ui.md` Rejected | Status mode has Licenses (y 72) and Forget pairing (y 114) at the right and no way into settings. Device-ui rejected settings while unpaired because status mode showed everything settings did |
| F19 | `ui.c:785-802`; `model.h:47-53` | A swipe past either end does nothing (`model_step` returns false) |
| F20 | `ui.c:636-666` | "Page x of y" exists only with 2 or more pages, as a child of `s_grid` |
| F21 | `display.c:13`, `:98-101`; Elecrow wiki | The backlight is GPIO31, set high once, into a boost EN with a 10K pull-down. The wiki lists `LCD_BK_EN(IO31)(PWM)` and `LCD_BK_POWER(IO29)`. The firmware never drives IO29 |
| F22 | generated `sdkconfig:4965`, `:5071-5075`, `:5322-5334`, `:5495-5634`, `:5700-5702` | `LV_MEM_SIZE` 65536. Keyboard, textarea, tabview, spinner, arc, list and msgbox are on. PNG, JPEG and BMP decoders, LVGL file system drivers and vector graphics (ThorVG) are off |
| F23 | `icons.c:61-93` | Icons are PSRAM pixels behind an `lv_image_dsc_t`, with no decoder, and the image cache is off |
| F24 | `protocol.md:144-145`; `proto.h:13`, `:34` | Text frames are at most 16 KB. The one binary frame, an icon, is at most 262,168 bytes |
| F25 | `server.rs:888-919` | The daemon sends every frame of an `icon_get` before it reads the next message |
| F26 | `server.rs:782-832` | Each session's `select!` has cap-gated branches (`stats` only with `status_bar`) |
| F27 | `tiles.rs:75-94`, `:280-341`, `:348-379`; `actions.rs:20-22` | The `Sample` trait, the poll loop (cooldown, `stale` after 3 skips) and `reading()` (first line or `json_pointer`, then redaction). `MAX_RUNNING` 8, of which tile polls take at most `MAX_TILE_RUNNING` 6 |
| F28 | `config.rs:75-90`, `:430-458` | `Config` refuses unknown fields. `Action` is tagged by `kind` with `deny_unknown_fields`, and names secrets in `secret_headers` |
| F29 | `secrets.rs:1-148` | `secrets.toml` is 0600, values are 1 to 4096 bytes of printable ASCII, and `Debug` redacts them |
| F30 | `Cargo.lock:1794-1865`; `Cargo.toml:34` | `hyper` 1.12, `hyper-util` 0.1.21, `http-body-util` 0.1.5 and `httparse` are in the lock through `reqwest`. Whether the server features are on is unchecked. `futures-util` is a direct dependency |
| F31 | `lib.rs:69-256`; `main.rs:4-14`; `tray.rs:14`, `:75-77` | Setup loads the identity, config and icons and binds the server before it shows the hidden window. Autostart passes `--hidden`. `main` has no CLI. The tray uses `default_window_icon` |
| F32 | `tauri.conf.json` `bundle.icon`; `index.html:5`; `App.tsx:43`; `src/assets/react.svg`, `public/vite.svg`, `public/tauri.svg` | Default Tauri icons, the Vite favicon, a text-only wordmark, and the template's placeholder images |
| F33 | `fonts/fonts.json:136-147` | Chrome fonts take their own glyph range (`bar`: 0x20-0x7E and 0xB0) and are generated only into `firmware/main/fonts/` |
| F34 | `CLAUDE.md`, Hardware facts | "Never format the card or write to it", and "Host writes apply on the next reboot" |
| F35 | `link.c:1388-1447` | The link task wakes at least once a second |
| F36 | `lvgl__lvgl/scripts/LVGLImage.py` | LVGL 9.6's image converter is in the managed component and needs `pypng`, which nothing in `mise.toml` pins |
| F37 | `actions/http.rs:203-224` | HTTP tests run a local `TcpListener`, so connector tests can serve canned replies |
| F38 | `ui.c:1241-1252`, `:1063-1093` | `ui_set_stats` keeps every frame whether or not the bar exists, and the clock runs on from the last one |
| F39 | Tauri 2 docs, "Splashscreen" and `tauri icon` | A splash is a second window closed by a two-flag handshake, and the docs call it usually unnecessary. `tauri icon` takes a square PNG or SVG |
| F40 | `crash.c:144-197` | A pending crash or reset report is kept in RAM with its kind, reason, task, pc, ra, cause and addr, until the daemon acks it |
| F41 | IDF `fatfs/src/ff.c` around 5378-5404; `tinyusb_msc.c:246-264`, `:1103-1139` | `f_rename` registers the new entry, removes the old one, then syncs, so a cut can leave two names on one chain. esp_tinyusb's sector callbacks look the storage up in a critical section, then take its lock and call the medium outside it, and `delete_storage` checks only deferred writes, so a runtime delete can race a SCSI command in flight |
| F42 | IDF `components/fatfs/CMakeLists.txt:14-20`, `components/fatfs/host_test/`; `ff.c:1131`, `:1562`; `sdkconfig` (`FATFS_DONT_TRUST_*`=0) | The `fatfs` component builds for the `linux` target and has its own host test. FatFs writes FSINFO without checking the result and refuses allocation at once on a stored free count of 0. The two `DONT_TRUST` options are dead in IDF 6.1: `ffconf.h:319` hard-codes `FF_FS_NOFSINFO 0`, so G2 resets `free_clst` and `last_clst` after mounting instead |
| F45 | `ff.c` around 1687-1703, 1795, 1844, 2352 | Growing a directory links the new cluster in the FAT before zeroing it. A cut between the two leaves stale data in the directory, and `dir_alloc` doesn't stop at the end marker, so a later create turns it into live junk entries. Found by G1's cut test; closed by the pre-check in `cardfs` (01-groundwork.md, G1 amendments) |
| F43 | `wifi.c:141-195` | `wifi_start` returns before the netif, `esp_wifi_init`, the retry timer and the event handlers when the SSID is empty or the Kconfig credentials are too long, so a board with no credentials can't scan or connect later. RSSI is read on the link task (`link.c:1376`) |
| F44 | `ui.c:27-28`, `:1254-1259`, `:1324` | Presses stay pending for up to 11 minutes and `ui_show_result` builds a toast; `ui_show_set` rebuilds the bar and the page. Both can run while a full-screen mode is up |

## Slices and waves

| Slice | Kind | Depends on | Board check |
|---|---|---|---|
| G1 Card store, `wifi.txt` codec, worker task, FatFs cut test | design judgment plus high complexity (the cut test), firmware (host-tested) | — | no |
| G2 Card ownership, read-write mount, USB handover | high complexity, firmware, **backup gate** | G1 | yes (batch A) |
| G3 Log ring and capture | design judgment, firmware (host-tested) | — | via T2 |
| G4 Wire contract: `sleep`, `asset_get`, asset frames, `feed`, `feed_dismiss` | mechanical, both parsers | — | no |
| G5 Daemon config schema: `[sleep]`, `[push]`, `[[integration]]` | schema, daemon | — | no |
| T1 UI modes and settings shell | high complexity, firmware LVGL | — | yes (batch A) |
| T2 Logs and Errors tabs | design judgment, firmware | T1, G3 | batch A |
| T3 Wi-Fi tab and editor | high complexity, firmware | T1, G1 (G2 to save on the board) | batch A |
| T4 Storage tab | mechanical, firmware | T1, G2 | batch A |
| P0 Backlight PWM probe | infra, board experiment | — | batch A |
| P1 Sleep screen on the board | high complexity, firmware LVGL | T1 (L4 for the final art) | batch A |
| P2 Daemon: sleep image and frames | design judgment, daemon | G4, G5 | via P3 |
| P3 Board: sleep config, image download, persistence | design judgment, firmware | P1, G2, G4 (board check needs P2) | batch B |
| P4 App: sleep screen form | design judgment, app | P2, G5 | window check |
| N1 Daemon feed, integration registry, polled stats | high complexity, daemon | G4, G5 | via N3 |
| V1 Vet `httpdate` (hyper's server feature) | reviewer, supply chain | — | no |
| N2 Push API and CLI | high complexity, daemon, security | N1, V1 | no |
| N3 The -1 page on the board | high complexity, firmware LVGL | T1, G4 (board check needs N1) | batch A |
| N4 App: Feed tab and integrations form | design judgment, app | N1, N2 | window check |
| N5 Prometheus and Grafana connectors | design judgment, daemon | N1, N2 | no |
| L1 Logo candidates | design, scratchpad | — | user pick |
| L2 Logo asset pipeline | mechanical, tooling | L1 picked | no |
| L3 App branding and splash | mechanical, app | L2 | window check |
| L4 Device boot logo and default sleep art | mechanical, firmware | L2, P1 | batch B |
| H1 Hardware runs and docs | operator plus scribe | everything | batches A and B |

**Waves**, each slice in its own worktree:

| Wave | Slices | Shared files to merge |
|---|---|---|
| 1 | G1, G3, G4, G5, T1, L1, V1 | G1 and G3 add sources to `firmware/main/CMakeLists.txt` and the host test build (one-line merges). G4 and G5 both touch `protocol.rs`/`config.rs` only at their ends. T1 restructures `ui.c`: merge it first in the wave so later `ui.c` slices start from it |
| 2 | G2, T2, T3, T4, P0, P1, P2, N1, N3, L2 | T2, T3, T4 each add one `tab_*.c` file and one line in the tab table (T1 made that table for this). T3 also changes `wifi.c` and `link.c` (the init split, RSSI); G2 changes `wifi.c` only to drop the release, so merge G2 first. P1 and N3 both touch `ui.c` at separate hooks (P1: `touch_track`, `grid_gesture`, the mode switch and `uimode.c`; N3: `swipe_page`); merge P1 first. P2 and N1 both add a `select!` branch in `server.rs`; merge P2 first |
| 3 | P3, P4, N2, N4, N5, L3, L4 | P3 and L4 touch `sleep.c`; merge L4 after P3. N2 and N5 both route in the push server; merge N2 first |
| 4 | H1 | — |

**Board batches** (each needs the user at the panel; every flash needs the user's yes, and nobody runs `erase_flash`):

- **Batch A**, end of wave 2: the card backup gate, then G2's write and power-cut runs, T1 to T4, P0's probe build, P1, and N3 against the N1 daemon.
- **Batch B**, end of wave 3: P3 with P2, N2 to N5 end to end, L4, and the memory runs.

Between merges, `main` may show a settings page without its later tabs, or a sleep screen without the logo (P1 uses a placeholder until L4). Both are expected.

### H1 Hardware runs and docs

Run on the board with the dev daemon, both from the same commit. Evidence is the serial log (pyserial from the IDF Python env when there's no TTY, CLAUDE.md) and the daemon log, captured into the slice's results.

- **Batch A** (after wave 2): the backup line; G2 steps 2 to 6; P0; T1 to T4's acceptance; P1's touch matrix and triggers; N3 against the N1 daemon; the stack high-water marks (G3).
- **Batch B** (after wave 3): P3's downloads and the offline reboot; N2's CLI and a `curl` from another host with `lan = true`; N5 if a Prometheus or Grafana is reachable; L4.
- **Memory** (batch B): the pages fixtures as in device-ui S8 (4 pages of 8x5 with 160 icons; 4 pages of 4x1), each new screen opened on top: settings on each tab, the Wi-Fi editor with the keyboard, the sleep screen with an image, the feed page at its limits. For each: pool in use, largest free block, internal heap minimum, PSRAM free. 10 rounds of each with no drift over 200 bytes. The internal minimum within 10 KB of pages S8's 273,816 (the svc task's stack and the log hook are the new internal costs).
- **Docs**: `site/src/protocol.md` (final), `config.md` (Sleep screen, Integrations, Push API pointers), `integrations.md` (new, N2), `wifi.md` (editing from the screen, sharing over USB from Settings > Storage), `README.md` (the logo, Settings, the feed page), `CLAUDE.md` (Hardware facts: the card rule and handover, the screen modes with sleep and the feed sub-state, the long swipe down, settings tabs and status-mode Settings, the log ring, the backlight result; Daemon facts: `sleep` and `feed`, `screens/`, the integration interface, the push API ports, tokens and limits, the CLI; Conventions: generated `logo_img.c`, the no-secrets-in-logs rule), and a results section in each feature file.
- **Acceptance**: every step passes or its failure is recorded with a decision.

## Open questions

The user took every recommended answer below on 2026-10-23. Q5 stays a gate at L1.

| # | Question | Recommended (decided) |
|---|---|---|
| Q1 | Who owns the TF card at boot? Today it goes to USB at every boot, and the firmware never sees it again | **The firmware.** The sleep image arrives on every connect and can't be saved while the card is shared. Sharing becomes a tap in Settings > Storage, and Stop sharing restarts the board (about 8 s, the pairing kept) because a runtime take-back races esp_tinyusb (F41). Pulling the card out stays the fallback. Not persisted: a reboot always starts with the firmware owning the card |
| Q2 | How does the screen wake from a manual (privacy) sleep? | **Any touch**, swallowed so it presses nothing. A deliberate gesture (a long swipe up) is safer against bumps but harder to find. It can be a later `[sleep] wake = "swipe"` |
| Q3 | Should the push API be reachable from the LAN (Home Assistant on another host)? | **Off by default.** With `[push] lan = true` it also serves HTTPS on 47823 with the daemon's self-signed certificate, which Home Assistant's `rest_command` takes with `verify_ssl: false` or the shown fingerprint. Never plain HTTP on the LAN, since the bearer token would cross it in clear |
| Q4 | Does a board that boots paired and can't reach its daemon show the sleep screen after 10 s? Device-ui decided it stays in status mode | **Yes, the sleep screen** with the spinner and the link status. A touch shows status mode, with Forget pairing and Settings, for as long as the user keeps touching. `offline_after_s = 0` restores the old behavior |
| Q5 | Logo direction | **Decided 2026-10-23: candidate A, Deck** (see 05-logo.md, L1 results) |

## Risks (top 8; each file has the rest)

| Risk | Mitigation |
|---|---|
| A firmware write corrupts the card's FAT and the user's data | The backup gate. Writes go only to two names, through one module with an allowlist, rarely (user actions and config changes). A prek hook refuses any format or esp_tinyusb mount call. Five power-cut runs and `fsck.vfat -n` in batch A |
| esp_tinyusb formats the GPT card if it ever mounts it to the app (F3) | Never call `tinyusb_msc_set_storage_mount_point` or `tinyusb_msc_format_storage`, and keep `auto_mount_off`. The prek hook greps for them and for `tinyusb_msc_delete_storage`. Share uses `tinyusb_msc_new_storage_blockdev` with `do_not_format = true`; Stop sharing is a reboot |
| An interrupted write or a cleanup frees a live file | No `rename` on the card (F41); intent records plus write-once images; the FatFs host test cuts at every write, with torn sectors, and checks for cross-linked chains |
| A host's unflushed writes are lost when the device takes the card back | Stop sharing asks the user to eject on the computer first; the reboot then ends the USB session, as a reset does today |
| The log hook overflows a small task stack, or leaks a secret through a cut | A 224-byte buffer, `va_copy` first, redaction before the cut (including a cut-off prefix), appended under a spinlock. Batch A logs the stack high-water mark of every task that logs |
| A Wi-Fi trial or scan stalls behind the link task or the LVGL task | Both run on the svc task (D4). Batch A times a scan with settings open |
| The push API opens an unauthenticated hole | Loopback by default; bearer token from `secrets.toml`, constant-time compare; LAN only over HTTPS and only by config; body and rate caps; N2's tests send each failure case |
| The long swipe down sleeps the screen by accident, or presses a button | The same 200 px and 2:1 rule as the long swipe up, from `PRESSING` with the wait for release. P1 runs the device-ui S6 touch matrix for the new direction |
| The pool runs out on a big settings tab or the feed page | Create on show, one tab at a time, log text kept outside the pool (`lv_label_set_text_static` on PSRAM copies). Each slice logs the pool with its screen up |

## Rollback

- Each slice is its own commit, so `git revert` undoes one.
- The firmware rolls back by flashing the previous build with the daemon from the same commit. The partition table and NVS don't change, so the pairing survives.
- After G2 the card holds `watchdeck/` and maybe a firmware-written `wifi.txt`. An older build ignores `watchdeck/` and reads `wifi.txt` the same way, so the card needs no cleanup. The backup restores it if anything else went wrong.
- The riskiest slices are G2 (the card) and T1 (the `ui.c` restructure). G2's rollback is a revert plus a flash, and the backup. T1's is a revert of T1 and everything in waves 2 and 3 that hooks into its tables.

## Critique log

One adversarial pass on the plan (groundwork-heavy), codex in plan mode. Each point was checked against the code before it was logged.

| # | Point | Verdict |
|---|---|---|
| 1 | Runtime `tinyusb_msc_delete_storage` races SCSI callbacks (lookup in a critical section, lock and medium call outside it) | adopted: verified at `tinyusb_msc.c:246-264`. Stop sharing is now a reboot; `delete_storage` joins the prek hook's banned list (G2, D1, F41) |
| 2 | An interrupted `f_rename` leaves both names on one chain; "both exist, delete the temp" frees the live file | adopted: verified in `ff.c`. No `rename` on the card; intent record plus in-place write (G1, D2) |
| 3 | A missing final file doesn't prove the temp file is complete | adopted: the intent record carries length, SHA-256 and a header CRC, and recovery trusts only a valid record (G1) |
| 4 | `wifi_start` returns before Wi-Fi init with no credentials, so the editor can't scan or connect | adopted: verified at `wifi.c:150-165`. `wifi_init`/`wifi_apply` split, and a no-credentials boot case (T3, D6, F43) |
| 5 | POSIX host tests don't exercise FatFs; FSINFO is written unchecked and trusted at mount | adopted: a `linux`-target FatFs app with a RAM disk and cut and tear injection at every write; `DONT_TRUST_FREE_CLUSTER_CNT` and `DONT_TRUST_LAST_ALLOC` on; a stated failure model (G1, G2, F42) |
| 6 | Set `do_not_format = true` on the shared storage | adopted (G2) |
| 7 | The armed retry timer can fire during a trial, and a `GOT_IP` isn't tied to the trial | adopted: one connect state machine, the timer stopped on entry, the SSID checked, restore failure falls back to the retry; host-tested in `wifistate.c` (T3) |
| 8 | RSSI stays on the link task; a 20 s trial monopolizes svc | adopted: RSSI moves to svc and skips during scans; the trial is event-driven and svc runs only its steps (T3, G1, D4) |
| 9 | `va_copy` before either formatter; the ring ready before the hook; the hook must not log; early and ISR logs aren't captured | adopted (G3) |
| 10 | Redact before truncation | adopted: redaction on the 224-byte text, including a cut-off prefix, then the cut; host cases at every position (G3) |
| 11 | Persistence needs one owner, snapshots, coalescing, and GC only after commit | adopted: `sleepstore.c` generations, one save job with a dirty flag, GC after `sleep.cfg` commits (P3, D3) |
| 12 | "Write the image only if absent" reuses a torn file | adopted: reuse only after the size and SHA-256 check; an invalid file is deleted and fetched again (P3) |
| 13 | Late results (pending up to 11 min) and set applies while asleep build widgets | adopted: verified at `ui.c:28`, `:1254`, `:1324`. A mode table (`uimode.c`) and P1 cases (D7, F44) |
| 14 | A 200 px downward swipe can't start from the bottom two rows of a 600 px panel | adopted: the acceptance swipes start in the top half; the limit is in the risks and docs (P1) |

Also fixed during the pass, not from the critique: the feed slices were F1 to F5, the same names as findings F1 to F5, so they are now N1 to N5.

second opinion: codex (consult tool), model gpt-6.1-sol as observed, effort high, plan mode, one pass on the whole plan with the groundwork as focus. No second pass on the feature files (the coordinator asked to wrap up).
