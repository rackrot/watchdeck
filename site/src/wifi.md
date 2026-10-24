# Wi-Fi setup

The board joins a 2.4 GHz network through its ESP32-C6 co-processor. It reads credentials once at boot from one of two places:

| Source | Where | Change it by |
|---|---|---|
| SD card | `wifi.txt` at the root of the TF card | Editing the file while the card is shared over USB, then stopping the share |
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

You can edit the file without removing the card:

1. Plug a USB-C cable into the board's **USB 2.0** port, which is not the serial port. Until you share the card, the computer sees an empty card reader, because the board keeps the card for itself.
2. On the board, open Settings > Storage and tap Share over USB, then tap it again to confirm. The card shows up as a USB drive with a `tf-data` partition. While it's shared, the board can't save anything to the card.
3. Edit `wifi.txt`, then eject the drive on the computer.
4. Tap Stop sharing, then I ejected it, restart, or press reset. The board restarts (about 8 seconds, and it stays paired), takes the card back and reads the new file.

Always eject before unplugging or stopping. The board acknowledges a USB write before it reaches the card, so unplugging early can lose data.

## The card

The board writes to the card only to save `wifi.txt` and the files in its own `watchdeck` folder. It never formats the card. Each save is recoverable: if the power goes off in the middle, the board finishes or undoes the save the next time it starts, so `wifi.txt` holds either the old or the new settings.

- If the power went off while the board was saving `wifi.txt`, let it start once with the card in before you edit the file on a computer. Otherwise the save it finishes at that start replaces your edit.
- The board never makes a folder bigger. On about 1 card in 16, creating the `watchdeck` folder needs room the card's top level doesn't have yet, and the board reports the card folder as full. Create a `watchdeck` folder at the top of the card on a computer, or add or delete one file there.
- FAT32 has no journal, so a power cut at the wrong moment can, rarely, damage the file system. If the board reports a card error, check the card on a computer (`fsck.vfat` on Linux, `chkdsk` on Windows). Keep nothing on the card you can't replace.
- A card that isn't FAT32 works read-only: the board reads `wifi.txt` but can't save to it.

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
