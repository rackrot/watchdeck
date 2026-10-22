# Pages: several button pages per layout (protocol v2)

Status: planned 2026-10-21, revised 2026-10-22 against `main` at `f75bf5c`. No slice started.

Adds pages to the one layout that serves every paired device. Comes from the backlog item in `plans/desktop-app-v1.md` (Backlog, "Pages (protocol v2)", `:590`), decided on 2026-10-21. The v1 plan assumed "One layout serves every paired device, with no pages" (`plans/desktop-app-v1.md:57`). That stays true except for "no pages".

## Revision 2026-10-22

- **User decisions of 2026-10-21 applied to the body.** Swipe is the only navigation. A small "Page x of y" label sits below the grid, and the grid gives up a strip of height for it. No page bar, no page-link buttons, no `Action::Page`, no `to` on the wire. At most 4 pages. Every page's tiles poll. No wrap at the ends.
- **Fonts landed first** (`ffa76f9`, `d6f0abc`, `d1f54f5`, the fonts S4 commit, and `f75bf5c` for the licenses page). `hello.caps`, the per-session view, `DeviceInfo.caps`, the font fields and the fixed text slots already exist. This plan now adds the `pages` name to the existing list and a "home page only" rule to the existing view.
- **Geometry decided.** The grid shrinks by a 31 px strip only when a layout has 2 or more pages, so a file without pages keeps today's cells, icon sizes and icon ids. One shared table pins the numbers on the board, the daemon and the preview (S2).
- **Page labels dropped.** Nothing on the board shows a page's name any more, so `page_label` leaves the wire. The config's `[[page]]` keeps `id` and an optional editor-only `label` (open question A, settled 2026-10-22).
- **Icon cache bound redone for 4 pages.** Worst case 3,625,216 bytes (3.46 MiB) of the 8 MiB cap, and 160 table entries.
- **Slices re-cut.** S1 is now the wire contract only. A new S2 holds the geometry contract. Page links, the page bar and the page action left every slice. Eight slices in four waves.
- **Findings re-checked** against `f75bf5c`, with moved line refs fixed and stale ones replaced. The `insecure_tls` and SVG work this plan used to route around has landed (`d2468c2`, `4437fd4`), so that assumption is gone. Three new findings matter: `layoutTargets` in `edit.ts` (F24), `icons.c` `drop` taking the LVGL lock (F25), and the ack order written in `protocol.md` (F26).
- **Critique.** A self-review of 14 points, since the consult tool refused both outside reviewers. See "Revision 2026-10-22" in the critique log.

## Goal and done

- `config.toml` can declare up to 4 pages. Each button belongs to one page. A file without pages works exactly as it does today: the same `layout` bytes on the wire, the same geometry and icon sizes on the board, and a save from the window keeps it in its old form.
- A board on the new firmware shows one page at a time and moves between pages with a horizontal swipe. With 2 or more pages it shows "Page x of y" under the grid. Moving never asks the daemon for anything, and it works while the link is down.
- A board on today's firmware, against the new daemon, still works. It shows the first page, and the daemon says so in its log and in the Devices tab.
- A board on the new firmware, against today's daemon, still works. It shows the single grid at today's size, with no page label.
- `layout_ack` still turns the save bar from "sent" into "applied", once per push, also for a board without `pages` when the push reached it. The board now sends it before its `icon_get`s, and `protocol.md` changes to say so (F26).
- Tiles on every page keep their values, so a page shows its current values the moment it is built.
- Icons of every page are cached in PSRAM, so a page switch sends no `icon_get`.
- The config editor can add, rename, reorder and delete pages, move a button to another page, and preview each page with its label.
- `cargo test`, `pnpm test` and `mise run fw:test` cover the new rules. The hardware run (S8) passes on the board, with serial-log evidence.
- `site/src/protocol.md`, `site/src/config.md`, `CLAUDE.md` and the status in `plans/desktop-app-v1.md` are updated.

## Assumptions

- One grid (`[layout] cols, rows`) serves every page. Per-page grid sizes are left for later (open question 3). The wire still carries `cols` and `rows` on every page message, so a later change needs no firmware.
- The current page is local to each board. Two boards can show different pages. Nothing persists it in NVS, which keeps holding only the pairing blob. A reconnect keeps the page. A reboot starts on the first page.
- Global button ids stay unique across pages. That keeps `press`, `result` and `tile` unchanged on the wire.
- The LVGL pool is 64 KB of internal RAM (`CONFIG_LV_MEM_SIZE=65536`, generated `sdkconfig:4965`). Fonts S5 recorded 18,036 of 62,604 bytes used after the first layout, 20,032 at most, and a min free internal heap of 300,060 bytes (`plans/fonts.md`, "S5 as run"), on 5×3 and 5×5 grids. A full 8×5 page with 40 icons and 40 tiles was never measured (`plans/desktop-app-v1.md:472-474`). UNCONFIRMED that it fits, so S5 measures it first.
- The "Page x of y" label draws in the grid's default font at slot 24. That is fonts open question 8, which kept its recommended default (`plans/fonts.md`, Open questions). Chrome (status lines, toast, result modal, Forget pairing, Licenses, pairing views) stays Montserrat.
- A later daemon that allows more than 4 pages must name that under a new capability. "pages" means at most 4.

## Findings

Checked against `f75bf5c`. F19 is retired, and F21 to F28 are new in this revision.

| # | Where | What |
|---|---|---|
| F1 | `server.rs:586-589`, `server.rs:496-503` | The daemon closes any `hello` whose `v` isn't exactly 1, with `error` `version`. A board that sends `v:2` to today's daemon gets refused every time |
| F2 | `link.c:491-493` | An unpaired board lists a daemon for pairing only when TXT `v` is absent or exactly `1`. A daemon that advertised `v=2` would vanish from every old board's pairing list |
| F3 | `proto.c:436-440`, `protocol.md:147` | The board ignores `welcome.v` and every unknown field. The daemon ignores unknown fields too (`protocol.rs:519`, `unknown_fields_are_ignored`). New optional fields reach old peers safely |
| F4 | `proto.c:292-300`, `proto.h:62-70` | A layout is one `proto_layout_t` of at most 40 buttons, parsed into one PSRAM `proto_msg_t`. A message without `buttons` fails to parse, so an old board can't take a reshaped `layout` |
| F5 | `config.rs:1597-1658` | The fullest 8×5 layout must fit in 16 KB. With every font field at its longest it is 13,282 bytes (run on `f75bf5c`). The page fields add about 95 bytes. Two full pages in one message would pass 16 KB |
| F6 | generated `sdkconfig:2347` (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`) | cJSON allocates each node separately, and allocations under 16 KB come from internal RAM. A layout of several pages in one message would multiply the transient internal-heap cost of the parse |
| F7 | `icons.c:83-101`, `icons.h:13-14` | `icons_retain` frees every icon the one layout doesn't name. The cache holds at most 40 icons and 8 MB, and its table is in internal `.bss` (`icons.c:12-18`) |
| F8 | `link.c:929`, `link.c:955`, `link.c:31` | `sync_icons` keeps `seen[40][16]` and a 2 KB buffer on the 8 KB link task stack. Sized for a set of 4 pages, `seen` alone would be 2.5 KB |
| F9 | `icons.h:10` | The icon cache is link-task-only. A page switch started by a swipe in the LVGL task can't read it directly |
| F10 | `ui.c:395-479`, `ui.c:62-76` | `build_grid` deletes and rebuilds the whole grid. Every per-button array is sized for one layout of 40 |
| F11 | `ui.c:517-538` | A `tile` for a button that isn't on screen is logged and dropped (`:524-526`). With pages, values for hidden pages would be lost |
| F12 | `lv_indev.c:1569-1577` (LVGL 9.6.0) | On release LVGL sends `CLICKED` to the pressed widget whenever nothing scrolled, gesture or not. The grid never overflows, so it never scrolls (`lv_indev_scroll.c:397-409`). A swipe that starts on a button would change the page and also press it, running its action |
| F13 | `lv_indev.c:1518-1531` | After `lv_indev_wait_release()`, the release sends `PRESS_LOST` and clears the active widget, so no `CLICKED` follows. That is the fix for F12 |
| F14 | `lv_obj.c:1507`, `lv_indev.c:1832-1845` | Every child widget has `gesture_bubble` set, so a gesture bubbles to the first ancestor without it. Today that is the screen. Clearing the flag on `s_grid` makes the grid the gesture target. `CONFIG_LV_USE_GESTURE_RECOGNITION` is off (`sdkconfig:5255`), so this is the classic `indev_gesture` path, which also sends `LV_EVENT_GESTURE_LEFT`/`RIGHT` (`lv_indev.c:1875-1897`). Threshold 50 px (`sdkconfig:5247`) |
| F15 | `tiles.rs:193-198` | The tile manager polls every tile in `config.buttons`. A flat button list across pages keeps it unchanged |
| F16 | `icons.rs:177-189`, `icons.rs:235-256` | `Icons::load` renders every icon of `config.buttons` at one `box_px(cols, rows)`. The paged geometry makes the box depend on the page count too, so `box_px` gains a `paged` argument. The render cache already keys on `side` (`icons.rs:211-215`, `:266`), so a change of box re-renders |
| F17 | `server.rs:43`, `server.rs:843-874` | `icon_get` takes at most 40 ids and is answered from the session's layout (`layout.icons`) only |
| F18 | `server.rs:738-748`, `server.rs:764-780`, `server.rs:834-842` | A session pushes a layout only when its view differs, bumps `rev` once per push, and takes an ack for a `rev` it sent and hasn't seen acked. Several messages sharing one `rev` need no change to the ack rule |
| F19 | — | Retired: it sized the status lines for the page bar, which is gone. The Licenses button now sits on the USB line (`ui.c:25-29`) |
| F20 | `ui.c:209-218` | The result modal names a button by looking it up in the grid on screen, and falls back to its id. After a page switch, a result from the old page would show the raw id |
| F21 | `protocol.rs:11-14`, `server.rs:593-599`, `server.rs:612`, `link.c:707-711` | Fonts built the cap list: `CAPS = [CAP_FONTS]`, the session keeps only known names, `serve` gets `fonts: bool`, and the board sends `{"fonts"}`. Fonts S2's host test already proves `{"fonts","pages"}` with a 32-character `fw` fits the 256-byte hello buffer |
| F22 | `server.rs:730-737`, `config.rs:192-200` | The session's view is a closure: `live.layout()`, then `without_fonts()` without the cap. It is compared and sent as is, so a "home page only" rule slots in beside it |
| F23 | `ui.c:15-16`, `ui.c:400-418`, `icons.rs:156-189`, `preview.ts:16-85`, `config.md:131` | The grid geometry is mirrored three times: `GRID_TOP` 204, `GRID_GAP` 8, `ICON_PAD` 4, `ICON_GAP` 2, `ICON_MIN` 24 and the slot-32 box 35. The grid is 396 px tall. Tests pin it on two sides (`icons.rs:1202-1216`, `preview.test.ts:17-53`). The firmware has no host test of it, because `ui.c` and `display.h` include `lvgl.h` |
| F24 | `edit.ts:121-137`, `ConfigPage.tsx:208` | `layoutTargets` picks the devices the save bar waits on. It already leaves out boards without `fonts` for a font-only edit, since the daemon pushes them nothing. An edit confined to page 2 or later pushes nothing to a board without `pages` either, so without a matching rule the save bar would wait on it forever |
| F25 | `icons.c:31-41`, `esp_lvgl_port.c:77` | `drop` takes `lvgl_port_lock` to clear LVGL's image cache entry. With a model lock, the link task must take the LVGL lock first. The port's mutex is recursive, so `drop` locking again inside is fine |
| F26 | `protocol.md:285`, `link.c:1041-1052` | The spec says the board acks "after it has built the grid and sent its `icon_get`", and the firmware does exactly that. The daemon sends every icon frame of a request before it reads the next message (`server.rs:843-874`), so today's ack waits behind up to 40 frames |
| F27 | `link.c:226-235`, `link.c:1004-1006` | A text frame can be lost: the websocket handler drops it after waiting 500 ms for queue room, and an unparsable one is dropped with a log. The daemon never resends an unchanged layout |
| F28 | `link.c:396-452`, `link.c:662`, `link.c:1209-1216` | The link task runs mDNS discovery inline (3 s PTR plus a 10 s budget) from `attempt()`. A UI event queued to the link task could wait 13 s while the daemon is away |

## Approach

### Config

The button list stays flat. Pages are a list of their own, and a button names its page:

```toml
version = 1

[layout]
cols = 5
rows = 3

[[page]]
id = "main"

[[page]]
id = "media"
label = "Media"

[[button]]
id = "plex"
page = "media"
pos = [0, 0]
label = "Plex"
action = { kind = "shell", argv = ["ssh", "nas", "docker", "restart", "plex"] }
```

- No `[[page]]` table: one page, as today. A button may not set `page` then.
- With `[[page]]` tables: 1 to 4 pages, in file order. The first is the home page. A button without `page` goes on the first page.
- A page `id` follows the button id rule (1 to 63 letters, digits, `_`, `-`, `.`) and is unique among pages. An optional `label` (printable ASCII, at most 24 characters, skipped when `None`) names the page's tab in the editor, which falls back to the id. The label is config-only: it never goes on the wire, the board shows only "Page x of y", and a label edit is not a layout change. Renaming a page means editing its label, so its id, and the page a board is showing, stay put (open question A, settled 2026-10-22).
- One button per cell per page. A button without `page` counts as being on the first page before cells are checked, so it collides with a button that names that page explicitly. Button ids stay unique across all pages.
- At most 40 buttons with a `tile` across all pages. That is today's ceiling, one per cell of 8×5, now written as a rule. It keeps polling load where desktop-app-v1 S6 tested it.
- `version` stays 1. The new keys only add to the format. Today's app refuses a file that uses them with "unknown field", through `deny_unknown_fields` (`config.rs:69`, `:88`).
  - While that app is running, it keeps its last good config.
  - When it starts against such a file, it serves an empty grid (`config.rs:638`, `:683`). Nothing persists a last good config across restarts.
- `Config` gains `#[serde(default, rename = "page", skip_serializing_if = "Vec::is_empty")] pages: Vec<PageConfig>`, and `ButtonConfig` gains `page: Option<String>` (skipped when `None`, placed before `action` because TOML wants plain values before tables). A file without pages serializes as it does today.
- Validation errors name the page as `page "media": ...`, like `button "x": ...`. A button's bad reference reads `button "x": page "y" is not one of the [[page]] ids`. Counts read `5 pages, at most 4` and `41 tiles across all pages, at most 40`.

### Wire

Pages is a capability on the list fonts built (F21), not a new `v`:

- The board names `pages` in `hello.caps`, beside `fonts`. `v` stays 1, TXT `v` stays `1` (F2), and the pairing label stays `watchdeck-pair-v1`.
- **A config of 2 to 4 pages, to a session whose hello names `pages`:** one `layout` message per page, back to back, all with the same `rev`:

  ```json
  {"t":"layout","rev":7,"page":1,"pages":3,"page_id":"media","cols":5,"rows":3,"font":"inter","buttons":[
    {"id":"plex","col":0,"row":0,"label":"Plex","bg":2001125,"fg":16777215}]}
  ```

  - `page` is 0-based, `pages` is 1 to 4, and `page < pages`. `page_id` is 1 to 63 bytes. The daemon sends 2 to 4; the board accepts 1 too.
  - Every page message carries the grid's `font` and the per-button font fields under the fonts rules, so each page stands alone. Without `fonts`, they are stripped as today.
  - Pages go out in order, 0 first. Each one stays a full layout of at most 40 buttons and 16 KB (F5, F6). The serve loop sends a set from one `select!` branch, so nothing interleaves inside a set.
- **A config of 0 or 1 page goes out as today's single `layout`**, without page fields, to every session. That keeps the bytes of a file without pages unchanged.
- **To a session without `pages`** (today's firmware), the daemon sends today's `layout`: the home page only. Its tiles and `icon_get` answers cover that page only. When the config has 2 or more pages, the daemon logs `<device>: firmware without pages, showing page 1 of N` once per session. An edit confined to the other pages pushes it nothing.
- The board accepts both shapes. A `layout` without `page` is a set of one page, which is how the new firmware runs against today's daemon and against a config without pages.
- `rev` keeps its meaning: one more per push on the session (F18). A push is the whole set.
  - The board collects a set into its own staging copy, with a mask of the pages received. It applies the set only when every page from 0 to `pages - 1` is in and the `pages` counts agree.
  - A page of a newer `rev` throws away a set still arriving.
  - Staging, `rev` and ack state belong to one connection (`s.gen`). A reconnect clears them, since every session starts again at `rev` 1 (`server.rs:739`). The current `page_id` is kept apart from them.
- **Recovery.** A page can be lost (F27). If a set breaks (a page out of order, counts that disagree, a page that won't parse) or isn't complete 5 s after its first page, the board drops the link and reconnects, and the new session sends a complete set. Nothing new goes on the wire for this. So that a deterministic fault can't spin, a broken set counts as a failed attempt: the next `welcome` doesn't reset the backoff until a set completes (today `welcome` resets it, `link.c:1011`).
- `layout_ack {rev}` goes out once per set, right after the board has applied it and built the current page, and **before** any `icon_get` (F26). That holds for a single `layout` too. `protocol.md:285` changes to say so. The daemon's ack rules don't change.
- **`icon_get`** keeps its limit of 40 ids, and the board keeps one request outstanding. Each request takes up to 40 missing ids: the current page's first, then the other pages' in order. The next goes out when every frame of the last has arrived, or after 5 s with no frame for it (the wait restarts on each frame; changed after the S5 baseline, below). A new set cancels what is left and sends its own request at once, so frames of the old request may still be on the way, exactly as when today's firmware gets a new layout. Apart from that a burst stays at 40 frames, and a set whose pages miss 40 icons or fewer in all takes one request. The daemon answers from every page of the session's view.
- The daemon sends `tile` for every tile button on every page of the session's view, as today.
- What a page is called and where the board is never goes on the wire. "Page x of y" is drawn from `page` and `pages`.

### Geometry

The label needs a strip under the grid. Two options:

| | Shrink always | Shrink only with 2 or more pages (chosen) |
|---|---|---|
| A file without pages | Smaller cells and icons, new icon ids, one `icon_get` of everything after the upgrade | Unchanged: same cells, icon sizes, icon ids and wire bytes |
| New firmware, today's daemon | Broken: today's daemon renders icons for the taller cell, so they overflow the shorter one (5×3: 44 px icons in a 33 px box) | Unchanged: today's daemon sends no `page`, so the board lays out as today |
| Old firmware, new daemon, paged config | Icons smaller than their box | The same: icons rendered for the paged box, shown smaller than the full-height box. Documented, and the Devices note says why |
| What sizes an icon | `(cols, rows)` | `(cols, rows, paged)`, where `paged` means 2 or more pages |
| Going from 1 page to 2 | Nothing moves | Every icon box changes, so every icon gets a new id and one `icon_get`, as a rows change does today |

The goal says a file without pages works exactly as today, and the second row is a compatibility promise, so the grid shrinks only with 2 or more pages.

- **Strip**: `PAGER_H` 31 px = the slot-24 line box (27, `fonts.h:12`) plus 4 px of margin below it, so descenders clear the bezel. The grid's own trailing gap of 8 px or more (cell heights reserve `rows * GRID_GAP`) separates the last row from the label.
- **Label**: a child of `s_grid`, so it dims with the grid while offline, a swipe on it reaches the grid (it isn't clickable, so the press lands on `s_grid`), and `ui_set_paired(false)` deletes it with the grid. Full width, centered, text `Page %u of %u`, in `wd_font(layout font, WD_SLOT_24)`, grey like the status text. Grid-relative y 365 to 392, screen y 569 to 596. Hidden with one page.
- **Paged grid height**: 396 - 31 = 365. Cell height `(365 - rows * 8) / rows`.

| Rows | Cell height | Label lines | Icon box | Paged cell height | Paged label lines | Paged icon box |
|---|---|---|---|---|---|---|
| 1 | 388 | 2 | 256 | 357 | 2 | 256 |
| 2 | 190 | 2 | 110 | 174 | 2 | 94 |
| 3 | 124 | 2 | 44 | 113 | 2 | 33 |
| 4 | 91 | 1 | 46 | 83 | 1 | 38 |
| 5 | 71 | 1 | 26 | 65 | 1 | 20 |

- The width cap doesn't change: cell width less padding is 1000, 492, 322, 238, 187, 153, 129 and 111 px for 1 to 8 columns, so one row of 4 or more columns is bound by width in both cases (238 px at 4 columns).
- **The slots still fit on every paged grid.** A tile's value stays at slot 32 on 1 to 4 rows (83 ≥ `TILE_BIG_MIN_H` 80) and slot 24 on 5 rows, as today. 5 rows: a tile without an icon needs 27 + 2 + 27 + 8 = 64 of 65 px, a tile with an icon 20 + 2 + 27 + 8 = 57, an icon button 20 + 2 + 35 + 8 = 65. 4 rows: a tile with an icon needs 38 + 2 + 35 + 8 = 83 of 83, the same exact fit as today's 46 + 2 + 35 + 8 = 91. Wrapping labels without an icon overflow on 5 rows today already, and that stays as it is.
- On 5 rows with pages the icon is 20 px, under `ICON_MIN` 24. `ICON_MIN` only decides 1 or 2 label lines, so nothing breaks, but the icons are small (open question B).
- **Where the constants move**:
  - Firmware: a new `firmware/main/grid.c/.h`, with no LVGL, holds `GRID_TOP`, `GRID_GAP`, `ICON_PAD`, `ICON_GAP`, `ICON_MIN`, `PAGER_H` and `SLOT24_H`/`SLOT32_H` (moved from `fonts.h`, which includes `grid.h`), plus `grid_geometry(cols, rows, paged)`, which returns the cell size, label lines, icon side and grid height. `ui.c` calls it and checks `GRID_SCREEN_W`/`H` against `DISPLAY_H_RES`/`V_RES` with a `_Static_assert`. The host test build compiles `grid.c`, so the table gets a firmware test for the first time (F23).
  - Daemon: `icons.rs` gains `PAGER_H` 31, and `box_px(cols, rows, paged)`. `Icons::load` passes `config.pages.len() >= 2`.
  - Preview: `preview.ts` gains `PAGER_H` 31, and `cellSize`, `labelLines`, `iconBox`, `cellAt` and `layoutButton` take `paged` (default false). `GRID_H` stays the stage height, and the preview draws the label in the strip.
  - Docs: `config.md:131` gets the paged numbers beside today's, and the Pages section says why icons get smaller with pages.
  - The table above is pinned by one test per side: `icons.rs` `the_box_follows_the_firmware_button`, `preview.test.ts`, and a new host test over `grid_geometry`.

### Board

- **Model.** A new `model.c` holds the pages of the applied set, the latest tile value for every tile button, and the icon cache, all in PSRAM behind one mutex. The link task's staging copy sits outside it until the set is complete, then swaps in under the lock.
- **Set assembly** is a pure function in a new `pageset.c/.h` (no LVGL, no FreeRTOS) over parsed pages: received mask, counts, newer `rev`, duplicates. The host tests compile it.
- **Navigation is local and stays in the LVGL task.** A swipe builds the target page straight from the model. It takes the model lock inside the LVGL lock, and reads buttons, icons and tile values. It doesn't go through the link queue (F28).
  - Lock order is always LVGL, then model. A link-task path that needs both takes the LVGL lock first. That includes `icons_retain`, since `drop` takes the LVGL lock (F25). The link task never calls a UI callback while it holds the model lock.
  - This replaces the link-task-only rule of `icons.h:10` (F9). Icons are still freed only after the UI has replaced the widgets that used them (`icons.h:21-25`). After a set applies, the link task frees the icons the new set doesn't name, after `on_set` has rebuilt the current page.
  - Navigation goes by `page_id`. A set without the current id shows the home page.
  - A build holds the model lock for its whole run, so the link task waits on it to store a tile. S6 logs each build's time. If one passes about 100 ms, copy the page out under the lock and build without it, so the websocket handler's 500 ms wait (F27) never comes near.
- **Swipe.** `s_grid` clears `LV_OBJ_FLAG_GESTURE_BUBBLE` and handles `LV_EVENT_GESTURE` (F14). Left goes to the next page, right to the previous. Navigation stops at the ends, with no wrap.
  - `s_grid` keeps its full size, `DISPLAY_V_RES - GRID_TOP` (`ui.c:402`). Only the cell math uses the paged 365 px, and the label sits inside it.
  - `build_grid` clears `LV_OBJ_FLAG_SCROLLABLE` on `s_grid`. `lv_obj_create` makes it scrollable (`lv_obj.c:1499`), and an object that scrolls gets no gesture (`lv_indev.c:1835`), so a one-pixel overflow from the label would turn every swipe into a scroll. Clearing the flag makes F12's "never scrolls" independent of the arithmetic.
  - `s_grid` stays clickable, as `lv_obj_create` makes it (`lv_obj.c:1496`), so a press in a gap or on the label strip lands on it and its gesture fires.
  - The handler calls `lv_indev_wait_release(lv_indev_active())`, so the release presses nothing (F12, F13). That includes a swipe outward at either end.
  - It defers the rebuild with `lv_async_call`, so the pressed widget isn't deleted inside its own event. The deferred call carries the direction, not a target index, and resolves it against the current `page_id` and the model when it runs, since a new set can apply in between.
- **Offline.** A rebuilt page keeps the link state. While the link is down it's dimmed like the old grid (`ui.c:540-549`), and a page build must not set `s_online` the way `ui_apply_layout` does (`ui.c:485`). A swipe works offline. A tap on a button stays blocked by the offline check (`ui.c:358-361`).
- **Current page.** It's kept by `page_id`. A new `rev` keeps the page with that id, or shows the home page if it's gone. A reconnect keeps it. A set of one page (a legacy `layout`) has an empty id.
- **Geometry.** `build_grid` takes the page and the page count, and uses `grid_geometry(cols, rows, pages >= 2)` (Geometry above).
- **Icons.** `icons_retain` runs against every page of the complete set. `ICONS_MAX` becomes `PROTO_MAX_PAGES * PROTO_MAX_BUTTONS` = 160. The table moves to PSRAM with one `heap_caps_calloc` at start, since `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` is off (`sdkconfig:2353`), which keeps about 9 KB of internal RAM free. The byte cap stays at 8 MiB. The worst valid set is 4 pages of 4×1, 16 icons of 238 × 238 × 4 bytes, 3,625,216 bytes in all, so a valid config never reaches the cap. A unit test pins that bound. `sync_icons` moves its `seen` list and request buffer off the link task stack (F8).
- **Tiles.** A `tile` updates the stored value. If the button is on screen it also updates the widget. Building a page shows the stored values at once (F11). The store holds 40 values; a 41st is logged and dropped.
  - Applying a set or reconnecting clears every stored value, because the daemon resends each tile after every layout (`server.rs:774-777`). Those tiles follow the set's last page from the same `select!` branch, so they never arrive while the set is still in staging.
  - A source edited under the same id keeps showing its old value until the new source's first reading. That's how a visible tile behaves today, since the daemon withholds old readings (`server.rs:681-684`) but sends nothing to blank one.
- **Results.** A pending press keeps its button's label, so the modal names it after a page switch (F20).
- **LVGL memory.** Only the visible page has widgets. A page switch runs `build_grid` for that page, which costs what a layout push costs today. S5 measures a full 8×5 page first, then repeated switching, using the heap line `ui_apply_layout` already logs (`ui.c:489-491`).
- **PSRAM.** The applied set and the staging copy are about 6 KB a page each (`proto_layout_t`, 40 buttons of about 152 bytes), so about 49 KB for both at 4 pages.

### Daemon

- `Live::layout()` returns the pages: each page's id and buttons, the grid fields, and the icons they name. `Icons::load` renders at `box_px(cols, rows, pages >= 2)`.
- Each session takes a view from it, extending F22: `without_fonts()` without `fonts`, then `home_only()` without `pages`. `home_only` keeps page 0 and only the icons it names. The session compares views to decide whether to push, so an edit to page 3 sends nothing to an old board.
- `Layout::messages(rev)` gives one message for a view of one page (today's shape), or one per page with `page`, `pages` and `page_id`.
- `TileFeed` and `icon_get` work from the session's view (F17). The tile manager doesn't change (F15).
- `CAPS` becomes `[CAP_FONTS, CAP_PAGES]`. `serve` takes the session's caps instead of `fonts: bool`.
- `DeviceInfo.caps` already carries `pages` once the board names it, so `commands.rs` and `api.ts` don't change.
- A press from an old board for a button on a page it can't see runs like any press. Only an authenticated device can send one, and it can already press any button.

### Config UI

- Page tabs above the grid editor, shown once there are 2 pages: add, rename (edits the optional `label`, which the tab shows, falling back to the id; the id stays, so a board on that page stays there and nothing is pushed), reorder with move left and right, and delete. Delete asks first and names the buttons it removes. Add is off at 4 pages. Adding the first extra page turns the implicit page into `[[page]] id = "main"`. Deleting back to one page removes the `[[page]]` tables and every `page` key, and the file goes back to the old form.
- The grid editor and preview show the selected page, with the paged geometry and "Page x of y" in the strip when there are 2 or more pages. ButtonForm gets a page select.
- `layoutChanged` (`edit.ts:93`) counts the page ids in order and each button's page. `layoutTargets` (`edit.ts:125`) compares each device's view, as the daemon does: the full layout for a device with `pages`, else the home page with the `paged` flag (icon ids depend on it), with fonts stripped for a device without `fonts` (F24).
- `errors.ts` routes `page "x": ...` to the page tabs, `button "id": page ...` to the page select, and the count messages to the grid panel. `replaceNotice` (`saveflow.ts:48`) names the page count when there are 2 or more.
- The Devices tab marks a connected board without `pages` as "shows page 1 only. Update its firmware", beside the fonts note (`Devices.tsx:78-82`).

## Rejected alternatives

| Option | Why not |
|---|---|
| Bump `v` to 2 in `hello` | Today's daemon refuses it (F1). The board would need a downgrade retry and state about which daemon spoke what |
| `hello` `vmax` with the version echoed in `welcome` | Works, but it orders features in a line. The capability list fonts built lets each feature stand on its own |
| Advertise TXT `v=2` | Hides the daemon from today's boards when they pair (F2) |
| All pages in one `layout` message | Breaks the 16 KB cap with two full pages (F5), and the parse multiplies internal-heap use (F6) |
| A per-page `rev` with only changed pages resent | Needs per-page acks and partial updates on the board. Resending every page costs at most about 54 KB on the LAN per edit |
| Navigation that asks the daemon for each page | A round trip per switch and per-session page state on the daemon. Presses need the daemon anyway, but looking at pages shouldn't |
| `lv_tileview` holding every page's widgets | Builds up to 160 buttons in a 64 KB pool that one full page may already fill |
| A page bar in the right column (the old plan's choice) | User decision 2026-10-21: swipe only, with a small "Page x of y" label under the grid |
| Page-link buttons and `Action::Page` | User decision 2026-10-21 |
| `to` on the wire for page links | User decision 2026-10-21: no page links, so nothing reads it |
| Up to 8 pages | User decision 2026-10-21: at most 4 |
| Wrapping from the last page to the first | User decision 2026-10-21 |
| Poll only the tiles of visible pages | User decision 2026-10-21: all pages poll. It would also need the board to report its page, per-session state and a union across boards |
| `page_label` on the wire | Nothing on the board shows a page's name since the page bar went. The `[[page]]` `label` stays in the config for the editor's tabs only (open question A) |
| Shrinking the grid for the label always | Changes every existing config's icons and cells, and breaks new firmware against today's daemon (Geometry) |
| The label as a sibling of `s_grid`, made once | A swipe on the label strip wouldn't reach the grid, and offline dimming would need a second widget |
| Nested `[[page.button]]` tables | Every reader of `config.buttons` would change: tiles, icons, secrets, presses. Long table names in TOML too |
| Per-page grid sizes now | `Icons::load` renders at one box per config (F16), so this needs an icon cache keyed by size. The wire already allows it later |
| Fetch a page's icons only when it is first shown | Pages first show labels alone and wait a round trip. The cache bound above makes fetching them all safe |
| Navigation through the link queue (`EV_PAGE`), with the icon cache kept link-task-only | Waits behind inline mDNS discovery, up to 13 s offline (F28). A blocking post from the UI would invert the lock order with `lvgl_port_lock` |
| Every page's `icon_get` sent at once | `s.want` tracks one batch and resets on each sync (`link.c:130`, `link.c:932`). Up to 160 frames at once against a 16-slot queue that drops after 500 ms |
| An application-level resend message for a lost page | A reconnect already yields a full set, so no new message is needed |

## Slices

| Slice | Kind | Depends on |
|---|---|---|
| S1 Wire contract | mechanical, both parsers | — |
| S2 Geometry contract | mechanical, three mirrors | — |
| S3 Config model | design judgment, daemon | S1, S2 |
| S4 Daemon sessions | high complexity, daemon | S1, S3 |
| S5 Board data path and page label | high complexity, firmware | S1, S2 (hardware check also S4) |
| S6 Swipe on the board | design judgment, firmware | S5 |
| S7 Config UI | design judgment, UI | S2, S3 |
| S8 Hardware run, compatibility, docs | operator plus scribe | S4, S6, S7 |

S1 stays a slice of its own although fonts already built `caps`: it is the contract that lets the daemon (S3, S4) and the board (S5) be built in parallel against the same fields and host cases.

**Waves**, each slice in its own worktree:

| Wave | Slices in parallel | Shared files to merge |
|---|---|---|
| 1 | S1, S2 | Both append `RUN_TEST` lines to `app_main` in `test_proto.c` (a one-line merge) |
| 2 | S3, S5 | None: S3 is `app/src-tauri` and docs, S5 is `firmware/` |
| 3 | S4, S6, S7 | None: S4 is `server.rs` and tests, S6 is `ui.c`, S7 is `app/src`. Merge S4 before S7: the daemon reports `pages` in `DeviceInfo.caps` only from S4 on, so S7's Devices note would mark every board until then |
| 4 | S8 | — |

S5 can be reviewed and committed on its host tests, its build, and a run against today's daemon. Its run against a paged daemon waits for S4. Every flash in S5, S6 and S8 needs the user and an explicit yes before `mise run fw:flash`.

### S1 Wire contract

- **Files**
  - `site/src/protocol.md`: status line, the `pages` row in the capability table (`:199-201`), the paged `layout` fields and set rules, what a 0- or 1-page config sends, the ack rule per set and before `icon_get` (`:285`), `icon_get` across pages (`:301-303`), what an old board gets, and that 4 pages is part of the capability. It also states the geometry rule, since icon sizes depend on it: a device lays out the shorter grid when `pages` is 2 or more, and the daemon renders that set's icons for it. So a later daemon that sent `page` 0 of 1 would render for the full-height grid.
  - `app/src-tauri/src/daemon/protocol.rs`: `CAP_PAGES` (not yet in `CAPS`; S4 adds it). `Message::Layout` gains `page`, `pages` and `page_id` as `Option`s, skipped when `None`.
  - `app/src-tauri/tests/fixtures/layout_page.json`: new, with `font` and one button with `label_font`. `layout.json` and `layout_fonts.json` stay as they are.
  - The one `Message::Layout` constructor, `Layout::message` (`config.rs:182-190`), sets the new fields to `None`. Every `device_sim` pattern already ends in `..`, and `examples/fault_daemon` doesn't name the variant.
  - `firmware/main/proto.h`, `proto.c`: `PROTO_MAX_PAGES 4`. `proto_layout_t` gains `page`, `pages` and `page_id[PROTO_ID_MAX]`. `parse_layout` reads them when `page` is present and gives page 0 of 1 with an empty id when it isn't.
  - `firmware/test/host_proto/main/test_proto.c`: new cases.
  - `link.c` doesn't name `pages` yet. A build that did, before S5 can assemble a set, would apply each page as a whole grid and ack the `rev` early (`link.c:1041-1052`).
- **Host cases**: no `page` (0 of 1, empty id); `page` 3 of 4; `page` equal to `pages`; `pages` 0 or 5; `page` without `pages` or without `page_id`; `pages` without `page`; `page_id` empty, of 63 bytes and of 64 bytes; `page` as a string; a page message with `font` and font fields resolves them as a single layout does.
- **Acceptance**: every fixture round-trips. The old-shape and fonts-shape layouts still parse on both sides with the same result. The board refuses each malformed case. `cargo clippy -- -D warnings` and CI are green.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml protocol` and `mise run fw:test`.

#### S1 as built (2026-10-22)

- `protocol.md` specifies the `pages` capability, the paged `layout` fields, the set rules (complete before apply, a newer `rev` discards, a set never spans connections, a broken or 5 s incomplete set reconnects and keeps the backoff), 0- and 1-page configs as today's single `layout`, the geometry rule, the 4-page limit, and `layout_ack` before `icon_get` for a device that names `pages`. Earlier firmware still acks after its `icon_get`.
- Daemon: `CAP_PAGES` exists but isn't in `CAPS`. `Message::Layout` has `page`, `pages` and `page_id`, skipped when `None`. The `layout_page.json` fixture round-trips.
- Firmware: `PROTO_MAX_PAGES` 4. `parse_layout` requires `page`, `pages` and `page_id` together (a lone `pages` or `page_id` is refused) and resets them on a plain layout. That's 11 new host cases, 60 in all.
- Departure: `examples/fault_daemon/session.rs` builds `Message::Layout` without `..`, so it needed the three `None`s too. The S1 file list was wrong about it.
- `link.c` doesn't advertise `pages`, so no board or daemon behaves differently.

### S2 Geometry contract

Behavior-neutral: every caller passes "not paged" until S3, S5 and S7.

- **Files**: `firmware/main/grid.c` and `grid.h` (new), `fonts.h` (the slot constants move to `grid.h`), `ui.c` (`build_grid` calls `grid_geometry(cols, rows, false)`; its local constants go), `firmware/main/CMakeLists.txt`, `firmware/test/host_proto/main/CMakeLists.txt` (adds `grid.c` and `test_grid.c`), `test_grid.c` (new) and two lines in `test_proto.c` to run it; `app/src-tauri/src/daemon/icons.rs` (`PAGER_H`, `box_px(cols, rows, paged)`, `Icons::load` passes `false`, the comment at `:156-159` and the doc at `:173-176`); `app/src/preview.ts` and `app/tests/preview.test.ts`; `site/src/config.md` (`:131`, the paged numbers).
- **Tests**
  - The Geometry table, row for row and both columns, on all three sides: cell height, label lines and icon box for 1 to 5 rows, and the width cap for 1 to 8 columns.
  - Rust, the cache bound: for every grid from 1×1 to 8×5, 4 × cells × `box_px(cols, rows, true)`² × 4 and 1 × cells × `box_px(cols, rows, false)`² × 4 are each at most 8 MiB, with the worst case 3,625,216 printed. A comment names `icons.h` `ICONS_MAX_BYTES` and `PROTO_MAX_PAGES` as the board's side.
  - Firmware: for every paged grid, a tile without an icon, a tile with an icon and an icon button each fit their cell (the sums in Geometry).
  - The existing tests pass unchanged with `paged` false: `the_box_follows_the_firmware_button` and `preview.test.ts`.
- **Acceptance**: `mise run fw:build` gives the same `cells WxH, icon box N px` log line as before for a given grid (a diff of `build_grid` behavior is none), `mise run fw:test`, `cargo test`, `pnpm test` and rumdl pass.
- **Cheapest check**: `mise run fw:test`, `cargo test --manifest-path app/src-tauri/Cargo.toml icons::tests::the_box`, and `pnpm test` in `app/`.

#### S2 as built (2026-10-22)

- `firmware/main/grid.c` and `grid.h` hold the geometry constants and `grid_geometry(cols, rows, paged)`, with no LVGL. `SLOT24_H`, `SLOT32_H` and `TILE_BIG_MIN_H` moved there; `fonts.h` includes `grid.h`. `ui.c` calls `grid_geometry(..., false)` and `_Static_assert`s the 1024×600 screen.
- `icons.rs` has `PAGER_H` 31 and `box_px(cols, rows, paged)` over a private `cell_geometry`, and `Icons::load` passes `false`. `preview.ts` has `PAGER_H`, and `cellSize`, `iconBox`, `cellAt` and `layoutButton` take `paged = false`. `labelLines` keeps its `Cell` argument, whose height already carries `paged`.
- The Geometry table is pinned on all three sides (`test_grid.c`, three `icons.rs` tests, `preview.test.ts`) and matched the plan in every cell. The cache-bound test prints 3,625,216 bytes. `test_grid.c` runs the fit sums over all 80 grids.
- Unpaged output is unchanged: `grid_geometry` agreed with the old `build_grid` arithmetic on all 40 grids in a host comparison, and the old tests pass as they were. Not yet checked on the board.
- `config.md` documents the paged icon boxes (256, 94, 33, 38, 20) ahead of S3, which makes pages reachable.

### S3 Config model

- **Files**: `app/src-tauri/src/daemon/config.rs`, `icons.rs` (`Icons::load` passes `config.pages.len() >= 2`), `server.rs` (only to keep sessions on the home page view: see below), `site/src/config.md` (a Pages section: the keys, the rules, that the grid shrinks for the label with 2 or more pages and what that does to icon sizes, the tile cap, the downgrade note).
- **Changes**
  - `PageConfig { id, label: Option<String> }` (label printable ASCII, at most 24 characters; config only, not in `Live::layout()` or any view), `Config.pages`, `ButtonConfig.page`, `MAX_PAGES` 4, `MAX_TILES` 40.
  - `check`: the rules under Config. Cells are checked per page. Ids are unique per kind. `page` must name a page, and is refused without `[[page]]` tables.
  - `Live::layout()` builds the pages. The first page is the implicit one when no `[[page]]` exists. `Layout` gains its pages (each an id and its buttons), `home_only()` and `messages(rev)`. `without_fonts()` works across pages.
  - `server.rs`: the view becomes `without_fonts` as today, then `home_only()` for every session, since no session can name `pages` until S4. That's already the right behavior for every board in the field, and `TileFeed` iterates the view's buttons.
- **Tests**
  - Today's example and `DEFAULT` parse, and saving them writes no `page` key and no `[[page]]` table. They parse back equal. A save always adds its header and drops comments (`config.rs:762`), so the test checks equality, not bytes.
  - Round trip (serialize, parse, compare): several `[[page]]` and `[[button]]` tables; implicit and explicit membership; a button with HTTP headers, a tile and every font key.
  - Every new rule has a negative case:
    - a `page` with no `[[page]]`, or an unknown page
    - a duplicate, empty or 64-byte page id, or one with a space
    - 5 pages
    - two buttons in one cell of one page (the same cell on two pages passes), including a button without `page` colliding with one that names the first page
    - 41 tiles across pages, and 40 passing
  - A config without pages gives the same `layout` JSON as on `f75bf5c`, with and without fonts.
  - The fullest page fits in 16 KB: 4 pages of 8×5, every id 63 bytes, the fullest page's message with `page` 3, `pages` 4, a 63-byte `page_id` and every font field at its longest. Expected about 13,377 bytes. Extends `config.rs:1597`, and prints the size.
  - `Icons::load` renders the same file at 44 px for one page and 33 px for two on 5×3 (a 2-page config, then the same with one page).
  - `home_only` keeps only the icons page 0 names.
- **Acceptance**: `cargo test` and clippy are clean. A config with pages survives a save and load unchanged. A file without pages saves with no page keys. Every existing `device_sim` test passes unchanged.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml config`.

#### S3 as built (2026-10-22)

- `config.rs`: `PageConfig`, `Config.pages`, `ButtonConfig.page`, `MAX_PAGES` 4, `MAX_TILES` 40, `MAX_PAGE_LABEL` 24. `Layout` holds `pages: Vec<LayoutPage>` and has `buttons()` (a flat iterator), `messages(rev)`, `home_only()` and a page-wide `without_fonts()`. `message(rev)` is gone.
- Any layout of exactly one page has an empty page id, in `Live::layout()` and in `home_only()`. So adding a single `[[page]]` table, or renaming page 0, pushes nothing to a one-page view.
- `server.rs`: every session's view is `without_fonts` (without that cap), then `home_only()`. It sends `messages(rev)` in a loop, and `TileFeed` iterates `layout.buttons()`. `CAPS` is unchanged.
- `Icons::load` renders at `box_px(cols, rows, pages >= 2)`: 44 px for 0 or 1 page and 33 px for 2 on 5×3.
- The fullest page message is 13,377 bytes, as estimated. A config without pages sends byte-identical JSON, with and without fonts, pinned by full literals.
- Departures:
  - `page` with no `[[page]]` tables reads `button "x": page "y" is set, but the file has no [[page]] tables`.
  - A button naming an unknown page gets that one error and no collision entry.
  - An empty `label` is refused (`page "x": label is empty; leave it out to show the id`).
- Tests: 199 lib tests and 59 integration tests pass.

### S4 Daemon sessions

- **Files**: `app/src-tauri/src/daemon/server.rs`, `protocol.rs` (`CAPS` gains `CAP_PAGES`), `tests/device_sim.rs`, `site/src/protocol.md` only if S1's text needs a fix.
- **Changes**: the session keeps its caps. They are moved into `shared.authenticate` (`server.rs:613`), so take a copy, or both flags, before that call, as `fonts` is taken today (`:612`). Its view adds `home_only()` only without `pages`. It sends `messages(rev)` for each push with one `rev`, answers `icon_get` from the view, feeds tiles from the view, and logs an old board once per session when the config has 2 or more pages.
- **`device_sim` tests**
  - A hello without `caps`, against a 3-page config, gets today's `layout` shape. It holds the home page only, with no `page` fields. Tiles come for home-page buttons only. `icon_get` for a page-2 icon gets nothing. The log line appears once.
  - A hello with `["fonts","pages"]` gets three `layout` messages, `page` 0, 1 and 2 with one `rev` and the same `font`, then the tiles of every page. `icon_get` for a page-2 icon gets its frame.
  - A hello with `["pages"]` gets the three messages with no font fields.
  - An edit to page 2 pushes `rev` 2 to the `pages` session. The other session gets nothing and stays on `rev` 1.
  - Going from 1 page to 2 pushes to both sessions, and a home-page button's icon id changes.
  - `layout_ack {2}` after the set gives `LayoutApplied(device, 2)`.
  - The ack comes before uncached icons: a `layout_ack` sent before an `icon_get` of 40 ids gives `LayoutApplied` before the frames finish.
  - A config without pages gives a `pages` session one message without `page` fields, byte for byte what the `f75bf5c` daemon sent.
- **Acceptance**: every existing `device_sim` test passes unchanged. The new tests fail on the S3 build, so they tell the two apart.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml --test device_sim`.

### S5 Board data path and page label

- **Files**: `firmware/main/model.c` and `model.h` (new), `pageset.c` and `pageset.h` (new), `link.c`, `link.h`, `icons.c`, `icons.h`, `ui.c`, `ui.h`, `main.c`, `CMakeLists.txt`, the host test build (`pageset.c`), and `sdkconfig.defaults` only if the memory step calls for it.
- **Changes**
  - Set assembly in the link task:
    - Staging with a received mask, through `pageset`.
    - Per-connection state.
    - The 5 s and broken-set reconnect, with the backoff kept until a set completes.
    - The swap into the model.
    - `layout_ack` before the first `icon_get`, for a set and for a single `layout`.
    - One outstanding `icon_get` of up to 40 ids, current page first, with a 5 s timeout and cancellation on a new set.
  - `hello` sends `{"fonts","pages"}` (`link.c:708`), in the same commit as the assembly.
  - `on_layout` becomes `on_set` (rebuild the current page). Page builds run in the LVGL task, with the lock order from the Board section.
  - The model holds the tile values and the icon cache. The icon table moves to PSRAM with `ICONS_MAX` 160, and `icons_retain` runs against the whole set under the LVGL lock, then the model lock. `sync_icons` takes its lists off the stack (F8).
  - `build_grid` builds one page with the paged geometry and the "Page x of y" label. `s_grid` keeps its full height and loses `LV_OBJ_FLAG_SCROLLABLE` (Board, Swipe). A pending press keeps its label. A rebuilt page keeps the offline dimming.
- **Host cases** (`pageset`): a missing middle page, a missing last page, a count change mid-set, a newer `rev` mid-set, a duplicate page, an older `rev` after a newer one, a single `layout` while a set is arriving, and a complete set of 4.
- **Memory baseline first**, on the `f75bf5c` firmware against the `f75bf5c` daemon. It's a baseline, not a gate. If the board doesn't run `f75bf5c` yet, flash it first, with the user's yes.
  - Two fixtures, each with distinct icon hashes:
    - most widgets: 8×5 with 40 icons and 40 tiles on one page
    - most pixels: 4×1
  - Sample once every icon has arrived. Open a result modal with 1 KB of text, then record the `LVGL heap` line, the largest free block, and the min free internal heap. Today's heap line logs before icons arrive (`ui.c:481-499`), so add a settled sample.
  - If the pool is short, raise `CONFIG_LV_MEM_SIZE` in `sdkconfig.defaults` and check the generated `sdkconfig`. A bigger pool is static internal RAM, so the internal-heap baseline (300,060 bytes after fonts) moves with it. Record the new baseline.
  - Repeat with pages and the label.
- **Acceptance (serial log)**
  - Against the S4 daemon with a 3-page config: `layout rev N: 3 pages applied, showing page 1`, then `layout_ack`, then `icon_get` requests one at a time, the current page's ids first.
  - The geometry line shows the paged numbers (`icon box 33 px` on 5×3), and the board shows "Page 1 of 3".
  - A tile for a page-3 button logs that it was stored, not `ignored`.
  - The link task stack high-water mark keeps at least 2 KB free after the S8 step 6 workload.
  - Against the `f75bf5c` daemon (S8 step 8) the board shows one page, no label, and today's geometry (`icon box 44 px` on 5×3).
- **Cheapest check**: `mise run fw:test`, `mise run fw:build`, then the board against the S4 daemon with a 3-page config.

#### S5 as built (2026-10-22)

Code, host tests and build only. The serial-log acceptance and the memory baseline still wait for the board, with the user.

- **New files**:
  - `pageset.c/.h`: set assembly with no LVGL, FreeRTOS or clock (the caller passes `now_ms`).
  - `model.c/.h`: one mutex over the applied set, 40 tile slots and the shown page id, in PSRAM. `model_apply` swaps storage pointers with the staging area, so nothing is copied under the lock.
- **`link.c`**:
  - `on_set` and `on_icons_settled` replace `on_layout`, and `hello` names `{"fonts","pages"}`.
  - A broken set, or one still incomplete after 5 s, drops the link ("layout set broken"). `welcome` and `EV_GOT_IP` reset the backoff only after a set has completed.
  - `layout_ack` goes out after the apply and page build and before any `icon_get`.
  - One `icon_get` of up to 40 ids is outstanding at a time, from a PSRAM queue with the shown page first. It times out after 5 s with no frame for it (the timer restarts on each accepted frame), and ids that timed out aren't asked for again for that set.
- **`icons.c`**: `ICONS_MAX` 160, with the table in PSRAM. `icons_retain` runs over the whole set.
- **`ui.c`**:
  - `ui_show_set` and `build_page`, with geometry from `grid_geometry(cols, rows, pages >= 2)`.
  - The "Page x of y" label is created only with 2 or more pages.
  - `s_grid` is no longer scrollable.
  - A pending press keeps its label.
- **Lock order as built**: the LVGL lock, then the model lock. No UI callback and no send runs under the model lock. `drop` re-takes the recursive LVGL lock.
- **Logs**:
  - `layout rev N: K pages applied, showing page X`
  - `tile for X stored` once per button per set
  - `LVGL heap (<when>)` at page built, icons settled and result shown. The settled sample is skipped with `icons gave up waiting (N missing)` after a timeout.
- **Departures**:
  - An icon frame is accepted for any id the applied set names. That's still bounded by the set and `ICONS_MAX`.
  - Each page builds from its own `cols` and `rows`.
  - Booting unpaired skips `icons_clear`.
- **Checks**: 78 host tests, 13 of them for `pageset`. The image is 1,887,824 bytes, 5,200 more than before. The link task stack stays 8192, and the 2 KB `icon_get` buffer, the `seen` list and the staging area all moved to PSRAM.

#### S5 memory baseline (2026-10-22)

The firmware was the f75bf5c build, already on the board, so no flash. The daemon was `mise run app:dev` at 74cc3cf, where S1 and S2 change no behavior. Each fixture had distinct SVG icons. A "settled" sample is the heap line of a second push, made after every icon had arrived, with all icons served from the cache. The old firmware logs the LVGL pool only when it applies a layout, and the internal-heap minimum only on the first layout after boot.

| Fixture | LVGL pool used, settled | Pool peak since boot | Pool size | Min free internal |
|---|---|---|---|---|
| 8×5, 40 icons (26 px), 40 tiles, one page | 44,676–45,696 | 48,440; 52,196 after a result screen with 1 KB of text | 59,832 | 280,648–280,720 (first layout, before icons) |
| 4×1, 4 icons (238 px) | 14,020 | 52,196 (left over from the 8×5 run) | 63,104 | not logged after boot |

- An 8×5 page with a 1 KB result screen peaks at 52,196 of 59,832 bytes (87%). That's about 7.6 KB of headroom, so `CONFIG_LV_MEM_SIZE` stays as it is. The S5 firmware builds only one page's widgets plus a label, so it should land within a few hundred bytes of this. The S8 run repeats the measurement with pages.
- The internal-heap minimum with 8×5 is 280,648, against 300,060 for the fonts baseline, which used the user's 5×5 config.
- Icons of 238×238 px arrive about one every 0.5 s (4 took 2.2 s), so a 4-page 4×1 set takes about 8 s in one request. That would have tripped a 5 s timeout counted from the request, so the timeout now restarts on each frame (Wire, and `protocol.md`).
- Opening the serial port with pyserial reset the board once more, as noted in CLAUDE.md. That reset lost the first result-screen peak, and the tap was repeated after it.

### S6 Swipe on the board

- **Files**: `firmware/main/ui.c`.
- **Changes**: the gesture handler on `s_grid`, with `lv_indev_wait_release` and the rebuild deferred with `lv_async_call`. It works before the offline check and sends nothing to the link. A switch logs `page 2/3 shown in X ms`, measured from the gesture to the end of the build.
- **Acceptance (on the board, from the serial and daemon logs)**: neither log shows a `press` for any swipe.
  - Swipes in both directions: some starting on a button, some kept inside one big button (a 2×1 grid), some starting in a gap, some on the label strip, and some going outward at each end, where they do nothing.
  - Plain taps still press. A tap that closes the result modal doesn't navigate.
  - A slow drag that isn't a gesture behaves as it does today.
  - No `icon_get` after any page switch.
  - A tile on page 3 shows its value at once on the first visit.
  - Pending result: press a slow button on page A, swipe to page B, and the result names A's button. Two slow presses whose results come back in reverse order each name their own button.
  - The label: "Page 2 of 3" in the grid's font with `layout.font` set to the widest font (Orbitron), nothing clipped, and the bottom row of buttons doesn't touch it.
- **Cheapest check**: the board, with `mise run fw:monitor` and the daemon log side by side.

### S7 Config UI

- **Files**: `app/src/config.ts`, `ConfigPage.tsx`, `GridEditor.tsx`, `ButtonForm.tsx`, `ButtonPreview.tsx` (if the strip is drawn there), `preview.ts` (the label), `edit.ts`, `saveflow.ts`, `errors.ts`, `Devices.tsx`, and `app/tests/*.test.ts`.
- **Acceptance**
  - Page tabs work: add (off at 4), rename, reorder, delete with confirmation, and move a button between pages, with a taken cell on the target page refused in the form.
  - Adding the first extra page writes `[[page]]` tables. Deleting back to one page removes them and every `page` key, and the file goes back to the old form.
  - The preview of a 2-page 5×3 grid shows 33 px icon boxes and "Page 1 of 2". The same grid with one page shows 44 px and no label.
  - Field errors from Rust land on the right page or button.
  - The save bar still reaches "applied" on the board, and doesn't wait on a board without `pages` for an edit it won't get.
  - TS tests cover `layoutChanged` with pages, `layoutTargets` (an edit on page 2 targets only `pages` devices; a home-page edit targets both; going from 1 to 2 pages targets both; a font-only edit on page 2 targets only devices with both caps), the error routing, and the page-to-implicit round trip.
- **Cheapest check**: `pnpm test`, `pnpm build`, then `mise run app:dev`. The window check needs the user, as desktop-app-v1 S7 and S9 did.

### S8 Hardware run, compatibility, docs

Run on the board with the dev daemon unless a step says otherwise. Evidence is the serial log (pyserial from the IDF Python env when there's no TTY) and the daemon log. "Old firmware" and "old daemon" both mean the build at the commit before S1: `f75bf5c` unless something else lands first, so record the hash actually used. Every flash needs the user's yes.

1. **Old file.** With today's `config.toml`, the board logs one page, shows no label and today's geometry, and the save bar still reaches "applied".
2. **Three pages**: 5×3, about 15 buttons each, icons on all three, two tiles on page 3.
   - Swipes switch pages, and "Page x of 3" follows.
   - Switching sends no `icon_get`.
   - Page 3's tiles show values on the first visit.
3. **Live edits**
   - Edit page 2 while it's on screen: the board stays on page 2 and shows the edit.
   - Delete page 2 while it's on screen: the board goes to the home page.
   - Move page 2 to the end while it's on screen: the board stays on it, and the label says "Page 3 of 3".
   - Go from 3 pages to 1: the label goes, the grid grows back, and one `icon_get` fetches the larger icons.
4. **Reconnect.** Restart the daemon with the board running. The board keeps its page and requests no icons.
5. **Link down.** Stop the daemon and swipe.
   - Every page shown stays dimmed, including while the board is running discovery.
   - Ordinary buttons send nothing and leave no pending entry.
   - Navigation latency stays under 200 ms.
6. **Memory**, with the two S5 fixtures spread over 4 pages: one with the most widgets (4 pages of 8×5, 160 distinct icons, 40 tiles), one with the most pixels (4 pages of 4×1, 16 distinct icons).
   - Switch through every page 3 times after a warm-up round, then push a new set while frames of the old one are still arriving.
   - Compare the LVGL heap in use (not `max_used`, which only rises) at the same settled point each round. Do the same for the largest free block, and for the PSRAM minimum and largest block.
   - The min free internal heap stays within 10 KB of the baseline S5 recorded.
   - Every icon logs `received, validated and cached`, and none `dropped`.
   - The link stack high-water mark is logged at the end.
7. **Old firmware, new daemon.** Flash the old firmware. The board shows page 1 only, at full height, with icons rendered for the paged box (smaller than today's). The daemon logs `firmware without pages`, and the Devices tab shows the note. An edit to page 2 leaves the save bar waiting on no one. Then flash the new build back. The pairing survives both flashes, since NVS is untouched.
8. **New firmware, old daemon.** Build the old app in a worktree.
   - Quit the tray app, then copy the config dir to a temp dir with `/usr/bin/cp -a` (`cp` is aliased to `-i`). The copy keeps `identity.pem`, `identity.key` and `state.json`, so the board's pin and token still match.
   - Check that the copied `identity.key` is still 0600 and owned by you. The daemon refuses to start otherwise (`plans/desktop-app-v1.md:312`).
   - Replace the copy's `config.toml` with one that has no pages, and run the old daemon with `XDG_CONFIG_HOME` pointing at the temp dir. The new board shows one page with no label and today's geometry.
   - Then put a paged `config.toml` in the copy and restart the old daemon. It serves an empty grid and logs the unknown field. That's the downgrade case.
   - The real config dir is never touched. Restart the tray app afterwards.

- **Docs**: `plans/desktop-app-v1.md` status and backlog, the Assumptions line at `:57`, `site/src/protocol.md` and `site/src/config.md` finished, `CLAUDE.md` (Daemon facts: up to 4 `[[page]]` tables and the `pages` cap; Hardware facts: the icon cache holds every page's icons), and a results section in this plan.
- **Acceptance**: every step passes, or its failure is recorded with a decision.

## Risks

| Risk | Mitigation |
|---|---|
| A swipe also presses the button it started on, running a shell or HTTP action (F12) | `lv_indev_wait_release` in the gesture handler (F13), with the rebuild deferred. S6's swipe cases check it on the board |
| The board, the daemon and the preview disagree on the paged geometry, so icons overflow or shrink | One table, pinned by a test on each side (S2), and `grid.c` gives the firmware its first host test of it |
| One full 8×5 page fills the 64 KB LVGL pool, or switching fragments it | S5 records a settled baseline (not a gate) and repeats it with pages. Raise `CONFIG_LV_MEM_SIZE` if the pool is short. S8 step 6 checks for drift |
| Link task stack overflow from per-set arrays (F8) | Arrays move off the stack. Log the high-water mark |
| An icon burst overruns the link queue, where the websocket handler waits 500 ms and then drops | One outstanding `icon_get` of at most 40 ids, as today, with a 5 s timeout. S8 step 6 counts drops |
| A lost or broken page leaves the board on an old set, since the daemon never resends an unchanged layout | A set that is broken or not complete within 5 s makes the board reconnect, and the new session sends a full set. S5's host cases cover assembly |
| A daemon bug that breaks every set makes the board reconnect in a tight loop | A broken set doesn't let the next `welcome` reset the backoff, so it backs off to 30 s |
| The 5 s drop runs the same teardown as any drop, and a daemon that stalled mid-frame wedges `esp_websocket_client_destroy` (CLAUDE.md, the wedge note) | No new exposure: such a peer wedges today's firmware the same way, and the keepalive in `open_client` stays. Recovery is an RTS pulse, which keeps the pairing |
| A long page build holds the model lock while the link task waits to store a tile, and the websocket handler drops a frame after 500 ms | S6 logs every build's time. Over about 100 ms, the build copies the page out under the lock instead |
| Navigation stalls behind mDNS discovery on the link task | Navigation runs in the LVGL task against the locked model and never uses the link queue. S8 step 5 measures latency during discovery |
| Lock inversion between `lvgl_port_lock` and the model lock, including through `icons.c` `drop` (F25) | One order, LVGL then model. The link task never calls a UI callback while it holds the model lock |
| The save bar waits forever on a board that gets no push (F24) | `layoutTargets` mirrors the daemon's per-session view. S7 tests it, and S8 step 7 checks it |
| Hidden-page tiles starve the 6 tile slots (`actions.rs:22`, `MAX_TILE_RUNNING`) and show `stale` | The cap of 40 tiles keeps the load desktop-app-v1 S6 tested, including its 3-skip `stale` rule |
| Going from 1 page to 2 changes every icon box, so every board refetches every icon | One `icon_get` per board, as a rows change today. Documented in `config.md` |
| A board on old firmware shows a paged config with smaller icons and page 1 only | Daemon log, the Devices tab note, and `config.md` |
| Downgrading the app with a paged `config.toml` serves an empty grid at startup | Documented in `config.md`. The fix is to remove the `[[page]]` tables and the `page` keys, or restore a `.bak` |
| The board's caps and the daemon's mirrored constants drift: 4 pages, 40 buttons, 40 tiles, 8 MiB, `box_px` | The S2 bound test, plus constants named the same on both sides, as `box_px` and `grid.c` will be |

**Rollback.**

- Each slice is its own commit, so `git revert` undoes one.
- The firmware rolls back by flashing the previous build. The partition table and NVS don't change, so the pairing survives.
- The daemon rolls back with a config that has no pages, since an older app refuses the new keys.
- The protocol change is additive. Each old peer works with each new one, so the board and the app roll back independently.

## Open questions

Settled by the user on 2026-10-21 and applied in this revision: navigation is swipe only with a "Page x of y" label under the grid (1, 5, 6); all pages' tiles poll (2); at most 4 pages (4); fonts first (9). No wrap at the ends (5). Questions 3, 7 and 8 keep their recommended defaults.

New in this revision, settled by the user on 2026-10-22:

| # | Question | Recommended default | Settled |
|---|---|---|---|
| A | Do pages have a display label? | No: `[[page]]` has only `id`; a rename changes the id, so a board on that page goes home | **Id plus an optional `label`**, config and editor only, never on the wire (see Config). S3 adds the field and its checks; S7 shows it on the tabs and edits it as the rename |
| B | With 2 or more pages the grid gives up the label strip; on 5 rows icons are 20 px (26 today). Shrink only with pages? | Yes, shrink only when there are 2 or more pages | **Yes, as recommended** |

The original table, for the record:

| # | Question | Recommended default then |
|---|---|---|
| 1 | How does a board move between pages? | All three: the page bar, a swipe, and page-link buttons. Settled: swipe only |
| 2 | Which tiles poll: the visible page only, or all? | All, with a cap of 40 tiles across pages. Settled: all |
| 3 | Can each page have its own grid size? | Not yet. One `[layout]` for every page. The wire already carries `cols` and `rows` per page |
| 4 | How many pages? | 8. Settled: 4 |
| 5 | Does navigation wrap from the last page to the first? | No. Settled: no |
| 6 | Where does the page bar go? | The right column. Settled: no page bar; a label under the grid |
| 7 | Does the current page survive a reboot? | No. NVS keeps holding only the pairing. A reconnect keeps it |
| 8 | What does a board on old firmware get from a paged config? | The home page, plus a log line and a Devices-tab note |
| 9 | Which lands first, pages or selectable fonts? | Pages. Settled: fonts, which has landed |

## Built on fonts

Fonts landed in `ffa76f9`, `d6f0abc`, `d1f54f5` and the fonts S4 commit (`feat(fonts): font pickers and font-accurate previews`), with `f75bf5c` adding the licenses page. What this plan reuses:

- **`hello.caps`** on both sides (F21). S1 documents `pages`, S4 adds it to `CAPS`, and S5 makes the board send `{"fonts","pages"}`.
- **The session view** (F22). S3 adds `home_only()` beside `without_fonts()`, and S4 applies it per session.
- **`DeviceInfo.caps`** and the Devices note (`Devices.tsx:78-82`). S7 adds a second note from the same list.
- **Font fields**: every page message carries `font`, and its buttons carry `label_font` and `value_font` under the fonts rules. The board's model stores the resolved bytes `parse_layout` already fills (`proto.c:306`, `:335-336`).
- **Fixed text slots** (`SLOT24_H` 27, `SLOT32_H` 35). The label's strip is built on `SLOT24_H`, so no font choice moves it, and S2 moves the two constants into `grid.h`.
- **The preview** (`preview.ts`, `lvtext.ts`, `fonts.ts`). S2 adds `paged` to the geometry functions, and S7 draws the label with `slotFont(layout font, 24)`.
- **The 16 KB test** already carries every font field (13,282 bytes). S3 adds the page fields to it.
- **The Licenses button** (`ui.c:25-29`, `:329-340`) sits in the status area, above the grid, so the label at the bottom never meets it.

## Critique log

### Revision 2026-10-22

No outside review this time. The consult tool refused both reviewers: the codex app server wasn't started locked down, and the agy reviewer's hook blocked its own file reads. So the critique below is a self-review that argued against each slice and the chosen approach, with each point checked against the code at `f75bf5c`.

| # | Point | Outcome |
|---|---|---|
| R1 | S1 listed `device_sim` and the fault daemon as breaking on the new `Layout` fields | adopted: only `Layout::message` (`config.rs:183`) constructs the variant. Every `device_sim` pattern ends in `..` (for example `:417-422`, `:1960-1965`) |
| R2 | `caps` is moved into `shared.authenticate` (`server.rs:613`), so `serve` can't simply take it | adopted: S4 copies it, or takes both flags, before the call |
| R3 | The geometry rule is part of the contract. A later daemon that sent `page` 0 of 1 must know the board lays it out at full height | adopted: S1's `protocol.md` text states the rule |
| R4 | A deferred page build captured as an index can land after a new set applied, and point past its end | adopted: the deferred call carries a direction and resolves it when it runs |
| R5 | With no page labels, a rename is an id change, so a board on that page goes home | adopted: written into open question A; the user then chose an editor-only `label`, so a rename keeps the id |
| R6 | "A burst stays at 40 frames" isn't true across a new set, whose request goes out while the old one's frames still stream | adopted: the text now says so. Not changed: today's firmware does the same on every new layout, and holding the request would delay the new set's icons |
| R7 | The 5 s incomplete-set drop calls the teardown that can wedge on a peer stalled mid-frame | adopted as a risk row. No new exposure, and the keepalive stays |
| R8 | A page build holds the model lock while the link task waits to store a tile, which could cost a frame at the 500 ms handler wait | adopted: S6 measures build time, with a copy-out fallback over about 100 ms |
| R9 | Clearing stored tiles on apply assumes no tile of the new set arrives before its last page | adopted as a stated fact: tiles follow the set from the same `select!` branch (`server.rs:764-777`) |
| R10 | Against shrinking only with pages: adding a second page refetches every icon on every board, old boards included | rejected: a one-time cost, the same as a rows change today. The alternative changes every existing config and breaks new firmware against today's daemon |
| R11 | S2 is a behavior-neutral refactor across three languages before any feature needs it | rejected: it fixes the numbers before three parallel slices consume them, and gives the firmware its first geometry test. Each side is small |
| R12 | S1 has shrunk enough to fold into S3 and S5 | rejected: it is the shared contract that lets the daemon and board waves run in parallel worktrees against one set of fields and host cases |
| R13 | The label inside `s_grid` could make it overflow by a pixel. `s_grid` is scrollable by default (`lv_obj.c:1499`), and a scrolling object gets no gesture (`lv_indev.c:1835`) | adopted: `s_grid` keeps its full height and clears `LV_OBJ_FLAG_SCROLLABLE` |
| R14 | S7's Devices note reads `pages` from `DeviceInfo.caps`, which the daemon reports only once S4 adds it to `CAPS` | adopted: merge S4 before S7 within wave 3 |

### Original plan (2026-10-21)

Outside review: codex gpt-6-astra/high, two passes in plan mode, with read access to the repo. Pass 1 covered the protocol, the daemon and compatibility. Pass 2 covered the firmware, memory, threading and the hardware steps. Each point was checked against the code. Rows marked "obsolete" concern features the user dropped on 2026-10-21.

| # | Point | Outcome |
|---|---|---|
| 1.1 | S1 put `caps` in `hello` before the firmware could assemble a set, and S7's "old firmware" was ambiguous | adopted: caps are sent from the board slice's commit, and the hardware run pins the hash before S1 |
| 1.2 | A dropped page leaves the board stale, with no resend | adopted: a 5 s or broken-set reconnect, plus host cases |
| 1.3 | `rev` restarts at 1 each session, so set state must reset on reconnect | adopted: set state is per connection, and `page_id` is kept apart |
| 1.4 | An ack sent after the `icon_get`s waits behind every frame | adopted: ack before `icon_get`, documented as an order change, with a `device_sim` test |
| 1.5 | "Byte-identical after a no-op save" contradicts `save` | adopted: semantic equality and round-trip tests |
| 1.6 | Page id rules incomplete, and the implicit-home collision untested | adopted |
| 1.7 | Downgrade: an old app at startup serves an empty grid, and the hardware run changed the real config dir | adopted: the Config text is corrected, and the run uses a temp `XDG_CONFIG_HOME` copy |
| 2.1 | `EV_PAGE` through the link queue waits behind inline discovery, with a lock-inversion risk | adopted: a locked model in `model.c`, navigation in the LVGL task, and a fixed lock order |
| 2.2 | All `icon_get`s at once don't fit `s.want` and let up to 320 frames through | adopted: one outstanding batch, a 5 s timeout, cancelled by a new set |
| 2.3 | "Last page arrived" isn't completeness, and navigation needs a stable identity | adopted: a received mask, count checks, navigation by `page_id` |
| 2.4 | The memory gate sampled before icons and with too few tiles | adopted: settled samples, 40 tiles, the largest result modal, the largest free block |
| 2.5 | A rebuild undoes the offline dimming, and page links would be blocked by the `s_online` check | adopted for the dimming. The page-link half is obsolete |
| 2.6 | The dense fixture doesn't test peak pixels, and `max_used` is not a leak measure | adopted: two fixtures, heap in use compared at settled points |
| 2.7 | The pending-label promise had no test | adopted: S6 delayed-result cases |
| 2.8 | The stored tile value has no freshness rule, and the 40-tile cap doesn't solve slot contention | partly adopted: stored values clear on every set and reconnect. Rejected: a new freshness rule, since an edited source under the same id behaves as a visible tile does today. Slot contention keeps the tested `stale` path, and the cap only bounds the total |
| 2.9 | "Gets `PRESS_LOST`" is too absolute, the rebuild could delete the pressed widget, and the swipe cases were too narrow | adopted: the test asserts "no press", the rebuild is deferred with `lv_async_call`, and the swipe cases are wider |
| 2.10 | Page bar text and touch geometry weren't checked | obsolete: no page bar. Its successor is S6's label check |

Strongest dissent kept: 2.8. The reviewer would add per-tile freshness on the board. Rejected, because it needs a `rev` or a definition id on `tile`, and today's behavior on the visible page already has the same window.
