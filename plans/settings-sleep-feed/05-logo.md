# Logo: candidates, assets, app branding, splash, and the device

Part of [`00-index.md`](00-index.md). Findings F1 to F44 are there.

Today the app has Tauri's default icons, the Vite favicon and a text-only wordmark (F32), and the board has no logo. After this plan the chosen mark is the app's icon set (taskbar, tray, installer), the favicon, beside the wordmark in the header, on an in-window splash, on the board's boot screen, and on the default sleep screen.

## Decisions

### The brief for the candidates (L1)

- **What it stands for**: a Stream Deck-style controller for a homelab, a grid of keys on a desk that watches and runs things. The name is "watchdeck".
- **Theme**: the app is dark (`--bg-0` #0a0d11) with a sky-blue accent (`--accent` #38bdf8) and a monospace wordmark (`theme.css`, `App.css`). The board is black with grey text.
- **Constraints**:
  - Reads at 16 and 32 px (tray, taskbar), so a few bold shapes, no thin lines, no text inside the mark.
  - One color plus at most one accent, so it works as a recolorable single-color bitmap on the board and on both dark and light backgrounds.
  - Square, on a 1024 viewBox, with the shapes inside a 10% margin (the app icon gets a rounded-square background added by the pipeline).
- **Three directions**, each one SVG:
  - **A, Deck**: a 3 x 2 grid of rounded keys, one of them lit in the accent. Says "keys on a panel" directly.
  - **B, Watch**: a rounded screen with a pulse line across it, whose peak is a key. Says "watching".
  - **C, W**: a monogram W built from three key caps stepping down and up, the middle one lit.
- **Preview**: each SVG rendered to PNG at 1024, 256, 64, 32 and 16 px, on `--bg-0`, on white, and as a white single-color version, into one contact sheet per candidate, plus a mock of the app header and of the board's boot screen. Rendering uses resvg, already a direct dependency of the daemon (`Cargo.toml`), from a scratch example run in a throwaway worktree, so no tool is installed.
- **Gate**: the user picks one (or asks for changes) before L2 starts.

### The device asset (D15)

| Option | Flash | Pool and PSRAM | Verdict |
|---|---|---|---|
| A4 bitmap (16 alpha levels), 240 x 240, recolored by the image style | 28,800 bytes | one image widget, about 200 bytes of pool; draws from flash | **chosen** for a single-color mark |
| I4 (16-color palette), 240 x 240 | 28,800 plus a 64-byte palette | the same | chosen instead if the picked mark needs its accent on the board |
| A8 or RGB565A8 | 57,600 or 172,800 | the same | more flash for no visible gain at this size |
| LVGL vector graphics (ThorVG) | hundreds of KB of library | a renderer on the LVGL task | off in this build (F22), and slow in software |
| Drawn from LVGL rectangles and arcs | none | 10 to 20 objects (2 to 3 KB of pool) | ties the design to boxes and circles, and every change is C code |
| A PNG on the TF card | none | a decoder that's off (F22) | must work with no card |

- One size, 240 px, is drawn on the boot screen and centered in the sleep screen's image area. Recoloring to the sleep screen's `fg` comes free with A4.

### The pipeline (L2)

- Sources live in `logo/` at the repo root: `mark.svg` (the chosen mark), `app-icon.svg` (the mark on a rounded square of `--bg-0`, for the OS icon), and `README.md` (the colors and where each output goes).
- `mise run logo:gen`:
  1. `pnpm tauri icon ../logo/app-icon.svg` from `app/` (the Tauri CLI version pinned by the lock) writes the whole set into `app/src-tauri/icons/` (F39).
  2. `cargo run --example logo_gen` in `app/src-tauri` renders `mark.svg` with resvg at 240 px, quantizes alpha to A4 (or the palette to I4), and writes `firmware/main/logo_img.c` and `logo_img.h` (an `lv_image_dsc_t` named `wd_logo`). It also copies `mark.svg` to `app/src/assets/logo.svg` and `app/public/favicon.svg`.
- `mise run logo:check` regenerates the C file into a temp dir and fails on any difference, like `fonts:check`. The PNGs from `tauri icon` are checked in and not diffed (encoder output may differ between versions), only regenerated on a change to the source.
- `firmware/main/logo_img.c`, like the generated fonts, is never edited by hand (CLAUDE.md conventions gain the line).
- Why not LVGL's `LVGLImage.py` (F36): it needs `pypng`, which nothing pins, and a second image tool when resvg already renders our SVGs.

### The splash (D16)

- **In the main window**: `index.html` holds a static splash (the mark, the wordmark, a small CSS spinner) styled by `splash.css`, linked from `index.html` so the production CSP (`style-src 'self'`) allows it. React removes it once `get_config` has answered, and keeps it at least 300 ms so it doesn't flash.
- **When it shows**: only on a visible start. An autostart with `--hidden` (F31) shows no window, so no splash.
- **Measure first** (L3): log the time from process start to `show_window`, and from `show_window` to React's first render. If setup itself (identity, config, icon rendering, the bind, which all run before the window shows) takes over 1.5 s, the in-window splash can't cover it, and the rejected second window (below) comes back as an option with that number.

## Goal and done

- The user has picked a mark from the L1 sheets.
- `mise run logo:gen` regenerates every output from `logo/`, and `logo:check` passes in CI's lint job.
- The app shows the new icon in the taskbar, the tray and the installer; the favicon and the header use the mark; the template's `react.svg`, `vite.svg` and `tauri.svg` are gone.
- A visible start shows the splash until the config loads.
- The board shows the mark on boot, and on the sleep screen when no image is set, recolored to `fg`.
- Flash grows by under 32 KB for the device asset.

## Slices

### L1 Logo candidates

- **Output** (scratchpad only, nothing in the repo): `logo-a.svg`, `logo-b.svg`, `logo-c.svg`; one contact-sheet PNG each (the sizes and backgrounds above); a header mock and a boot-screen mock (1024 x 600) per candidate.
- **Acceptance**: the user picks one, or names changes and a second round runs once.
- **Cheapest check**: the user's look at the sheets.
- **Owner**: design.

### L2 Logo asset pipeline

- **Files**: `logo/` (new), `app/src-tauri/examples/logo_gen.rs` (new), `mise.toml` (`logo:gen`, `logo:check`), `.pre-commit-config.yaml` or `ci.yml` (`logo:check` in the lint job), `app/src-tauri/icons/*` (regenerated), `firmware/main/logo_img.c/.h` (generated), `firmware/main/CMakeLists.txt`, `app/src/assets/logo.svg`, `app/public/favicon.svg`, `CLAUDE.md` (the generated-files line).
- **Tests**: `logo:check` passes on a clean tree and fails after a one-byte edit to `logo_img.c`; the generator's own test renders a known SVG square to the expected A4 bytes.
- **Acceptance**: `mise run logo:gen` twice gives no diff; `mise run fw:build` passes and logs the image size growth; `pnpm tauri build` produces the new icons.
- **Cheapest check**: `mise run logo:check`.
- **Owner**: mechanical, tooling.

### L3 App branding and splash

- **Files**: `index.html` (favicon, the splash markup), `app/src/splash.css` (new), `main.tsx` (remove the splash after the first config load), `App.tsx` and `App.css` (the mark beside the wordmark), `lib.rs` (the two timing log lines), `public/vite.svg`, `public/tauri.svg`, `src/assets/react.svg` (deleted).
- **Acceptance (window)**: a visible start shows the splash and then the app with no flash of an empty page; the header shows the mark at 20 px; the CSP stays as it is; the two timings are recorded.
- **Cheapest check**: `pnpm build`, then `mise run app:dev`.
- **Owner**: mechanical, app.

### L4 Device boot logo and default sleep art

- **Files**: `ui.c` (a boot view from `ui_init`: the mark, "watchdeck" in Montserrat 32, the firmware version in 24; shown for at least 1.2 s and at most 3 s, then status mode, so status texts are never hidden longer), `sleep.c` (the mark in place of P1's placeholder, recolored to `fg`).
- **Acceptance (batch B)**: the boot view shows after the panel comes up and gives way to status mode within 3 s; the default sleep screen shows the mark in `fg`; the pool at "page built" doesn't move (the boot view is deleted).
- **Cheapest check**: `mise run fw:build`, then batch B.
- **Owner**: mechanical, firmware.

## Rejected alternatives

| Option | Why not |
|---|---|
| A second splash window (the Tauri guide's way) | Needs setup's work moved into a task and a two-flag handshake, for a window that's usually hidden at start (autostart). Tauri's own docs call it usually unnecessary (F39). Kept as the fallback if L3's timing says setup is slow |
| A splash on every start, including autostart | The window is hidden then; a splash would pop up at login |
| Text inside the mark | Unreadable at 16 px |
| A multicolor logo | Can't be one recolorable bitmap on the board, and fights both light and dark backgrounds |
| Keeping the boot view up until Wi-Fi connects | Hides status texts during a slow or failing join; 3 s at most |
| LVGL's `LVGLImage.py` | An unpinned Python dependency (F36) |
