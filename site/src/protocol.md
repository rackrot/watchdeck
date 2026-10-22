# Device protocol

This page defines how the device and the desktop daemon talk. Both sides implement it, and the daemon's test fixtures use these exact examples.

Status: **protocol v1.** Sessions run over TLS (`wss://`) pinned to the daemon's certificate, and every session needs the token the device made at pairing. See `plans/desktop-app-v1.md` for the full design.

## Ports

| Port | Transport | Open |
|---|---|---|
| 47820 | `wss` | Whenever the daemon runs |
| 47821 | plain `ws` | Only while the desktop's pairing dialog is open, for at most 120 s |

## Discovery

The daemon advertises itself over mDNS:

| Field | Value |
|---|---|
| Service | `_watchdeck._tcp` |
| Port | 47820 |
| TXT | `v=1`, `fp=<16 hex>`, and `pair=47821` only while pairing is open |

`fp` is the first 16 hex digits of SHA-256 over the daemon's certificate PEM, byte for byte.

- A paired device keeps the instances whose `fp` matches the certificate it pinned at pairing. It tries their IPv4 addresses in order, its own subnet first, and moves to the next at once when one fails. It connects to `wss://<addr>:47820/`.
- An unpaired device lists up to 4 instances that carry `v=1` and a `pair` port, and looks again every 4 s. The user taps one to pair.

## Transport security

- The daemon serves `wss` on 47820 only. Plain `ws` is refused.
- The certificate is self-signed P-256, created on the daemon's first run and stored with its key in the daemon's config directory.
- The device trusts exactly the certificate it received at pairing (a byte-for-byte pin) and checks neither its name nor its dates.
- The `fp` in mDNS only selects which instance to try. Any LAN peer can advertise it, so the pin is what authenticates the daemon.
- The device shows "daemon identity changed" when instances are visible but none carry the pinned `fp`, and "wrong cert from daemon" when an instance with the right `fp` presents another certificate.

## Pairing

Pairing runs over plain `ws` on 47821, so anyone on the LAN can read it. Nothing secret crosses it. The device makes a random 32-byte token and sends only its SHA-256 (`commit`). A commit/reveal exchange then gives both sides the same 6-digit code. The device shows the code, the user types it into the desktop app, and then the user confirms on the device that the app shows the pairing.

The daemon commits to its nonce before it sees the device's, and reveals it only after. Neither side can steer the code, so a man in the middle gets a 1 in 10^6 chance per attempt that the desktop accepts the typed code, and one wrong guess closes the window. That check protects the desktop only. The device can't see whether the code matched, and `pair_result` is unauthenticated, so anyone on the pairing socket can send `ok:true`. The device therefore saves nothing until the user confirms on it that the desktop app says "Paired `<device>`". The device's side relies on the user checking the desktop before tapping Yes.

```text
device                          daemon (dialog open)
  |--- pair_req ----------------->|   within 10 s of the upgrade
  |<-- pair_offer ----------------|
  |--- pair_nonce --------------->|   within 10 s
  |<-- pair_reveal ---------------|   both sides derive the code
  |      (the user types the code into the desktop app)
  |<-- pair_result ---------------|   then close
  |      (on ok:true the device asks: does the app say "Paired <device>"?
  |       Yes saves the pairing and connects; No or 60 s saves nothing)
```

Binary fields are lowercase hex: `commit` and `c_host` are 64 digits, `n_dev` and `n_host` 32.

### `pair_req` (device → daemon)

```json
{"t":"pair_req","v":1,"device":"wd-000000000001","commit":"630dcd2966c4336691125448bbb25b4ff412a49c732db2c8abc1b8581bd710dd"}
```

`commit` is SHA-256(token). `device` has the same form as in `hello`.

### `pair_offer` (daemon → device)

```json
{"t":"pair_offer","name":"buck-desktop","cert":"-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----\n","c_host":"816b9e7c25d559c5766755b3bbb36654ad451e080ffa93694a793d6eed41f40a"}
```

| Field | Meaning |
|---|---|
| `name` | The daemon host's name, for display only |
| `cert` | The daemon's certificate PEM, which the device will pin. At most 2047 bytes |
| `c_host` | SHA-256(n_host), where n_host is 16 random bytes |

The device refuses a `cert` that isn't exactly one PEM certificate that mbedTLS parses.

### `pair_nonce` (device → daemon)

```json
{"t":"pair_nonce","n_dev":"202122232425262728292a2b2c2d2e2f"}
```

`n_dev` is 16 random bytes.

### `pair_reveal` (daemon → device)

```json
{"t":"pair_reveal","n_host":"303132333435363738393a3b3c3d3e3f"}
```

The device aborts unless SHA-256(n_host) equals `c_host`.

### The code

The code is `BE_u32(SHA-256(enc)[0..4]) mod 10^6`, zero-padded to 6 digits. `enc` joins these fields in order, each as a u16-BE length and then its bytes:

1. `"watchdeck-pair-v1"`
2. `device`
3. `commit`, as 32 raw bytes
4. `cert`, the PEM bytes
5. `n_dev`, as 16 raw bytes
6. `n_host`, as 16 raw bytes

`name` stays out of `enc`, because the device shows it but pins only `cert`.

Test vector, matching the examples above:

| Input | Value |
|---|---|
| token | bytes `00` to `1f` |
| `device` | `wd-000000000001` |
| `cert` | `-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----\n` |
| `n_dev` | bytes `20` to `2f` |
| `n_host` | bytes `30` to `3f` |
| code | `333724` |

### `pair_result` (daemon → device)

```json
{"t":"pair_result","ok":true}
```

`ok` is true only when the typed code matched and the daemon stored SHA-256(token) for `device`. The daemon closes after sending it.

On `ok:true` the device keeps its token, the daemon's name and the certificate in RAM, and asks the user whether the desktop app says "Paired `<device>`". Yes stores them in NVS and connects over `wss`. No, or no answer within 60 s, wipes them and shows "pairing cancelled".

The daemon stores its side before the user answers, so a No leaves a token hash on the desktop that nobody can use, since only the device ever held the token. The device's question screen tells the user to Forget the device on the desktop after a No. Nothing syncs this automatically.

### Pairing rules

- The listener serves one device at a time.
- An attempt that fails before the code leaves the window open for another. A step slower than 10 s is closed. Before the code, a malformed or out-of-order message gets `error` with `bad_request`, and a wrong `v` gets `version`, then a close.
- Once the daemon sends `pair_reveal`, the device must stay silent until `pair_result`. Any message from it, or a disconnect, ends the window with `pair_result` `ok:false`.
- The user gets one guess per window. A wrong code gets `pair_result` with `ok:false`, stores nothing, and closes the window.
- Cancelling the dialog or reaching the 120 s limit closes the listener. A device waiting on the code gets `pair_result` with `ok:false`.
- Pairing a device again replaces its token, sends its live session `error` with `unauthorized`, and closes it.

## Connection

- The daemon is the WebSocket server, and the device is the client.
- Messages are JSON in text frames. Every message has a string field `t` that names its type.
- Each message is a single frame of at most 16 KB in either direction, on both ports. The receiver closes the connection on anything larger.
- The one exception is an [icon frame](#icon-frames), a binary frame from the daemon to the device of at most 262,168 bytes. The device never sends binary frames, and the daemon answers one with `error` `bad_request` and a close.
- Fragmented messages aren't supported. The device drops every frame without FIN, both the first fragment and its continuation frames, so the daemon must send each message as one frame. The device logs `fragmented message dropped` for the first fragment and keeps the link up, so the message gets no reply.
- Both sides ignore unknown fields. An unknown `t` is logged and ignored.
- Liveness uses WebSocket ping frames. The device pings every 10 s and drops the link after 25 s without a pong. The daemon drops a session that has been silent for over 30 s.
- If the link drops, the device reconnects:
  1. It backs off 1, 2, 4, 8, 16 and then 30 s (cap), each with ±20% jitter.
  2. It re-runs mDNS discovery before every attempt, so a changed desktop IP is picked up.
  3. Getting an IP or receiving a `welcome` resets the backoff.

## Session

```text
device                      daemon
  |--- hello ---------------->|   within 5 s of connecting
  |<-- welcome ---------------|   or error, then close
  |<-- layout ----------------|   and again whenever the grid changes
  |--- layout_ack ----------->|   once the grid is built
  |--- icon_get ------------->|   for icons the device doesn't have
  |<-- icon frame (binary) ---|   one per icon
  |--- press ---------------->|
  |<-- result ----------------|   exactly one per press
  |--- report --------------->|   a crash or reset from an earlier boot, if the device has one
  |<-- report_ack ------------|   after the daemon logs it
```

- Presses run concurrently. Results arrive in the order the actions finish, and pings, presses and layouts keep flowing while an action runs.
- A device that authenticates with the same `device` id replaces its old session. A `hello` that fails authentication never touches the live session, so a copied id can't push the real device off.
- Forgetting a device on the desktop deletes its token hash, sends its live session `error` with `unauthorized`, and closes it.
- Forget pairing on the device erases only the device's copy. The desktop keeps listing the device until it's forgotten there too.

## Messages

### `hello` (device → daemon)

```json
{"t":"hello","v":1,"device":"wd-e8f60ae276ab","token":"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f","fw":"0.2.0","screen":[1024,600],"caps":["fonts"]}
```

| Field | Type | Meaning |
|---|---|---|
| `v` | int | Protocol version. The daemon accepts only `1` |
| `device` | string | `wd-` plus the device's Wi-Fi MAC as 12 lowercase hex digits |
| `token` | string | The device's 32-byte token from pairing, as 64 lowercase hex digits |
| `fw` | string | Firmware version |
| `screen` | [int, int] | Screen width and height in pixels |
| `caps` | [string] | Optional, default empty. The capabilities the device supports, listed below |

`caps` is how the protocol grows without a new `v`, which the daemon accepts only as `1`:

- A device names only capabilities it fully supports, and the firmware that adds a capability's support is the firmware that starts naming it. A `hello` without `caps` means none.
- The daemon ignores names it doesn't know, so a newer device can name more than an older daemon understands. An older daemon ignores the whole field, so a device that names capabilities still works with it, and gets none of their fields.
- The daemon decides per session. It sends a capability's fields only to a session whose `hello` names it. A session lasts one connection, so a device that updates its firmware names the new capability on its next `hello`.
- Adding a capability doesn't change `v`, the mDNS TXT `v` or the pairing label.

| Capability | The device |
|---|---|
| `fonts` | Draws the seven fonts listed under [Fonts](#fonts), and reads the font fields of `layout` |

The daemon hashes `token` and compares the result in constant time with the hash it stored for `device`. A missing, malformed or wrong token and an unknown device all get the same `error`, then a close:

```json
{"t":"error","code":"unauthorized","msg":"unauthorized"}
```

### `welcome` (daemon → device)

```json
{"t":"welcome","v":1,"name":"buck-desktop"}
```

`name` is the daemon host's name, which the device can show.

### `error` (daemon → device)

```json
{"t":"error","code":"version","msg":"unsupported protocol version 2"}
```

The daemon closes the connection after sending it. `code` is one of `version`, `bad_request` or `unauthorized`. A device that gets `unauthorized` over `wss` erases its pairing and goes back to listing daemons open for pairing.

### `layout` (daemon → device)

```json
{"t":"layout","rev":1,"cols":5,"rows":3,"buttons":[
  {"id":"hello","col":0,"row":0,"label":"Hello","bg":2001125,"fg":16777215,"icon":"d95bd7f461bd7bc39755b97d11f8a270"},
  {"id":"plain","col":1,"row":0,"label":"Plain","bg":0,"fg":16777215},
  {"id":"nas-load","col":2,"row":0,"label":"NAS load","bg":2001125,"fg":16777215,"tile":true}
]}
```

| Field | Type | Meaning |
|---|---|---|
| `rev` | int | 1 for the session's first layout, then one more for each layout pushed on it |
| `cols`, `rows` | int | Grid size, 1 to 8 columns by 1 to 5 rows |
| `buttons[].id` | string | Stable button id, used in `press`. At most 63 bytes |
| `buttons[].col`, `row` | int | Zero-based cell |
| `buttons[].label` | string | Printable ASCII, at most 47 bytes |
| `buttons[].bg`, `fg` | int | Colors as `0xRRGGBB` integers |
| `buttons[].icon` | string | Optional. The icon's id as 32 lowercase hex digits. Absent for a button without one. A layout with any other value, `null` included, is refused |
| `buttons[].tile` | bool | Optional, default `false`. `true` for a button that shows a live value, which arrives as [`tile`](#tile-daemon--device) messages. Any other type is refused |
| `font`, `buttons[].label_font`, `buttons[].value_font` | string | Optional, only to a session with the `fonts` capability. See [Fonts](#fonts) |

The daemon sends `layout` after `welcome`. When the user's config changes the grid, a button, a label, a color or a position, the daemon pushes a new `layout` on the same session, and the device replaces its grid without reconnecting. An edit that changes only actions sends nothing. Empty cells have no button. A layout that turns a tile on or off for a button counts as a change to a button.

The layout carries only icon ids. The device keeps the icons it has, and asks for the rest with `icon_get`. A button without its icon yet shows its label alone and gains the picture when the frame arrives.

#### Fonts

A device that names `fonts` in `hello.caps` draws button text in Montserrat (built in) or any of six more fonts. The ids are `montserrat`, `inter`, `roboto`, `fira-code`, `roboto-mono`, `orbitron` and `oxanium`.

```json
{"t":"layout","rev":3,"cols":5,"rows":3,"font":"inter","buttons":[
  {"id":"plain","col":0,"row":0,"label":"Plain","bg":0,"fg":16777215},
  {"id":"nas-load","col":2,"row":0,"label":"NAS load","bg":2001125,"fg":16777215,"tile":true,"label_font":"orbitron","value_font":"fira-code"}
]}
```

| Field | Where | Meaning |
|---|---|---|
| `font` | message | The grid's default font. Absent means `montserrat` |
| `label_font` | button | The label's font. Absent means the message's `font` |
| `value_font` | button | A tile's value font. Absent means the button's label font |

- The device resolves a label font as `label_font`, else `font`, else `montserrat`, and a value font as `value_font`, else the label font. The daemon sends `label_font` only when the button's label font differs from the message's `font`, and `value_font` only when the value font differs from the label font. So a button that uses the grid's font carries nothing, and `montserrat` is sent where a button pins it under another grid font.
- The daemon sends `font` whenever the grid's font isn't `montserrat`, even when no button uses it, because a later field of the layout can draw in it.
- The device draws status lines, the pairing screens and result screens in Montserrat whatever the layout says.
- A font never changes a button's size, so changing only fonts changes no icon id and needs no new `icon_get`.
- **Fallback.** A device that gets a font id it doesn't know, or a value that isn't a string, draws that text in Montserrat, logs it, and still builds and acks the layout. A font is cosmetic, so the layout is never refused for one.
- **Without the capability** a session gets none of these fields. Its layout is the one a config without fonts would produce, and a change to fonts alone sends it no new `layout`, so its `rev` doesn't move. With the capability, a change to fonts alone pushes a new `layout` with the next `rev`.

### `layout_ack` (device → daemon)

```json
{"t":"layout_ack","rev":2}
```

| Field | Type | Meaning |
|---|---|---|
| `rev` | int | The `rev` of the `layout` whose grid the device has built |

The device sends it for every `layout`, after it has built the grid and sent its `icon_get`. Icons still on the way don't hold it back, because a button shows its label until its picture arrives. The daemon tells the desktop window that the device applied the layout.

- The daemon logs and ignores an ack for a `rev` it never sent on this session (0 or above the latest), and one no newer than the latest ack it already took.
- A `rev` that is not a non-negative integer of at most 32 bits is refused like any malformed message: `error` `bad_request` and a close.
- A daemon from before this message ignores it as an unknown `t`. A board from before it never sends one, and the daemon then reports only that it sent the layout. Adding `layout_ack` doesn't change `v`.

### `icon_get` (device → daemon)

```json
{"t":"icon_get","ids":["9f86d081884c7d659a2feaa0c55ad015","d95bd7f461bd7bc39755b97d11f8a270"]}
```

| Field | Type | Meaning |
|---|---|---|
| `ids` | [string] | Icon ids from the current layout that the device lacks. At most 40, each exactly 32 lowercase hex digits |

The daemon answers each id that the session's current layout names with one [icon frame](#icon-frames), in the order asked, and sends nothing for any other id. A repeated id gets one frame, since the daemon drops duplicates. A list that is too long or has a malformed id gets `error` `bad_request` and a close.

The device sends one `icon_get` per layout, listing every distinct id it doesn't hold, and sends none when it holds them all. It keeps the icons of the layout on screen only. A layout that drops an icon frees it, and a link that drops and returns keeps the rest, so a reconnect to an unchanged layout fetches nothing.

### Icon frames

An icon is a binary WebSocket frame from the daemon:

```text
offset  size    field
0       4       "WDI1"
4       16      id
20      2       width, u16 LE
22      2       height, u16 LE
24      w*h*4   pixels, row by row, from the top left
```

- Pixels are LVGL's ARGB8888: the bytes **B, G, R, A** for each pixel, with straight (not premultiplied) alpha.
- The `id` is the first 16 bytes of SHA-256 over the frame from byte 20 on: width, height and pixels. Equal pictures share an id.
- Width and height are each 1 to 256, and the frame is exactly 24 + w·h·4 bytes, so a frame is at most 262,168 bytes.
- The device checks, in this order, that the frame has a header, that it starts with `WDI1`, that width and height are in range, that its length is exactly right, and that the hash matches `id`. It also checks that it asked for that `id`. A frame that fails any check is dropped and logged, and the session goes on. The device never reads a pixel before the length check passes.

Test vector, a 2×1 icon of one opaque red pixel and one fully transparent pixel:

| Field | Value |
|---|---|
| pixels | `00 00 ff ff` `00 00 00 00` |
| `id` | `d95bd7f461bd7bc39755b97d11f8a270` |
| frame | `57 44 49 31`, the 16 id bytes, `02 00 01 00`, then the 8 pixel bytes: 32 bytes in all |

The daemon sizes every icon to fit its button, with room left for the label. See [Icons](config.md#icons) for the box and how pictures are scaled.

### `press` (device → daemon)

```json
{"t":"press","btn":"hello","seq":17}
```

`seq` increases with each press from that device. The daemon echoes it back in `result`.

### `result` (daemon → device)

```json
{"t":"result","btn":"hello","seq":17,"ok":true,"code":0,"summary":"exit 0","text":"","ms":12}
```

| Field | Type | Meaning |
|---|---|---|
| `btn` | string | The button id from the `press` |
| `ok` | bool | Exit 0 for a shell action, a 2xx status for an HTTP action |
| `code` | int | The exit code or HTTP status, or `-1` when there is neither |
| `summary` | string | One line, at most 96 bytes, on what happened |
| `text` | string | The last 1 KB of output: stdout then stderr, or the HTTP body. Printable ASCII and `\n` only. Often empty |
| `ms` | int | How long the action ran. `0` when nothing ran |

The daemon answers every press with exactly one `result`, including when nothing runs. Typical summaries:

| `summary` | `ok` | `code` |
|---|---|---|
| `exit 0`, `exit 3` | exit 0 only | The exit code |
| `killed by signal 9` | false | `-1` |
| `timed out after 30 s` | false | `-1` |
| `could not start ssh: …` | false | `-1` |
| `HTTP 200`, `HTTP 503`, `HTTP 302, redirect not followed` | 2xx only | The status |
| `request failed: …` | false | `-1` |
| `unknown button`, `already running`, `busy: 8 actions are running` | false | `-1` |

The device shows the result in a modal over the grid: the button's label with OK or Failed, the summary and time, and the text. It closes at a tap, after 4 s when `ok`, or after 10 s when not. A result whose `seq` the device didn't send is ignored. `summary` arrived with S4, so a device that finds none shows the code instead.

### `tile` (daemon → device)

```json
{"t":"tile","btn":"nas-load","text":"0.42","state":"ok"}
```

| Field | Type | Meaning |
|---|---|---|
| `btn` | string | The id of a button whose layout entry has `tile: true` |
| `text` | string | The tile's value: one line of printable ASCII, at most 64 characters |
| `state` | string | `ok` or `bad`. A device shows any other value, or none, as unknown |

The daemon polls each tile only while at least one device has a session, and sends a `tile` when a tile's value or state differs from what it last sent on that session. So:

- A session gets the current value of every tile that has one right after its `layout`, and again after each later `layout`, because a device rebuilds its grid for each layout and the new buttons start blank.
- A tile that hasn't answered yet has sent nothing. The device shows it as unknown until its first `tile`.
- A tile whose polls couldn't run (no action slot free, 3 times in a row) sends `bad` with the text `stale`. A device needs nothing special for it.
- The daemon never sends a value that a tile's source produced before the config edit it has applied.
- A `tile` for an unknown button, or a button without `tile`, is ignored.
- A session that the daemon has revoked or replaced gets no more `tile` messages. See [Config](config.md#tiles) for what the value and the state mean.

The device updates the button's text and color in place and doesn't rebuild the grid.

### `report` (device → daemon)

```json
{"t":"report","id":"a1b2c3d4","kind":"crash","reason":"panic","task":"link","pc":"0x4ff01234","ra":"0x4ff05678","cause":"LoadAccessFault","addr":"0x00000000","elf":"0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0","same_build":true}
```

| Field | Type | Meaning |
|---|---|---|
| `id` | string | 8 lowercase hex digits that name this report in `report_ack` |
| `kind` | string | `crash` for a coredump from an earlier boot, or `reset` for an abnormal reset with no dump |
| `reason` | string | `panic`, `int_wdt`, `task_wdt`, `wdt` or `brownout` |
| `task` | string | The task that was running, or empty. Printable ASCII, at most 16 characters |
| `pc`, `ra` | string | The faulting program counter and the return address, each `0x` and 8 lowercase hex digits |
| `cause` | string | The fault's name, such as `LoadAccessFault`. 1 to 32 printable ASCII characters |
| `addr` | string | The faulting address (`mtval`), in the same form as `pc` |
| `elf` | string | SHA-256 of the crashed firmware's ELF, as 64 lowercase hex digits. With the `pc` it finds the source line using `riscv32-esp-elf-addr2line` |
| `same_build` | bool | Optional, default `false`. `true` when the crashed firmware is the one now running, so the `fw` in `hello` names it |

`pc`, `ra`, `cause`, `addr`, `elf` and `same_build` are required for `crash` and ignored for `reset`, which has no dump to read them from.

- The device sends its report after `welcome` and again on every session until it gets the matching `report_ack`. It keeps the report until then.
- The daemon checks every field above, and a report counts toward the session's limit of two whether it passes or not.
  - A report with a valid `id` that fails a check, or whose fields have the wrong JSON type, is rejected. The daemon logs the failed field without the report's values and sends `report_ack` so the device erases it.
  - A report without a valid `id`, or one that arrives after the session's second, is logged and dropped with no `report_ack`.
  - Either way the session goes on.
- The daemon writes one `warn` log line per accepted report and then sends `report_ack`.
- A daemon from before this message ignores it as an unknown `t` and never acks, so the device keeps the report for a newer daemon. Adding `report` and `report_ack` doesn't change `v`.

### `report_ack` (daemon → device)

```json
{"t":"report_ack","id":"a1b2c3d4"}
```

`id` is the `id` of an accepted `report`. The device then erases the dump.
