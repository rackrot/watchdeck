# Wi-Fi setup

The board joins a 2.4 GHz network through its ESP32-C6 co-processor. It reads credentials once at boot from one of two places:

| Source | Where | Change it by |
|---|---|---|
| SD card | `wifi.txt` at the root of the TF card | Editing the file, then pressing reset |
| Compiled in | `firmware/sdkconfig.defaults.local` | Rebuilding and reflashing |

A valid `wifi.txt` takes priority. If the card has no `wifi.txt`, or the file is invalid, the board uses the compiled-in credentials. If neither source has an SSID, the board doesn't connect.

## Option 1: `wifi.txt` on the SD card

Create a plain text file named `wifi.txt` at the top level of the card. Line 1 is the SSID and line 2 is the password:

```text
MyNetwork
my-password
```

- Don't use quotes or escaping. Every character on the line is used as-is.
- Windows (CRLF) and Unix (LF) line endings both work.
- The SSID can be up to 32 characters and the password up to 64. A longer line makes the whole file invalid.
- For an open network, leave out the password line.
- A blank first line means "don't connect". The board won't fall back to the compiled-in credentials.

You can edit the file without removing the card. Plug a USB-C cable into the board's **USB 2.0** port, which is not the serial port. The card then shows up as a USB drive with a `tf-data` partition. Edit `wifi.txt`, eject the drive, and press reset.

Always eject before unplugging. The board acknowledges a USB write before it reaches the card, so unplugging early can lose data.

## Option 2: compiled-in credentials

Create `firmware/sdkconfig.defaults.local`. Git ignores this file.

```text
CONFIG_WATCHDECK_WIFI_SSID="MyNetwork"
CONFIG_WATCHDECK_WIFI_PASSWORD="my-password"
```

- Keep the double quotes.
- Put a backslash before any `"` or `\`. For example, `pa"ss\word` becomes `"pa\"ss\\word"`.
- For an open network, set the password to `""`.

Then delete the generated config and flash:

```sh
rm firmware/sdkconfig
mise run fw:flash -- -p /dev/ttyUSB0
```

The `rm` step is required. The defaults files only fill in settings that are missing from `firmware/sdkconfig`, so an old config keeps the old values.

## Checking the connection

The status screen's Wi-Fi line shows the progress. The device shows the status screen from boot until its first layout arrives, and whenever it is unpaired. Once the grid is up, the same lines are in settings: swipe up on the grid to open it.

| Screen text | Meaning |
|---|---|
| `connecting to MyNetwork (source: sd)` | Credentials came from `wifi.txt` |
| `connecting to MyNetwork (source: config)` | Credentials came from the firmware |
| `connected 192.168.0.56 (watchdeck.local)` | Joined the network |
| `no Wi-Fi credentials` | Neither source has an SSID |
| `invalid Wi-Fi credentials` | The compiled-in SSID or password is too long |

Once the board is connected, it answers as `watchdeck.local` on the local network:

```sh
ping watchdeck.local
```

If the board can't find the network, it keeps retrying every few seconds. The serial log (`mise run fw:monitor -- -p /dev/ttyUSB0`) shows the reason.

## Security

- `wifi.txt` holds the password in plain text. Anyone with the card, or with the USB drive port, can read it.
- Compiled-in credentials stay out of git, but anyone who dumps the board's flash can read them.
