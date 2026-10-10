# watchdeck

A Stream Deck-style homelab controller: an Elecrow CrowPanel Advanced 10.1" ESP32-P4 display plus a desktop app that talks to it.

## Layout

| Path | What |
|---|---|
| `firmware/` | ESP-IDF v6.1 C project for the ESP32-P4 |
| `app/` | Tauri 2 desktop app: React + TypeScript + Vite (pnpm), Rust in `app/src-tauri/` |
| `site/` | mdBook site deployed to GitHub Pages (`https://rackrot.github.io/watchdeck/`) |
| `site/src/device-spec.md` | Hardware reference, transcribed from the vendor page |

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
- The second USB-C port (J16, native high-speed USB, device only, separate from the CH340 console port) exposes the raw TF card to a host as a mass-storage drive. The firmware reads `wifi.txt`, then releases its own mount and hands the card to USB. Host writes apply on the next reboot.
- USB MSC writes are acked to the host before they reach the card, and card write errors never reach the host (upstream esp_tinyusb behavior, unchanged as of 2026-09), so eject before unplugging.
- Pin map, from Elecrow's V1.2 schematic and examples:
  - Display: EK79007 over 2-lane MIPI-DSI, backlight on GPIO31 (active high).
  - Touch: GT911 on I2C0, SDA 45 / SCL 46 at 400 kHz, RST 40, INT 42 (INT has no pull-up, so it runs in polling mode). Verified at address 0x5D, product ID "911". The axes need no swap or mirror (knobs at the top of `main/touch.c`).
  - TF card: SDMMC slot 0, 1-bit only: CLK 43, CMD 44, D0 39.
  - ESP32-C6 Wi-Fi: SDIO slot 1, 4-bit: CLK 18, CMD 19, D0–D3 17/16/15/14, reset GPIO32. Elecrow's lesson configs say 54, which is wrong.
- Boot order matters:
  1. Acquire LDO3 (2500 mV, DSI PHY) and LDO4 (3300 mV, IO bank).
  2. Mount the SD card.
  3. Start the display.
  4. Start Wi-Fi.
  The SD card and ESP-Hosted share one SDMMC host, and ESP-Hosted probes the C6 before `app_main`.
- The TF card is GPT-partitioned, so `main/sdcard.c` mounts the first GPT partition read-only through its own diskio driver. Never format the card or write to it.
- The C6 runs ESP-Hosted slave 2.12.3 and the host is 3.0.9. Hosted logs a "major version mismatch" line, which is only advisory. Don't OTA the C6 without asking.
- `monitor` needs a real TTY. Without one, read the boot log with pyserial from the IDF Python env (`~/.espressif/python_env/idf6.1_py3.12_env/bin/python`). Opening the port resets the board, so every capture starts from boot.

## Conventions

- `firmware/sdkconfig` is generated and gitignored. Persistent config goes in `firmware/sdkconfig.defaults`. Kconfig silently drops unknown names, so check the generated `sdkconfig` after adding one.
- C follows ESP-IDF style (4 spaces, `.clang-format`). TS/JSON uses Biome, from `app/biome.json`. Run biome from `app/`, because the repo root errors with "nested root configuration".
- Never commit secrets. Wi-Fi credentials live in the gitignored `firmware/sdkconfig.defaults.local` or in `wifi.txt` on the SD card.

## CI

- `ci.yml` runs four jobs: lint, firmware (in the `espressif/idf:v6.1` container), app (clippy plus a debug Tauri build), and site.
  - The lint job skips the clippy and gitleaks hooks. The app job runs clippy, and a separate step runs a full-history gitleaks scan.
- `pages.yml` deploys `site/book` on pushes to `main` that touch `site/`.
- Every action is pinned to a commit SHA with a version comment, and zizmor enforces this.
- When bumping the ESP-IDF version, change `ESP_IDF_VERSION` in `mise.toml` and the container tag in `ci.yml` together.
