# Device protocol

This page defines how the device and the desktop daemon talk. Both sides implement it, and the daemon's test fixtures use these exact examples.

Status: **protocol v1, S2 subset.** S2 has no authentication and runs over plain WebSocket. S3 adds pairing, the `token` field and TLS. See `plans/desktop-app-v1.md` for the full design.

## Discovery

The daemon advertises itself over mDNS:

| Field | Value |
|---|---|
| Service | `_watchdeck._tcp` |
| Port | 47820 |
| TXT | `v=1` |

The device browses for `_watchdeck._tcp`, takes the first IPv4 address (preferring its own subnet), and connects to `ws://<addr>:47820/`.

## Connection

- The daemon is the WebSocket server, and the device is the client.
- Messages are JSON in text frames. Every message has a string field `t` that names its type.
- Each message is a single frame of at most 16 KB in either direction. The receiver closes the connection on anything larger.
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

A device that reconnects with the same `device` id replaces its old session.

## Messages

### `hello` (device → daemon)

```json
{"t":"hello","v":1,"device":"wd-e8f60ae276ab","fw":"0.2.0","screen":[1024,600]}
```

| Field | Type | Meaning |
|---|---|---|
| `v` | int | Protocol version. The daemon accepts only `1` |
| `device` | string | `wd-` plus the device's Wi-Fi MAC as 12 lowercase hex digits |
| `fw` | string | Firmware version |
| `screen` | [int, int] | Screen width and height in pixels |

### `welcome` (daemon → device)

```json
{"t":"welcome","v":1,"name":"buck-desktop"}
```

`name` is the daemon host's name, which the device can show.

### `error` (daemon → device)

```json
{"t":"error","code":"version","msg":"unsupported protocol version 2"}
```

The daemon closes the connection after sending it. `code` is one of `version` or `bad_request`. S3 adds `unauthorized`.

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

- The daemon serves one hard-coded button, `hello`, which runs `notify-send watchdeck "Hello from <device>"` on the desktop. Nothing else can run until S3 adds pairing.
- A press on an unknown button id gets `ok:false`, `code:-1`, `text:"unknown button"`.
