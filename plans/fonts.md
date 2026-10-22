# Selectable fonts: Inter, Roboto, Fira Code, Roboto Mono, Orbitron, Oxanium

User request of 2026-10-21: add six fonts that can be picked for the display, with a global default, a per-button override and per-widget overrides. Static Roboto, not Roboto Flex. Non-ASCII labels stay refused.

## Goal and done

- `config.toml` takes a font at three levels: `layout.font` (the grid's default), `font` on a button, and `font` on a button's `tile` (its value). An unknown name is refused like any other config error, and the last good config stays live.
- This plan introduces the `hello.caps` capability list, exactly as `plans/pages.md` S1 specifies it, with `"fonts"` as its first entry; pages later adds `"pages"`. A board on the new firmware names `fonts` there. To such a session the daemon sends the grid's default font on every `layout` message and, per button, only the label and value fonts that differ from what they inherit. Sessions without the capability get no font fields, so a config with no font keys, or any config sent to an old board, produces today's bytes.
- The firmware draws button labels and tile values in Montserrat (built in) or any of the six fonts at both text sizes the grid uses. Status lines, toast, result modal, Forget pairing and the pairing views stay Montserrat.
- A board on older firmware ignores the new fields and draws Montserrat. A board that gets a font name it doesn't know draws Montserrat, logs it, and still applies and acks the layout.
- Picking a font never changes a button's geometry: icon boxes (`icons.rs` `box_px`), label line counts and the editor's cell layout stay the same for every font, so a font-only edit pushes a layout but triggers no `icon_get`.
- The config editor has a font selector at all three levels with visible inheritance, and the preview draws in the chosen font, bundled locally, with line breaks and dots computed from the same metrics the board uses.
- The generated firmware fonts are checked in, reproducible from pinned inputs with one mise task, and a hook fails when they drift. The firmware job in the `espressif/idf` container only compiles them.
- License texts ship with the app bundle and sit beside the generated firmware sources.
- Hardware checks below pass, with flash and heap numbers recorded.

## Assumptions

- **Weight.** Every family uses its Medium (500) static instance, matching LVGL's built-in Montserrat, which is generated from `Montserrat-Medium.ttf` (`lv_font_montserrat_32.c:4`). Open question 2.
- **Sources.** Each family has an official static TTF at that weight. UNCONFIRMED per file; S1 pins each by URL and SHA-256. Candidates: Inter `Inter-4.1.zip` (rsms/inter v4.1), Roboto `Roboto_v3.016.zip` (googlefonts/roboto-3-classic v3.016, OFL since v3.009), Fira Code (tonsky/FiraCode release zip), Roboto Mono (googlefonts/RobotoMono), Orbitron (`Orbitron Medium.ttf` in theleagueof/orbitron, or a static instance from googlefonts/orbitron-vf; google/fonts has only `Orbitron[wght].ttf`), Oxanium (`oxanium-2.000.zip`, sevmeyer/oxanium 2.000). If a family has no static Medium, S1 stops and asks rather than instancing a variable font (instancing is a modification, see the Orbitron risk).
- **Licenses.** All six are OFL 1.1 in google/fonts. Only Orbitron reserves its name (`ofl/orbitron/OFL.txt`: "with Reserved Font Name: "Orbitron""). Inter, Roboto, Roboto Mono, Fira Code and Oxanium list no RFN. The file actually pinned decides: an upstream repo may ship an older license (Roboto Mono was Apache 2.0 before), and S1 ships whatever text comes with the pinned file.
- **lv_font_conv 1.5.3** (npm, MIT, last published 2024-06-10) is current. `npm view` shows no install or postinstall script (only `prepublishOnly`). It renders with a bundled FreeType wasm, so with its dependency tree locked (`fonts/pnpm-lock.yaml`) its output should depend only on its inputs. UNCONFIRMED until S1's fresh-clone and CI runs match.
- **Line height.** lv_font_conv sets `ascent`/`descent` from the rendered glyph boxes of the converted range, not from the font's hhea/OS2 metrics (`lib/collect_font_data.js`), and LVGL's `line_height` is their difference. An ASCII-only font therefore has a different `line_height` at `--size 32` than Montserrat (35, inflated by its FontAwesome glyphs), and each family differs.
- **Fonts land before pages** (the user's decision of 2026-10-21, reversing `plans/pages.md` open question 9). So this plan owns the capability mechanism, and pages S1 rebases onto it (see Coordination). Pages as now revised adds a "Page x of y" label below the grid, navigates by swipe only, and allows at most 4 pages.

## Findings

| # | Where | What |
|---|---|---|
| F1 | `ui.c:90,108,181-183,266,537,563-602` | Chrome text: status lines (32), toast (32), result title/summary/text (48/32/24), Forget pairing (32), pairing views (32, code in 48). None of it comes from config |
| F2 | `ui.c:371-401` | The only text on a button: the label (`montserrat_32`, or `montserrat_24` on a tile) and a tile's value (`montserrat_32` when `cell_h >= TILE_BIG_MIN_H` (80), else 24). The icon is an image. So a button has two text widgets, and the grid uses exactly two sizes |
| F3 | `ui.c:335-338,377,389,397` | Geometry reads font metrics: `line_h = lv_font_get_line_height(&lv_font_montserrat_32)` picks 1 or 2 label lines and the icon side; the icon label's height is `label_lines * line_h`; the tile label and value heights come from their fonts |
| F4 | `icons.rs:34-64`, `preview.ts:66-79`, `montserrat.ts` | The same geometry is mirrored with `LABEL_LINE = 35` (daemon `box_px`, which sizes icons) and `MONTSERRAT_32.lineHeight` (editor). A per-font line height would desynchronize all three, or make icon ids depend on the font |
| F5 | `grep LV_SYMBOL firmware/main` | No `LV_SYMBOL` anywhere, and `copy_printable` (`ui.c:191-199`) turns everything outside 0x20-0x7E and `\n` into `?`. Range 0x20-0x7E is enough. LVGL's `LONG_DOT` writes `...`, which is ASCII |
| F6 | `proto.c:266-310`, `protocol.md:147` | `parse_layout` reads named keys with `cJSON_GetObjectItemCaseSensitive` and never walks the keys, so current boards ignore new button fields. The spec already says both sides ignore unknown fields |
| F7 | `protocol.rs:89`, `server.rs:566-569` | `Message` is `#[serde(tag = "t")]` with no `deny_unknown_fields` in `protocol.rs`, so the daemon ignores unknown `hello` fields such as `caps`. But it closes any `hello` whose `v` isn't 1, which is why pages and fonts both negotiate through `caps` |
| F7b | `server.rs:724-738` | A session pushes a layout only when `live.layout()` differs from what it last sent. Fonts turns that into a per-session view (strip font fields without the cap), which pages later extends with its own rule |
| F7c | `link.c:707-709`, `protocol.rs:91-99`, `server.rs:67-71` | The board builds `hello` in a 256-byte buffer (about 140 bytes used today, up to about 170 with a 32-character `fw`). `Hello` has no `caps` field. `DeviceInfo` is `{id, connected}` |
| F8 | `config.rs:68,77,84,106` | Every config struct has `deny_unknown_fields`. Font keys are refused by today's daemon, which matters for rollback |
| F9 | `config.rs:1475-1509` | `the_fullest_layout_fits_in_one_message` guards the 16 KB frame (`protocol.rs:12`, `proto.h:12`) with 40 buttons of 63-byte ids and escaped labels |
| F10 | `edit.ts:76-93` | `drawn()` decides whether an edit changes what the device draws. Font fields must be in it |
| F11 | `errors.ts:81-104` | `fieldOf` routes Rust messages to form fields by prefix. Needs `font` and `tile.font` |
| F12 | `firmware/build` ELF (`nm -S`) | Montserrat 4 bpp, 158 glyphs (ASCII, `°`, `•`, 61 FontAwesome): `glyph_bitmap` 23,884 B at 24, 41,096 at 32, 91,912 at 48; `glyph_dsc` 1,264; `kern_class_values` 2,989; class maps 2 × 158 |
| F13 | `partitions.csv:4`, `firmware/build/watchdeck.bin` | Factory partition 4 MB (4,194,304 B). Current image 1,695,888 B (40%) |
| F14 | `lv_font_montserrat_32.c:4,6195,6233` | Built-ins: `--no-compress --no-prefilter --bpp 4 --force-fast-kern-format`, `static_bitmap = 1`. `CONFIG_LV_USE_FONT_COMPRESSED` is off (`sdkconfig:5476`) |
| F15 | `sdkconfig:4929,4965`, `display.c:82-90` | LVGL's builtin allocator, 64 KB pool. Draw buffers are already in PSRAM (`buff_spiram`). LVGL 9.6.0 (`dependencies.lock`) |
| F16 | `tauri.conf.json:23-24` | CSP has no `font-src`, so `default-src 'self'` governs fonts. A Vite-inlined `data:` font would be blocked |
| F17 | `.pre-commit-config.yaml:2,47-51`, `mise.toml:99` | clang-format checks every `firmware/*.c`, and `mise run fmt` rewrites them. Generated font C needs excluding from both |
| F18 | `.forgejo/workflows/ci.yml` | The lint job runs `mise run lint` on ubuntu-latest with mise tools (node included); the firmware job has no node |
| F19 | worktrees `agent-a44f…`, `agent-afa5…` | The insecure_tls and SVG-icon work modify `config.rs`, `config.ts`, `errors.ts`, `edit.test.ts`, `ButtonPreview.tsx`, `icons.rs` and `config.md`. No dependency, but textual conflicts |

### Memory

Generated fonts are `const` data with `static_bitmap = 1` (`lv_font_montserrat_32.c:6233`, same options): the bitmaps stay in flash and are read through the cache, like Montserrat today, plus a few bytes of `.bss` glyph cache per font. That is a claim about storage. Rendering allocations aren't zero, but they depend on the largest glyph drawn, and Montserrat 48 (chrome) already draws bigger glyphs than any slot-32 font. Expectation: no measurable change to the 64 KB LVGL pool or PSRAM, and nothing to move to PSRAM. S5 measures it with the existing heap lines (`ui.c:414-422`) on the same layout before and after.

## Approach

### Scope of a font

- **Widgets.** A button has two: its label and, on a tile, its value (F2). The result modal repeats the label as its title, but that is chrome.
- **Chrome stays Montserrat.** Status and pairing views appear before any layout arrives, and an unpaired board has no config at all. Following the global font there would mean persisting it in NVS, plus 48 px builds of six fonts (about 300 KB more). Open question 1.

### Config (`config.toml`)

```toml
[layout]
cols = 5
rows = 3
font = "inter"            # the grid's default. Absent: montserrat

[[button]]
id = "nas-load"
pos = [2, 0]
label = "NAS load"
font = "orbitron"         # this button's text. Absent: the grid's font
action = { kind = "shell", argv = ["true"] }
tile = { every_s = 30, font = "fira-code", source = { kind = "shell", argv = ["cat", "/proc/loadavg"] } }
#                          ^ the value only. Absent: the button's font
```

- Ids: `montserrat`, `inter`, `roboto`, `fira-code`, `roboto-mono`, `orbitron`, `oxanium`. `montserrat` is valid everywhere, so a button can pin it under a non-default grid.
- Resolution: label = `button.font` ?? `layout.font` ?? `montserrat`. Value = `tile.font` ?? label font.
- Three levels cover every combination with two widgets: any (label, value) pair is `font = label`, `tile.font = value`. A separate `label_font` key would duplicate `button.font` (open question 3).
- An unknown name gives `layout.font "x" is not one of montserrat, inter, roboto, fira-code, roboto-mono, orbitron, oxanium`, or the same after `button "id": font` / `button "id": tile.font`. Validated in `Config::check` and `TileConfig::check`, not with a serde enum, so messages match the rest of the file and `errors.ts` can route them.
- A button's `font` goes before `action`, and `tile.font` before `source`, since TOML wants plain values before tables (`config.rs:98-99`).

### Wire: the `fonts` capability and `layout` fields

Negotiation is a capability list, not a `v` bump, because today's daemon closes any `hello` whose `v` isn't 1 (`server.rs:566-569`) and old boards filter TXT `v` (`link.c:491-493`). Fonts introduces the list, in the form `plans/pages.md` S1 specifies, so pages only adds an entry.

- **`hello.caps`.** An optional array of strings. The daemon parses it as `Hello.caps: Vec<String>` with `#[serde(default)]`, so a hello without it means no capabilities, and it ignores names it doesn't know. Today's daemon ignores the whole field (F7). A board sends only names it fully supports, in the same commit as the support. The new firmware sends `"caps":["fonts"]`; pages later makes it `["fonts","pages"]`. `v`, TXT `v` and the pairing label don't change. "fonts" means the board knows the seven ids of this plan and the fields below.

  ```json
  {"t":"hello","v":1,"device":"wd-e8f60ae276ab","token":"…","fw":"0.3.0","screen":[1024,600],"caps":["fonts"]}
  ```

- **Message default.** Every `layout` message carries `font`, the grid's default (`layout.font`), omitted when it is `montserrat`. It is needed even when no button uses it, because pages' "Page x of y" label draws in it (Coordination). With pages, every page message of a set carries it, so each page stands alone in the board's model.
- **Per button, only what differs.** `label_font` is sent when the button's resolved label font differs from the message's `font`. `value_font` is sent on a tile button when its resolved value font differs from the resolved label font. The board resolves `label = label_font ?? font ?? montserrat` and `value = value_font ?? label`, which is the config's own inheritance shifted onto the wire.

  ```json
  {"t":"layout","rev":3,"cols":5,"rows":3,"font":"inter","buttons":[
    {"id":"plain","col":0,"row":0,"label":"Plain","bg":0,"fg":16777215},
    {"id":"nas-load","col":2,"row":0,"label":"NAS load","bg":2001125,"fg":16777215,"tile":true,"label_font":"orbitron","value_font":"fira-code"}]}
  ```

- **Sessions without the capability** (today's firmware) get no font fields at all: the session keeps its caps from the hello, and its view of the layout (`strip_fonts` when `fonts` is absent) is what it stores, compares and sends (`server.rs:727-732`). `Layout` carries no `rev` (it's added in `message(rev)`, `config.rs:166`), and `rev` goes up only when the stored view differs, so a font-only edit bumps nothing on an old board's session. Pages later adds its own rule to the same view. So a font-only edit pushes nothing to an old board, and a config without font keys sends today's bytes to everyone. Old boards would ignore the fields anyway (F6); stripping just saves them a pointless rebuild.
- `DeviceInfo` (`server.rs:67-71`) gains `caps: Vec<String>` from the hello (empty while disconnected), and the Devices tab notes "draws every font as Montserrat, update its firmware" for a connected board without `fonts`. Pages later reads `pages` from the same list.
- **Board fallback** (new firmware). A missing field means its inherited font. A name the board doesn't know, a non-string, or an over-long string means Montserrat, with one `ESP_LOGW` per layout naming the first such value. A layout is never refused for a font. This is deliberately looser than `icon`/`tile`, which refuse wrong types, because a font is cosmetic and a later daemon may add fonts the board lacks (such a daemon would advertise them under a new capability, see open question 5).
- **Size.** Names, not integers: they read in logs and need no shared enum order across versions. Worst case, every button differing on both: about 54 bytes a button (`,"label_font":"roboto-mono","value_font":"roboto-mono"`), 2.2 KB over 40 buttons, plus 20 bytes for `font`. The fullest layout is about 11 KB today (`plans/pages.md` F5), so about 13.3 KB of 16 KB, leaving pages about 2.7 KB for its per-message fields (about 100 bytes). The fullest-layout test (`config.rs:1475`) gets all font fields at their longest.

### Geometry: fixed text slots

- The grid has two text slots, named by their Montserrat size: **slot 24** (line box 27 px) and **slot 32** (line box 35 px). These are today's Montserrat line heights, so nothing moves for existing configs.
- Every layout calculation uses the slot's line box, never the font's `line_height`: `label_lines` and `icon_side` (`ui.c:335-344`), the fixed heights of the icon label (`:377`), the tile label (`:389`) and the value (`:397`). `box_px` (`icons.rs`) and `iconBox`/`labelLines` (`preview.ts`) already use 35 and need only a comment change.
- Each generated font is converted at the largest `--size` no bigger than the slot's nominal size (24 or 32) whose `line_height` fits the line box (≤ 27 or ≤ 35). The manifest pins the size, and the generator fails if the rule breaks. Text in a fixed box is never clipped.
- **Same line count in a fixed box.** LVGL 9.6 decides the DOTS cut from the box height, not from a line count: a line is the last one when `y + 2 * line_height > max_h` (`lv_label.c:360,466,589`), and the dots go on the line found at `max_h` rounded down to whole lines (`lv_label.c:1396-1418`). With a font shorter than the box, that still gives the slot's line count as long as `floor(box / line_height)` equals it: 2 lines in 70 px needs `line_height` ≥ 24, 1 line in 35 px needs ≥ 18, 1 line in 27 px needs ≥ 14. The generator asserts these lower bounds too. Example: Montserrat-like `line_height` 33 in the 70 px icon label: last line at y = 33 (33 + 66 > 70), dots at y = 66, line 2, as today.
- `lvtext.ts` `dotLines` changes to take the box height in pixels instead of a line count, and applies LVGL's two rules with the font's `lineHeight`, so the preview mirrors `lv_label.c` exactly for any font.
- A wrapping label (plain button, no icon, `ui.c:373`) grows to its text, so it keeps the font's own `line_height`, and the preview uses the font's `lineHeight` there. That doesn't feed into icons.
- So a font change can never change an icon box or an icon id, and S5 checks this on hardware: a font-only edit gives `layout_ack` and no `icon_get`.

### Fit and truncation

- The rules don't change: a tile value is one line with `LONG_DOT`, an icon label is 1 or 2 lines with `LONG_DOT`, and a plain label wraps. A wide font such as Orbitron simply shows fewer characters before the dots.
- No automatic shrink to slot 24 when a value doesn't fit (open question 4). The editor preview shows exactly where each font cuts, because it breaks lines from the same advance widths.

### Conversion pipeline

- **Inputs** in `fonts/` at the repo root:
  - `fonts/fonts.json`, the manifest: for each id its display name, file, source URL, SHA-256, license file and `--size` for slots 24 and 32.
  - `fonts/<id>/<file>.ttf`, unmodified, plus that family's license text, as downloaded.
  - `fonts/generate.mjs`, the generator (node, from mise).
- **Tool**: lv_font_conv 1.5.3 from a lockfile, not a global install. Its dependencies are semver ranges (`opentype.js ^1.3.4`, `pngjs ^6.0.0` and others), so `npm install -g`, which is what mise's `npm:` backend does, could resolve a different tree later and change the output. `fonts/package.json` pins `"lv_font_conv": "1.5.3"` exactly and `fonts/pnpm-lock.yaml` fixes the tree. node and pnpm already come from `mise.toml`, so no new mise tool is needed. `fonts:gen` and `fonts:check` run `pnpm --dir fonts install --frozen-lockfile` first. Vet the package and its tree once in S1 (the `dependency-vetting` skill); `npm view` already shows no install scripts.
- **Inputs only from the tree.** Generation reads the committed TTFs and never downloads; the manifest's URLs and hashes are provenance, checked by S1 once.
- **Path-independent output.** lv_font_conv writes its command line into the file's `Opts:` comment, `--font` and `-o` paths included (`lv_font_montserrat_32.c:4`). The generator runs it with the input directory as cwd and bare file names for `--font` and `-o`, so a run into a temp dir (`fonts:check`) and a run into the tree produce the same bytes.
- **Options**, as LVGL's own built-ins (F14) except the range: `--bpp 4 --no-compress --no-prefilter --force-fast-kern-format --format lvgl --lv-include lvgl.h -r 0x20-0x7E --size <n> --lv-font-name wd_font_<id>_<slot>`. Kerning stays on, as for Montserrat. `CONFIG_LV_USE_FONT_COMPRESSED` stays off.
- **Outputs**, all checked in:
  - `firmware/main/fonts/wd_font_<id>_<24|32>.c`: 12 files. The generator prepends the family's copyright line, `SPDX-License-Identifier: OFL-1.1` and a pointer to the license file, and normalizes trailing whitespace and the final newline so the builtin hooks don't rewrite them.
  - `firmware/main/fonts/fonts.cmake`: the source list for `main/CMakeLists.txt`.
  - `firmware/main/font_list.h`: an X-macro of the 7 ids in enum order, Montserrat first.
  - `app/src/fonts.gen.ts`: per id, the display name, CSS family and, per slot, `px`, `lineHeight` and the 95 `adv` values, parsed from the generated C (`glyph_dsc[].adv_w`, `.line_height`). Same shape as `montserrat.ts`. Piped through the mise-pinned `biome format` before writing, so the biome hook and `fonts:check` agree.
  - `app/src-tauri/src/daemon/fonts_gen.rs`: `pub const FONT_IDS: [&str; 7]`, piped through `rustfmt`. The validation message's list is built from it.
  - The catalog has one source: the manifest. `font_list.h`, `fonts_gen.rs` and `fonts.gen.ts` are generated, and `fonts.c` builds its table from the `font_list.h` X-macro, so no hand-kept list can drift.
- **Tasks**: `fonts:gen` (write the outputs) and `fonts:check` (generate into a temp dir and diff against the tree, failing on any difference).
- **CI**: a local prek hook `fonts-generated` (`pass_filenames: false`, `files:` matching `fonts/`, `firmware/main/fonts/`, `font_list.h`, `fonts.gen.ts` and `fonts_gen.rs`) runs `mise run fonts:check`. The lint job installs every `mise.toml` tool through `mise-action` (`ci.yml:28-30`), node and pnpm included, and its `mise run lint` is `prek run --all-files`, which runs any hook whose `files` match a tracked file. So `ci.yml` doesn't change. S1 proves this with one CI run, not by reading. If the hook makes a local `mise run lint` slower than about 30 s, move the check to a step in the app job instead. The firmware job compiles the checked-in C as today.
- **Hook exclusions**: `firmware/main/fonts/` goes in the clang-format hook's `exclude` and in the `fmt` task's `git ls-files` (`:!firmware/main/fonts/`). Nothing else is excluded: the global exclude stays as is, so `fonts-generated`, gitleaks, typos and the whitespace hooks still see these files, and the generated TS and Rust are already formatted. S1's acceptance is a full `mise run lint` (and `prek run --files` for the new untracked files).
- **Licenses for the firmware.** Each generated C file carries its family's copyright line and license id, and `fonts/<id>/` holds the full text in the same tree. No firmware binary is published today; if one ever is, the `fonts/*/` license files go with it.

### Firmware

- `font_ids.c/.h` (no LVGL): `wd_font_t wd_font_from_name(const char *)` over `font_list.h`, returning `WD_FONT_MONTSERRAT` for anything unknown. `proto.c` uses it, and the host test build adds the file.
- `proto_layout_t` gains `uint8_t font` and `proto_button_t` gains `uint8_t label_font, value_font`, already resolved by `parse_layout` (message `font`, then `label_font`, then `value_font`, as in the Wire section), so `ui.c` and pages' model just store and use them. A string goes through `wd_font_from_name`; any other type, or an unknown or over-long name, gives Montserrat and sets an `unknown_font` flag for `ui.c` to log.
- `proto_build_hello` gains `const char *const *caps, size_t n_caps` and adds `caps` when `n_caps > 0`. `link.c:708` passes `{"fonts"}`. The 256-byte buffer still fits with a 32-character `fw` and room for `"pages"` later; a host case asserts it.
- `fonts.c/.h` (LVGL): `const lv_font_t *wd_font(wd_font_t, wd_slot_t)` (pages' page label calls it with the layout's `font` and slot 24). Its table is built from the `font_list.h` X-macro: the built-in Montserrat 24/32, then the 12 generated fonts.
- `ui.c`: `SLOT24_H 27` and `SLOT32_H 35` replace the font-derived heights (F3). Label and value fonts come from `wd_font`. Chrome code doesn't change.

### Desktop app

- `@font-face` per family in a new `app/src/fonts.css` with `url("../../fonts/<id>/<file>.ttf")` (relative to `app/src/`, so it reaches the repo-root `fonts/`). Vite emits them as hashed assets. `vite.config.ts` gets `server.fs.allow: [".", "../fonts"]` so `app:dev` serves them; that setting is only for the dev server, so S4 also checks a production build.
- CSP: add `font-src 'self'` to `csp` and `devCsp`. The fonts are all well over Vite's 4 KB inline limit. S4 checks `dist/` for `.ttf` files and no `data:font`.
- `preview.ts`: `layoutButton` takes the resolved label and value font ids. `TextBox` gains `family` and `px` (the em size for CSS), with `lineHeight` from the font. Fixed heights use the slot line boxes. `lvtext.ts` `dotLines` takes the box height in pixels (Geometry). Text sits at the top of its box in both LVGL and the preview, so a font shorter than its slot leaves the gap at the bottom on both.
- `fonts.ts` (hand-written) joins `montserrat.ts` and `fonts.gen.ts` into one table and exports `resolveFonts(config, button)`, so the preview and `drawn()` share the inheritance rule.
- **Selectors**, all plain `<select>`, with a sample (`Aa 0123`) beside each in the selected font, since WebKitGTK doesn't style `<option>`:
  - Grid panel (`GridEditor.tsx`, beside Columns and Rows): "Font", listing the 7 fonts. Picking Montserrat removes `layout.font`.
  - Button form, Look section: "Font", whose first option is "Grid font (Inter)", meaning inherit (the key is removed), then the 7 fonts. An explicit choice is pinned even if it equals the inherited one, so a later grid change leaves it alone.
  - Tile section: "Value font", first option "Button font (Orbitron)", then the 7 fonts.
- `config.ts`: `layout.font?`, `ButtonConfig.font?`, `TileConfig.font?`. `edit.ts` `drawn()` adds the resolved label and value fonts, and the layout key adds `layout.font`. `errors.ts` adds `font` and `tile.font` fields, and the grid's problem list takes `layout.font`.
- **Licenses**: `bundle.resources` in `tauri.conf.json` ships `fonts/<id>/` license files as `fonts/licenses/<id>.txt`.

## Rejected alternatives

| Option | Why not |
|---|---|
| FreeType or Tiny TTF on the board (`LV_USE_FREETYPE`, `LV_USE_TINY_TTF`), rendering TTFs at run time | Adds a rasterizer, a glyph cache in RAM and run-time cost to save about 250 KB of flash we have plenty of (F13). Bitmap fonts match Montserrat's pipeline |
| Fonts on the TF card | The card is read-only and handed to USB after boot (`sdcard.c`, CLAUDE.md), and a missing card would break the display |
| Sending fonts over the link, like icons | Large binaries for something the firmware can ship. Needs a new frame type and cache |
| lv_font_conv through mise's `npm:` backend | A global `npm install` resolves its semver-range dependencies afresh, so the output could change without any pinned input changing. A small locked `fonts/package.json` costs nothing extra, since node and pnpm are already pinned |
| Generating C at build time in CMake | The `espressif/idf` container has no node (F18, CLAUDE.md). Checked-in output plus a drift hook gives the same guarantee |
| Each font at `--size 24/32` with its own line height feeding the geometry | Changes icon boxes per font, so `box_px` and icon ids would depend on fonts across three codebases (F4) |
| A serde enum for font ids | Gives "unknown variant" errors in serde's wording that `errors.ts` can't route, unlike the `check()` messages |
| Sending the three config levels raw (`layout.font`, `button.font`, `tile.font`) | The board would have to know the config's rules. The wire's message default plus differing overrides keeps the same inheritance with board logic of two `??`s |
| Fully resolved ids on every button, no message default | Simplest board, but every button repeats the grid font, and pages' page label needs the grid font even on a page whose buttons all override it |
| Integer font ids on the wire | Saves about 1.5 KB at worst, but ties boards and daemons to one enum order and makes logs unreadable. Names fit the 16 KB budget |
| A `v` bump, or a fonts-only flag such as `hello.fonts: true` | Today's daemon refuses `v` other than 1 (F7). A flag would be a second scheme next to the `caps` list pages needs |
| No negotiation, font fields sent to every board | Works (old boards ignore them, F6), but a font-only edit would rebuild an old board's grid for nothing, and the app couldn't say why the board shows Montserrat |
| Refusing a layout with an unknown font | A blank grid on the board for a cosmetic mismatch the user can't see there |
| Variable fonts instanced with fontTools | A modification under OFL (an Orbitron RFN problem) and one more pinned tool. Official static files exist for most families |
| WOFF2 or subsetted fonts in the app | Subsetting is a modification under OFL FAQ 2.6, and WOFF2 needs another tool. Unmodified TTFs cost about 1.5 MB of bundle |
| 48 px builds for chrome | See open question 1 |

## Slices

| Slice | Kind of worker | Depends on |
|---|---|---|
| S1 Sources, generator, generated outputs | infra + needs judgment | open question 2 (weight) |
| S2 Firmware, including `hello.caps` | needs design judgment | S1 |
| S3 Daemon: config, `hello.caps`, session view, wire, docs | needs design judgment | S1 |
| S4 Desktop app | needs design judgment | S1, S3 |
| S5 Hardware verification | operator + coder | S2, S3, S4 |

Order: S1, then S2 and S3 in parallel, then S4, then S5. All five land before pages S1, which then rebases (Coordination). Nothing here depends on pages.

### S1 Sources, generator, generated outputs

- **Files**: `fonts/` (manifest, TTFs, license texts, `generate.mjs`, `package.json`, `pnpm-lock.yaml`), `mise.toml` (`fonts:gen`, `fonts:check`, `fmt` exclusion), `.pre-commit-config.yaml` (`fonts-generated` hook, clang-format exclude), `.gitignore` (`fonts/node_modules/`), and the generated files listed above.
- **Steps**
  1. Download each candidate into its own empty scratch directory, check that it is a static TTF at weight 500 (`usWeightClass`), record URL and SHA-256, and copy the TTF and its license file into `fonts/<id>/` unmodified.
  2. Write the generator. For each (id, slot), try sizes from nominal downward until `line_height` fits the slot, and record the sizes in the manifest. Afterwards the generator only uses the pinned sizes and asserts the rule.
  3. Generate, then run `fonts:gen` a second time and confirm no diff.
- **Acceptance**
  - `mise run fonts:check` passes on a clean tree and fails after a one-byte edit to any generated file.
  - Every slot-32 font has 24 ≤ `line_height` ≤ 35, every slot-24 font 14 ≤ `line_height` ≤ 27, and each `--size` is ≤ its nominal size.
  - The generated `glyph_bitmap` and table arrays are summed from the C (a byte count, before any compile) and recorded as the first flash number.
  - `mise run lint` passes locally, including `fonts-generated`, typos, biome, cargo-fmt and gitleaks over the new files (for untracked files, `prek run --files`), and the lint job passes in CI on a branch, which proves the hook runs there.
  - `fonts:check` passes from a fresh clone (no `fonts/node_modules`).
  - Each `fonts/<id>/` has the license text that came with its file, and the manifest records the license.
- **Cheapest check**: `mise run fonts:check`, and `grep -h line_height firmware/main/fonts/*.c`.

#### S1 as built (2026-10-21)

- **Manifest** is `fonts/fonts.json`, not `fonts.toml`: node has no TOML parser, and a parser would be a second vetted dependency. It records per font the file, its SHA-256, the source URL (and the archive's SHA-256), the license file with its hash and URL, the copyright line and the slot sizes. The generator refuses to run when a TTF or license hash differs.
- **Sizes** (`--size`, resulting `line_height`), slot 24 then slot 32: Inter 22 (26) and 29 (35); Roboto 24 (26) and 32 (35); Fira Code 24 (27) and 31 (34); Roboto Mono 24 (26) and 32 (35); Orbitron 21 (26) and 28 (34); Oxanium 24 (25) and 32 (33). All inside 14..27 and 24..35.
- **Generated arrays**: 155,939 bytes for the 12 fonts (`node fonts/generate.mjs --stats`): `glyph_bitmap` 136,335, `glyph_dsc` 9,216, kern class maps 1,536, `kern_class_values` 8,852. The sums match `nm -S` of the C compiled with the host gcc, apart from the per-font structs (about 170 bytes each). That is under the 200 to 320 KB estimate, so the S2 stop trigger is unlikely.
- **Orbitron has no `^`** (U+005E; the League of Moveable Type TTF). Its `^` comes from Oxanium Medium (also OFL-1.1): both Orbitron builds are converted from Orbitron for 0x20-0x5D and 0x5F-0x7E plus Oxanium for 0x5E, at the same `--size`, and the line-height rule still holds. The manifest records this as a `fallback`, the generated C header credits Oxanium, and the generator fails if any of the 95 glyphs is missing. The other five cover all 95 glyphs.
- **Directory check**: the generator also fails on any file in `fonts/` or `fonts/<id>/` that the manifest doesn't name.
- **Licenses**: Inter and Oxanium ship theirs in the release zip. The Roboto and Fira Code zips carry none, so their texts are the repo's at the release tag (URLs in the manifest). Roboto Mono and Orbitron come from a pinned commit of the upstream repo, as no release archive carries a static Medium. Only Orbitron has a Reserved Font Name. Orbitron's TTF and license were renamed on copy (spaces in the names); the bytes are unchanged.
- **`static_bitmap`**: lv_font_conv 1.5.3 does not write `.static_bitmap = 1`, unlike the built-in Montserrat. LVGL's software renderer reads it only for A8 glyphs (`lv_draw_sw_letter.c:144`), so it has no effect on these 4 bpp fonts, and the C stays as the converter wrote it.
- **Dependencies**: lv_font_conv 1.5.3 bundles its seven dependencies in the tarball (`bundledDependencies`), so the lockfile pins one integrity hash. None has an install script or a native binary. The tasks pass `--ignore-scripts` anyway.
- **Not done here**: the CI run of the lint job on a pushed branch.

### S2 Firmware

- **Files**: `firmware/main/font_ids.c/.h`, `fonts.c/.h`, `proto.c/.h`, `ui.c`, `CMakeLists.txt`; `firmware/test/host_proto/main/CMakeLists.txt` and `test_proto.c`.
- **Files, also**: `link.c` (the `"fonts"` cap). The cap goes in the same commit as the parser and `ui.c` changes, so no build advertises fonts it can't draw.
- **Host test cases** (layout):
  - No font fields: everything Montserrat, no flag.
  - Message `font` only: every label and value in it.
  - `font` inter, a button with `label_font` orbitron and no `value_font`: its value is orbitron. With `value_font` fira-code: value fira-code.
  - `"label_font":"nope"`, a number, `null`, an object, a 200-character string, and the same for message `font`: Montserrat, flag set, layout accepted.
  - The fullest layout with every font field parses.
  - `proto_build_hello` with no caps prints no `caps` key (today's bytes). With `{"fonts"}` it prints `"caps":["fonts"]`. With `{"fonts","pages"}` and a 32-character `fw` it still fits 256 bytes.
- **Acceptance**
  - `mise run fw:build` passes with no new warnings, and `idf.py size` reports the image.
  - The font total, summed from `riscv32-esp-elf-nm -S` over `wd_font_*` symbols and their `glyph_*`/`kern_*`/`cmaps` tables, is recorded. Expected 200 to 320 KB (estimate below). If the image grows by more than 600 KB over the baseline 1,695,888 bytes, stop and revisit kerning or compression.
  - `mise run fw:test` passes, and CI's host-test step is green.
- **Also**: pin `CONFIG_LV_USE_FONT_PLACEHOLDER=y` in `firmware/sdkconfig.defaults` (it is on in the generated `sdkconfig` today, but a default is not a pin), and check the generated `sdkconfig` after adding it.
- **Cheapest check**: `mise run fw:test`, then `mise run fw:build`.

### S3 Daemon config, wire and docs

- **Files**: `config.rs` (`Grid.font`, `ButtonConfig.font`, `TileConfig.font`, checks, the wire fields in `Live::layout`), `protocol.rs` (`Hello.caps: Vec<String>` with `#[serde(default, skip_serializing_if = "Vec::is_empty")]`; message `font`, `Button.label_font`, `Button.value_font`, all skipped when `None`), `server.rs` (the session keeps the hello's caps; its view of the layout strips font fields without `fonts`; `DeviceInfo.caps`), `app/src/api.ts` and `Devices.tsx` (the caps and the note), `tests/fixtures/hello.json` (with `"caps":["fonts"]`, matching the protocol.md example, plus an inline test that a hello without `caps` parses to an empty list), every `Message::Hello` pattern and constructor that adding a field breaks (`server.rs:559`, `protocol.rs:546`, `tests/device_sim.rs:228`; `examples/fault_daemon/session.rs:346` already uses `..`), `CLAUDE.md` (Conventions: `firmware/main/fonts/`, `font_list.h`, `fonts.gen.ts` and `fonts_gen.rs` are generated by `mise run fonts:gen` and never edited by hand; Daemon facts: `hello.caps`), `icons.rs` (comment only: `LABEL_LINE` is the slot-32 line box, the same for every font; the SVG worktree edits this file, so take both), `tests/fixtures/layout_fonts.json` (new, with `layout.json` unchanged), `tests/device_sim.rs`, `site/src/config.md` (a Fonts section: ids, the inheritance, what each level covers, chrome stays Montserrat, wide fonts cut sooner, the downgrade note), `site/src/protocol.md` (`hello.caps` as a general list with its rules, the `fonts` cap, the three fields and how the board resolves them, the fallback rule, what a session without the cap gets; adding caps doesn't change `v`).
- **Tests**
  - An unknown name refused at each level, with the message prefix `errors.ts` expects.
  - The wire matrix: none set, grid only (message `font` only), button only, tile only, all three, an explicit `montserrat` button under an `inter` grid (`label_font":"montserrat"` is sent, because it differs), a button equal to the grid (nothing sent), a tile whose value equals its label (no `value_font`), a non-tile button with `tile.font` impossible by construction.
  - A config without font keys gives the same `layout` JSON as before, for sessions with and without the cap.
  - `device_sim`: a hello without `caps` gets no font fields, and a font-only edit pushes it nothing. A hello with `["fonts"]` gets them, and a font-only edit pushes a new `rev` naming the same icon ids. A hello with `["fonts","bogus"]` works like `["fonts"]`. Every existing `device_sim` test passes unchanged.
  - The fullest-layout test (`config.rs:1475`) with message `font` and both button fields at `roboto-mono` on all 40 buttons stays ≤ 16 KB, and logs its size so pages can see its margin.
  - Round trip: a config with fonts at all three levels saves and reloads equal.
- **Acceptance**: `cargo test`, `cargo clippy -- -D warnings`, `mise run site:build` and rumdl pass.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml config`.

### S4 Desktop app

- **Files**: `app/src/fonts.css`, `fonts.ts`, `preview.ts`, `lvtext.ts`, `ButtonPreview.tsx`, `GridEditor.tsx`, `ButtonForm.tsx`, `config.ts`, `edit.ts`, `errors.ts`, `main.tsx` (import the CSS), `app/vite.config.ts`, `app/src-tauri/tauri.conf.json` (CSP, `bundle.resources`); tests in `app/tests/preview.test.ts` and `edit.test.ts`.
- **Tests**
  - `iconBox` and `labelLines` give the same numbers for every font on every grid size.
  - `dotLines` with a box height: the fonts with the smallest and the largest generated `lineHeight` cut a long label on line 2 in the 70 px icon label and on line 1 in the 35 and 27 px boxes, and the Montserrat results equal today's (the existing `preview.test.ts` cases pass with the box height 70 or 35 passed for their line counts).
  - Every generated slot's `lineHeight` fits its line box (a guard on the generated file).
  - A fixed string at a fixed width breaks differently in Orbitron and Inter (proves the tables are wired).
  - `layoutChanged` is true for a change at each of the three levels, and false for an action-only edit.
  - `resolveFonts` matches the Rust matrix.
- **Acceptance**
  - `pnpm test`, `biome check` (from `app/`) and `pnpm tauri build --debug --no-bundle` pass.
  - In `app:dev`, each font renders in the preview (not a fallback face) and the console shows no CSP violation.
  - The built `dist/` holds the six `.ttf` files, and `grep -r "data:font" dist` finds nothing.
  - A bundled build (`mise run app:build`) contains the license files.
- **Cheapest check**: `pnpm test` in `app/`, then one look at `app:dev`.

### S5 Hardware verification

Run with the tray app in `app:dev` and serial capture on. Don't edit under `app/src-tauri/` during it.

1. **Baseline**: before flashing, apply an 8×5 config with icons, tiles and wrapping labels, and record the `LVGL heap … at most` and `min free internal heap` lines and a photo.
2. **Same config, new firmware** (`mise run fw:flash`; NVS and pairing survive): the photo matches, the heap lines are within 1 KB, and `idf.py size` shows the image under 2.1 MB.
3. **Each font in turn** as `layout.font`, on a 5×3 grid (slot-32 values) and a 5×5 grid (slot-24 values, `cell_h` 71 < 80): the board redraws without reconnecting, logs `layout_ack` and **no `icon_get`**. Line breaks and dots match the editor preview for a 2-line icon label, a wrapping label, a tile label and a long tile value.
4. **Overrides**: button font and value font on one tile button, everything else on the grid font. Only that button changes.
5. **Old firmware, new daemon**: flash the build at the commit before S2 (record its hash here), keep the font config. Its hello has no `fonts` cap, so the daemon sends no font fields (check the daemon's debug log of the layout), a font-only edit pushes nothing, and the Devices tab shows the note. The board draws Montserrat and acks every layout. Then reflash the new build; the pairing survives both flashes.
6. **Unknown font on new firmware**: optional, with the fault harness sending `"label_font":"nope"`. The host test already covers the parser, so run it only if harness time is cheap.

- **Acceptance**: every step passes, and the plan records the measured image size, font total, heap lines and photos.

## Flash estimate

- Montserrat's 4 bpp bitmaps (F12) hold 95 ASCII glyphs plus 63 others, of which the 61 FontAwesome icons are the largest. ASCII-only works out to about 35-60% of those: roughly 9-14 KB at slot 24 and 15-25 KB at slot 32, plus about 4 KB of descriptors, class maps and kerning per font. Medium weights and Orbitron's width push some fonts up to 1.5×.
- **Estimate: 200-320 KB for 12 fonts**, about 5-8% of the 4 MB factory partition. The image grows from 1.70 MB to about 1.9-2.0 MB, leaving about half the partition free.
- Adding chrome sizes (48 px) for all six would add about 300 KB more.

## Coordination

### With `plans/pages.md`: fonts first, pages rebases

The user chose to land fonts first. Every fonts slice lands before pages S1.

#### What pages S1 and later slices rebase onto

- **`hello.caps` already exists** on both sides, as pages S1 specified it: `Hello.caps: Vec<String>` (serde default, unknown names ignored), `proto_build_hello(…, caps, n_caps)`, the `hello.json` fixture with `caps`, `DeviceInfo.caps`, and the general rules in `protocol.md`. Pages S1 drops those items and adds only the `"pages"` name. Pages S4 makes `link.c` pass `{"fonts","pages"}` in the commit that adds set assembly, as pages already plans for its own cap.
- **The session view exists**: `server.rs` compares and sends `view(&live.layout(), &caps)`, which today only strips font fields. Pages S3 adds "home page only, no page links" for sessions without `pages`.
- **`layout` carries `font`** at message level, and buttons may carry `label_font`/`value_font`. Pages puts `font` on every page message of a set (same value on each, since one `[layout]` serves all pages). Its `layout_page.json` fixture should include `font` once.
- **Parser**: `parse_layout` already fills `proto_layout_t.font` and per-button `label_font`/`value_font` bytes. Pages' new fields sit beside them, and pages' PSRAM model stores the bytes with no font logic.
- **16 KB**: the fullest-layout test already carries every font field (about 13.3 KB). Pages adds its per-message fields (about 100 bytes) to the same test.
- **Preview**: `preview.ts` has fonts' `layoutButton(…, fonts)` signature, `TextBox.family`/`px` and slot line boxes, plus `fonts.ts`/`fonts.gen.ts`. `lvtext.ts` `dotLines` takes a box height in pixels. Pages filters which buttons show and, for its bottom label, shrinks the grid area; it meets fonts in `GridEditor.tsx`'s call to `layoutButton` and in `edit.ts` `drawn()`.
- **Devices tab**: the caps note exists. Pages adds its own line from the same list.
- **Flash**: fonts take about 200-320 KB. Pages adds code, not data, so the 4 MB partition is not a concern for it.

#### Pages' "Page x of y" label

- It draws in the **grid's default font** (`layout.font`, the message `font`) at **slot 24**, in Montserrat when none is set. It belongs to the grid, not to any button, so per-button overrides don't apply, and slot 24 needs no new font sizes.
- Its line box is the slot-24 box, **27 px for every font** (fonts' fixed slots), so pages can reserve a fixed strip below the grid (27 px plus its own margins) that no font choice moves. That keeps pages' geometry change (a shorter grid, new `box_px` numbers) one-time and font-independent.
- This is why the message carries `font` even when every button overrides it.

**What fonts deliberately doesn't do for pages**: no per-page default font (one `layout.font` serves all pages; the wire would already allow it per message later), and no change to `GRID_TOP` or the grid height. Pages owns the height change, and since fonts made `box_px`, `ui.c` and `preview.ts` all read the same slot constants, pages changes one grid-height constant in three places as before.

### Other work in flight

- The insecure_tls and SVG-icon worktrees touch the same files (F19). Merge whichever lands first, then rebase. Nothing here depends on them.

## Risks

| Risk | Mitigation |
|---|---|
| A font's glyph-box line height forces a smaller em size, so it looks small next to Montserrat | Visible in the preview. Sizes live in the manifest and can be tuned within the rule |
| The preview and the board differ by a pixel or two vertically (the browser places the baseline from the TTF's own metrics, LVGL from `base_line`) or in glyph shapes (FreeType 4 bpp against the browser's rasterizer) | Line breaks and dots, the part users act on, come from the same advance tables. Kerning is left out of the tables as today (`montserrat.ts:5-6`) |
| Orbitron's Reserved Font Name: the OFL FAQ (1.21, 2.2) treats a format change in firmware as a Modified Version that can't use the RFN | The app ships the unmodified TTF under its own name. The converted bitmap has no user-facing name and carries the license header. Open question 6 |
| Pages rebases onto a changed `hello`, `layout`, `server.rs` session loop and `preview.ts` | Fonts implements `hello.caps` exactly as pages S1 wrote it, and Coordination lists every touch point. The pages planner should update its S1 file list before starting |
| lv_font_conv 1.5.3 output differs between machines | Its rasterizer is bundled wasm and its dependency tree is locked. The `fonts:check` hook runs on dev and in CI, so any drift shows at once; S1 proves a fresh clone and CI match |
| Flash use above estimate | S1 counts the arrays, S2 measures the image, with a stop trigger at 600 KB of growth |
| A dependency bot bumps `fonts/pnpm-lock.yaml` and the output changes, turning lint red on that PR | No Renovate config is tracked in the repo today. If one is added, group `fonts/` updates, and a bump PR must include a `fonts:gen` rerun with the output diff reviewed. A red lint there is the intended signal |
| A downgraded app refuses a config with font keys (F8) | Rollback note: remove the font keys before going back to an older app, or the last good config (or an empty grid on a fresh start) stays live |
| Textual conflicts with the two worktrees and with pages | Rebase in landing order. The slices are small per file |

### Rollback

- Each slice reverts on its own. Reverting S2 leaves boards drawing Montserrat with a font-aware daemon, which is the supported old-firmware case.
- Reverting S3 needs font keys removed from `config.toml` first (F8).

## Open questions

Settled 2026-10-21 by the user, all at the recommended default:

- **(1)** Status, pairing and result screens stay Montserrat.
- **(2)** All six fonts are built at Medium (500).
- **(4)** A tile value that doesn't fit is cut with dots.
- **(6)** Orbitron ships as `orbitron`.

Questions 3, 5, 7 and 8 keep their recommended defaults. Unrelated to these answers, but recorded the same day: pages is now capped at 4, and its "Page x of y" label sits below the grid (`plans/pages.md`).

1. **Do the status, pairing and result screens follow the global font?** Recommended: no. They show before any layout and on an unpaired board, so the font would have to live in NVS, and they'd need 48 px builds (+~300 KB). Alternative: the result modal alone follows the pressed button's label font at 32 and 24, which needs no new sizes.
2. **Weight.** Recommended: Medium (500) for all six, matching the built-in Montserrat Medium. Alternative: Regular (400), lighter on the panel.
3. **A separate `label_font` key on buttons.** Recommended: no. With two widgets, `button.font` plus `tile.font` covers every combination. Add it only if a third widget is planned.
4. **Shrink a tile value to slot 24 when it doesn't fit at slot 32.** Recommended: no; keep dots, as today, and let the preview show the cut. Alternative: measure with `lv_text_get_width` in `ui_set_tile` and fall back to the slot-24 font of the same family.
5. **Fonts added after this plan.** Recommended: a later font set gets its own cap (for example `fonts2`), and the daemon sends a font id only to sessions whose caps cover it, falling back to `montserrat` otherwise. Until then, the board's Montserrat fallback covers any mismatch. No decision needed now.
6. **Orbitron and its Reserved Font Name.** Recommended: ship it as `orbitron`. The app bundles the unmodified TTF with its OFL, so the name the user sees in the selector names that unmodified font. The converted bitmap carries the license header and has no user-facing name of its own (LVGL fonts have no name field; the C symbol `wd_font_orbitron_*` isn't presented to users, and the RFN clause covers "the primary font name as presented to the users"). Alternative, if the user wants no doubt: drop Orbitron, or ask its authors for permission.
7. **Font files in git.** Recommended: check in the unmodified TTFs (about 1.5-2 MB) so the app build and `fonts:check` work offline in CI. Alternative: download them in `fonts:gen` and in the app CI job, verified by the manifest's SHA-256.
8. **Font of pages' "Page x of y" label.** Recommended: the grid's default font at slot 24 (Montserrat when none is set), with a fixed 27 px line box. Alternative: always Montserrat, treating it as chrome like the status lines.

## Critique log

Outside review: codex (gpt-6.1-sol, high), plan mode. Two earlier calls returned nothing usable (the first got no plan text, the second couldn't read the scratchpad file). The third got the plan inline but couldn't open the repo, so its points are plan-level; each was checked against the code here.

| # | Point | Outcome |
|---|---|---|
| 1 | `url(../fonts/…)` in `app/src/fonts.css` resolves to `app/fonts/` | adopted: `../../fonts/…`, and a production-build check, since `server.fs.allow` only affects the dev server |
| 2 | Stripping per session needs a rule for `rev`, or stripped layouts could compare unequal | adopted as a clarification: `Layout` has no `rev` (`config.rs:157-173`), `rev` bumps only when the stored view differs (`server.rs:727-732`). The `device_sim` test already covered it |
| 3 | Equal outer boxes don't prove equal wrapping and dots | adopted: checked `lv_label.c:360,1396-1418`. LVGL cuts by box height, so the generator also asserts lower bounds on `line_height`, `lvtext.ts` `dotLines` takes the box height, and S4 tests the smallest and largest fonts |
| 4 | Not shown that the lint job installs the tool and runs the hook | adopted: `mise-action` installs every tool (`ci.yml:28-30`); the converter now comes from a pnpm lockfile, with node and pnpm already pinned; S1 accepts only on a green CI lint run |
| 5 | Generated TS and Rust meet biome, rustfmt and typos | adopted: the generator formats both with the pinned tools before writing; only clang-format is excluded; S1 runs the full lint |
| 6 | Flash estimate speculative; "const" doesn't mean zero render allocations | adopted: S1 counts array bytes from the generated C first, S2 measures the image, the stop trigger is defined as image growth, and the memory claim is scoped to storage with S5 measuring the heap |
| 7 | License facts depend on the pinned files; firmware notices unspecified | adopted: S1 already audits each pinned file; added the firmware-side notice (headers plus `fonts/*/` texts, shipped with any published binary) |
| 8 | Pinning the converter version doesn't pin its dependency tree | adopted: `fonts/package.json` + `pnpm-lock.yaml` instead of mise's `npm:` backend (its deps are `^` ranges per `npm view`); generation reads only committed inputs; fresh-clone check in S1 |
| 9 | Fallback parser tests should be mandatory | rejected: already in S2's host cases (absent, valid, unknown, non-string, over-long, at message and button level, inheritance under a non-Montserrat default); the reviewer saw a condensed plan. The fault-harness case stays optional |
| 10 | Several hand-kept font catalogs could drift | adopted: the manifest is the one source; `font_list.h`, `fonts_gen.rs`, `fonts.gen.ts` are generated, and `fonts.c` builds its table from the X-macro |

Own review additions after the outside pass, all adopted: path-independent `Opts:` headers so `fonts:check` can diff a temp-dir run; the `Message::Hello` patterns that adding `caps` breaks (`server.rs:559`, `protocol.rs:546`, `device_sim.rs:228`); a risk row for lockfile bumps; a CLAUDE.md note that the font outputs are generated.

Coordination changes made during planning, from the coordinator: align with `plans/pages.md` (caps list, `font` on every page message, 16 KB test, shared preview files), then fonts-first ordering with fonts owning `hello.caps`, and a font rule for pages' "Page x of y" label. All adopted above.
