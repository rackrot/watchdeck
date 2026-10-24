# Sleep and privacy screen

Part of [`00-index.md`](00-index.md). Findings F1 to F44 are there. Needs G1, G2 and G4 from [`01-groundwork.md`](01-groundwork.md), and T1 from [`02-settings.md`](02-settings.md).

A screen that replaces everything else on the panel:

- **Offline**: when the link has been down (offline, searching, backing off) for `offline_after_s`, default 10 s. A spinner and the link status show in the strip at the bottom.
- **Privacy**: at once, after a long swipe down on the grid or the feed page. No spinner while the link is up.
- The app sets the image, the colors, which elements show (clock, date, the desktop's name, a line of text) and where, the spinner style and color, the offline delay and the backlight level. The device keeps all of it on the card, so it shows with no daemon after a reboot.

## Decisions

### The screen

```text
y 0   +--------------------------------------------------+
      |                                                  |
      |      image area 1024 x 512                       |
      |      (the picture, or the logo on bg)            |
      |                                                  |
      |  14:32                     <- elements at anchor |
      |  Wed 22 Oct   devbox                             |
y 512 +--------------------------------------------------+
      |        (o)  searching for devbox                 |  strip, 88 px: spinner and
y 600 +--------------------------------------------------+  status, only while offline
```

- The image area is fixed at 1024 x 512, so an image is always 1,048,576 bytes of RGB565 (D10). The strip below is `bg`. Without an image the logo (L4) sits centered in the area, recolored in `fg`.
- Elements are a column at the anchor, 32 px from the area's edges: the clock in Montserrat 48, then the date, the host and the text in Montserrat 32, all in `fg`. The clock and the date show only once a `stats` frame has given the time since this boot (F38), and the host only once a `welcome` has named it. A reboot with no daemon shows neither, which is honest about not knowing.
- The strip shows a spinner (48 px) and the daemon status text (`searching`, `retry in 8 s`, `offline, waiting for Wi-Fi`) only while the link is down.
- Spinner styles, all built from widgets the build already has (F22): `arc` (`lv_spinner`), `dots` (three circles with an opacity animation), `bar` (an indeterminate `lv_bar`), `pulse` (a circle whose opacity and size breathe), `none`.

### A mode, not an overlay (D7)

Entering sleep closes the license viewer and the result modal, deletes settings (if open), the grid or the feed page, the bar, and status mode's widgets, then builds the sleep screen on the active screen. Waking deletes it and rebuilds the mode it came from (grid with the bar, or status mode). So:

- Host stats leave the screen with the bar. That's what privacy means here. `ui_set_stats` keeps storing frames (F38), so the bar and the clock come back current.
- The pool holds about 3 KB for the sleep screen instead of a page of up to 49 KB, which leaves room for a spinner animation.
- **Every UI entry point is mode-aware** (F44). Things keep arriving while asleep: a `result` for a press made just before the swipe (presses stay pending for up to 11 minutes, `ui.c:28`, and `ui_show_result` builds a toast, `ui.c:1324`), a new set after a reconnect (`ui_show_set` rebuilds the bar and the page, `ui.c:1254`), tiles, icons and stats. In `MODE_SLEEP`: `ui_show_set` updates the model and marks "rebuild on wake" without creating widgets; `ui_set_tile` and `ui_set_icon` update the model only; `ui_show_result` keeps only the pending entry's bookkeeping (the press is cleared) and logs `result while asleep: <button> <ok>` with no toast; `ui_set_stats` stores as today. `ui_link_down` and `ui_set_paired(false)` still act: the first updates the strip, the second wakes to status mode (an unpair always shows it). The same table applies to settings, which already defers a layout push to its close (device-ui S6).
- Pairing views exist only unpaired, when sleep never starts.

### Reasons and wake (D8)

| | Offline | Privacy |
|---|---|---|
| Starts | Paired, the link down for `offline_after_s` (0 disables), no touch for that long, not in settings | A long swipe down: 200 px down, `|dy| > 2|dx|`, from the grid, a cell or the feed page, fired from `PRESSING` |
| Spinner | yes | only while the link is down |
| Link comes back (a set applies) | wakes | stays asleep, and the spinner goes |
| A touch | wakes; offline sleep can start again after `offline_after_s` without a touch | wakes (Q2) |

- A touch wakes on release, and the release is swallowed: the sleep screen is one clickable full-screen object, and the rebuild runs from `lv_async_call` after the release, so nothing under the finger is pressed.
- **The long swipe down** follows the long swipe up exactly (F15). `grid_gesture` stops waiting for release on `LV_DIR_BOTTOM`, as it already doesn't on `LV_DIR_TOP` (F14), so `PRESSING` keeps coming past LVGL's 50 px. The release then reaches `drag_consumed`, which refuses any touch over 20 px, so a long swipe never presses.
- **No clash with settings**: settings closes on LVGL's 50 px swipe down, but the grid is gone while settings is open and the settings page doesn't run `touch_track`, so a swipe down there only closes settings. **No clash with page turns**: those need `|dx| > 2|dy|`.
- **At boot**: a paired board that can't reach its daemon shows the sleep screen after `offline_after_s` instead of staying in status mode (Q4). A touch shows status mode, with Forget pairing and Settings, until the user stops touching.

### Backlight (D9)

- GPIO31 drives the boost converter's enable, with a 10K pull-down, and is set high once today (F21). Elecrow marks IO31 "PWM", but whether the converter dims cleanly from PWM on its enable, and at what frequency, is unknown. P0 finds out on the board.
- If P0 passes: LEDC on GPIO31 at the frequency P0 picks; `backlight` is a percent, and the sleep screen sets it on entry and restores 100 on wake.
- If it fails: `backlight` 0 turns the light off while asleep and anything else leaves it on. The docs and the form say so. Touch still works with the light off (the GT911 is polled), so a touch wakes it.
- GPIO29 (`LCD_BK_POWER`) stays untouched, since the panel works without it today.

### Config, image and persistence

- **Daemon** (P2): `[sleep]` (G5) names an image in `screens/`. The daemon decodes it with the icon pipeline's checks (PNG, JPEG, WebP, the SVG subset and its cost check), fits it to 1024 x 512 by `fit` (`cover` crops, `contain` letterboxes on `bg`), converts it to RGB565 little-endian, and names it by its hash. The session sends `sleep` (G4) after the first set and when the session's sleep view changes, and answers `asset_get`.
- **Board** (P3):
  - A `sleep` frame whose image the board doesn't have starts a download: one `asset_get` at a time, offset 0, then each next 262,144 bytes, with the 5 s timeout of `icon_get` restarted on each frame. It waits until the set's icons have settled, so the grid's icons come first.
  - Chunks go into a 1 MiB PSRAM buffer. When the last arrives, the SHA-256 is checked; a mismatch drops the image (logged) and keeps the old one.
  - The verified image swaps in under the LVGL lock (the sleep screen, if up, re-points its image).
  - **Persistence has one owner, `sleepstore.c`, and works on snapshots.** The applied state is `{generation, frame text, image id, image buffer}`; each new `sleep` frame or verified image makes a new generation, and the previous buffer is freed only when nothing references it (the screen, a running save). One save job at most is queued or running: a new generation while one runs only sets "dirty", and the job, when done, saves again if the generation moved (so ten quick edits cost at most two saves). The job takes an immutable snapshot (the frame text copied, the image buffer referenced) and:
    1. If the image isn't on the card **as a valid file** (its size is 1,048,576 and its SHA-256 matches the id; checked once per boot and cached), writes it write-once to `watchdeck/sleep-<id>.rgb565`, deleting an invalid file of that name first.
    2. Writes `watchdeck/sleep.json` (the frame text) with `cardfs_write_small`.
    3. Only then deletes every other `sleep-*.rgb565`: the committed `sleep.json` no longer names them. A cut before step 3 leaves an extra file, which the next save removes.
  - With the card shared or missing, the save job does nothing and the Storage tab says "sleep screen not saved". Nothing is queued across a share: Stop sharing reboots (G2), and on the next connect the daemon sends `sleep` again and the board saves it then.
  - At boot, after the mount and `cardfs_recover`, a svc job reads `sleep.json`, parses it with the same `proto_parse`, and loads the named image into PSRAM with the size and hash checks. A file that fails is deleted and logged, the logo shows, and the next `sleep` frame fetches it again. That's before Wi-Fi, so an offline boot shows the custom screen.
- One parser for the frame and the file means one set of host cases and no second format.

## Goal and done

- The two triggers and the wake rules in the table above, on the board.
- Twenty long swipes down starting in the top half of the grid (y 44 to 300, so a 200 px swipe fits on the 600 px panel; from the bottom rows it can't), on 5x3 and 8x5 grids, half fast and half slow, on cells and on gaps, each sleep the screen, and neither log shows a `press` or a page turn. Twenty downward flicks of 50 to 150 px, twenty page-turn flicks and twenty taps don't sleep it.
- A touch wakes it and presses nothing, ten times out of ten, including a touch that lands where a button is.
- The app's image shows within 5 s of a save with a board connected, and after a reboot with the daemon stopped.
- The backlight dims to the configured level if P0 passed, or goes off at 0 if not.
- Pool: entering and leaving the sleep screen 10 times leaves "page built" within 200 bytes.
- PSRAM: one image in use (1 MiB), two while a new one downloads, and at most three when a save still holds a replaced one (a new download waits while three are held).

## Slices

### P0 Backlight PWM probe

- **What**: a throwaway build (not merged) that drives GPIO31 from LEDC and steps through 1 kHz, 5 kHz, 20 kHz and 25 kHz, each at 100, 60, 30, 10 and 3% duty, holding each for 5 s and logging the step. The user watches for flicker (also through a phone camera, which shows PWM banding), listens for coil whine, and notes the lowest duty that still lights the panel evenly.
- **Decides**: whether P1 implements dimming, at which frequency, and the minimum duty the percent maps to. Recorded in this plan's results.
- **Acceptance**: a recorded verdict. Stop condition: any step that turns the panel off and doesn't recover at 100% ends the probe; reflash the normal build.
- **Owner**: infra, board. Runs in batch A, in five minutes.

### P1 Sleep screen on the board

- **Files**: `firmware/main/sleep.c/.h` (new: the screen, the spinners, the element layout, the backlight call), `ui.c` (`MODE_SLEEP`, the long swipe down in `touch_track`, `LV_DIR_BOTTOM` in `grid_gesture`, the offline watchdog timer, enter and wake), `display.c/.h` (`display_backlight(percent)`, LEDC or on/off per P0), `sleepcfg.c/.h` (new, pure: the config struct with defaults and the anchor layout math, host-tested), `uimode.c/.h` (new, pure: the entry-point table), host `test_sleepcfg.c`, `test_uimode.c`.
- **Changes**:
  - Built-in defaults until P3 brings a config: `bg` black, `fg` light grey, `show` clock and date, anchor `bottom_left`, spinner `arc` in the app's accent (#38bdf8), `offline_after_s` 10, `backlight` 100, the logo placeholder (a drawn rounded square) until L4.
  - The offline watchdog: one 1 s `lv_timer` created in `ui_init`, which reads `s_online`, how long the link has been down (stamped in `ui_link_down`, and at `ui_init` for a paired boot), the mode, and `lv_display_get_inactive_time`.
- **Host cases** (`sleepcfg`): each anchor's element positions inside 1024 x 512 with 32 px margins; elements hidden when their data is missing; `offline_after_s` 0 never triggers.
- **Host cases** also cover the mode table: a pure `uimode.c` decides, for each entry point and mode, build, defer or drop (the table in Decisions), so a new mode can't forget one.
- **Acceptance (batch A)**: the touch matrix in Goal and done; press a button whose action takes 5 s, swipe down at once, and the result arrives asleep with no toast and a `result while asleep` log, and nothing shows on wake; with privacy sleep up, restart the daemon: the set applies with no widgets built (the pool unchanged in the log) and the grid shows the new set on wake; offline after 10 s with the daemon stopped, with the spinner and the status text following the link (`searching`, `retry in N s`); wake on reconnect from offline only; privacy stays across a reconnect; the bar and host stats are gone while asleep and current on wake; 10 rounds within 200 bytes; each spinner style draws (switch with a temporary build constant); the backlight per P0.
- **Cheapest check**: `mise run fw:test`, `mise run fw:build`, then batch A.
- **Owner**: high complexity, firmware LVGL. Merge before N3, which hooks the same gesture code.

### P2 Daemon: sleep image and frames

- **Files**: `daemon/screens.rs` (new: load `screens/`, render to RGB565 1024 x 512, the hash, a cache keyed like icons), `icons.rs` (`decode` and the SVG checks shared with a target box instead of a square side), `config.rs` (`Live` carries the rendered screen, so the frame and its pixels come from one load), `server.rs` (the `sleep` view per session, the `select!` change check beside the layout's, `asset_get` answered from the session's view, only with `sleep`), `protocol.rs` (`CAPS` gains `CAP_SLEEP`), `commands.rs` and `lib.rs` (`import_screen_image`, `list_screen_images`, the asset-protocol scope gains `screens/`), `tests/device_sim.rs`.
- **Tests**:
  - Render: a 4:3 photo under `cover` fills 1024 x 512 and crops top and bottom equally; under `contain` it is letterboxed on `bg`; a pure red pixel becomes `0x00 0xF8` (RGB565 LE); the id is the first 16 bytes of SHA-256 over the 1,048,576 pixel bytes; an SVG over the cost limit is refused with the icon rule's message.
  - `device_sim`: a hello with `sleep` gets `sleep` after the first set and its tiles, and none without the cap; `asset_get` at 0, 262,144, 524,288 and 786,432 gets four frames that reassemble to the rendered pixels; an offset that isn't a multiple, or past the end, gets `bad_request` and a close; an id that isn't the current image gets nothing; a `[sleep]`-only edit pushes `sleep` and no `layout`; a new image changes the id and the old id then gets nothing.
- **Acceptance**: `cargo test`, clippy. Every existing `device_sim` test passes unchanged.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml screens` and `--test device_sim`.
- **Owner**: design judgment, daemon.

### P3 Board: sleep config, image download, persistence

- **Files**: `link.c` (dispatch `sleep`, `asset_get` after icons settle, the hello names `sleep` in this commit), `assetdl.c/.h` (new, pure: the expected offset, the chunk checks, the final hash, the 5 s deadline from a caller-given clock, like `pageset.c`), `sleepstore.c/.h` (new: the generations and snapshots, the dirty flag and the one save job, the boot load job; its decisions split into a pure `sleepsave.c` for host cases: coalescing, the step order, what GC may delete), `sleep.c` (apply a new config live), `main.c` (the boot load job), `tab_storage.c` (the sleep rows), host `test_assetdl.c`.
- **Host cases** (`sleepsave`): a new generation during a save runs exactly one more; GC never lists the image the newest committed `sleep.json` names; an invalid file of the target name is deleted before the write. (`assetdl`): four good chunks complete; a chunk at the wrong offset, of the wrong length, or for another id is refused and the download keeps waiting; a flipped byte fails the hash at the end; a new id mid-download restarts; the deadline passes with no frame.
- **Acceptance (batch B, against the P2 daemon)**:
  - Save a 4000 x 3000 JPEG in the app: the board logs four `asset_get`s, `sleep image <id> verified`, and the persist job's three steps, within 5 s; the privacy screen shows it.
  - Restart the board with the daemon stopped: after 10 s the offline screen shows the same image.
  - Change only `spinner` in the app: no `asset_get`, a new `sleep.json`, the spinner changes live.
  - With the card shared: "sleep screen not saved" on the Storage tab; Stop sharing reboots; after the reconnect the save runs and the tab shows the files.
  - Ten spinner changes saved in the app within 5 s: at most two `sleep.json` saves in the log, and the last one holds the last change.
  - Corrupt the image file on the card from a computer (one byte), boot with the daemon stopped: `sleep image <id> invalid, deleted`, the logo shows; start the daemon: the image is fetched and saved again.
  - A bad image is covered by `test_assetdl.c`; on the board, the log shows `sleep image <id> verified` with its time for each download.
  - The card's `watchdeck/` holds exactly `sleep.json` and one image after three image changes.
  - PSRAM free during and after a download is logged: at most 2 MiB more during, 1 MiB after.
- **Cheapest check**: `mise run fw:build`, then batch B.
- **Owner**: design judgment, firmware.

### P4 App: sleep screen form

- **Files**: `app/src/SleepForm.tsx` (new), `GridEditor.tsx` (a "Sleep screen" button beside "Status bar" in the panel head), `ConfigPage.tsx` (the panel switch), `api.ts` (`import_screen_image`, `list_screen_images`), `preview.ts` (the sleep layout: the 1024 x 512 area, the anchor math, mirroring `sleepcfg.c`), `app/tests/preview.test.ts`, `errors.ts` routing (G5 made the bucket).
- **Form**: Image (pick, import, none), Fit, Background and Text colors (the `ColorField` from `fields.tsx`), Show (four checkboxes and the text field), Position (five choices drawn as a 3x3 pad with the middle and corners), Spinner (five choices with a CSS preview of each), Spinner color, "Sleep after the link is down for" (seconds, 0 = never), Backlight while asleep (a slider if P0 passed, else On and Off). A scaled preview of the whole screen, with the strip shown as "while offline".
- **Tests**: the preview's element positions match `test_sleepcfg.c`'s table for each anchor; `sleepChanged` is true for a `[sleep]`-only edit and `layoutTargets` is empty for it; each `sleep.*` rule message lands next to its field.
- **Acceptance (window)**: import an image, save, and the board shows it (with P3); every control writes the matching key and reads back after a reload.
- **Cheapest check**: `pnpm test`, `pnpm build`, Biome from `app/`.
- **Owner**: design judgment, app.

## Rejected alternatives

| Option | Why not |
|---|---|
| The sleep screen on the top layer over the grid | Keeps the grid's widgets in the pool under it, and the bar's live stats would stay in the widget tree under a "privacy" screen |
| A full-screen image with the spinner drawn over it | The user asked for the spinner below the customized area. A fixed strip keeps the image's bottom edge clear and the text readable on any picture |
| Any image size | The board would need a scaler, or the daemon a per-board size. One size is 1 MiB, the same on every board |
| JPEG or PNG on the card, decoded on the board | Decoders are off (F22) and would cost RAM and boot time. RGB565 is the panel's format and draws with no decode |
| The daemon pushes chunks unasked | A 1 MiB burst would sit behind the 500 ms queue drop (F10). Pulling one chunk at a time is how `icon_get` already paces itself |
| The sleep config in NVS | NVS keeps only the pairing (CLAUDE.md). The image has to be on the card anyway, and one place for both keeps them consistent |
| The clock from SNTP when the daemon is gone | Device-ui rejected SNTP (no time zone on the board, internet access). The clock hides when the time is unknown |
| Waking on a reconnect from privacy too | The user put the screen there on purpose |
| Requiring a long swipe up to wake from privacy | Q2: harder to discover, and a tap swallowed on release can't press anything |
| A screen-saver timeout with no link loss | Not asked for. Can be `idle_after_s` later with the same machinery |

## Risks

| Risk | Mitigation |
|---|---|
| A long swipe down sleeps the screen by accident, or presses a button | 200 px and 2:1, from `PRESSING` with the wait for release; P1's touch matrix |
| The long swipe down can't start from the bottom rows (under 200 px of travel left) | Said in the docs ("swipe down from the upper half"); the same holds for the long swipe up from the top rows today. A lower threshold for down only is a later knob if the user finds it awkward |
| A result, a set or a tile arrives while asleep and builds widgets under the sleep screen | The mode table (`uimode.c`) and P1's result-while-asleep and set-while-asleep cases |
| The wake touch presses the button under it | The rebuild runs after the release; P1 tests touches on buttons |
| The offline screen flashes during a daemon restart (`mise run app:dev` restarts it on every change) | 10 s by default; `offline_after_s` is configurable, 0 disables |
| PWM on the boost enable flickers or whines | P0 decides; on/off is the fallback |
| A corrupt image file on the card | Size and SHA-256 checked at load; a bad file is skipped and logged, and the logo shows |
| Two images in PSRAM during a download | 2 MiB of 32 MB, logged in P3 |
| The download competes with icon fetches | It starts only after the set's icons settle |
| An `asset_get` for a stale id after a config change | The daemon answers only the current id; the board's 5 s timeout ends the old download, and the new `sleep` frame starts a new one |

## Memory estimates

| What | Pool | PSRAM |
|---|---|---|
| Sleep screen: image widget, up to 4 labels, the strip, a spinner and its animation | about 3 KB | the image, 1 MiB (none for the logo) |
| During a download | — | 1 MiB more |
| Config | — | under 2 KB |
