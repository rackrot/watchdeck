# watchdeck

A Stream Deck-style homelab controller: an Elecrow CrowPanel Advanced 10.1" ESP32-P4 display plus a desktop app that talks to it.

## Layout

| Path | What |
|---|---|
| `firmware/` | ESP-IDF v6.1 C project for the ESP32-P4 |
| `app/` | Tauri 2 desktop app: React + TypeScript + Vite (pnpm), Rust in `app/src-tauri/` |
| `site/` | mdBook docs site; moving to a separate repo |
| `site/src/device-spec.md` | Hardware reference, transcribed from the vendor page |

## Hosting

- The repo lives on Forgejo at `https://git.voidrot.dev/rackrot/watchdeck` (remote `origin`). Issues, pull requests, releases and CI all happen there, and `#N` means a Forgejo issue or PR.
- Use `fj`, the Forgejo MCP tools and the `forgejo-*` skills and agents (`forgejo-issues`, `forgejo-pr`, `forgejo-actions`, `forgejo-ship`, `forgejo-readme`, `renovate`). Never use `gh` or the GitHub skills (`gh-issues`, `gh-pr`, `gh-actions`, `ship`, `dependabot`, `gh-readme`) for this repo.
- The `github` remote (`github.com/rackrot/watchdeck`) is the old home. Don't push to it or open issues or PRs there unless the user asks.

## Tooling

mise pins every tool and holds the tasks (`mise.toml`). Run `mise tasks` for the full list.

- `mise run idf:install`: one-time ESP-IDF install into `~/.espressif` (version from `ESP_IDF_VERSION`)
- `mise run fw:build`, `fw:flash`, `fw:monitor`: pass the port after `--`, e.g. `mise run fw:flash -- -p /dev/ttyUSB0`
- `mise run app:dev`, `app:build`
- `mise run site:build`, `site:serve`
- `mise run lint`: prek runs every hook in `.pre-commit-config.yaml`
- `mise run fmt`: formats Rust, TS, C, TOML and Markdown

Add new tools to `mise.toml`, not to CI or the hooks. The hooks call the mise-pinned binaries.

## Hardware facts

- Chip revision v1.3, so `sdkconfig.defaults` sets `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`. ESP-IDF v6.x defaults to v3+ silicon, and a build without this setting won't boot.
- The ECDSA peripheral also needs v3.0+ silicon, so `sdkconfig.defaults` turns off `CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY`. With it on, every TLS handshake logs a `psa_crypto_driver_esp_ecdsa` error and falls back to software anyway.
- 32 MB PSRAM at 200 MHz, 16 MB flash.
- The board enumerates as `/dev/ttyUSB0`. Serial access needs the `uucp` group.
- The board advertises `watchdeck.local` via mDNS (hostname only, no services yet).
- The board keeps its pairing (token, daemon name, pinned cert) as one NVS blob, namespace `watchdeck`, key `pairing`. It saves the blob only after a person taps Yes on its "Paired <device>?" question, and holding Forget pairing for 2 s erases it.
- The second USB-C port (J16, native high-speed USB, device only, separate from the CH340 console port) exposes the raw TF card to a host as a mass-storage drive. The firmware reads `wifi.txt`, then releases its own mount and hands the card to USB. Host writes apply on the next reboot.
- A 512 KB `coredump` partition (after `factory`, at 0x410000) holds the ELF coredump of the last panic. `main/crash.c` sends its summary to the daemon as a `report` and erases the dump on `report_ack`. Forget pairing erases it too, since a dump may hold the token. A normal `mise run fw:flash` installs the new partition table, and NVS and the pairing survive because the NVS offset is unchanged. Never `erase_flash` for this, since it wipes the pairing.
- USB MSC writes are acked to the host before they reach the card, and card write errors never reach the host (upstream esp_tinyusb behavior, unchanged as of 2026-09), so eject before unplugging.
- Pin map, from Elecrow's V1.2 schematic and examples:
  - Display: EK79007 over 2-lane MIPI-DSI, backlight on GPIO31 (active high).
  - Touch: GT911 on I2C0, SDA 45 / SCL 46 at 400 kHz, RST 40, INT 42 (INT has no pull-up, so it runs in polling mode). Verified at address 0x5D, product ID "911". The axes need no swap or mirror (knobs at the top of `main/touch.c`).
  - TF card: SDMMC slot 0, 1-bit only: CLK 43, CMD 44, D0 39.
  - ESP32-C6 Wi-Fi: SDIO slot 1, 4-bit: CLK 18, CMD 19, D0–D3 17/16/15/14, reset GPIO32. Elecrow's lesson configs say 54, which is wrong.
- Screen modes (`ui.c`): status mode (from boot until the first layout applies, and while unpaired) shows the five status lines, Licenses, Settings (on the touch line, hidden from the tap on a daemon until the attempt ends) and, while paired, Forget pairing. Grid mode (from the first applied layout until an unpair) shows the 36 px bar (`BAR_H`) and the grid from y 44 (`GRID_TOP`). Settings (`settings.c`) opens from grid mode by the long swipe or from status mode by its button, and replaces that mode's widgets while open; `settings_leave` in `ui.c` rebuilds the mode it came from (the page, or the status lines and the daemon list). Opened from status mode, a "Settings" title takes the bar's band. Its page has a 64 px tab row at y 44 with Close at the right end, and one tab's content below, built when shown and deleted when left (the tab table in `settings.c`; only Status so far, which holds the device and connection rows, Licenses and Forget pairing). A set that applies under settings opened from status mode makes grid mode the return mode.
- The long swipe up (`touch_track`, `ui.c`) opens settings at 200 px up (`SETTINGS_SWIPE_PX`), more than twice as far up as sideways (`DRAG_SWIPE_RATIO`), from the grid or a cell. Settings closes on Close, a swipe down, 60 s without a touch (`SETTINGS_IDLE_MS`, not while the license viewer is open), or an unpair. `grid_gesture` doesn't wait for release on `LV_DIR_TOP`, so the long swipe still reaches `PRESSING`.
- The bar font is Roboto Mono Regular 20, from the `chrome` array in `fonts/fonts.json` (`wd_font_bar`, not selectable and not in the app). Montserrat 20 is its fallback for the arrows.
- The bar's values use 12 px cells in that font. A percent takes 4 cells, a temperature 3 plus its unit, and a rate 6 (`si`) or 7 (`binary`, `bits`), each right-aligned (`hoststat.c`). `bar_create` (`ui.c`) measures each group's worst case over every unit form, with markup, then spreads the leftover width as equal gaps (within 1 px) between the four groups and the clock, with 12 px margins. The log line `bar groups (x/width)` shows the result, and `bar slot overflow` shows when the groups don't fit.
- A value past a threshold is drawn with LVGL recolor markup, `#rrggbb 97%#`, in the group's one label. The padding stays outside the markup, so the prefixes and units stay grey. The four host labels have recolor on, the clock doesn't, and `CONFIG_LV_TXT_COLOR_CMD` is `#`. A label or arrow with a `#` in it would break the markup, so none may have one.
- Touch is read every 10 ms (`TOUCH_READ_MS` in `firmware/main/touch.c`), not at LVGL's 33 ms default. At 33 ms a fast flick gave too few samples and landed as a press. The LVGL task sleeps about one FreeRTOS tick (10 ms at `CONFIG_FREERTOS_HZ=100`) in each of its two sleeps per pass, so reads land about 10 to 15 ms apart (measured on the board, pages S8).
- Boot order matters:
  1. Acquire LDO3 (2500 mV, DSI PHY) and LDO4 (3300 mV, IO bank).
  2. Mount the SD card.
  3. Start the display.
  4. Start Wi-Fi.
  The SD card and ESP-Hosted share one SDMMC host, and ESP-Hosted probes the C6 before `app_main`.
- The TF card is GPT-partitioned, so `main/sdcard.c` mounts the first GPT partition read-only through its own diskio driver. Never format the card or write to it.
- The C6 runs ESP-Hosted slave 2.12.3 and the host is 3.0.9. Hosted logs a "major version mismatch" line, which is only advisory. Don't OTA the C6 without asking.
- The icon cache holds every page's icons (`ICONS_MAX` 160 slots in a PSRAM table, under an 8 MiB byte cap), so a reset (an RTS pulse included) empties it. To check that a reconnect hits the cache, restart the daemon and leave the board running.
- `monitor` needs a real TTY. Without one, read the boot log with pyserial from the IDF Python env (`~/.espressif/python_env/idf6.1_py3.12_env/bin/python`). Opening the port doesn't reliably reset the board. To capture a boot, pulse RTS as esptool's hard reset does: DTR off, RTS on for 100 ms, then RTS off.
- The esp_websocket_client receive loop can keep running with its lock held when a peer sends part of a frame and then stalls. Tearing the client down (`esp_websocket_client_destroy` in `drop_client`, `link.c`) then waits on the websocket task and blocks the link task indefinitely. TCP keepalive in `open_client` (idle 5 s, interval 5 s, count 3) drops a peer that has vanished in about 20 s; on 2026-10-21 the link dropped 19 s after the desktop's address went away. A live peer that stalls mid-frame still wedges the board until a reboot. That's accepted, with no vendored patch: it takes a daemon bug or an unauthenticated peer on the pairing socket. Don't remove the keepalive settings. To recover a wedged board, pulse RTS, which keeps the pairing.

## Daemon facts

- The daemon serves `wss` on 47820 and opens plain `ws` on 47821 only while the pairing dialog is open. `site/src/protocol.md` is the wire spec.
- Its TLS identity and `state.json` (device id → SHA-256 of its token) live in `$XDG_CONFIG_HOME/watchdeck/` (`~/.config/watchdeck/` by default). The daemon moves the old `~/.config/dev.rackrot.watchdeck/` there on first start. A new identity makes every paired device pair again, so tests use temp dirs.
- `config.toml` (the grid and actions) and `secrets.toml` (0600, header values for HTTP actions) live there too. The daemon polls both every 500 ms, comparing file metadata plus a SHA-256 of each file's bytes (so a same-size edit in the same mtime tick still counts; icons stay metadata-only), and an invalid edit is refused while the last good config stays live. `site/src/config.md` documents the format.
- Button icons are PNG, JPEG, WebP or SVG files in `icons/` beside `config.toml`, named by a button's `icon`. The daemon shrinks each raster to fit its button (an SVG is drawn with resvg at the box size, loading no external files or fonts, after `icons.rs` checks it against a strict subset and a render-cost estimate), converts it to B, G, R, A pixels, and names it by a hash. The device fetches the ones it lacks with `icon_get` and keeps them in PSRAM while the applied set's pages name them. `site/src/protocol.md` has the frame, and `site/src/config.md` has the limits.
- The dev and test profiles build tiny-skia, tiny-skia-path, usvg and resvg optimized (`Cargo.toml`), because unoptimized they draw 20 to 50 times slower and the SVG timing test in `icons.rs` would measure the wrong thing. Recalibrate the cost weights in `render_cost` when bumping resvg.
- `hello.caps` (an optional list of strings) is how a device names what it supports beyond the base protocol, with no `v` bump. The daemon keeps only the names it knows, per session. A session without `fonts` gets a layout with every font field stripped, so a font-only edit pushes it nothing; with `fonts` it gets a new `rev`. `DeviceInfo.caps` shows the list in the Devices tab. A new capability goes in `CAPS` in `protocol.rs` and in `site/src/protocol.md`.
- `status_bar` (`CAP_STATUS_BAR`) gates `stats`, which goes once a second to each session that names it. `hoststats.rs` samples only while a session is open, from `/proc/stat`, `meminfo`, `net/route` (the default-route interface) and `net/dev`. The UTC offset comes from `time`'s `local-offset` feature, named in `Cargo.toml`.
- The bar's GPU is picked from `gpu.rs`, which lists display controllers by PCI address and never by card number. NVIDIA is read through `nvml-wrapper` =0.13.0 (`gpu/nvidia.rs`). `libnvidia-ml.so.1` is loaded at runtime, only while a session is open and the selected GPU is NVIDIA, on an `nvml` thread per sampling period (while any session is open, `status_bar` or not). A missing library ends that thread with one log line and no retry, and other open failures retry every 60 s. A reading older than 3 s shows `--`. AMD is read through sysfs: `gpu_busy_percent` and the card's own hwmon (`junction`, else `edge`, else `temp1`).
- hwmon sensors are found by their `name` file, never their number (`hwmon.rs`). The CPU temperature is `Tdie`, else `Tctl` (`k10temp` or `zenpower`), else the hottest `coretemp` package.
- `[status_bar]` in `config.toml` (`gpu`, `temp_unit`, `net_unit`, `thresholds`) is parsed in `config.rs` with `deny_unknown_fields`. Its rules are in `StatusBarConfig::check`, and the default steps are in `Thresholds::defaults`.
- The daemon decides each value's color in display units (`alerts.rs`): whole °F from whole °C, and rates in MB/s, MiB/s or Mb/s. Hysteresis is 2 points for percent and °C, 4 for °F, and a tenth of the step for rates. A change to the units or steps, or the end of a sampling period, resets every level.
- The board converts for display (`hoststat.c`). `stats` carries °C, bytes per second and both unit names in every frame.
- A sysfs read waits at most 500 ms per tick (`SYSFS_WAIT`). A tick that misses it publishes no CPU temperature and no AMD GPU reading, and the next tick waits on the same read instead of starting another.
- Pages: a config has up to 4 `[[page]]` tables (`MAX_PAGES`). With 2 or more, each page goes out as its own `layout`, all under one `rev`, to a session whose `hello.caps` names `pages`. A session without it gets the home page only, with its icons and tiles, and the daemon logs `firmware without pages, showing page 1 of N` once. With 0 or 1 page every session gets today's single `layout`.
- `mise run app:dev` rebuilds and restarts the daemon on any change under `app/src-tauri/`, test fixtures included, and the board then reconnects. Don't edit there during a check that needs an unbroken session.

## Conventions

- `firmware/sdkconfig` is generated and gitignored. Persistent config goes in `firmware/sdkconfig.defaults`. Kconfig silently drops unknown names, so check the generated `sdkconfig` after adding one.
- C follows ESP-IDF style (4 spaces, `.clang-format`). TS/JSON uses Biome, from `app/biome.json`. Run biome from `app/`, because the repo root errors with "nested root configuration".
- `firmware/main/fonts/`, `firmware/main/font_list.h`, `app/src/fonts.gen.ts` and `app/src-tauri/src/daemon/fonts_gen.rs` are generated by `mise run fonts:gen` from `fonts/`. Never edit them by hand; `mise run fonts:check` fails on any drift. Chrome fonts (`chrome` in `fonts/fonts.json`) generate only into `firmware/main/fonts/`.
- `firmware/main/logo_img.c` and `.h`, `app/src/assets/logo.svg`, `app/public/favicon.svg` and `app/src-tauri/icons/` are generated from `logo/` by `mise run logo:gen`. Never edit them by hand; `mise run logo:check` fails on drift in the text outputs.
- Enabling a new `CONFIG_LV_FONT_MONTSERRAT_*` in `sdkconfig.defaults` doesn't reach an existing generated `firmware/sdkconfig` that has `# ... is not set` for it. Move `firmware/sdkconfig` aside once, rebuild, and check it.
- `prek run --all-files` skips untracked files. Run `prek run --files <new files>` for those, or typos and the other hooks never see them.
- No log line may hold a token, a Wi-Fi password or a `secrets.toml` value. The firmware's log ring (`logcap.c`, `logring.c`) redacts the token and the Wi-Fi password (8 characters or more) as a backstop, but it can't know any other secret. `logcap_init()` is the first line of `app_main`, and nothing else may call `esp_log_set_vprintf`. About 60 s after boot the board logs the free stack bytes of its main tasks (`stack bytes free:`).
- Never commit secrets. Wi-Fi credentials live in the gitignored `firmware/sdkconfig.defaults.local` or in `wifi.txt` on the SD card. Never print device tokens, `identity.key` or `secrets.toml`; both sides redact tokens in their logs.

## CI

- CI runs on Forgejo Actions. The workflow lives in `.forgejo/workflows/ci.yml`. Check runs with `fj` or the Forgejo MCP tools, and use the `forgejo-actions` skill (not `gh-actions`) to edit or debug it. Every job has `runs-on: ubuntu-latest`, which has to match a label registered on a runner at git.voidrot.dev.
- Actions are referenced by full `https://github.com/...` URL, because short names only resolve against the instance's default actions host.
- The `espressif/idf` image has no `node`, so the firmware job can't run JavaScript actions and checks out with plain `git`.
- `MISE_GITHUB_TOKEN` comes from a server-wide Forgejo variable (`vars.`, not `secrets.`). It's a zero-permission GitHub token that only lifts mise's API rate limit, so it's a variable on purpose.
- `ci.yml` runs four jobs: lint, firmware (in the `espressif/idf:v6.1` container), app (clippy plus a debug Tauri build), and site.
  - The lint job skips the clippy and gitleaks hooks. The app job runs clippy, and a separate step runs a full-history gitleaks scan.
- The docs site will move to a separate repo, so nothing here deploys it. The `site` job only builds it.
- Every action is pinned to a commit SHA with a version comment, and zizmor enforces this.
- When bumping the ESP-IDF version, change `ESP_IDF_VERSION` in `mise.toml` and the container tag in `ci.yml` together.
