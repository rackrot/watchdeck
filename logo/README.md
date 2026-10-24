# Logo

The watchdeck mark: a solid panel with six keys cut into it, the top-right key lit in the accent.

| File | What |
|---|---|
| `mark.svg` | The mark on a transparent 1024 viewBox. Body `#e7edf3`, lit key `#38bdf8`. The source of every output below. |
| `app-icon.svg` | The mark at 90% on a rounded square of `#0a0d11` (the app's `--bg-0`). The source of the OS icon set. |

## Outputs

Run `mise run logo:gen` after any change here, and commit the result. `mise run logo:check` (in the lint hook) fails when a text output is out of date.

| Output | Made from | By |
|---|---|---|
| `app/src-tauri/icons/*` (PNG, `icon.icns`, `icon.ico`, plus the `ios/` and `android/` sets `tauri icon` writes) | `app-icon.svg` | `pnpm tauri icon` |
| `firmware/main/logo_img.c`, `logo_img.h` (`wd_logo`, 240 px A4 bitmap) | `mark.svg` | `app/src-tauri/examples/logo_gen.rs` |
| `app/src/assets/logo.svg`, `app/public/favicon.svg` | `mark.svg` | `app/src-tauri/examples/logo_gen.rs` |

The PNG and icon files are not diffed by `logo:check`, since encoder output can change between versions. Regenerate them only when `logo/` changes.

## The device bitmap

The board can't draw the accent, so `wd_logo` is single-color A4: each pixel's alpha, quantized to 16 levels, two pixels a byte with the left one in the high nibble. The image style recolors it to the screen's foreground. The accent key therefore comes out as a solid square of the same alpha as the panel.

Do not edit the generated files by hand. Change `mark.svg` and run `mise run logo:gen`.
