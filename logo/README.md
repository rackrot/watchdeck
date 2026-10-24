# Logo

The watchdeck mark: a solid panel with six keys cut into it, the top-right key lit in the accent.

| File | What |
|---|---|
| `mark.svg` | The mark on a transparent 1024 viewBox. Panel `#e7edf3`, lit key `#38bdf8`. The source of every output below. |
| `app-icon.svg` | The mark at 90% on a rounded square of `#0a0d11` (the app's `--bg-0`). The source of the OS icon set. |

## Outputs

Run `mise run logo:gen` after any change here, and commit the result. `mise run logo:check` fails when a text output is out of date. CI runs it in the app job, and it's also useful locally.

| Output | Made from | By |
|---|---|---|
| `app/src-tauri/icons/*` (PNG, `icon.icns`, `icon.ico`) | `app-icon.svg` | `pnpm tauri icon`, then pruned to what `bundle.icon` lists |
| `firmware/main/logo_img.c`, `logo_img.h` (`wd_logo`, 240 px I4 bitmap) | `mark.svg` | `app/src-tauri/examples/logo_gen.rs` |
| `app/src/assets/logo.svg`, `app/public/favicon.svg` | `mark.svg` | `app/src-tauri/examples/logo_gen.rs` |

`logo:gen` deletes what `tauri icon` writes beyond the desktop bundle: the iOS and Android sets and the Store logos. It keeps the files `bundle.icon` lists in `tauri.conf.json`, plus `icon.png` as a precaution, since `tray.rs` uses the default window icon.

The icon images are not diffed by `logo:check`, since encoder output can change between versions. Regenerate them only when `logo/` changes.

## The device bitmap

The board draws `wd_logo` as LVGL I4: a 16-entry palette, then two pixels a byte with the left one in the high nibble. Each palette entry is B, G, R, A. Entries are:

| Index | Entry | Used for |
|---|---|---|
| 0 | clear | outside the mark and in the keys' holes |
| 1 to 8 | the panel at alpha 31 to 255 | the antialiased outer and hole edges, and the panel itself at 8 |
| 9 to 11 | the panel blended toward the key at 1/4, 1/2 and 3/4 | the key's antialiased edge over the panel |
| 12 | the key, `#38bdf8` | the lit key |
| 13 to 15 | clear | unused |

The palette is fixed, so the board draws the panel in `#e7edf3` and the key in `#38bdf8` whatever the screen's foreground. The sleep screen can't recolor it.

Do not edit the generated files by hand. Change `mark.svg` and run `mise run logo:gen`.
