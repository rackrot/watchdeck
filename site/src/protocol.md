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

The daemon commits to its nonce before it sees the device's, and reveals it only after. Neither side can steer the code, so a man in the middle gets a 1 in 10^6 chance per attempt that the desktop accepts the typed code, and one wrong guess closes the window. That check protects the desktop only. The device can't see whether the code matched, and `pair_result` is unauthenticated, so anyone on the pairing socket can send `ok:true`. The device therefore saves nothing until the user confirms on it that the desktop app says "Paired <device>". The device's side relies on the user checking the desktop before tapping Yes.

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

On `ok:true` the device keeps its token, the daemon's name and the certificate in RAM, and asks the user whether the desktop app says "Paired <device>". Yes stores them in NVS and connects over `wss`. No, or no answer within 60 s, wipes them and shows "pairing cancelled".

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
  |<-- layout ----------------|
  |--- press ---------------->|
  |<-- result ----------------|   exactly one per press
```

- A device that authenticates with the same `device` id replaces its old session. A `hello` that fails authentication never touches the live session, so a copied id can't push the real device off.
- Forgetting a device on the desktop deletes its token hash, sends its live session `error` with `unauthorized`, and closes it.
- Forget pairing on the device erases only the device's copy. The desktop keeps listing the device until it's forgotten there too.

## Messages

### `hello` (device → daemon)

```json
{"t":"hello","v":1,"device":"wd-e8f60ae276ab","token":"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f","fw":"0.2.0","screen":[1024,600]}
```

| Field | Type | Meaning |
|---|---|---|
| `v` | int | Protocol version. The daemon accepts only `1` |
| `device` | string | `wd-` plus the device's Wi-Fi MAC as 12 lowercase hex digits |
| `token` | string | The device's 32-byte token from pairing, as 64 lowercase hex digits |
| `fw` | string | Firmware version |
| `screen` | [int, int] | Screen width and height in pixels |

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
  {"id":"hello","col":0,"row":0,"label":"Hello","bg":1959605,"fg":16777215}
]}
```

| Field | Type | Meaning |
|---|---|---|
| `rev` | int | Increases each time the layout changes |
| `cols`, `rows` | int | Grid size |
| `buttons[].id` | string | Stable button id, used in `press` |
| `buttons[].col`, `row` | int | Zero-based cell |
| `buttons[].label` | string | Printable ASCII |
| `buttons[].bg`, `fg` | int | Colors as `0xRRGGBB` integers |

The daemon sends `layout` after `welcome`, then again only when the layout changes. Empty cells have no button. S5 adds `icon`, and S6 adds `tile`.

### `press` (device → daemon)

```json
{"t":"press","btn":"hello","seq":17}
```

`seq` increases with each press from that device. The daemon echoes it back in `result`.

### `result` (daemon → device)

```json
{"t":"result","btn":"hello","seq":17,"ok":true,"code":0,"text":"","ms":12}
```

| Field | Type | Meaning |
|---|---|---|
| `ok` | bool | Whether the action succeeded |
| `code` | int | Exit code, or `-1` when there is no exit code (unknown button, already running, timeout) |
| `text` | string | Up to the last 1 KB of output, or an error message |
| `ms` | int | How long the action took to run |

The daemon answers every press with exactly one `result`, including failures. The device shows it as a short toast.

## S2 behavior

- The daemon serves one hard-coded button, `hello`, which runs `notify-send watchdeck "Hello from <device>"` on the desktop. Configured actions arrive in S4.
- A press on an unknown button id gets `ok:false`, `code:-1`, `text:"unknown button"`.
