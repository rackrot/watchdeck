# Groundwork: card writes, the log ring, the wire contract, the config schema

Part of [`00-index.md`](00-index.md). Findings F1 to F44 are there.

Three features need these: settings (the `wifi.txt` editor, the Storage, Logs and Errors tabs), the sleep screen (an image on the card, a config frame, chunked transfer) and the feed (a frame and the `[[integration]]` schema). They land first so the feature slices can run in parallel against fixed contracts.

## Goal and done

- The firmware can write `wifi.txt` and `watchdeck/<name>` on the TF card, and nothing else. After a cut at any point, recovery leaves each small file old or new and each image valid or detected invalid: proven at every write of every operation in the FatFs host test, sampled by five power cuts on the board. A write while the card is shared over USB, or with no card, fails with a reason and changes nothing.
- The card has one owner at a time. The firmware owns it from boot; Share hands it to USB at run time; Stop sharing reboots to take it back. The firmware never formats and never lets esp_tinyusb mount the card.
- The device keeps its log since boot in a 64 KiB PSRAM ring, with each line's level, and nothing secret in it.
- `sleep`, `asset_get`, the asset frame, `feed` and `feed_dismiss` are specified, parsed and built on both sides, with fixtures and host cases.
- `config.toml` takes `[sleep]`, `[push]` and `[[integration]]` (kinds `push` and `stat`; N5 adds `prometheus` and `grafana`), with `deny_unknown_fields`, rule messages, and a round trip through the Layout tab's save.

## Approach

### Card store (G1)

- **`cardfs.c/.h`**, plain POSIX (`fopen`, `fwrite`, `fsync`, `unlink`, `mkdir`, `stat`) under a root path: `/sdcard` on the board. **No `rename` anywhere.** FatFs's `f_rename` registers the new entry, then removes the old one, then syncs (`ff.c` around 5378 to 5404, F41), so a cut between the two leaves two names on one cluster chain, and deleting either then frees the other's data. And FatFs refuses a rename onto an existing name (F6). A scheme that never renames avoids both.
  - **Allowlist**: `wifi.txt`, and `watchdeck/<name>` where `<name>` is 1 to 63 of `[a-z0-9._-]`, no leading `.`, no `..`. Lowercase only, since FAT is case-insensitive and `WIFI.TXT` is the same file: the check refuses other casings rather than folding them. Every write, remove and listing goes through it.
  - **Small files (`wifi.txt`, `watchdeck/sleep.json`): an intent record, then an in-place write** (D2). `cardfs_write_small(rel, data, len)`, at most 4 KiB:
    1. Write `watchdeck/<key>.new` (`wifi.new`, `sleep.new`): a 48-byte header (`"WDI1"`, a u32 length, a u32 generation, the 32-byte SHA-256 of the payload, a u32 CRC32 of the header's first 44 bytes), then the payload; `fflush`, `fsync`, `fclose`.
    2. Write `rel` in place (`fopen("w")`, the bytes, `fflush`, `fsync`, `fclose`).
    3. Read `rel` back and compare; then `unlink` the intent record.
  - **`cardfs_recover()`**, run after every mount and before anything reads the card: for each intent record, if its header or its SHA-256 doesn't check out, it was cut during step 1, so `rel` is untouched: delete the record. If it checks out, make `rel` equal its payload (rewrite it if it differs or is missing, as step 2), then delete the record. Recovery is idempotent, so a cut during recovery is recovered the same way at the next mount. A delete only ever frees the record's own chain, since no two names ever share one.
  - **What a cut can leave**: the old `rel`, or the new one, never a torn one once recovery has run. FAT32 has no journal, so a cut can also leave lost clusters (a cut inside a cluster allocation); `fsck.vfat` reclaims them, and they don't touch other files' chains.
  - **Big files are write-once and self-validating** (D3): a sleep image goes straight to its content-addressed final name, `watchdeck/sleep-<id>.rgb565`, and a reader trusts it only after checking its size and the SHA-256 against the id. A file that fails is deleted and fetched again (P3). Nothing ever overwrites a valid image, and GC deletes an image only after the `sleep.json` that stops naming it has been committed.
  - Writes go in 32 KiB pieces so ESP-Hosted's SDIO traffic interleaves on the shared host.
  - `cardfs_read`, `cardfs_remove`, `cardfs_list("watchdeck/")`.
  - **FatFs settings** (`sdkconfig.defaults`): `CONFIG_FATFS_DONT_TRUST_FREE_CLUSTER_CNT=1` and `CONFIG_FATFS_DONT_TRUST_LAST_ALLOC=1`, so a stale FSINFO from a host or a cut never steers allocation (FatFs returns "full" at once on a stored free count of 0, `ff.c:1562`, and writes FSINFO without checking the result, `:1131`). The cost is one FAT scan before the first allocation after a mount, measured in G2 (a 32 GB card's FAT is about 4 MB at the 1-bit bus's speed).
- **FatFs-level host test** (`firmware/test/host_fatfs/`, new): an ESP-IDF `linux`-target app like `host_proto`, linking IDF's `fatfs` component (which builds for `linux`, as its own `components/fatfs/host_test` shows, F42) over a RAM disk registered with `ff_diskio_register`, formatted FAT32 by the test itself. The RAM disk counts sector writes and can "cut the power" after write N: later writes are dropped, and optionally sector N is torn (its first 256 bytes new, the rest old). For each operation (a first `wifi.txt`, a replace, a `sleep.json` replace, a 1 MiB image), the test runs the operation once to count its writes W, then for every N from 1 to W: restore the image from before, cut at N, remount, run `cardfs_recover`, and check that `rel` is old or new, the image is absent, valid, or detected invalid, and every other file on the disk (three planted files, hashed before) is unchanged. It also runs `f_getfree` and a FAT walk to find cross-linked chains (a cluster in two chains fails the test). This is the test the power-cut runs on the board only sample.
- **`wifitxt.c/.h`**, pure (a POSIX-only host test is enough for it): the reader moves out of `wifi.c` (`read_line`, `read_card_credentials`, F8) behind `wifitxt_parse(FILE *, ...)`, and gains `wifitxt_format(ssid, pass, buf)`: `SSID\n` for an open network, else `SSID\npassword\n`. It refuses an SSID of 0 or over 32 bytes or with CR, LF or NUL, and a password that isn't empty, 8 to 63 printable ASCII, or 64 hex digits. One host test feeds the formatter's output to the parser.
- **`svc.c/.h`**, the worker task (D4): one FreeRTOS task, 6 KiB stack in internal RAM, priority 3 (under the link task's 5), with an 8-slot queue of `{fn, arg}`. `svc_post` never blocks; a full queue returns an error the caller shows. Card I/O, the Wi-Fi scan, the steps of a trial connect, the RSSI read (moved off the link task, T3), the share and image loads run here, never on the link or LVGL task. No job waits on a network event: a trial connect is a state machine in `wifi.c` driven by Wi-Fi events and a timer (T3), and svc only runs its short steps, so a 20 s trial never holds the queue. Each job logs its run time when it exceeds 1 s.
  - Lock order: a job takes no lock while it does I/O, then takes the LVGL lock (and, if needed, the model lock after it) only to hand the result to the UI. It never holds the cardfs mutex while waiting on the LVGL lock.

### Card ownership and USB handover (G2)

```text
          boot: mount RW, recover
 NO_CARD <----------------------- (card init fails)
             |
             v
          DEVICE  --- Share (svc) --->  SHARED  --- Stop sharing --->  reboot
   (cardfs writable)                 (FS unmounted, storage mapped     (esp_restart; the pairing,
                                      to USB, cardfs refuses writes)    NVS and the coredump stay)
```

- **Boot.** `sdcard_mount` mounts read-write (`part_disk_init` returns 0 once writable, `part_disk_write` gets the same bounds check as the read, then `sdmmc_write_sectors`), then runs `cardfs_recover`. `wifi_start` reads `wifi.txt` and no longer releases the FS. `usb_msc_start` becomes `usb_start`: it installs the TinyUSB driver and the MSC driver with `auto_mount_off` set, and maps no storage, so a host sees an empty card reader (F4). The card stays the firmware's (D1, Q1).
- **`CTRL_SYNC`** stays a no-op: `sdmmc_write_sectors` waits for the card to finish programming, so a returned write is on the card. `CONFIG_FATFS_IMMEDIATE_FSYNC` stays off; `cardfs` syncs explicitly.
- **Share** (a svc job): wait for any cardfs write to finish (its mutex), set cardfs read-only, `f_mount(NULL)`, unregister the VFS path and the diskio (today's `sdcard_release_fs`), then `sdmmc_get_blockdev` and `tinyusb_msc_new_storage_blockdev` with `TINYUSB_MSC_STORAGE_MOUNT_USB` and `.fat_fs.do_not_format = true` (F3: the format path checks it, `tinyusb_msc.c:524`). This is today's boot-time path, moved to a tap, so nothing new runs on the USB side.
- **Stop sharing is a reboot** (D1). The Storage tab says "Eject the drive on your computer first. The board restarts to take the card back (about 8 s); the pairing stays", then "I ejected it" calls `esp_restart()`. Taking the card back at run time is unsafe with this esp_tinyusb: its read and write callbacks look the storage up inside a critical section but take its lock and call the medium after leaving it (`tinyusb_msc.c:246-264`), and `tinyusb_msc_delete_storage` checks only for deferred writes (F4), so a delete can free the storage under a SCSI command in flight. A reboot ends the USB session (the host sees the drive go), and the next boot mounts and recovers as usual. Nothing is lost: the daemon sends the sleep config again on connect, and the board fetches what it lacks. This is also today's behavior, where a host's writes apply on the next reboot.
- **Never**: `tinyusb_msc_set_storage_mount_point`, `tinyusb_msc_format_storage`, `tinyusb_msc_delete_storage` (see above), clearing `auto_mount_off` (each of the first two, and an eject with `auto_mount_off` clear, `tinyusb_msc.c:1364-1374`, leads to the app mount and its format path, F3), `f_mkfs`, `esp_vfs_fat_sdcard_format`, `esp_vfs_fat_sdmmc_mount`. A prek hook greps `firmware/main` (not the host tests, which format their RAM disk) for these names and fails.
- **Status text**: the USB line now says `USB: card on the device`, `USB: sharing the card (connected to a computer)` or `USB: sharing the card (no computer)`, from the share state and TinyUSB's attach events.
- **Power loss: the failure model.** FAT32 has no journal. What a cut can do, and what covers it:
  - Our file mid-write: recovery (G1) leaves it old or new; an image fails its hash and is fetched again.
  - Lost clusters (allocated, in no chain) or a stale free count: harmless to other files; `fsck.vfat` reclaims them, and the free count isn't trusted (G1's FatFs settings).
  - A torn FAT or directory sector, if the card itself tears a sector write on power loss: this is the one case that can touch another file's entry or chain, because a FAT sector holds 128 clusters' links and a directory sector 16 entries. Nothing in software prevents it on FAT. What bounds it: writes are rare (a user action or a config change, a few a week), our directory is `watchdeck/` (its own sectors, apart from the root's), `wifi.txt` is the only root entry we touch, the host test checks every cut point with a torn sector, and the backup gate. The docs say to run `fsck.vfat` (or `chkdsk`) on the card if the Storage tab ever reports a read error, and to keep nothing irreplaceable on the card.
- **Backup gate.** Before the first flash of any build that can write to the card (G2 and later), the user makes a full image of the card and says so. Suggested: the card in a reader, `sudo dd if=/dev/sdX of=~/watchdeck-card-2026-10-xx.img bs=4M status=progress conv=fsync`, then `sha256sum` it. The G2 results record the date, size and hash. No flash of a writing build happens before that line exists.

### Log ring (G3)

- **`logring.c/.h`**, pure: a byte ring of length-prefixed lines with a level (`E`, `W`, `I`, from the first character of esp_log's line) and a sequence number, so the Logs tab can page stably while new lines arrive. Lines are cut at 159 bytes. ANSI color codes are stripped, and anything outside printable ASCII becomes `?`, since the device's fonts have nothing else.
- **`logcap.c`**, on the board: `logcap_init()` is the first line of `app_main`. It allocates and initializes the 64 KiB PSRAM ring (about 600 lines) and its spinlock, and only then calls `esp_log_set_vprintf` (F13), so the hook never sees an unready ring. The hook takes a `va_copy` of its arguments **before** calling the previous `vprintf` (which consumes the original), so the UART stays exactly as it is; then it formats the copy into a 224-byte stack buffer with `vsnprintf`, redacts (below), cuts to 159 bytes, and appends under the spinlock (a memcpy, no formatting inside it). The hook itself never logs, allocates or blocks: a failure (a full ring is not one, it overwrites the oldest) bumps a dropped-lines counter the Logs tab shows.
- **What it can't capture**, said in the Logs tab's footer and the docs: the bootloader's lines and anything before `app_main` (ESP-Hosted's early probe), and `ESP_EARLY_LOG`/`ESP_DRAM_LOG` lines (ISR and early paths), which bypass `vprintf`. The UART still has all of them.
- **Redaction, before the cut**: the ring inherits the rule that secrets never reach a log (F12), and CLAUDE.md gains: "no log line may hold a token, a Wi-Fi password or a `secrets.toml` value". As a backstop, `logring_redact` runs on the 224-byte formatted text, before the 159-byte cut: it replaces every whole occurrence of the paired token's 64 hex digits and of the current Wi-Fi password (when at least 4 characters) with `<redacted>`; and if `vsnprintf` truncated the text, it also replaces a tail that is a prefix (4 characters or more) of either secret, since the rest of that secret was cut off. Only then is the line cut. So a secret is either redacted whole, redacted as a cut-off prefix, or entirely outside what is stored. `wifi.c` hands the password to `logcap` when it sets one, and `link.c` the token after the pairing loads; both are copied into a static buffer under the spinlock.
- **Errors from elsewhere**: `diag.c/.h`, pure, holds counters and the last reason for Wi-Fi disconnects, link drops, broken sets, icon timeouts and card errors, plus `wifi_reason_name(int)` (201 "network not found", 15 "wrong password?", 202 "authentication failed", 2 "authentication expired", 205 "connection failed" and so on). `wifi.c`, `link.c` and `sdcard.c` call `diag_count`. T2 shows them.

### Wire contract (G4)

Two capabilities, named in `hello.caps` beside the three today. With a 32-character `fw`, `{"fonts","pages","status_bar","sleep","feed"}` makes a 234-byte hello in the 256-byte buffer (F11; the host test pins the new number).

- **`sleep`**: the device shows a sleep screen and reads `sleep` and asset frames.

  ```json
  {"t":"sleep","image":"5b1f0c7e9d3a2b4c6d8e0f1a2b3c4d5e","bg":0,"fg":16777215,"show":["clock","date","host"],"text":"Back soon","anchor":"bottom_left","spinner":"arc","spinner_color":3718648,"offline_after_s":10,"backlight":100}
  ```

  | Field | Type | Meaning |
  |---|---|---|
  | `image` | string | Optional. The image's id, 32 lowercase hex digits: the first 16 bytes of SHA-256 over its pixels. Absent: the built-in logo |
  | `bg`, `fg` | int | `0xRRGGBB`. The screen's background and the text color |
  | `show` | [string] | Any of `clock`, `date`, `host`, `text`, each at most once. Unknown names are ignored |
  | `text` | string | Optional, printable ASCII, at most 47 bytes. Shown when `show` names `text` |
  | `anchor` | string | `center`, `top_left`, `top_right`, `bottom_left` or `bottom_right`: where the elements sit in the image area |
  | `spinner` | string | `arc`, `dots`, `bar`, `pulse` or `none` |
  | `spinner_color` | int | `0xRRGGBB` |
  | `offline_after_s` | int | 0 to 3600. 0: no sleep for a lost link |
  | `backlight` | int | 0 to 100, the backlight while asleep. 0 is off. A device that can't dim treats 1 to 100 as on |

  The image is always 1024 x 512 RGB565, little-endian, 1,048,576 bytes. A malformed `sleep` is logged and dropped; the device keeps its current one.
- **`asset_get`** (device → daemon), one outstanding: `{"t":"asset_get","id":"5b1f...","offset":262144}`. `offset` is a multiple of 262,144 below the asset's size. The daemon answers with one asset frame, or nothing if the id isn't the session's current sleep image.
- **Asset frame** (daemon → device, binary): `"WDA1"`, the 16-byte id, the offset as u32 LE, then up to 262,144 bytes. At most 24 + 262,144 = 262,168 bytes, exactly today's binary cap (F24), so the receive path and its limits don't change. The device checks the magic, that it asked for this id and offset, and the length (262,144, or what is left), and checks the SHA-256 over the whole image once the last chunk is in.
- **`feed`** (daemon → device): a snapshot that replaces the last one.

  ```json
  {"t":"feed","sources":2,"cards":[{"id":"nas-load","label":"NAS load","value":"0.42","state":"ok"}],"notes":[{"id":"ha/backup","title":"Backup done","body":"Nightly backup finished in 4 min","level":"ok","src":"Home Assistant","at":1792692433}]}
  ```

  | Field | Limits |
  |---|---|
  | `sources` | 0 to 16: configured integrations. 0 means the device shows no -1 page |
  | `cards[]` | At most 8. `id` 1 to 63 bytes; `label` at most 23; `value` at most 15; `state` `ok` or `bad`, anything else shown as unknown |
  | `notes[]` | At most 12, newest first. `id` 1 to 63; `title` 1 to 47; `body` at most 159; `level` `info`, `ok`, `warn` or `error`; `src` at most 23; `at` Unix seconds |

  Every string is printable ASCII. The fullest snapshot, with every string at its limit and every character escaped, stays under 16 KB (a daemon test prints the size).
- **`feed_dismiss`** (device → daemon): `{"t":"feed_dismiss","id":"ha/backup"}`. An unknown id is ignored.
- The daemon sends `sleep` after the first `layout` set and its tiles, and again when the session's view of `[sleep]` changes. It sends `feed` after that, and on each change, at most twice a second. Neither goes between the pages of a set.

### Config schema (G5)

```toml
[sleep]
image = "lake.jpg"          # a file in screens/ beside config.toml; absent shows the watchdeck logo
fit = "cover"               # "cover" fills and crops, "contain" shows it whole on bg
bg = "#000000"
fg = "#ffffff"
show = ["clock", "date", "host"]
text = "Back soon"          # needed when show has "text"
anchor = "bottom_left"
spinner = "arc"
spinner_color = "#38bdf8"
offline_after_s = 10
backlight = 100

[push]
lan = false                 # true also serves https on 47823

[[integration]]
id = "ha"
kind = "push"
label = "Home Assistant"    # what a note shows as its source
token_secret = "push-ha"    # a name in secrets.toml

[[integration]]
id = "nas-load"
kind = "stat"
label = "NAS load"
every_s = 30
json_pointer = "/load"
source = { kind = "http", method = "GET", url = "http://nas.lan/api/load" }
```

- `Config` gains `sleep: Option<SleepConfig>`, `push: Option<PushConfig>` and `#[serde(rename = "integration")] integrations: Vec<IntegrationConfig>`, each skipped when empty, so a file without them saves as today.
- `IntegrationConfig` is tagged by `kind` with `deny_unknown_fields`, like `Action` (F28). G5 has `push` and `stat`; N5 adds `prometheus` and `grafana`. String enums (`fit`, `anchor`, `spinner`, the `show` names) stay strings checked in `check()`, so a typo gets the rule's message and not a decode error (status-bar-2 R18).
- **Rules** (word for word in the tests):
  - `sleep.image "x": no such file in screens/` and the icon file-name rule; `sleep.fit "x" is not cover or contain`; `sleep.anchor "x" is not one of center, top_left, top_right, bottom_left, bottom_right`; `sleep.spinner "x" is not one of arc, dots, bar, pulse, none`; `sleep.show: "x" is not one of clock, date, host, text`; `sleep.show: "date" is listed twice`; `sleep.text is needed when show has "text"`; `sleep.text: at most 47 printable ASCII characters`; `sleep.offline_after_s 4000 is outside 0 to 3600`; `sleep.backlight 120 is outside 0 to 100`; colors as `#rrggbb`.
  - `integration "x": id ...` (the button id rule); `integration ids must be unique: "x"`; `17 integrations, at most 16`; `9 card sources (stat and prometheus), at most 8`; `integration "x": label: at most 23 printable ASCII characters`; `integration "x": token_secret "y" is not in secrets.toml`; `integration "x": token "y" is shorter than 32 characters`; a `stat` takes the tile rules for `every_s` and `json_pointer` and the `Action` rules for its `source`.
- `secret_users` counts `token_secret` and a stat source's `secret_headers`, so Secrets can't delete a token in use.
- A `[sleep]`-, `[push]`- or `[[integration]]`-only edit leaves `Live::layout()` equal, so it pushes no layout (as `[status_bar]` does, status-bar-2 F6).
- `app/src/config.ts` gets the types; `errors.ts` gets `sleep` and `integrations` buckets (from `^sleep\.` and `^integration "`); `edit.ts` gets `sleepChanged` and `feedChanged`, so the save bar says "the sleep screen applies within 2 s" instead of "layout unchanged".
- `site/src/config.md`: Sleep screen, Integrations, and Push API sections (the last two point to a new `site/src/integrations.md`, written in N2).

## Rejected alternatives

| Option | Why not |
|---|---|
| Keep the card on USB at boot and take it back for each write | The sleep config and image arrive on every connect, so the device would take the card from a host that may be writing to it, or never save. A write needs a moment when no host can touch the card, and a user-started share gives that |
| Share on USB attach, take back on detach | A board powered from a computer's port would never own its card. A detach with no eject can leave a half-written FAT that the device then writes into |
| esp_tinyusb's own app mount (`set_storage_mount_point(APP)`) | It mounts the raw GPT disk, finds no FAT and formats it (F3) |
| Taking the card back from USB at run time (`tinyusb_msc_delete_storage`, then mount) | Races SCSI commands in flight: the callbacks take the storage's lock after leaving the lookup's critical section (`tinyusb_msc.c:246-264`), and delete checks only deferred writes (F4). Safe only with a patched esp_tinyusb, and the repo vendors no patched components today (CLAUDE.md chose not to for the websocket client too). `esp_restart` gives a software reset, which `crash.c` doesn't report (`crash.c:49-60` names only panics, watchdogs and brownouts). A reboot costs about 8 s and the icon cache, for a rare action |
| Taking it back only after a USB detach | Without VBUS sensing (no VBUS GPIO is known on this board) an unplug may show as a suspend, not a detach, and a callback can still be in flight at the moment of the unplug |
| Write in place with `fopen("w")` alone | A cut mid-write leaves a truncated `wifi.txt`, and the board boots with no network. The intent record makes the in-place write redoable |
| Write `X.tmp`, delete `X`, rename (the first draft) | `f_rename` registers the new name before removing the old (F41), so a cut leaves two names on one chain and the cleanup's delete frees the live file. And with no final file, a present temp file proves nothing about being complete |
| `rename` over the old file | FatFs refuses it (F6) |
| Two slots (`sleep.a`, `sleep.b`) with generations | Works for `sleep.json`, but `wifi.txt` must stay one plain file a person can edit, so it needs the intent record anyway; one mechanism for both |
| A general journal of our files | A file system in a file, for writes that happen a few times a week. An intent record per small file and write-once images cover it |
| Card writes on the link task | A 1 MiB write takes about a second and would stall frames behind the 500 ms drop (F10) |
| Logging through a FreeRTOS mutex in the hook | The hook runs on every task that logs, inside the logging call. A spinlock around a memcpy can't block or invert priorities; formatting stays outside it |
| A log of the whole boot on the card | Card writes on every log line, against the "rare writes" rule above |
| One `vsnprintf` for both the UART and the ring | Cuts UART lines at the ring's width. The UART is the debugging tool and stays whole |
| Generic redaction of every 64-hex run | Would hide the ELF SHA in crash lines, which the user needs. The token is redacted by literal match |
| A full-screen image in one binary frame | 1 MiB frames: a 1 MiB PSRAM receive buffer per frame and a 2 s block on the session. Four frames at today's cap reuse every limit |
| ARGB8888 or PNG on the wire | ARGB doubles transfer and card time for a background with no alpha. PNG needs a decoder on the board (lodepng is off, F22) and its RAM |
| One `display` capability for sleep and feed | Two features that land at different times. Separate names keep the `device_sim` tests strict, as for `status_bar` |
| Integrations in their own file (`integrations.toml`) | A second file with its own validation, last-good copy and watcher. `config.toml` already has all of that, and the Layout tab's save already round-trips unknown tables (status-bar-2 B2) |

## Slices

### G1 Card store, `wifi.txt` codec, worker task

- **Files**: `firmware/main/cardfs.c/.h`, `wifitxt.c/.h`, `svc.c/.h` (new); `wifi.c` (reader moved to `wifitxt.c`, behavior unchanged); `CMakeLists.txt`; `sdkconfig.defaults` (the two FatFs trust settings, checked in the generated `sdkconfig`); `firmware/test/host_proto/main/CMakeLists.txt`, `test_cardfs.c`, `test_wifitxt.c` (new), `test_proto.c` (`RUN_TEST` lines); `firmware/test/host_fatfs/` (new app: `CMakeLists.txt`, `sdkconfig.defaults`, `main/ramdisk.c` with the cut and tear injection, `main/test_cuts.c`); `mise.toml` (`fw:test` runs both host apps).
- **Host cases**:
  - allowlist: `wifi.txt`, `watchdeck/sleep.json` pass; `WIFI.TXT`, `watchdeck/../wifi.txt`, `/wifi.txt`, `watchdeck/.x`, `watchdeck/a/b`, a 64-character name, `other.txt` are refused;
  - atomic write: a new file; a replace; a write while not writable fails and leaves the old bytes; a write whose temp file can't be created leaves the old file;
  - recovery (POSIX, logic only): no record (untouched); a record with a bad magic, a bad header CRC, a short payload or a bad SHA-256 (deleted, `rel` untouched); a good record and `rel` old, torn, missing or already new (rewritten or left, record deleted); recovery run twice gives the same result;
  - the FatFs host test (`host_fatfs`, Approach): every cut point of a first `wifi.txt`, a replace, a `sleep.json` replace and a 1 MiB image, with and without a torn sector; three planted files unchanged; no cross-linked chain;
  - `wifitxt`: format then parse for an open network, an 8-character and a 63-character password, a 64-hex key; refused: an SSID of 33 bytes, one with `\n`, a 7-character password, a 64-character non-hex one; CRLF input parses as LF; a blank first line still means "don't connect".
- **Acceptance**: `mise run fw:test` passes with the new cases, and the cut test prints the number of cut points it ran per operation (none may be 0); `mise run fw:build` passes; on the board (with G2) nothing changes yet, since nothing is writable.
- **Stop condition**: if IDF's `fatfs` component won't build for `linux` outside its own host test, the slice reports it, and the fallback is the same test driving `ff.c` directly with a minimal `ffsystem` and diskio (the FatFs sources are plain C).
- **Cheapest check**: `mise run fw:test`.
- **Owner**: design judgment, firmware C. The cut test is the high-complexity half.

### G2 Card ownership, read-write mount, USB handover

- **Precondition: the backup gate** (Approach). No flash of this build before the user has imaged the card.
- **Files**: `sdcard.c/.h` (writable diskio, remount, state), `usb.c/.h` (driver at boot, share and stop, handles kept), `wifi.c` (no release), `main.c` (boot order: mount, recover, display, Wi-Fi, `usb_start`), `diag.c` (card error counters, if G3 landed), `Kconfig.projbuild` (`WATCHDECK_TEST_CARD`, off by default, like `WATCHDECK_TEST_CRASH`), `.pre-commit-config.yaml` (the no-format hook), `CLAUDE.md`, `site/src/wifi.md`.
- **Changes**: the state machine above. `sdcard_owner()` and an owner-changed callback for the UI. With `WATCHDECK_TEST_CARD` on, a svc job at boot alternates `cardfs_write_small("watchdeck/selftest.json", ...)` (a counter and a fill to 1 to 4 KiB) and a write-once `watchdeck/selftest-<id>.bin` (0 to 1 MiB of counter-seeded bytes, named by the first 16 bytes of its SHA-256 as images are) 20 times, reads each back, logs `card self-test N/20 ok (size, ms)`, and stops on the first mismatch; it logs the first allocation's time (the FAT scan) and each write's time; then it shares the card after 30 s.
- **CLAUDE.md**: the TF card fact becomes "The firmware mounts the first GPT partition read-write and writes only `wifi.txt` and `watchdeck/*`, through `cardfs.c`: small files by an intent record (`watchdeck/<key>.new`) and an in-place write, images write-once under content-addressed names. Never `rename` on the card (an interrupted `f_rename` leaves two names on one chain). Never format; never call esp_tinyusb's app mount, format or `delete_storage`. The card goes to USB only while Settings > Storage shares it, and the firmware refuses writes until Stop sharing reboots the board." The USB fact keeps "host writes apply on the next reboot", now "when sharing stops (a reboot)".
- **Board acceptance (batch A)**:
  1. The backup line is in the results.
  2. Self-test build: 20 of 20 writes pass; the first allocation's FAT scan time is logged (a finding if over 3 s). Then 5 power cuts at random moments during the loop (the user pulls USB power): every boot logs what `cardfs_recover` did, `selftest.json` always parses with a counter, and each `selftest-<id>.bin` is valid or logged invalid and deleted.
  3. Share after 30 s: the computer mounts `tf-data`. Copy a file into its `watchdeck/` folder, then eject. Stop sharing (a temporary serial command or the T4 button): the board reboots, the self-test lists `watchdeck/` and logs the copied file's name and size, and the next write succeeds.
  4. Share, then pull the cable without ejecting, then Stop sharing: the boot mounts, or the Storage state says why.
  5. After the runs, on the computer, `sudo fsck.vfat -n /dev/sdX1` reports nothing beyond the dirty bit, the free count, or lost clusters. The user's own files on the card are unchanged (compare a listing and their hashes with the backup's).
  6. Build without the self-test: boot reads `wifi.txt`, joins, the link comes up, and the host sees an empty reader until a share.
- **Cheapest check**: `mise run fw:build`, the prek hook on a planted `f_mkfs` (it fails), then batch A.
- **Owner**: high complexity, firmware. Needs the user for the backup and every flash.

### G3 Log ring and capture

- **Files**: `firmware/main/logring.c/.h`, `logcap.c/.h`, `diag.c/.h` (new); `main.c` (install first); `wifi.c`, `link.c`, `sdcard.c` (`diag_count` calls; `wifi.c` passes the password to `logcap_set_secret`; `link.c` passes the token); host `test_logring.c`, `test_diag.c`.
- **Host cases**: wrap-around keeps whole lines; a 300-byte line is cut at 159; levels from `E (123) link: x`, `W (...)`, `I (...)`, and a line with no prefix (level `I`); ANSI codes stripped; non-ASCII becomes `?`; sequence numbers keep increasing across wraps; a page read from sequence N gives the same lines after more are appended; redaction of the token and the password at every start position from 0 to 223 in a formatted line (whole, straddling the 159-byte cut, straddling the 224-byte truncation, and wholly past it), with no stored line ever holding 4 or more consecutive characters of a secret at its end or whole; a 3-character password is left alone; `wifi_reason_name` for each listed code and an unknown one.
- **Acceptance**: `mise run fw:test`; `fw:build`; on the board (batch A, through T2): the boot log appears in the Logs tab from the first line, and the UART log is unchanged. Stack high-water marks of the link, LVGL, svc, `wifi`/hosted and `esp_timer` tasks after 10 minutes are logged and each keeps at least 512 bytes.
- **Cheapest check**: `mise run fw:test`.
- **Owner**: design judgment, firmware C.

### G4 Wire contract

- **Files**: `site/src/protocol.md` (the two capabilities, the four frames, the session diagram, the refusals); `app/src-tauri/src/daemon/protocol.rs` (`CAP_SLEEP`, `CAP_FEED`, not yet in `CAPS`; `Message::Sleep`, `AssetGet`, `Feed`, `FeedDismiss`; the asset frame builder beside the icon frame); fixtures `sleep.json`, `sleep_min.json`, `asset_get.json`, `feed.json`, `feed_empty.json`, `feed_dismiss.json`; `firmware/main/proto.h/.c` (`PROTO_MSG_SLEEP`, `PROTO_MSG_FEED`, `proto_sleep_t`, `proto_feed_t`, `proto_parse_asset`, `proto_build_asset_get`, `proto_build_feed_dismiss`); `test_proto.c`.
- **Host cases**: each fixture; `sleep` with `image` of 31 or 33 digits or uppercase hex (refused), `show` with an unknown name (ignored) and a repeat (refused), `text` of 48 bytes (refused), `offline_after_s` -1 and 3601, `backlight` 101, an unknown `spinner` (refused); asset frames: wrong magic, unasked id, wrong offset, a short middle chunk, a long last chunk, the exact 262,168-byte frame; `feed` with 9 cards or 13 notes (refused), a 48-byte title (refused), an unknown `level` (refused), an unknown `state` (unknown), `sources` 17 (refused); the hello with five caps and a 32-character `fw` fits in 256 bytes (the number printed and pinned).
- **Acceptance**: fixtures round-trip in Rust; the board accepts each fixture and refuses each malformed case; existing tests pass unchanged; clippy and CI green. `link.c` doesn't name the new caps yet (P3 and N3 add them, each with its handler).
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml protocol` and `mise run fw:test`.
- **Owner**: mechanical, both parsers.

### G5 Daemon config schema

- **Files**: `config.rs` (`SleepConfig`, `PushConfig`, `IntegrationConfig`, the rules, `secret_users`), `icons.rs` (the file-name rule shared with `screens/`), `commands.rs` (`ConfigView` carries what the forms need), `app/src/config.ts`, `errors.ts`, `edit.ts`, `app/tests/edit.test.ts`, `site/src/config.md`.
- **Tests**: every rule message above, word for word; the example parses; a file without the new tables parses and saves with none of them; a round trip through `Configs::save` of every new key; an `edit.test.ts` case that a button edit keeps `sleep` and `integration` in the draft; a `[sleep]`-only edit leaves `Live::layout()` equal; `token_secret` naming a missing secret is refused, and deleting a secret a push integration uses is refused with the integration's id.
- **Acceptance**: `cargo test`, clippy, `pnpm test` pass. A `config.toml` without the new tables loads and saves as before.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml config::` and `pnpm test`.
- **Owner**: schema, daemon and TS.

## Risks

| Risk | Mitigation |
|---|---|
| A torn FAT sector on a power cut damages other files | The backup gate; writes rare and confined; the host test's torn-sector cuts; G2's five power-cut runs and `fsck.vfat -n`. A failed run stops the round and goes back to the user before any feature writes |
| A runtime `new_storage_blockdev` (Share) misbehaves with a host already attached | G2 step 3 checks it. If it fails, Share also becomes a reboot (an NVS flag read once at boot, cleared as it's read, maps the card to USB before the mount), and the rest of the plan is unchanged |
| A host that never ejected leaves cached writes unwritten | The dialog asks for the eject first. The reboot ends the USB session either way; the host's unflushed writes are lost as they are today when the board resets (CLAUDE.md's MSC fact) |
| The FAT scan before the first allocation is slow on a large card | Logged in G2; it runs on svc, once per boot, and only when something is written |
| Card writes slow ESP-Hosted, which shares the SDMMC host | 32 KiB pieces, writes rare. G2 logs each write's time; a 1 MiB write over 3 s is a finding |
| The hook adds stack on a small task | A 224-byte buffer; G3's high-water check of every task that logs |
| A use-after-consume of the `va_list` garbles the UART or the ring | `va_copy` before the first formatter; the board check compares a captured UART log with the ring's lines |
| A secret reaches the ring through a log line someone adds later | The CLAUDE.md rule, plus the literal-match backstop for the token and the Wi-Fi password |
| The new hello outgrows its buffer | The host test pins the size; keep capability names short |
| A save from the Layout tab drops `[sleep]` or `[[integration]]` | G5's round-trip tests on both sides |
