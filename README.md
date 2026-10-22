# watchdeck

A Stream Deck-style controller for your homelab: a touch screen that runs shell commands and HTTP requests from a grid of buttons, driven by a desktop app.

watchdeck is two parts. An Elecrow CrowPanel Advanced 10.1" display (ESP32-P4) shows the buttons and sends presses over the network. A Tauri desktop app on your computer holds the button layout, runs the actions, and pairs with the display.

[![CI](https://github.com/rackrot/watchdeck/actions/workflows/ci.yml/badge.svg)](https://github.com/rackrot/watchdeck/actions/workflows/ci.yml)

## What it does

- Shows a grid of up to 8 columns by 5 rows of buttons on the display, with labels, colors and icons.
- Runs a shell command or an HTTP request when you press a button. HTTP requests can send header values from a secrets file, so tokens stay out of the config.
- Shows live values on buttons (tiles). A tile polls a shell command or HTTP endpoint on an interval and displays the result.
- Keeps the layout in a config file that the desktop app reloads as you save it. A bad edit is refused, and the last good config stays live.
- Talks to the display over TLS. Pairing is confirmed on the display, and you can forget a pairing from the display.
- Runs in the desktop app's tray, with an optional start at login.

The desktop app has three pages: Layout, Secrets and Devices.

## Hardware

- Elecrow CrowPanel Advanced 10.1" with an ESP32-P4 (1024 × 600 IPS touch display, 32 MB PSRAM, 16 MB flash). Wi-Fi comes from an ESP32-C6 co-processor, which supports 2.4 GHz networks only.
- A USB-C cable and a 5 V, 2 A power supply.
- A microSD (TF) card, needed only if you set Wi-Fi with a `wifi.txt` file.
- A computer on the same network as the display, running the desktop app.

The [device spec](https://rackrot.github.io/watchdeck/device-spec.html) lists the full hardware details.

## Getting started

watchdeck has no published releases yet, so you build both parts from source. mise installs the pinned toolchain and runs every task.

### 1. Get the source and tools

```sh
git clone https://github.com/rackrot/watchdeck.git
cd watchdeck
mise install
```

### 2. Flash the firmware

Install ESP-IDF once, then build and flash the display. Replace the port with the one your board uses. On Linux it is usually `/dev/ttyUSB0`.

```sh
mise run idf:install
mise run fw:flash -- -p /dev/ttyUSB0
```

Your user needs permission to open the serial port. On many Linux systems that means being in the group that owns the device.

### 3. Give the display Wi-Fi

The display needs a 2.4 GHz network. Choose one of two ways:

- Put a `wifi.txt` file on the microSD card. Line 1 is the SSID and line 2 is the password. For an open network, leave out line 2. Insert the card and reboot the display.
- Or compile the credentials into the firmware with `firmware/sdkconfig.defaults.local`. Delete `firmware/sdkconfig` before you reflash, because an existing `sdkconfig` keeps its old values.

Both methods, the USB drive mode for the card, and the messages the screen shows are in the [Wi-Fi setup guide](https://rackrot.github.io/watchdeck/wifi.html).

Credentials in `wifi.txt` are stored as plain text on the card. Credentials compiled into the firmware can be read from the flash chip. Treat both as sensitive.

### 4. Install and run the desktop app

```sh
cd app
pnpm install
cd ..
mise run app:dev
```

`mise run app:dev` starts the app in development mode. To build an installable app, run `mise run app:build`. Building needs the Tauri 2 system prerequisites for your operating system, as listed in the Tauri documentation.

The app runs a tray icon and a background daemon. The daemon serves the display's control connection over TLS on port 47820. It opens the pairing connection on port 47821 only while the pairing dialog is open.

### 5. Pair the display

1. In the app, open the **Devices** tab and click **Pair a device**. Pairing stays open for 2 minutes.
2. On the display, under "Tap your desktop to pair:", tap your computer.
3. Type the 6-digit code shown on the display into the app, and click **Pair**.
4. The display then asks whether the app says "Paired <device>". Check that it does, and tap **Yes**. Nothing is saved on the display until you tap Yes.

To remove a pairing, hold **Forget pairing** on the display for 2 seconds. The display then erases its saved pairing.

If the display says "No desktop is ready to pair", the app's pairing dialog isn't open. Click **Pair a device** again. If your computer never shows up in the list, check that the display has joined your Wi-Fi network: `ping watchdeck.local` from a computer on the same network should get a reply.

## Configure buttons

The button grid lives in `config.toml` in the app's config directory, `$XDG_CONFIG_HOME/watchdeck/` (`~/.config/watchdeck/` by default). The app moves an existing `~/.config/dev.rackrot.watchdeck/` there on first start. The app writes a default file on first run, with one button that shows a desktop notification.

This example defines a button that restarts a container over SSH, and a tile that shows disk usage:

```toml
version = 1

[layout]
cols = 5
rows = 3

[[button]]
id = "plex"
pos = [0, 0]
label = "Restart Plex"
bg = "#e65100"
action = { kind = "shell", argv = ["ssh", "nas", "docker", "restart", "plex"], timeout_s = 60 }

[[button]]
id = "disk"
pos = [1, 0]
label = "Disk"
action = { kind = "shell", argv = ["sh", "-c", "df -h / | tail -n 1"] }
tile = { every_s = 30, source = { kind = "shell", argv = ["sh", "-c", "df --output=pcent / | tail -n 1"] } }
```

Save the file and the change reaches the display within about a second. The [buttons and actions reference](https://rackrot.github.io/watchdeck/config.html) covers every key, HTTP actions, tiles, icons and the secrets file.

## Documentation

- [Wi-Fi setup](https://rackrot.github.io/watchdeck/wifi.html)
- [Buttons and actions](https://rackrot.github.io/watchdeck/config.html)
- [Device protocol](https://rackrot.github.io/watchdeck/protocol.html), the wire format between the display and the app
- [Device spec](https://rackrot.github.io/watchdeck/device-spec.html), the hardware reference

The documentation source is in [`site/`](site/). It is built with mdBook.

## Building from source and contributing

The repository has three parts:

| Path | What |
| :- | :- |
| `firmware/` | ESP-IDF v6.1 project for the ESP32-P4 display |
| `app/` | Tauri 2 desktop app: React and TypeScript front end, Rust back end in `app/src-tauri/` |
| `site/` | mdBook documentation site |

Every tool version is pinned in [`mise.toml`](mise.toml). Run `mise tasks` to list all tasks. The ones you are likely to use:

| Task | What it does |
| :- | :- |
| `mise run idf:install` | Installs ESP-IDF into `~/.espressif` |
| `mise run fw:build` | Builds the firmware |
| `mise run fw:flash -- -p <port>` | Flashes the firmware |
| `mise run fw:monitor -- -p <port>` | Opens the serial monitor |
| `mise run fw:test` | Runs the host unit tests for the device message parsers |
| `mise run app:dev` | Runs the desktop app in development mode |
| `mise run app:build` | Builds the desktop app |
| `mise run site:serve` | Serves the documentation locally |
| `mise run lint` | Runs every pre-commit hook on all files |
| `mise run fmt` | Formats Rust, TypeScript, C, TOML and Markdown |

Run `mise run lint` before you open a pull request. CI runs the same checks, plus the firmware and app builds.

## Status

Early development. The app is at version 0.1.0, and there are no releases yet.

## License

[MIT](LICENSE)
