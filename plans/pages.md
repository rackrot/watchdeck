# Pages: several button pages per layout (protocol v2)

Status: planned 2026-10-21. No slice started.

Adds pages to the one layout that serves every paired device. Comes from the backlog item in `plans/desktop-app-v1.md` (Backlog, "Pages (protocol v2)"), decided on 2026-10-21. The v1 plan assumed "One layout serves every paired device, with no pages" (`plans/desktop-app-v1.md:57`). That stays true except for "no pages".

## Goal and done

- `config.toml` can declare up to 8 pages. Each button belongs to one page. A file without pages works exactly as it does today, and a save from the window keeps it in its old form.
- A board on the new firmware shows one page at a time. It moves between pages with a page bar, a horizontal swipe on the grid, or a page-link button. Moving never asks the daemon for anything.
- A board on today's firmware, against the new daemon, still works. It shows the first page, and the daemon says so in its log and in the Devices tab.
- A board on the new firmware, against today's daemon, still works. It shows the single grid with no page bar.
- `layout_ack` still turns the save bar from "sent" into "applied" (S9), once per push. The board now sends it before its `icon_get`s, which is consistent with `protocol.md:248`: icons on the way don't hold the ack back.
- Tiles on every page keep their values, so a page shows its current values the moment it is built.
- Icons of every page are cached in PSRAM, so a page switch sends no `icon_get`.
- The config editor can add, rename, reorder and delete pages, move a button to another page, and preview each page.
- `cargo test`, `pnpm test` and `mise run fw:test` cover the new rules. The hardware run below passes on the board, with serial-log evidence.
- `site/src/protocol.md`, `site/src/config.md` and the status in `plans/desktop-app-v1.md` are updated.

## Assumptions

- One grid (`[layout] cols, rows`) serves every page. Per-page grid sizes are left for later (open question 3). The wire format still carries `cols` and `rows` on every page, so a later change needs no firmware.
- The current page is local to each board. Two boards can show different pages. Nothing persists it in NVS, which keeps holding only the pairing blob. A reconnect keeps the page. A reboot starts on the first page.
- Global button ids stay unique across pages. That keeps `press`, `result` and `tile` unchanged on the wire.
- The LVGL pool is 64 KB of internal RAM (`CONFIG_LV_MEM_SIZE=65536` with the built-in allocator, in the generated `sdkconfig`). S6 measured 16 KB used with 9 buttons, 3 of them tiles (`plans/desktop-app-v1.md:472`). A full 8×5 page with icons and tiles was never measured on hardware (`plans/desktop-app-v1.md:474`). UNCONFIRMED that it fits, so S4 measures it first.
- Another agent is changing `app/src-tauri/src/daemon/actions/http.rs` and `icons.rs` in a separate worktree (from the backlog, most likely `insecure_tls` and SVG icons). This plan doesn't edit `icons.rs` or `http.rs`. If that work adds a field to `Action::Http` in `config.rs` or to `ActionForm.tsx`, S2 and S6 hit textual merge conflicts there. Nothing here depends on that work.

## Findings

| # | Where | What |
|---|---|---|
| F1 | `server.rs:566-569`, `server.rs:476-484` | The daemon closes any `hello` whose `v` isn't exactly 1, with `error` `version`. A board that sends `v:2` to today's daemon gets refused every time |
| F2 | `link.c:491-493` | An unpaired board lists a daemon for pairing only when TXT `v` is absent or exactly `1`. A daemon that advertised `v=2` would vanish from every old board's pairing list |
| F3 | `proto.c:406-410`, `protocol.md:147` | The board ignores `welcome.v` and every unknown field. The daemon ignores unknown fields too (`protocol.rs` test `unknown_fields_are_ignored`). New optional fields reach old peers safely |
| F4 | `proto.c:266-274`, `proto.h:59-65` | A layout is one `proto_layout_t` of at most 40 buttons, parsed into one PSRAM `proto_msg_t`. A message without `buttons` fails to parse, so an old board can't take a reshaped `layout` |
| F5 | `config.rs:1475-1509` | The fullest 8×5 layout must fit in 16 KB. By count it's about 11 KB (about 276 bytes a button). Two full pages in one message would pass 16 KB |
| F6 | generated `sdkconfig` (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`) | cJSON allocates each node separately, and allocations under 16 KB come from internal RAM. A layout of several pages in one message would multiply the transient internal-heap cost of the parse |
| F7 | `icons.c:83-101`, `icons.h:13-14` | `icons_retain` frees every icon the one layout doesn't name. The cache holds at most 40 icons and 8 MB, and its table is in internal `.bss` |
| F8 | `link.c:927`, `link.c:953`, `link.c:31` | `sync_icons` keeps `seen[40][16]` and a 2 KB buffer on the 8 KB link task stack. Sized for every page, `seen` alone would be 5 KB |
| F9 | `icons.h:10` | The icon cache is link-task-only. A page switch started by a tap in the LVGL task can't read it directly |
| F10 | `ui.c:322-404`, `ui.c:59-67` | `build_grid` deletes and rebuilds the whole grid. Every per-button array is sized for one layout of 40 |
| F11 | `ui.c:442-463` | A `tile` for a button that isn't on screen is logged and dropped. With pages, values for hidden pages would be lost |
| F12 | `lv_indev.c:1569-1576` (LVGL 9.6.0) | On release LVGL sends `CLICKED` to the pressed widget whenever nothing scrolled, gesture or not. The grid never overflows, so it never scrolls (`lv_indev_scroll.c:397-409`). A swipe that starts on a button would change the page and also press it, running its action |
| F13 | `lv_indev.c:1518-1531` | After `lv_indev_wait_release()`, the release sends `PRESS_LOST` and clears the active widget, so no `CLICKED` follows. That is the fix for F12 |
| F14 | `lv_obj.c:1507`, `lv_indev.c:1836-1845` | Every child widget has `gesture_bubble` set, so a gesture bubbles to the first ancestor without it. Today that is the screen. Clearing the flag on `s_grid` makes the grid the gesture target |
| F15 | `tiles.rs:193-198` | The tile manager polls every tile in `config.buttons`. A flat button list across pages keeps it unchanged |
| F16 | `icons.rs:110-131` | `Icons::load` renders every icon of `config.buttons` at one box size from `[layout]`. With one grid for every page it needs no change |
| F17 | `server.rs:43`, `server.rs:803-834` | `icon_get` takes at most 40 ids and is answered from the session's layout only |
| F18 | `server.rs:724-740`, `server.rs:794-802` | A session pushes a layout only when its `Layout` differs, bumps `rev` once per push, and takes an ack for a `rev` it sent and hasn't seen acked. Several messages sharing one `rev` need no change here |
| F19 | `ui.c:87-97`, `ui.c:313`, `wifi.c:132`, `sdcard.c:175-182`, `usb.c:27-84` | The status lines run 984 px wide. The Wi-Fi text (`connected 192.168.0.56 (watchdeck.local)`) needs most of that. The SD and USB texts stay under about 34 characters |
| F20 | `ui.c:202-210` | The result modal names a button by looking it up in the grid on screen, and falls back to its id. After a page switch, a result from the old page would show the raw id |

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
label = "Main"

[[page]]
id = "media"
label = "Media"

[[button]]
id = "plex"
page = "media"
pos = [0, 0]
label = "Plex"
action = { kind = "shell", argv = ["ssh", "nas", "docker", "restart", "plex"] }

[[button]]
id = "to-media"
pos = [4, 2]
label = "Media"
action = { kind = "page", page = "media" }
```

- No `[[page]]` table: one page, as today. A button may not set `page` then.
- With `[[page]]` tables: 1 to 8 pages, in file order. The first is the home page. `id` follows the button id rule and is unique among pages. `label` is optional, printable ASCII, at most 24 characters. A button without `page` goes on the first page.
- One button per cell per page. Button ids stay unique across all pages.
- At most 40 buttons with a `tile` across all pages, which is today's ceiling (one per cell of 8×5). It keeps polling load where S6 tested it (open question 2).
- A page-link action, `{ kind = "page", page = "<id>" }`, must name an existing page. It can't be a tile's `source`. The daemon never runs it. A crafted `press` for it gets `ok:false`, "opens a page on the device".
- `version` stays 1. The new keys only add to the format. Today's app refuses a file that uses them with "unknown field", through `deny_unknown_fields`.
  - While that app is running, it keeps its last good config.
  - When it starts against such a file, it serves an empty grid (`config.rs:563-588`). Nothing persists a last good config across restarts.
- A page `id` follows the button id rule (1 to 63 letters, digits, `_`, `-`, `.`) and is unique among pages. A button without `page` counts as being on the first page before cells are checked, so it collides with a button that names that page explicitly.
- `Config` gains `#[serde(default, rename = "page", skip_serializing_if = "Vec::is_empty")] pages`, and `ButtonConfig` gains `page: Option<String>` (skipped when `None`). A file without pages serializes as it does today.
- Validation errors name the page as `page "media": ...`, like `button "x": ...`.

### Wire

Protocol v2 is a capability, not a new `v`:

- `hello` gains `"caps":["pages"]`. `v` stays 1, TXT `v` stays `1` (F2), and the pairing label stays `watchdeck-pair-v1`.
- To a session whose hello names `pages`, the daemon sends one `layout` message per page, back to back, all with the same `rev`:

  ```json
  {"t":"layout","rev":7,"page":1,"pages":3,"page_id":"media","page_label":"Media","cols":5,"rows":3,"buttons":[
    {"id":"plex","col":0,"row":0,"label":"Plex","bg":2001125,"fg":16777215},
    {"id":"to-main","col":4,"row":2,"label":"Main","bg":2001125,"fg":16777215,"to":0}]}
  ```

  - `page` is 0-based, `pages` is 1 to 8, and `page < pages`. `page_id` is at most 63 bytes, `page_label` at most 24 and may be empty.
  - `to` marks a page-link button with the index of its target page. The board switches page on a tap and sends no `press`.
  - Pages go out in order, 0 first. Each one stays a full layout of at most 40 buttons and 16 KB (F5, F6).
- To a session without `pages` (today's firmware), the daemon sends today's `layout`: the home page only, with page-link buttons left out. Its tiles and `icon_get` answers cover that page only. When the config has more than one page, the daemon logs `<device>: firmware without pages, showing page 1 of N` once per session.
- The board accepts both shapes. A `layout` without `page` is a single page, which is how the new firmware runs against today's daemon.
- `rev` keeps its meaning: one more per push on the session (F18). A push is the whole set of pages.
  - The board collects a set into its own staging copy, with a mask of the pages received. It applies the set only when every page from 0 to `pages - 1` is in and the `pages` counts agree.
  - A page of a newer `rev` throws away a set still arriving.
  - Staging, `rev` and ack state belong to one connection (`s.gen`). A reconnect clears them, since every session starts again at `rev` 1 (`server.rs:698-699`). The current `page_id` is kept apart from them.
- **Recovery.** A text frame can still be lost: the websocket handler drops it after waiting 500 ms for queue room (`link.c:226-233`), and an unparsable one is dropped too (`link.c:1002`). The daemon never resends an unchanged layout, so a lost page would leave the board stale. If a set breaks (a page out of order, counts that disagree, a page that won't parse) or isn't complete 5 s after its first page, the board drops the link and reconnects. The new session sends a complete set. Nothing new goes on the wire for this.
- `layout_ack {rev}` goes out once per set, right after the board has applied it and built the current page, and **before** any `icon_get`. The daemon answers an `icon_get` by sending every frame before it reads the next message (`server.rs:803-834`). An ack sent after the requests would wait behind all the icons. This changes the order today's firmware uses (`link.c:1043-1050`), and `protocol.md` says so. The daemon's ack rules don't change.
- `icon_get` keeps its limit of 40 ids, and the board keeps **one request outstanding**. It asks for the current page's missing icons first. It asks for the next page's when every frame of the last request has arrived or 5 s have passed. A new set cancels what is left. That keeps a burst at 40 frames, as today. The daemon answers from every page of the session's layout.
- The daemon sends `tile` for every tile button on every page of the session's layout, as today.
- Rejected wire options are listed below.

### Board

- **Model.** A new `model.c` holds the pages of the applied set, the latest tile value for every tile button, and the icon cache, all in PSRAM behind one mutex. The link task's staging copy sits outside it until the set is complete, and then swaps in under the lock.
- **Navigation is local and stays in the LVGL task.** A tap on the page bar, a swipe or a page-link button builds the target page straight from the model. It takes the model lock inside the LVGL lock, and reads labels, icons and tile values. It doesn't go through the link queue. The link task runs mDNS discovery inline (3 s PTR plus a 10 s budget, `link.c:396-452`, called from the loop at `link.c:1207-1213`), so a queued `EV_PAGE` could wait 13 s while the daemon is away.
  - Lock order is always LVGL, then model. The link task takes the model lock only while it doesn't hold the LVGL lock, and never calls a UI callback while it holds the model lock.
  - This replaces the link-task-only rule of `icons.h:10` (F9). Icons are still freed only after the UI has replaced the widgets that used them (`icons.h:21-25`). After a set applies, the link task frees the icons the new set doesn't name, after `on_set` has rebuilt the page.
  - Navigation goes by `page_id`. A tap that names a page the applied set no longer has goes to the home page.
- **Page bar.** It sits in the right column over the SD and USB lines (about y 40 to 108), above Forget pairing, as 300 px holding "‹", "2/3 Media" and "›". The SD and USB labels narrow as the touch label does (F19). The Wi-Fi line keeps its full width. The bar is hidden with one page. The grid's position and size don't change, so `box_px`, `preview.ts` and every icon size stay as they are.
- **Swipe.** `s_grid` clears `LV_OBJ_FLAG_GESTURE_BUBBLE` and handles `LV_EVENT_GESTURE` (F14). Left goes to the next page, right to the previous. Navigation stops at the ends.
  - The handler calls `lv_indev_wait_release(lv_indev_active())`, so the release presses nothing (F12, F13). That includes a swipe outward at either end.
  - It defers the rebuild with `lv_async_call`, so the pressed widget isn't deleted inside its own event.
- **Offline.** A rebuilt page keeps the link state. While the link is down it's dimmed like the old grid (`ui.c:465-474`), and `build_grid` must not set `s_online` the way `ui_apply_layout` does (`ui.c:410`). A page-link tap works offline: it's handled before the offline check in `cell_event` (`ui.c:288`). An ordinary button stays blocked.
- **Current page.** It's kept by `page_id`. A new `rev` keeps the page with that id, or shows the home page if it's gone. A reconnect keeps it.
- **Icons.** `icons_retain` runs against every page of the complete set. The cache table moves to PSRAM, with room for 8 × 40 = 320 icons. The byte cap stays at 8 MB. With 8 pages on one grid the worst case is 7.25 MB, at 4×1 (32 icons of 238 × 238 × 4 bytes), so a valid config never reaches the cap. A unit test pins that bound. `sync_icons` moves its `seen` list and request buffer off the link task stack (F8).
- **Tiles.** A `tile` updates the stored value. If the button is on screen it also updates the widget. Building a page shows the stored values at once (F11).
  - Applying a set or reconnecting clears every stored value, because the daemon resends each tile after every layout (`server.rs:734-735`).
  - A source edited under the same id keeps showing its old value until the new source's first reading. That's how a visible tile behaves today, since the daemon withholds old readings (`server.rs:652-655`) but sends nothing to blank one.
- **Results.** A pending press keeps its button's label, so the modal names it after a page switch (F20).
- **LVGL memory.** Only the visible page has widgets. A page switch runs `build_grid` for that page, which costs what a layout push costs today. S4 measures a full 8×5 page first, then repeated switching, using the heap line `ui_apply_layout` already logs (`ui.c:414`).

### Daemon

- `Live::layout()` returns the pages: id, label, buttons and the icons each names. Each session takes a view from it: all pages for a `pages` session, or the home page without page links otherwise. It compares views to decide whether to push, so an edit to page 3 sends nothing to an old board.
- `TileFeed` and `icon_get` work from the session's view (F17). The tile manager and `Icons::load` don't change (F15, F16).
- `begin_press` refuses a page-link button before it claims a run slot. `test_action` does the same.
- `DeviceInfo` gains `pages: bool` from the hello, for the Devices tab.

### Config UI

- Page tabs above the grid editor: add, rename (label), reorder with move left and right, and delete. Delete asks first and names the buttons it removes. Adding the first extra page turns the implicit page into `[[page]] id = "main"`.
- The grid editor and preview show the selected page. ButtonForm gets a page select. ActionForm gets the `page` kind with a page select.
- `layoutChanged` (`edit.ts:89`) counts page order, labels and each button's page. `replaceNotice` (`saveflow.ts:48`) counts pages and buttons. `errors.ts` routes `page "x": ...` messages to the page tabs.
- The Devices tab marks a connected board without `pages` as "shows page 1 only, update its firmware".

## Rejected alternatives

| Option | Why not |
|---|---|
| Bump `v` to 2 in `hello` | Today's daemon refuses it (F1). The board would need a downgrade retry and state about which daemon spoke what |
| `hello` `vmax` with the version echoed in `welcome` | Works, but it orders features in a line. A capability list lets a later feature, such as reporting the visible page, stand on its own |
| Advertise TXT `v=2` | Hides the daemon from today's boards when they pair (F2) |
| All pages in one `layout` message | Breaks the 16 KB cap with two full pages (F5), and the parse multiplies internal-heap use (F6) |
| A per-page `rev` with only changed pages resent | Needs per-page acks and partial updates on the board. Resending every page costs at most about 90 KB on the LAN per edit |
| Navigation that asks the daemon for each page | A round trip per switch and per-session page state on the daemon. Presses need the daemon anyway, but looking at pages shouldn't |
| `lv_tileview` holding every page's widgets | Builds up to 320 buttons in a 64 KB pool that one full page may already fill |
| A page bar inside the grid area | Changes the cell geometry, so `box_px`, `preview.ts`, `ui.c` and every documented icon size move together |
| Nested `[[page.button]]` tables | Every reader of `config.buttons` would change: tiles, icons, secrets, presses. Long table names in TOML too |
| Per-page grid sizes now | `Icons::load` renders at one box per config (F16), so this needs an icon cache keyed by size, in the file the other agent is editing. The wire already allows it later |
| Fetch a page's icons only when it is first shown | Pages first show labels alone and wait a round trip. The cache bound above makes fetching them all safe |
| Poll only the tiles of visible pages | Needs the board to report its page, per-session state, and a union across boards. A newly shown page would sit grey for at least the 2 s cooldown. It can come later as its own capability |
| A `goto` key beside `action` for page links | Makes `action` optional, which touches every reader of `b.action` |
| Navigation through the link queue (`EV_PAGE`), with the icon cache kept link-task-only | Waits behind inline mDNS discovery, up to 13 s offline. A blocking post from the UI would invert the lock order with `lvgl_port_lock` |
| Every page's `icon_get` sent at once | `s.want` tracks one batch and resets on each sync (`link.c:130`, `link.c:930`). Up to 320 frames at once against a 16-slot queue that drops after 500 ms |
| An application-level resend message for a lost page | A reconnect already yields a full set, so no new message is needed |

## Slices

| Slice | Kind | Depends on |
|---|---|---|
| S1 Wire contract | mechanical, both parsers | — |
| S2 Config model | design judgment | S1 |
| S3 Daemon sessions | high complexity | S1, S2 |
| S4 Board data path and page bar | high complexity, firmware | S1 |
| S5 Swipe and page links on the board | design judgment, firmware | S4 (page links also S2) |
| S6 Config UI | design judgment, UI | S2 |
| S7 Hardware run, compatibility, docs | operator plus scribe | S3, S5, S6 |

Order: S1, then S2 and S4 in parallel, then S3 and S5 and S6, then S7. S4 can be built and host-tested against S1 alone, but its hardware check needs S3's daemon.

### S1 Wire contract

- **Files**
  - `site/src/protocol.md`: status line, `hello.caps`, the paged `layout` fields and set rules, `to`, the ack rule per set, `icon_get` across pages, what an old board gets.
  - `app/src-tauri/src/daemon/protocol.rs`: `Hello.caps: Vec<String>` (default empty). `Layout` gains `page`, `pages`, `page_id`, `page_label` as `Option`s, skipped when `None`. `Button` gains `to: Option<u32>`.
  - `app/src-tauri/tests/fixtures/`: `hello.json` with `caps`, a new `layout_page.json`. The existing `layout.json` stays as the v1 shape.
  - `app/src-tauri/examples/fault_daemon.rs` and `tests/device_sim.rs`: only what the new fields need to compile.
  - `firmware/main/proto.h`, `proto.c`: `PROTO_MAX_PAGES 8`, `PROTO_PAGE_LABEL_MAX 25`. `proto_layout_t` gains `page`, `pages`, `page_id`, `page_label`, and `proto_button_t` gains `to` (-1 for none). `parse_layout` reads them when `page` is present and defaults to page 0 of 1 when it isn't.
  - `proto_build_hello` takes a `caps` argument, but `link.c` keeps passing none until S4. A build that advertises `pages` before it can assemble a set would apply each page as a whole grid and ack the `rev` early (`link.c:1039-1050`).
  - `firmware/test/host_proto/main/test_proto.c`: new cases.
- **Host cases**: no `page` (0 of 1), `page` 7 of 8, `page` equal to `pages`, `pages` 0 or 9, `page` without `pages`, `page_id` of 63 and 64 bytes, `page_label` cut at 24, `to` -1, `to` at `pages`, `to` as a string, and a hello that carries `caps`.
- **Acceptance**: every fixture round-trips. An old-shape layout still parses on both sides. The board refuses each malformed case. CI is green.
- **Cheapest check**: `cargo test protocol` and `mise run fw:test`.

### S2 Config model

- **Files**: `app/src-tauri/src/daemon/config.rs`, `site/src/config.md` (a Pages section, the page-link action, the tile cap).
- **Changes**
  - `PageConfig { id, label }`, `Config.pages`, `ButtonConfig.page`, `Action::Page { page }`.
  - `check`: the rules under Config. Cells are checked per page. Ids are unique per kind. `page` must name a page. Page links must name a page and can't be a tile source. At most 8 pages and 40 tiles.
  - `Action::Page` arms in `timeout`, `check` and `secrets_in_clear` (`config.rs:391-503`), in the runner (`actions.rs:187`), and in `tiles.rs:355`. The last two are unreachable, so each just returns a fixed failed result.
  - `Live::layout()` builds pages. The first page is the implicit one when no `[[page]]` exists.
- **Tests**
  - Today's example and `DEFAULT` parse, and saving them writes no `page` key and no `[[page]]` table. They parse back equal. A save always adds its header and drops comments (`config.rs:685-688`), so the test checks equality, not bytes.
  - Round-trip tests (serialize, parse, compare): several `[[page]]` and `[[button]]` tables; implicit and explicit membership; `Action::Page`; a button with HTTP headers and a tile.
  - Every new rule has a negative case:
    - a `page` with no `[[page]]`, or an unknown page
    - a duplicate, empty or 64-byte page id
    - 9 pages, or a 25-character label
    - two buttons in one cell of one page (the same cell on two pages passes), including a button without `page` colliding with one that names the first page
    - a page link to a missing page, or a page link as a tile source
    - 41 tiles
  - The fullest page, with the page fields, fits in 16 KB (extends `config.rs:1475`).
  - Icon bound: for every grid from 1×1 to 8×5, 8 × cells × `box_px`² × 4 ≤ 8 MiB. This pins the board's cap from the Board section.
- **Acceptance**: `cargo test` and clippy are clean. A config with pages survives a save and load unchanged. A file without pages saves with no page keys.
- **Cheapest check**: `cargo test config`.
- **Merge note**: if the `insecure_tls` work has landed, rebase before starting. Both change `Action` in `config.rs`.

### S3 Daemon sessions

- **Files**: `app/src-tauri/src/daemon/server.rs`, `config.rs` (the view), `commands.rs` (`DeviceInfo.pages`), `tests/device_sim.rs`.
- **Changes**: the session keeps `caps`. It sends and compares its view, sends each page as a message with one `rev`, answers `icon_get` from the view, feeds tiles from the view, refuses page-link presses, and logs an old board once.
- **`device_sim` tests**
  - A hello without `caps`, against a 3-page config, gets today's `layout` shape. It holds the home page only, with no page links and no `page` fields. Tiles come for home-page buttons only. `icon_get` for a page-2 icon gets nothing.
  - A hello with `caps` gets three `layout` messages, `page` 0, 1 and 2 with one `rev`, then the tiles of every page. `icon_get` for a page-2 icon gets its frame.
  - An edit to page 2 pushes `rev` 2 to the `caps` session. The old-shape session gets nothing and stays on `rev` 1.
  - `layout_ack {2}` after the set gives `LayoutApplied(device, 2)`.
  - The ack comes before uncached icons: a `layout_ack` sent before 40 `icon_get` ids gives `LayoutApplied` before the frames finish.
  - A `press` of a page-link button gets `ok:false` and runs nothing. `test_action` on it gives the same.
  - A config without pages gives a `caps` session one message with `page` 0 of 1.
- **Acceptance**: every existing `device_sim` test passes unchanged. The new tests fail on the S2-only build, so they can tell the two apart.
- **Cheapest check**: `cargo test --test device_sim`.

### S4 Board data path and page bar

- **Files**: `firmware/main/model.c` and `model.h` (new), `link.c`, `link.h`, `icons.c`, `icons.h`, `ui.c`, `ui.h`, `main.c`, `CMakeLists.txt`, `firmware/test/host_proto` (set assembly, if it stays a pure function), and `sdkconfig.defaults` only if the memory step calls for it.
- **Changes**
  - Set assembly in the link task:
    - Staging with a received mask.
    - Per-connection state.
    - The 5 s and broken-set reconnect.
    - The swap into the model.
    - `layout_ack` before the first `icon_get`.
    - One outstanding `icon_get` batch, with a 5 s timeout and cancellation on a new set.
  - `hello` sends `caps:["pages"]`, in the same commit as the assembly.
  - `on_layout` becomes `on_set` (rebuild the current page). Navigation runs in the LVGL task, with the lock order from the Board section.
  - The model holds the tile values and the icon cache. The icon table moves to PSRAM with `ICONS_MAX` 320, and `icons_retain` runs against the whole set. `sync_icons` takes its lists off the stack (F8).
  - The page bar is added. A pending press keeps its label. A rebuilt page keeps the offline dimming.
  - If the assembly rules (mask, counts, newer `rev`) are a pure function over parsed pages, they get host tests. Cases: a missing middle page, a missing last page, a count change, a newer `rev` mid-set, a duplicate page.
- **Memory baseline first**, on today's firmware against today's daemon. It's a baseline, not a gate.
  - Two fixtures, each with distinct icon hashes:
    - most widgets: 8×5 with 40 icons and 40 tiles on one page
    - most pixels: 4×1
  - Sample once every icon has arrived. Open a result modal with 1 KB of text, then record the `LVGL heap` line, the largest free block, and the min free internal heap. Today's heap line logs before icons arrive (`ui.c:406-424`), so add a settled sample.
  - If the pool is short, raise `CONFIG_LV_MEM_SIZE` in `sdkconfig.defaults` and check the generated `sdkconfig`. A bigger pool is static internal RAM, so the internal-heap baseline of 305.5 KB moves with it. Record the new baseline.
  - Repeat with pages and the page bar.
- **Acceptance (serial log)**
  - `layout rev N: 3 pages applied`, then `layout_ack`, then `icon_get` batches one at a time, the current page first.
  - `page 2/3 shown in X ms`, measured from the tap to the end of the build.
  - No `icon_get` after any page switch.
  - A tile on page 3 shows its value at once on the first visit.
  - The link task stack high-water mark keeps at least 2 KB free after the S7 step 6 workload.
  - Against today's daemon (S7 compatibility) the board shows one page and no bar.
- **Cheapest check**: `mise run fw:build`, then the board against the S3 daemon with a 3-page config.

### S5 Swipe and page links on the board

- **Files**: `firmware/main/ui.c`.
- **Changes**: the gesture handler on `s_grid`, with `lv_indev_wait_release`. A tap on a button with `to` builds that page from the model, before the offline check, and sends no `press`. The page bar arrows are disabled at the ends.
- **Acceptance (on the board, from the serial and daemon logs)**: neither log shows a `press` for any swipe.
  - Swipes in both directions: some starting on a button, some kept inside one big button (a 2×1 grid), some starting in a gap, and some going outward at each end, where they do nothing.
  - Plain taps still press. Closing the result modal over the page bar doesn't navigate.
  - A page-link tap switches page and the daemon logs nothing, online and offline.
  - A slow drag that isn't a gesture behaves as it does today.
  - Pending result: press a slow button on page A, go to page B, and the result names A's button. Two slow presses whose results come back in reverse order each name their own button.
  - Page bar: check the longest label (24 characters), SD and USB error text beside it, the disabled end arrows, and Forget pairing right below it. Nothing overlaps, and each arrow takes a tap.
- **Cheapest check**: the board, with `mise run fw:monitor` and the daemon log side by side.

### S6 Config UI

- **Files**: `app/src/config.ts`, `ConfigPage.tsx`, `GridEditor.tsx`, `ButtonForm.tsx`, `ActionForm.tsx`, `edit.ts`, `saveflow.ts`, `errors.ts`, `Devices.tsx`, `api.ts`, and `app/tests/*.test.ts`.
- **Acceptance**
  - Page tabs work: add, rename, reorder, delete with confirmation, move a button between pages.
  - Adding the first extra page writes `[[page]]` tables. Deleting back to one page removes them, and the file goes back to the old form.
  - Field errors from Rust land on the right page or button.
  - Test is off, with a hint, for a `page` action (`testBlock` in `saveflow.ts`), since `test_action` only refuses it.
  - The save bar still reaches "applied" on the board.
  - TS tests cover `layoutChanged` with pages, the error routing, and the page-to-implicit round trip.
- **Cheapest check**: `pnpm test`, `pnpm build`, then `mise run app:dev`. The window check needs the user, as S7 and S9 did.
- **Merge note**: `ActionForm.tsx` and `config.ts` may conflict with the `insecure_tls` work. Take both.

### S7 Hardware run, compatibility, docs

Run on the board with the dev daemon unless a step says otherwise. Evidence is the serial log (pyserial from the IDF Python env when there's no TTY) and the daemon log.

1. **Old file.** With today's `config.toml`, the board logs `page 0 of 1`, shows no bar, and the save bar still reaches "applied".
2. **Three pages**: 5×3, about 15 buttons each, icons on all three, two tiles on page 3.
   - Bar, swipe and page link each switch pages.
   - Switching sends no `icon_get`.
   - Page 3's tiles show values on the first visit.
3. **Live edits**
   - Edit page 2 while it's on screen: the board stays on page 2 and shows the edit.
   - Delete page 2 while it's on screen: the board goes to the home page.
   - Rename it: the bar shows the new label.
4. **Reconnect.** Restart the daemon with the board running. The board keeps its page and requests no icons.
5. **Link down.** Stop the daemon, and try the bar, a swipe and a page link one at a time.
   - Every page shown stays dimmed, including while the board is running discovery.
   - Ordinary buttons send nothing and leave no pending entry.
   - Navigation latency stays under 200 ms.
6. **Memory**, with the two S4 fixtures spread over 8 pages: one with the most widgets, one with the most pixels (4×1, 32 distinct icons).
   - Switch through every page 3 times after a warm-up round, then push a new set while frames of the old one are still arriving.
   - Compare the LVGL heap in use (not `max_used`, which only rises) at the same settled point each round. Do the same for the largest free block, and for the PSRAM minimum and largest block.
   - The min free internal heap stays within 10 KB of the baseline S4 recorded.
   - Every icon logs `received, validated and cached`, and none `dropped`.
   - The link stack high-water mark is logged at the end.
7. **Old firmware, new daemon.** Flash the build at the commit before S1. Record its hash here; don't rely on "`main` before". The board shows page 1 only, with no page-link buttons. The daemon logs `firmware without pages`, and the Devices tab shows the note. Then flash the new build back. The pairing survives both flashes, since NVS is untouched.
8. **New firmware, old daemon.** Build the app at the commit before S2 (record its hash) in a worktree.
   - Quit the tray app, then copy the config dir to a temp dir with `/usr/bin/cp -a` (`cp` is aliased to `-i`). The copy keeps `identity.pem`, `identity.key` and `state.json`, so the board's pin and token still match.
  - Check that the copied `identity.key` is still 0600 and owned by you. The daemon refuses to start otherwise (`plans/desktop-app-v1.md:312`).
   - Replace the copy's `config.toml` with one that has no pages, and run the old daemon with `XDG_CONFIG_HOME` pointing at the temp dir. The new board shows one page with no bar.
   - Then put a paged `config.toml` in the copy and restart the old daemon. It serves an empty grid and logs the unknown field. That's the downgrade case.
   - The real config dir is never touched. Restart the tray app afterwards.
- **Docs**: `plans/desktop-app-v1.md` status and backlog, the Assumptions line at `:57`, `site/src/protocol.md` and `site/src/config.md` finished, and a results section in this plan.
- **Acceptance**: every step passes, or its failure is recorded with a decision.

## Risks

| Risk | Mitigation |
|---|---|
| A swipe also presses the button it started on, running a shell or HTTP action (F12) | `lv_indev_wait_release` in the gesture handler (F13), with the rebuild deferred. S5's swipe cases check it on the board |
| One full 8×5 page fills the 64 KB LVGL pool, or switching fragments it | S4 records a settled baseline (not a gate) and repeats it with pages. Raise `CONFIG_LV_MEM_SIZE` if the pool is short. S7 step 6 checks for drift |
| Link task stack overflow from per-page arrays (F8) | Arrays move off the stack. Log the high-water mark |
| An icon burst overruns the link queue, where the websocket handler waits 500 ms and then drops | One outstanding `icon_get` of at most 40 ids, as today, with a 5 s timeout. S7 step 6 counts drops |
| A lost or broken page leaves the board on an old set, since the daemon never resends an unchanged layout | A set that is broken or not complete within 5 s makes the board reconnect, and the new session sends a full set. S4's host cases cover assembly |
| Navigation stalls behind mDNS discovery on the link task | Navigation runs in the LVGL task against the locked model and never uses the link queue. S7 step 5 measures latency during discovery |
| Lock inversion between `lvgl_port_lock` and the model lock | One order, LVGL then model. The link task never calls a UI callback while it holds the model lock |
| Hidden-page tiles starve the 6 tile slots and show `stale` | The cap of 40 tiles keeps the load S6 tested, including its 3-skip `stale` rule |
| The board's caps and the daemon's mirrored constants drift: 8 pages, 40 buttons, 8 MB, `box_px` | The S2 bound test, plus constants named the same on both sides, as `box_px` and `ui.c` already are |
| Merge conflicts with the `insecure_tls` and SVG work | No edits to `icons.rs` or `http.rs`. Expect textual conflicts in `config.rs`'s `Action` and in `ActionForm.tsx`, and rebase before S2 and S6 |
| Downgrading the app with a paged `config.toml` serves an empty grid at startup | Documented in `config.md`. The fix is to remove the `[[page]]` tables and the `page` keys, or restore a `.bak` |
| A board on old firmware silently shows one page | Daemon log, plus the Devices tab note |

**Rollback.**
- Each slice is its own commit, so `git revert` undoes one.
- The firmware rolls back by flashing the previous build. The partition table and NVS don't change, so the pairing survives.
- The daemon rolls back with a config that has no pages.
- The protocol change is additive. Each old peer works with each new one, so the board and the app roll back independently.

## Open questions

Questions 1, 2, 4, 6 and 9 were settled by the user on 2026-10-21:

- **Navigation (1, 5, 6):** swipe only, with a small "Page x of y" label at the bottom, below the buttons. There's no page bar and there are no page-link buttons, so the page-bar part of S4, the page-link parts of S2, S5 and S6, and `Action::Page` are dropped. The label sits under the grid, so the grid loses a strip of height: S4 and the preview geometry (`preview.ts`) must shrink the grid by the label's height, and the preview shows the label too. Wrapping (5) stays at the default: no wrap.
- **Tiles (2):** all pages poll, as recommended.
- **Pages (4):** at most 4, not 8. The icon-cache budget gains headroom.
- **Order (9):** fonts first (`plans/fonts.md`). Fonts introduces `hello.caps` exactly as S1 specifies, and pages then adds `"pages"` to that list and builds on the font fields in `layout`. Rebase S1 on whatever fonts lands.

The table keeps the original recommendations for the record.

| # | Question | Recommended default |
|---|---|---|
| 1 | How does a board move between pages? | All three: the page bar, a swipe, and page-link buttons. Page links are the last part of S2, S5 and S6, so they can be dropped without touching the rest |
| 2 | Which tiles poll: the visible page only, or all? | All, as today, with a cap of 40 tiles across pages. Visible-only can come later as a capability |
| 3 | Can each page have its own grid size? | Not yet. One `[layout]` for every page. The wire already carries `cols` and `rows` per page, so it's a later daemon and UI change, after the icons work lands |
| 4 | How many pages? | 8, which keeps the worst-case icon cache under 8 MB |
| 5 | Does navigation wrap from the last page to the first? | No. The arrows are disabled at the ends, which also shows where you are |
| 6 | Where does the page bar go? | The right column over the SD and USB lines, above Forget pairing, with the grid unchanged |
| 7 | Does the current page survive a reboot? | No. NVS keeps holding only the pairing. A reconnect keeps it, and a reboot starts on the home page |
| 8 | What does a board on old firmware get from a paged config? | The home page, with page links left out, plus a log line and a Devices-tab note. Refusing it would make the daemon depend on firmware updates |
| 9 | Which lands first, pages or selectable fonts (`plans/fonts.md`)? | Pages. See Overlap with fonts |

## Overlap with fonts (`plans/fonts.md`)

A second plan adds selectable fonts: a global default, overrides per button and per widget, and font ids carried in the layout. It didn't exist when this plan was written, so this section works from that summary.

- **Same messages.** Both add optional fields to `layout` and to its buttons, and both need old boards to keep working. Fonts should reuse this plan's `caps` list (for example `caps:["pages","fonts"]`) instead of a second scheme. This plan makes the list a general mechanism for that reason.
- **Where the default goes.** With pages, a "layout" is one message per page. A global font default belongs on every page message, or on page 0 only with the rule that it applies to the set. Every page message is simplest, and the board's model keeps it per set.
- **The 16 KB budget.** Each page message must stay under 16 KB with all 40 buttons at their longest. The fullest-page test (`config.rs:1475`, extended in S2) must include the font fields once fonts land. Short integer font ids keep this cheap. Font names would not.
- **Board.** Font ids go into the PSRAM model per button and per page. Built-in LVGL fonts live in flash, so they cost the 64 KB LVGL pool little. Any font that adds a glyph cache or a FreeType or tiny_ttf allocation would come out of that pool, which S4 measures.
- **Preview.** Both change `preview.ts` and `lvtext.ts`. Pages only filter which buttons show. Fonts change the line-break metrics.
- **Order: pages first.**
  - Pages defines the capability list and the per-page message that fonts then extends.
  - Fonts is mostly additive fields on top of that.
  - The other way round, fonts would either invent its own negotiation or bump `v`, which old daemons refuse (F1), and pages would then have to fit around it.
  - If fonts must ship first, it should still add `caps` exactly as S1 here specifies (`hello.caps`, ignored by old daemons), so the two don't collide.

## Critique log

Outside review: codex gpt-6-astra/high, two passes in plan mode, with read access to the repo. Pass 1 covered the protocol, the daemon and compatibility. Pass 2 covered the firmware, memory, threading and the hardware steps. Each point was checked against the code.

| # | Point | Outcome |
|---|---|---|
| 1.1 | S1 put `caps` in `hello` before the firmware could assemble a set, and S7's "old firmware" was ambiguous | adopted: caps are sent from S4's commit, and S7 pins hashes before S1 and S2 |
| 1.2 | A dropped page leaves the board stale, with no resend (`link.c:231-233`, `link.c:1002`, `server.rs:724-728`) | adopted: a 5 s or broken-set reconnect, plus host cases |
| 1.3 | `rev` restarts at 1 each session (`server.rs:699`), so set state must reset on reconnect | adopted: set state is per connection, and `page_id` is kept apart |
| 1.4 | An ack sent after the `icon_get`s waits behind every frame (`server.rs:803-834`) | adopted: ack before `icon_get`, documented as an order change, with a `device_sim` test |
| 1.5 | "Byte-identical after a no-op save" contradicts `save` (`config.rs:685-688`) | adopted: semantic equality and round-trip tests |
| 1.6 | Page id rules incomplete, and the implicit-home collision untested | adopted |
| 1.7 | Downgrade: an old app at startup serves an empty grid, and S7 changed the real config dir | adopted: the Config text is corrected, and S7 step 8 uses a temp `XDG_CONFIG_HOME` copy |
| 2.1 | `EV_PAGE` through the link queue waits behind inline discovery (`link.c:396-452`, `link.c:1207-1213`), with a lock-inversion risk | adopted: a locked model in `model.c`, navigation in the LVGL task, and a fixed lock order |
| 2.2 | All `icon_get`s at once don't fit `s.want` (`link.c:130`, `link.c:930`) and let up to 320 frames through | adopted: one outstanding batch, a 5 s timeout, cancelled by a new set |
| 2.3 | "Last page arrived" isn't completeness, and navigation needs a stable identity | adopted: a received mask, count checks, navigation by `page_id` |
| 2.4 | The memory gate sampled before icons and with too few tiles | adopted: settled samples, 40 tiles, the largest result modal, the page bar, the largest free block |
| 2.5 | A rebuild undoes the offline dimming, and page links would be blocked by the `s_online` check (`ui.c:288`, `ui.c:410`) | adopted |
| 2.6 | The dense fixture doesn't test peak pixels, and `max_used` is not a leak measure | adopted: two fixtures, heap in use compared at settled points |
| 2.7 | The pending-label promise had no test | adopted: S5 delayed-result cases |
| 2.8 | The stored tile value has no freshness rule, and the 40-tile cap doesn't solve slot contention | partly adopted: stored values clear on every set and reconnect. Rejected: a new freshness rule, since an edited source under the same id behaves as a visible tile does today (`server.rs:652-655`). Slot contention keeps S6's tested `stale` path, and the cap only bounds the total |
| 2.9 | "Gets `PRESS_LOST`" is too absolute, the rebuild could delete the pressed widget, and the swipe cases were too narrow | adopted: the test asserts "no press", the rebuild is deferred with `lv_async_call`, and the swipe cases are wider |
| 2.10 | Page bar text and touch geometry weren't checked | adopted: one visual and touch check in S5 |

Strongest dissent kept: 2.8. The reviewer would add per-tile freshness on the board. Rejected, because it needs a `rev` or a definition id on `tile`, and today's behavior on the visible page already has the same window.
