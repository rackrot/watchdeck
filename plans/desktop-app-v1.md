# watchdeck v1: daemon, device protocol, screen config

## Status (2026-10-11)

| Slice | State | Commit | Waiting on |
|---|---|---|---|
| S1 Touch | ✅ | 5cc9501 | — |
| S2 Thin end-to-end | ✅ | 50e63c2, eeb664d | — |
| S3 Security gate (TLS, pairing) | ✅ | f48c5b2, 889e6c8 | — |
| S4 Config and actions | ✅ | d42c1d0 | — |
| S5 Icons | ✅ | 147e695 | — |
| S6 Tiles | ✅ | 1d05fdf | — |
| S7 Config UI | Built, reviewed, audit fixes in | c9e9fe9 | The user's manual check (steps under "S7 results") |
| S8 App shape | Built, reviewed | e709869, 37016e3, merged in ceb6f62 | The user's manual check: a release build with autostart, tray, close-to-tray and single-instance |

- Every slice passed `review` and, where it touches security, `security-auditor`. Each fix round then got its own review.
- CI is green through c9e9fe9: lint, firmware, app (clippy, `cargo test`, `pnpm test`, debug Tauri build) and site.
- v1 is done when the S7 and S8 manual checks pass.
- Still untested on hardware, from earlier slices: the board recovering after the desktop's IP changes (S2), and a bad frame at the device's icon and tile parsers (S5, S6).
- Open beyond v1:
  - the Backlog section at the end (Sentry error reporting)
  - the S7 gaps listed in "S7 results": no secret delete, no `list_icons`, no `test_action`
  - optionally, avahi `deny-interfaces` for docker bridges, so `_watchdeck._tcp` isn't advertised on them

## Decisions

| Topic | Decision |
|---|---|
| Daemon host | The Tauri tray app runs the daemon in-process. The window is only the config UI |
| Actions | Shell commands/scripts, HTTP requests, live status tiles. Pages come later |
| Trust | Pair once with a code shown on the device, then a token on every connection |

## Outcome

- A paired device shows the configured grid. A press runs a shell or HTTP action on the desktop daemon, and the result shows on the device.
- Tiles refresh live. Edits made in the Tauri window reach the device live.

Done when all of these hold:

- Touch works.
- The device finds the paired daemon over mDNS and recovers from a daemon restart, a desktop IP change, and a Wi-Fi drop without rebooting.
- Pairing is the only way in. Every session runs over wss pinned to the daemon's cert. An unpaired or forgotten device gets no layout and can't press.
- `cargo test` proves the shell, HTTP and tile behavior.
- A UI save reaches the device in under 2 s without a reconnect.
- The app autostarts into the tray, and closing the window doesn't stop the daemon.
- CI is green: lint, firmware, app (clippy plus `cargo test`), and site.

## Assumptions

- The desktop is Linux. The shell runner uses Unix process groups.
- One layout serves every paired device, with no pages. Pages can come later as protocol v2.
- The LAN is untrusted enough that a Wi-Fi sniffer or an ARP spoofer is in scope.
- GT911 RST 40, INT 42 and address 0x5D were verified on hardware in S1.

## 1. Architecture

### Daemon: `app/src-tauri/src/daemon/`

Nothing here imports `tauri`, so tests run headless.

| Module | Owns |
|---|---|
| `protocol.rs` | Message types (serde, tag `t`), size limits, icon-frame codec, SAS function |
| `config.rs` | TOML schema, validation, atomic save (tmp + rename, 0600), refusal on a stale revision, polling `config.toml` and `secrets.toml` |
| `secrets.rs` | `secrets.toml`: names out, values only into request headers |
| `store.rs` | `state.json` (device id → SHA-256(token)), identity cert and key, the 0600 write and check helpers. Files 0600, dir 0700 |
| `tls.rs` | rcgen self-signed P-256 cert, rustls `ServerConfig` |
| `pairing.rs` | Pairing listener (open only while the pairing dialog is open) and the pairing state machine |
| `server.rs` | wss listener, per-session auth and dispatch, layout push on config change |
| `mdns.rs` | avahi-daemon registration over D-Bus (zbus) |
| `actions.rs`, `actions/` | One run per button, the global limit, result text; the shell and HTTP runners |
| `tiles.rs` | Tile pollers |
| `icons.rs` | Decode, fit, ARGB8888, hash, import. The live config carries the rendered icons |

- A `watch<Arc<Config>>` carries live config. A `broadcast<DaemonEvent>` carries connects, pairing requests and action runs.
- `lib.rs` builds Tauri, plugins and the tray, starts the daemon and mDNS, and forwards events to the UI and the tray status line.
- `commands.rs` exposes the Tauri commands:
  - config: `get_config`, `save_config`
  - devices and pairing: `list_devices`, `forget_device` (closes the live session too), `start_pairing`, `pairing_state`, `cancel_pairing`, `confirm_pairing`
  - secrets: `list_secrets`, `set_secret` (write-only)
  - tiles: `tile_values`
  - icons: `import_icon`, which opens the native file dialog itself and takes no path from the window
  - `test_action` was planned but not built (an S7 gap)
- Events to the UI: `devices-changed`, `pairing`, `config-status`, `layout-pushed` and `tile-values`.
- React UI in `app/src`:
  - pages: ConfigPage (GridEditor, ButtonForm, ActionForm, SaveBar), Secrets, Devices with PairDialog
  - preview: `preview.ts`, `lvtext.ts`, `montserrat.ts`, which mirror the firmware geometry and LVGL line breaking
  - shared: `api.ts` (typed invoke wrappers), `config.ts` (types), `errors.ts`, `saveflow.ts`, `theme.css`
  - Rust validation is the authority.

### Firmware: `firmware/main/`

| File | Owns |
|---|---|
| `main.c` | Boot order unchanged, then touch, ui, link |
| `display.c` | Panel and LVGL port. The hello labels move to `ui.c` |
| `touch.c` | GT911 via `esp_lcd_touch_gt911` + `lvgl_port_add_touch`, swap/mirror `#define` knobs |
| `ui.c` | Status screen, daemon list, pairing code and question, grid, result modal, tiles, Forget pairing |
| `proto.c` | cJSON parse/build, icon-frame parse. Pure functions |
| `pairing.c` | Pairing session, SAS, NVS blob |
| `link.c` | Discovery, backoff, websocket lifecycle, dispatch, icon requests |
| `icons.c` | The PSRAM icon cache, by id, bounded. Link task only |

- All LVGL calls run under `lvgl_port_lock`.
- The touch callback and the websocket event handler only post to the link task's queue. The link task does every send and every stop/destroy.
- `link.c` listens for `IP_EVENT` and `WIFI_EVENT` itself, so `wifi.c` doesn't change.

## 2. Discovery and connection

- **Advertising.** The daemon advertises `_watchdeck._tcp` on port 47820 (wss), with TXT `v=1`, `fp=<16 hex of SHA-256(cert PEM)>`, and `pair=47821` only while the pairing dialog is open. avahi follows IP changes and answers on every interface, including docker and virbr.
- **Browsing.** The device runs `mdns_query_ptr`, then TXT/A follow-ups for missing fields.
  - Paired: it picks the instance whose `fp` matches NVS, which is how it chooses among several daemons. IPv4 only, own subnet first.
  - Unpaired: it lists the instances, and the user taps one to pair.
- **Reconnect.** The link task owns the loop: destroy the client, back off 1, 2, 4, 8, 16, 30 s (cap) with ±20% jitter, re-query mDNS, connect. `GOT_IP` and a successful `welcome` reset the backoff.
- **Liveness.** WS ping every 10 s with a 25 s timeout. The daemon drops sessions silent for over 30 s. A reconnecting device id replaces its old session.

## 3. Wire protocol

| Choice | Pick | Rejected |
|---|---|---|
| Transport | WebSocket. The daemon is the server (tokio-tungstenite) and the device the client (esp_websocket_client) | Raw TCP re-invents framing and keepalive. MQTT needs a broker |
| Encoding | JSON text frames; binary frames only for icon pixels | CBOR saves nothing that matters on a LAN and is harder to debug. protobuf adds codegen to two build systems |

Main port 47820 (wss):

```text
D→H hello    {"t":"hello","v":1,"device":"wd-a1b2c3","token":"<64 hex>","fw":"0.2.0","screen":[1024,600]}
H→D welcome  {"t":"welcome","v":1,"name":"buck-desktop"}
H→D error    {"t":"error","code":"unauthorized|version|bad_request","msg":"…"}   then close
H→D layout   {"t":"layout","rev":7,"cols":5,"rows":3,"buttons":[
               {"id":"plex","col":0,"row":0,"label":"Plex","bg":2001125,"fg":16777215,
                "icon":"9f86d081884c7d659a2feaa0c55ad015","tile":false}]}
D→H icon_get {"t":"icon_get","ids":["9f86d081884c7d659a2feaa0c55ad015"]}
H→D <binary> "WDI1"(4) | id(16) | w u16le | h u16le | w*h*4 bytes ARGB8888 (LVGL order B,G,R,A)
D→H press    {"t":"press","btn":"plex","seq":17}
H→D result   {"t":"result","btn":"plex","seq":17,"ok":false,"code":3,"summary":"exit 3","text":"<≤1 KB output tail>","ms":412}
H→D tile     {"t":"tile","btn":"nas-load","text":"0.42","state":"ok|bad"}   (the device shows anything else as unknown)
```

- The device sends `hello` within 5 s of connecting.
- The daemon sends `layout` after `welcome`, then again only when it changes. An action-only edit doesn't redraw.
- The device keeps icons by id, pulls any it's missing, and frees unreferenced ones.
- The daemon answers every press with exactly one `result`, including timeout, unknown button and already running.
- Versioning:
  - The daemon accepts `v`=1 and nothing else.
  - Adding a field doesn't bump `v`. Unknown fields are ignored.
  - An unknown `t` is logged and ignored.
- Limits:
  - Device to daemon: 16 KB.
  - Daemon to device: 16 KB text, binary up to 24 + 256·256·4 bytes.
  - The device closes on anything larger.
- The device reassembles chunked `WEBSOCKET_EVENT_DATA` into a PSRAM buffer and filters on `op_code`, because PING payloads arrive as DATA too.

## 4. Security

### Pairing (plain ws on port 47821; nothing secret crosses it)

| # | Dir | Message | Note |
|---|---|---|---|
| 1 | D→H | `pair_req {v, device, commit}` | token = 32 random bytes; commit = SHA-256(token) |
| 2 | H→D | `pair_offer {name, cert, c_host}` | cert PEM; c_host = SHA-256(n_host), where n_host is 16 random bytes |
| 3 | D→H | `pair_nonce {n_dev}` | n_dev is 16 random bytes |
| 4 | H→D | `pair_reveal {n_host}` | The device aborts unless SHA-256(n_host) = c_host |
| 5 | both | code = `BE_u32(SHA-256(enc)[0..4]) mod 10^6`, 6 digits | enc is each field as u16-BE length + bytes, in order: `"watchdeck-pair-v1"`, device, commit, cert PEM, n_dev, n_host. The device shows the code; the user types it into the app |
| 6 | H→D | `pair_result {ok}` | On a match the daemon stores device → commit. A mismatch or the 120 s timeout aborts. One pending request at a time |
| 7 | device | the person confirms | On `ok` the device asks whether the app says "Paired <device>". Yes saves and connects; No or 60 s saves nothing |

- **Why commit-then-reveal.** A man-in-the-middle sees the nonce only after committing its own inputs, so its best odds of getting the desktop to accept the typed code are 1 in 10^6. A single-round 6-digit code could be ground offline in milliseconds.
- **Why the device asks.** The code protects only the desktop. The device can't see whether the code matched, and `pair_result` is unauthenticated, so anyone on the pairing socket can send `ok:true`. The device pins nothing until the person confirms on it that the app shows the pairing, so its side relies on that person checking the desktop.
- **Encoding.** Binary fields travel as lowercase hex: `commit` and `c_host` are 64 digits, `n_dev` and `n_host` 32.
- **On the device,** after `ok`:
  - It keeps the token, cert and name in RAM and asks the question. No, or 60 s without an answer, wipes them.
  - On Yes it writes one atomic NVS blob `{ver, token, daemon name, cert PEM}` and derives `fp` from the cert at boot.
  - It connects with wss, with the cert pinned via `cert_pem` and the common-name check skipped because the IP changes. Chain verification stays on.
- **On the daemon:** it stores only SHA-256(token), so a leaked `state.json` gives no session. It stores on the code match, before the person answers on the device, so a No leaves an unusable hash until Forget on the desktop removes it.
- **Device reactions:** `error{unauthorized}` wipes the blob and goes back to the daemon list. A TLS verify failure shows "wrong cert from daemon". The status screen always offers Forget pairing, behind a 2 s hold. If the blob won't erase, the device stays paired and shows the error.

### TLS: use it

| | wss with the pinned cert (chosen) | Plain ws with the token |
|---|---|---|
| Protects | The token and output from sniffers, sessions from hijack, presses from a rogue daemon | Nothing. Sniffing the Wi-Fi gives the token and every action |
| Cost | mbedTLS heap in PSRAM, measured on 4.1: 4.4 KB internal + 22.9 KB PSRAM steady, 9.8 + 26.4 KB at the handshake peak. rustls + rcgen on the daemon | None |

### Desktop

- The TLS key, `state.json` and `secrets.toml` are 0600 in a 0700 config dir. No keyring: Secret Service doesn't protect against same-user processes and may be locked at login.
- Set a strict webview CSP. Today it's `null`, and the webview can call `save_config` and `set_secret`.

### Shell runner

- The device supplies only a button id. Nothing device-supplied reaches argv, env, URL, headers or body.
- Spawn: `Command::new(argv[0]).args(&argv[1..])`, never through a shell. A user who wants a shell writes `["sh","-c",…]` in their own config. stdin is null.
- Timeout: 10 s by default, 600 s max. Each run gets its own process group. On timeout send `killpg(SIGTERM)`, then `SIGKILL` after 2 s, then reap. Never signal after the leader is reaped.
- Output: keep 64 KB per stream and drain the rest. The result text is the last 1 KB.
- Concurrency: one run per button ("already running" otherwise), and a global semaphore of 8 that includes tiles.
- The autostart environment may lack the interactive `PATH` or `SSH_AUTH_SOCK`. Document this.

### HTTP runner

- reqwest with redirects off, no proxy, a timeout and a 64 KB body cap.
- `headers` go out as written. `secret_headers` pull values from `secrets.toml`, and a header name appears once across both, ignoring case.
- Secret values never reach the UI or the logs. In the reply the device sees, exact copies are blanked.
- Secret headers need `https://` or a loopback host. Plain http to another machine needs `insecure_http = true` on the action, and going live with it logs a warning.

### Security gate

S2 runs unauthenticated. Its only action is a compiled-in `notify-send`. Nothing configurable runs until S3 has merged.

## 5. Config model

Location: `~/.config/dev.rackrot.watchdeck/`, holding `config.toml` (owned by the user), `state.json` (owned by the daemon), `secrets.toml`, `identity.{pem,key}` and `icons/`.

```toml
version = 1
[layout]
cols = 5          # 1..8
rows = 3          # 1..5

[[button]]
id = "plex"
pos = [0, 0]
label = "Plex"
bg = "#1e88e5"
fg = "#ffffff"
icon = "3f2a9c.png"
action = { kind = "shell", argv = ["ssh", "nas", "docker", "restart", "plex"], timeout_s = 30 }

[[button]]
id = "office-light"
pos = [1, 0]
label = "Office"
action = { kind = "http", method = "POST", url = "https://ha.lan:8123/api/services/light/toggle",
           body = '{"entity_id":"light.office"}', secret_headers = { Authorization = "ha" } }
tile = { every_s = 15, json_pointer = "/state",
         source = { kind = "http", method = "GET", url = "https://ha.lan:8123/api/states/light.office",
                    secret_headers = { Authorization = "ha" } } }
```

- **Tiles.**
  - Text is the first stdout line, the body, or the `json_pointer` value, capped at 64 chars.
  - State is `ok` on exit 0 or a 2xx response, otherwise `bad`.
  - Pollers run every `every_s` (at least 2 s) and only while a device is connected. They never overlap, and they poll again right after a press on the same button.
- **Validation** happens in Rust: unique ids, inside the grid, no overlaps, non-empty argv, sane timeouts, http(s) URLs, valid colors, ASCII labels, decodable icons, and secret names that exist. An invalid config is never saved or pushed.
- **Saving:**
  1. The UI edits a copy, then the user presses Save.
  2. Rust validates it and refuses if the file changed since load. The revision is the SHA-256 of the file text.
  3. Rust writes atomically, the watch channel fires, and the layout pushes.
  4. Hand edits apply on their own. The daemon polls the mtime, size and inode of `config.toml` and `secrets.toml` every 500 ms and reloads once a change holds for two polls. No watcher crate.
- **Icons.** The daemon shrinks each icon to fit a box of min(cell) × 0.55, at most 256 px, and never scales one up. It converts the result to ARGB8888. The id is the first 16 bytes of SHA-256(w, h, pixels). Rejected: on-device PNG decode, which costs more CPU and code for no RAM saving.
- **Device storage.** The layout and icons stream into PSRAM on every connect. NVS holds only the pairing blob. Rejected: a flash cache, because actions need the daemon anyway, cached tiles would mislead, and it needs a partition change.
- **Offline.** The device shows a status screen: Wi-Fi state, "searching" or "daemon offline, retry in N s", and Forget pairing. If the link drops mid-session, the grid greys out under a banner and presses are blocked.

## 6. Crates and components (checked 2026-10-10)

| Rust crate | Version | Note |
|---|---|---|
| tokio | 1.53 | Tauri's runtime |
| tokio-tungstenite | 0.30 | `WebSocketConfig` size caps |
| tokio-rustls / rustls | 0.26.6 / 0.23.45 | One crypto provider (aws-lc-rs), installed explicitly at start |
| rcgen | 0.14 | Self-signed P-256 |
| zbus | 5.19 | Feature `tokio`. Registers the service with avahi-daemon |
| reqwest | 0.13 | Feature `rustls` |
| serde / serde_json / toml | 1 / 1 / 1.1 | |
| image | 0.25, `png,jpeg,webp` | |
| sha2 / getrandom | 0.11 / 0.4 | |
| nix | 0.31, `signal` | `killpg` |
| tauri `tray-icon`; plugins autostart 2.7, single-instance 2.5, log 2.10, dialog 2.8 | | Need tauri ^2.12 |

Tauri on Linux:

- The tray has no tooltip or click events, so status goes in a disabled menu item.
- Close-to-tray is `CloseRequested` → `prevent_close` + `hide`.
- single-instance needs a D-Bus session bus.

| Firmware component | Version | Note |
|---|---|---|
| espressif/esp_websocket_client | 1.8.0 | Apache-2.0 (the shipped `LICENSE` settles the "Custom" metadata). Settled |
| espressif/cjson | ^1.7.19 | IDF 6.0 removed the built-in `json` component |
| espressif/esp_lcd_touch_gt911 | 1.2.1 | Address latching needs RST, INT and `driver_data` |
| espressif/mdns | 1.14.0 | Already used |

## 7. Slices

| # | Slice | Acceptance | Check |
|---|---|---|---|
| S1 ✅ | Touch: GT911, plus a 5×3 test grid that highlights and logs taps. Done 2026-10-10: answers at 0x5D, pins as assumed, no axis swap or mirror, 0 phantom taps in 5 min | Every cell, corners included, maps correctly. No phantom touches in 5 min. Pins and address confirmed | `fw:build`, serial log on device |
| S2 ✅ | Thin end-to-end, no auth. Done 2026-10-10: tap → notify-send → toast verified on hardware; board reconnects after a daemon restart (IP change untested). Write `site/src/protocol.md` first. Daemon in Tauri, mDNS, plain ws, hello → one hard-coded button → `notify-send` → result toast. Add `cargo test` to CI | Connects within 10 s. Survives a daemon restart and an IP change. Result shows within 1 s. Heap logged | Fake-device integration test, `avahi-browse`, device |
| S3 ✅ | **Security gate.** S3a: TLS interop spike (rcgen + rustls ↔ mbedTLS). S3a done 2026-10-10: wss with the pinned cert works on hardware, and 47820 is wss-only (results below). S3b: pairing on both sides, Forget pairing. S3b built 2026-10-10: paired on hardware, and every hello needs its token (results below). Security review done 2026-10-10 and its fixes applied (review fixes below). tcpdump passed. On hardware, a run covered Forget from both sides, Yes, a wrong code, right-then-No, the 60 s timeout and re-pair, and all passed. **S3 ✅ 2026-10-10** | SAS test vector passes. A wrong code stores nothing. The device pins nothing without the person's Yes. A bad or forgotten token gets `unauthorized`. Oversize messages close the link. Device rejects a regenerated cert. tcpdump: TLS only on 47820, no token on 47821 | `cargo test`, device, tcpdump, security review |
| S4 ✅ | Config and actions: `config.toml`, validation, shell and HTTP runners, secrets, live layout push, result modal. S4 built 2026-10-10: `cargo test` covers the acceptance list, and on hardware a live edit pushed with no reconnect while invalid edits pushed nothing. The user's tap check passed all six steps. Review and security audit done 2026-10-10 and their fixes applied (results below). **S4 ✅ 2026-10-10** | A timeout kills grandchildren. Output caps, exit codes, "already running". HTTP: secret headers, no redirects, caps. Live push needs no reconnect | `cargo test`, device, security review |
| S5 ✅ | Icons: import, render, hash, `icon_get`, PSRAM cache. S5 built 2026-10-10: `cargo test` covers the acceptance list, and on hardware the icons were requested, received, validated, cached and hit again after a reconnect. The visual check and the recheck after the review fixes both passed (results below). **S5 ✅ 2026-10-10** | Exact-bytes render test. An RGB + transparent quadrant icon shows correctly | `cargo test`, device |
| S6 ✅ | Tiles. **S6 ✅ 2026-10-11** after the user's visual check, the review and the audit fixes (below). S6 built 2026-10-10: `src/daemon/tiles.rs` polls each `tile` source while a device is connected and the sessions push the values as `tile` messages. `cargo test` covers the acceptance list on a paused clock, and on hardware updates flowed, polling stopped when the board dropped and resumed when it returned. The user's visual check passed on 2026-10-11 (results below) | Paused-clock tests: interval, no overlap, idle stop, poll after press | `cargo test`, device |
| S7 | Config UI. S7 built 2026-10-11, manual check pending (steps and results below). Dark control-room design. Grid editor with a device-accurate preview, button form, secrets page, save flow with push confirmation, repair of an invalid file, strict CSP. Design note from the S4 review: with an invalid `config.toml` at startup the revision was empty, so `save_config` could never repair the file; fixed below | A save reaches the device within 2 s. Field errors write nothing. Secrets are never returned. Strict CSP | `pnpm build`, `app:dev` |
| S8 | App shape: tray menu, hidden start, close-to-tray, autostart, single-instance, logging. Built 2026-10-10, manual check pending: `src/tray.rs` holds a tray with a disabled status line (`Connected: ids` or `Offline`, refreshed on daemon events), Open window, a Start at login checkbox (the autostart plugin, no Tauri command) and Quit. The window starts hidden and shows unless launched with `--hidden`, which the autostart entry passes. `CloseRequested` hides the window. A second launch focuses it. `tauri-plugin-log` replaces env_logger (log dir file, 1 MB x 3, plus stdout in debug, level info, `RUST_LOG` gone). No capability changes: all plugins are used from Rust only. Pair-from-tray is left out, since the dialog belongs to the window (S7) | Tray-only after login. Closing the window keeps the device connected. A second launch focuses the window | `app:build`, manual check |

Order: S1 → S2 → S3 → S4 → {S5, S6, S8} → S7. S8 can start any time after S2, but it shares `lib.rs` with S3 and S4.

### S3a results (2026-10-10)

- **Negotiated:** TLS 1.2 `ECDHE-ECDSA-AES256-GCM-SHA384` over X25519. The device runs mbedTLS 4.1.0 (IDF 6.1, esp_websocket_client 1.8.0). The daemon runs rustls 0.23.45 on aws-lc-rs through tokio-rustls 0.26.6, with an rcgen 0.14.10 cert.
- **Pinning:** the device trusts only the compiled-in cert (`cert_pem`, name check off) and connects only to instances whose TXT `fp` matches it.
- **Discovery pass:** `discover()` collects every address of every fp-matching instance (at most 10, the PTR cap), own subnet first, and the link tries them in order. A candidate that rejects the cert, times out or refuses the connection hands over to the next at once. The backoff starts only after all have failed.
  - The per-instance TXT and A follow-ups share a 10 s budget per pass, so junk instances can't hold the link task for long. The log says when it runs out.
  - Status when nothing connects: "daemon identity changed" if instances were seen and none had the pinned fp; "wrong cert from daemon" if a candidate matched the fp but presented another cert (any LAN peer can advertise the public fp); "offline" if no instance was seen.
- **Spoof check on hardware:** a second `_watchdeck._tcp` instance on 47821 advertising the real fp, backed by an `openssl s_server` with another cert, sorted ahead of the real daemon. The device rejected it ("presented a cert other than the pinned one") and connected to the real daemon on 47820 in the same pass.
- **Daemon hardening:** at most 64 concurrent connections, and the rest are dropped at accept. A TLS handshake failure logs at debug. A socket that sends nothing, or stalls mid-ClientHello, is closed 5 s after the accept. `identity.key` must be 0600 and owned by the config dir's owner, or the daemon refuses to start with a fix command. Identity creation holds an exclusive `identity.lock`, opens its temp files `create_new` (O_EXCL, so a planted symlink fails instead of being followed) and fsyncs the dir.
- **TLS 1.3** also works (`TLS13_AES_256_GCM_SHA384`, X25519) with `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y`. It costs ~3.8 KB more PSRAM, ~0.4 KB more internal heap and ~30 ms. It stays off (the IDF default). Recheck `cert_rejected()` in `link.c` before turning it on.
- **Device cost,** from `BEFORE_CONNECT` to `CONNECTED` (mbedTLS heap in PSRAM):

  | | Internal | PSRAM |
  |---|---|---|
  | Per session | 4.4 KB | 22.9 KB |
  | Handshake peak | 9.8 KB | 26.4 KB |

- **Timing:** TCP + TLS + WebSocket upgrade takes 330–340 ms. By the end of the handshake the websocket task has used ~2.5 KB of its 6 KB stack.
- **Negative checks on hardware:**
  - With a regenerated daemon identity, the device skips every instance on `fp`, never connects, and backs off showing "daemon identity changed". "offline" is for when no instance is seen at all.
  - An instance advertising the old `fp` fails verification (mbedTLS -0x2700; the daemon logs `UnknownCA`). The device shows "wrong cert from daemon, retry in N s" once every candidate has failed.
  - Repeated rejections don't leak. After a one-time 1.3 KB on the first attempt, free internal heap held at ~363.5 KB over the next 8 attempts (155 s).
- **Surprises:**
  - On a failed first handshake, esp-tls reports no verify flags (`tls_flags=0`). `link.c` detects the rejection from the mbedTLS error instead.
  - esp_websocket_client dispatches `ERROR` before `DISCONNECTED`, and only `DISCONNECTED` carries the TLS error.
  - The P4's ECDSA peripheral needs silicon v3.0+. On v1.3 the PSA driver logged an error every handshake before falling back to software, so `sdkconfig.defaults` turns `CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY` off.
  - esp-tls warns on every connect that `skip_common_name` also disables SNI. That's expected with a pinned cert.
  - The daemon's 5 s hello deadline always started at the TCP accept. What changed is that it now covers the TLS handshake as well.
  - esp_websocket_client 1.8.0 ships an Apache-2.0 `LICENSE`, which settles the §6 question.
  - aws-lc-sys builds with `cc` (no cmake or clang). `sha2` 0.11 sits beside a transitive 0.10.

### S3b results (2026-10-10)

- **Paired on hardware.** The daemon log shows one full pairing, which needs a person to read the code off the board and type it into the dialog. Nobody captured serial or watched the screens during it. Daemon log, in UTC:
  - 00:00:32: window open, TXT gains `pair=47821`
  - 00:00:42: `pair_req` from `wd-a0f26283602c`
  - 00:00:48: code matched, `state.json` written, TXT drops `pair`
  - 00:00:52: first authenticated `hello`
- **Persistence.** After a reset the board reloads the blob ("paired with devbox, pinned daemon fp e2004f4d7b1747f2") and reaches `welcome` 9.1 s after power-on, 316 ms of it TCP + TLS + WebSocket. It logs `tx: hello (token redacted)`.
- **Impostors on hardware.** Four `hello`s over wss each got `error{unauthorized}` and a close: the paired id with no token, a random token and a malformed one, and an unknown id. The paired session stayed up through all four.
- **Pairing port.** 47821 refuses connections while the dialog is closed, and `avahi-browse` shows only `v` and `fp`.
- **Seam removed.** The firmware no longer compiles a cert in. The pin comes only from the pairing blob.
- **Covered by `device_sim` only:** a wrong code, cancel, both timeouts, a bad or oversize attempt, one device's token opening another, Forget closing the live session, and an impostor failing to kick a live session. A regenerated cert against the stored pin wasn't retried on hardware; S3a ran the same pin path with the compiled-in cert.
- **tcpdump, passed 2026-10-10 18:23** (`sudo tcpdump -i enp11s0 -A 'host 192.168.0.56 and (port 47820 or port 47821)'`, run during a re-pair). The capture was taken before the device-side confirm step was added, which changes no wire message.
  - 47821 carried only `pair_offer` (cert, `c_host`), `pair_reveal`, and `pair_result`. The device's masked frames match `pair_req` with a single 64-hex commit (125 B) and `pair_nonce` (61 B) by size. No token appeared.
  - 47820 carried only TLS 1.2 records after the TCP handshake. That covers the hello and a press.
- **Choices made in S3b:**
  - Binary fields are lowercase hex rather than base64, so each value has one spelling and the C parser stays exact.
  - `confirm_pairing(code)` replaces `submit_pair_code`. The user gets one guess per window.
  - `forget_device` and a re-pair send the live session `error{unauthorized}` before closing it, so the device drops its pairing at once instead of retrying a dead token.
  - The daemon verifies the token and registers the session under one lock. A forget can't slip between the two, and a failed `hello` never touches the live session. An unknown id is compared against a dummy hash.
  - `name` in `pair_offer` is display-only and stays out of the code. The device pins the cert, not the name.
  - Forget pairing on the device doesn't tell the daemon, so the desktop lists the device until it's forgotten there too.
- **Device cost:** min free internal heap 312.8 KB after the first layout of a paired boot.

### S3b review fixes (2026-10-10)

- **Device-side confirm, the audit's blocker.** The device pinned whatever cert sent it `pair_result{ok:true}`, and anyone on the plain socket can send that. Now it asks the person whether the app says "Paired <device>" and saves only on Yes. No, 60 s without an answer, or a Wi-Fi drop wipes the token, cert and name from RAM.
- **Lost `unauthorized` under load.** When a session closed with presses still unread, the kernel sent a reset. The reset discarded the `error` frame that Nagle was still holding. Sessions now send a close, then read until the device closes too, for at most 2 s.
- **Presses behind a revoke.** The session loop polls its kick first (`biased`) and checks it again right before running a press, so a press queued behind a revoke never runs. A press already past that check can still start within microseconds of the revoke.
- **Pairing window race.** A window publishes its end state and withdraws its port before it frees the slot, so a new window's `Waiting` can't be overwritten by the old window's end.
- **Smaller fixes:**
  - A device id must be exactly `wd-` plus 12 lowercase hex digits, as the firmware writes it.
  - A store save counts once its rename lands. A failed dir fsync after that only warns.
  - A `pairing_state` command lets the dialog recover from a missed event or a reload, and the dialog listens before it opens the window.
  - The code prompt shows the device's IP.
  - A Forget on the device that can't erase NVS keeps the pairing and shows the error.
- **New tests:** in `device_sim`, pipelined presses then a forget, a valid message padded past 16 KB on each port, a re-pair cutting a live session with `unauthorized`, and a device speaking while the code is shown. A store unit test covers the fsync contract.
- **Run on hardware 2026-10-10, 01:20–01:25 UTC, all passed:** Forget from both sides, Yes, a wrong code, the right code then No, the 60 s timeout, and a re-pair.

### S4 results (2026-10-10)

- **Live push on hardware,** in UTC, against the dev daemon:
  - 02:14:25.86: `config.toml` saved with a changed label and a second button.
  - 02:14:26: the daemon logs `applied` and `pushing layout rev 2`.
  - By 02:14:27.10: the device logs `layout rev 2 applied: 5x3, 2 buttons`. Neither side logs a link change between rev 1 and rev 2.
- **Invalid edits on hardware:** a button moved outside the grid (02:14:49) and an unterminated string (02:15:04) each logged `not applied, keeping the live config` with the reason. The device logged nothing.
- **Restart:** a `tauri dev` rebuild restarted the daemon at 02:19:53. The device logged `link lost`, reconnected and applied the layout by 02:19:58.
- **Tap check on hardware, passed 2026-10-10:** the user ran all six steps with the test buttons: Print, Fail, Slow once (the timeout), Slow twice (already running, then the timeout), Hello, and closing a result by tap and by timer.
- **Tests:** 85 unit and 38 `device_sim` after the fixes below. They cover every validation rule with a negative case, a timeout killing a grandchild, a SIGTERM-ignoring group killed by SIGKILL, output caps, exit codes, already running, and HTTP against a local responder (secret header sent, redirect not followed, 64 KB cap, timeout). They also cover a live push with no reconnect, invalid configs never pushed, and a trace-level logger that never sees the secret value.
- **Dependencies:** reqwest 0.13.5 (`rustls` only), nix 0.31.3, toml 1.1.8. `cargo tree --target all` shows no ring, openssl or native-tls. reqwest uses the process's aws-lc-rs provider.
- **Device cost:** firmware 0x197570 bytes, 60% of the app partition free. The minimum free internal heap after the first layout was 310.4 KB (S3b: 312.8 KB).
- **Choices made in S4:**
  - The daemon polls the files. That avoids a watcher crate, and a change applies within about 1 s.
  - `secrets.rs` is its own module. Values leave it only as request headers. Errors name a secret or a line, never a value.
  - A config's revision is the SHA-256 of its file text, not its mtime. A save refuses a stale revision, and also refuses when the file on disk has edits that aren't live.
  - Shell actions run in `$HOME` with the daemon's environment. The autostart `PATH` caveat is in `site/src/config.md`.
  - HTTP actions use HTTP/1.1, ignore proxy settings, and check HTTPS against the system's roots.
  - A press beyond 8 running actions fails at once with `busy` rather than queueing.
  - `icon`, `tile`, and any other unknown key are rejected until S5 and S6 add them.
  - Sessions no longer wait on actions. A run outlives its session, and a result for a closed session is dropped.
  - `set_secret` reloads the config, so a config held back by a missing secret applies at once.
  - A save rewrites `config.toml` without comments and replaces a symlink with a regular file.
  - Deleting `config.toml` while the daemon runs keeps the last good config. An invalid file at startup serves an empty grid.
  - `test_action`, `import_icon` and the TS wrappers wait for S7 and S5. Action runs aren't on the event broadcast yet; they log at info.
  - `result` gained `summary`, and the toast became a modal that closes at a tap, after 4 s (ok) or after 10 s (failed).
- **Surprises:**
  - S2's `bg` of 1959605 was `#1de6b5`, not `#1e88e5`. The layout now carries 2001125, so the Hello button changes from teal to blue.
  - Opening the serial port didn't reset the board this time. An RTS pulse, as esptool sends, did.

### S4 review and audit fixes (2026-10-10)

The review and the security audit found no blockers. These fixes followed:

- **Plain headers (the user's decision):** a per-action `headers` table beside `secret_headers`. Names must be valid and values printable ASCII, so CR and LF are refused. A name appears once across both tables, ignoring case, which also catches `Authorization` and `authorization` together in `secret_headers`. `get_config` shows plain values.
- **Secret headers over plain http (audit, major):** refused unless the host is `localhost`, 127.0.0.0/8 or `::1`, or the action sets `insecure_http = true`. Going live with the opt-in logs a warning. The docs' example moved to `https://`.
- **Redaction:** every copy of every used secret becomes a span, and overlapping spans merge. A secret inside a longer one, or two that overlap, leaks nothing whatever their order. When the body is cut at 64 KB, a secret's start at the very end is blanked too. Secrets must be printable ASCII, so spans fall on byte boundaries. `set_secret` refuses any other value, and loading `secrets.toml` refuses a hand-edited one by name, so the config check catches it before any press. Encoded echoes still pass, and `config.md` says so.
- **Logs:** a press's button id prints through Debug, so an unknown id can't inject a newline or ANSI codes.
- **Errors:** config errors name `url` instead of quoting it. A TOML error gives its line, column and message without toml's copy of the line. serde's message can still quote a value of the wrong type.
- **Tauri IPC (checked, no change):** in tauri 2.12.2 and wry 0.57.0, IPC bodies, `set_secret`'s included, are recorded only into `tracing` spans behind tauri's `tracing` feature. That feature is off, no subscriber is installed, and the `tracing` crate lacks its `log` feature, so env_logger sees none at any level. Debug builds do open the webview inspector, which shows IPC traffic to someone at the keyboard. S8 must not turn on tauri's `tracing` feature, or `tauri-plugin-log` at trace for IPC, without filtering it.
- **Polling:** the stamp adds mode and owner, so the chmod that the `secrets.toml` error asks for applies without a restart. `reload` records the stamps it read under its lock, and treats a file that changed mid-read as unread. The poller compares against that record, so an edit made before the poller starts is no longer lost.
- **Shell:** a leader still alive 5 s after SIGKILL, such as one stuck in the kernel, is given up on with a warning and "timed out after N s". That frees its button and slot, and tokio reaps it later. A failed wait now kills the group before letting it go.
- **Busy:** a finished run frees its slot before its button, under the same lock, so a press can no longer get a spurious "busy".
- **New tests:**
  - every header rule, the http and loopback cases, and errors that keep out a planted `?token=`
  - a chmod-only change, an edit before the poller starts, and a save round-trip of HTTP actions with every field
  - redaction of overlaps and of a cut-off start, a secret straddling byte 65536 end to end, and a hand-edited secret that can't be a header
  - plain headers on the wire, a two-thread "busy" stress test, and the go-live warning in `device_sim`
  - Each was checked to fail on the old code. The SIGKILL deadline has no test, since a unit test can't make a process outlive SIGKILL.
- **CSP:** still `null` until S7, as tracked. `save_config` and `set_secret` raise its priority: a script injected into the webview could write shell actions into `config.toml`, which run as the user.

### S5 results (2026-10-10)

- **On hardware,** with the dev daemon and the board that S4 used, against a config where four buttons show three distinct images, two of the buttons sharing one. The log is from the board's serial port, with ticks in ms since boot. The first run used the build from before `clang-format` (a layout-only change), and the final build repeated the first connect and the restart:
  - First connect, final build: `layout names 3 icon(s): 0 cached, 3 to request` at 9.81 s, one `icon_get` with the three ids, then each icon logged `received, validated and cached` at 9.96, 9.98 and 10.01 s. That is 200 ms from the layout to all three held.
  - Daemon restart with the board left running, final build: `link lost`, then `welcome` at 55.67 s, `icon 2f6a6b05 from cache` and two more, and `3 cached, 0 to request`. The board sent no `icon_get`.
  - Live edits, with no reconnect (first build): a changed icon set pushed rev 2, 3 and 4. Rev 3 logged `1 cached, 1 to request, 1 dropped` and fetched only the new id. Rev 4 re-fetched the id that rev 3 had dropped.
  - Min free internal heap after the first layout: 307.4 KB (S4: 310.4 KB). The firmware is 0x198520 bytes, 60% of the app partition free.
- **Tests:** 103 unit and 44 `device_sim`.
  - protocol.rs: an icon frame matches a vector computed with Python's hashlib, the largest frame round-trips at the cap, and `decode` refuses a short frame, a bad magic, a zero, oversize or 65535-wide side, a length off by one either way, a wrong id, a flipped pixel, and the same pixels read as another shape.
  - icons.rs: the exact-bytes render test (red, green, blue and a transparent quadrant give B, G, R, A bytes, a hashed id and the frame layout), shrinking that keeps the shape and never scales up, no color bleeding from transparent pixels, PNG, JPEG and WebP accepted and GIF, SVG and text refused, a truncated file, a picture over 4096 pixels, the file name rule, a missing, junk, directory and oversize file, and `import`.
  - config.rs: a config waiting on its icon until the poller sees the file, and a save that names an icon that exists.
  - `device_sim`: the layout carries the hash, `icon_get` returns frames that decode with the device's checks, unknown ids get nothing, malformed and 41-id lists close the session, an icon edit pushes a layout and a corrupt file pushes nothing, and a binary frame from the device is refused.
  - The firmware has no test harness in the repo. Its icon parser, `icon_get` builder and layout parser were compiled on the host against 16 frames built by an independent Python writer (the Rust vector among them) under AddressSanitizer: all pass. That harness isn't committed.
- **Not run on hardware:** the device's refusal of a bad frame, since the daemon only sends good ones. The host run covers `proto_parse_icon`, but not the link's request matching or the cache cap.
- **Dependencies:** image 0.25.10 with `png`, `jpeg` and `webp` only (zune-jpeg, image-webp, png, moxcms). `cargo tree --target all` shows no ring, openssl, native-tls or rayon.
- **Choices made in S5:**
  - The live config now carries its rendered icons (`Live`, a `Config` plus an icon set), so the layout and the frames it names always come from one load. `Config::layout()` became `Live::layout()`.
  - `icons::load` runs after `Config::check` in `reload` and `save`, so only the file name rule is in `check`. A file that is missing or won't decode holds back the whole config.
  - A picture is shrunk to fit, never scaled up. The shrink runs through premultiplied alpha, because `image`'s resize assumes it and otherwise the color of transparent pixels bleeds into the edge.
  - The daemon takes the grid geometry from constants that mirror `ui.c` and `display.h` (1024×600, top 204, gap 8), not from the device's `hello`, because one layout serves every device.
  - Limits, picked here since the plan named none: a file of at most 2 MB, a picture of at most 4096×4096, and 128 MB of decoder memory.
  - `icon_get` answers from the session's own layout. An id it doesn't name gets nothing, and a malformed or 41-id list closes the session.
  - The device sends one `icon_get` per layout. It frees icons the new layout doesn't name, and keeps the rest across a reconnect. Frames are accepted only for ids it asked for, and the cache holds at most 40 icons and 8 MB.
  - The websocket handler waits up to 500 ms for queue room before it drops a frame, so a burst of icons slows the daemon instead of losing icons. Dropped icons stay missing until the next layout.
  - The poller also watches the `icons/` directory's stamp, so adding a file applies a config that was waiting for it. Overwriting a file in place doesn't change the directory, so that needs a save of `config.toml`.
  - `import_icon` copies an image into `icons/` under the first 12 hex digits of its SHA-256 and the extension of its format. The TS wrapper waits for S7.
  - The firmware cache lives in `icons.c`, not `link.c` as §1 first said, because `link.c` was already over 1100 lines.
- **Surprises:**
  - An RTS reset empties the cache, since PSRAM is cleared. The reconnect check restarts the daemon instead.
  - `tauri dev` restarts the daemon on any file change under `app/src-tauri/`, so the Rust edits stopped before the hardware run.
  - The `image` crate has no GIF encoder with these features, so the GIF refusal test uses a hand-written header.
- **Visual check:** the user passed all 4 items on 2026-10-10. Review and the security audit then found two majors and nine minors, fixed in the same tree (below). On the same day the user rechecked the label layout fix. Two-line labels on icon buttons fit on 5×3, and the long label ends in "..." without overflowing.
- **Fixes after review (2026-10-10):**
  - Icon plus a two-line label clipped on 5×3, and a one-line label on 8×5. The box is now what the cell has left after padding (4 px each side), the 2 px gap and the label: 2 lines of 35 px, or 1 where 2 would leave under 24 px. Boxes: 256, 110, 44, 46, 26 px for 1 to 5 rows. `ui.c` fixes the label's height and ends long labels in dots. `box_px` and `ui.c` mirror each other, and a test pins every grid.
  - WebP decode ignored the 128 MiB cap. `check_webp` walks the RIFF chunks (bounded) and reads the VP8, VP8L and ANMF headers before decoding. A crafted file that claims 16383² is refused.
  - The unauthenticated pairing socket refuses binary frames without allocating (`s_binary_ok`, true only for pinned wss).
  - Decode moved to the blocking pool, renders are cached by file stamp and box size, and the config lock is not held across a decode. The sync commands are async.
  - The poll stamp includes each watched icon file's mtime and size, so in-place overwrites reload and a half-copied file is retried.
  - `icon_get` dedupes ids, checks for a kick between frames and encodes each frame once.
  - The import log uses `{:?}`. `"icon": null` is refused by the firmware, as the doc says. JPEG EXIF orientation is applied. `icons.c` drops the LVGL image cache entry on free. The firmware logs the LVGL heap after each layout.

### S6 results (2026-10-10)

- **On hardware,** against the dev daemon with three test tiles (Clock, a shell `date` every 3 s; Flag, ok or bad by whether `/tmp/watchdeck-flag` exists, every 30 s; Load, `/proc/loadavg` every 5 s on a button with an icon). Daemon log times are UTC, serial ticks are ms since boot:
  - First connect: the daemon logged `polling 3 tile(s)` at 05:50:30, and the board received `layout`, then a `tile` for each of the three within 200 ms. Later updates arrived every 3 s (Clock) and 5 s (Load), and only when the value changed.
  - Polling stops with the board: a hard reset with EN held low from 05:51:17 for 50 s. The daemon dropped the silent session at 05:51:38 (`no tiles to poll, stopped`, `DeviceDisconnected`) and logged no poll for the 38 s until the board came back at 05:52:16. Clock would have logged about 12 values in that time. On the return `polling 3 tile(s)` logged and all three tiles polled at once.
  - A shorter reset (an RTS pulse) was noticed in 6 s, and polling resumed with the reconnect.
  - Min free internal heap after the first layout: 305.5 KB (S5: 307.4 KB). The firmware is 0x198e10 bytes, 60% of the app partition free. The LVGL heap held 16 KB of 63 KB with 9 buttons, 3 of them tiles.
- **Visual check, passed 2026-10-11:** the user saw all the tiles right, and Flag flipped within a second of a tap, which is the poll after a press on the board.
- **Not run on hardware:** a 4 or 5 row grid with an icon and a tile (label hidden), and a bad frame at the device's tile parser.
- **Tests:** 140 unit and 51 `device_sim` after the fixes below (130 and 50 at the first build; S5: 103 and 44).
  - `tiles.rs` on a paused clock with a stand-in source: the interval (nothing at 4.999 s, a poll at 5 s), tiles with their own intervals, a 12 s poll against a 5 s interval never overlapping (starts at 0, 17, 34, 51, never two in flight), no polling with no device, a poll cancelled and the values dropped when the last device leaves, polling again on return, a poke polling only that tile at once, a poke during a poll costing exactly one extra poll after it, a stale poke adding nothing, config edits starting, stopping and restarting only what changed, and dropping the `Tiles` stopping every poller. Mutating the idle check or the poke wake-up made 3 tests fail each.
  - Mapping: first line of output, bad shell polls showing their first line or the summary, bad HTTP polls showing the summary, JSON pointer hits and every miss, ASCII and 64-character cut. The real source runs a shell command, parses 4 KB of JSON that a 1 KB press text couldn't, skips a poll when no slot is free, and doesn't block a press of the same button.
  - config.rs: every rule for `tile` with a negative case (`every_s` at 0, 1, 2, 86400 and 86401, a pointer without `/`, each action rule through `tile.source`, unknown keys, missing keys, a secret over plain http), a save round trip, and the 40-button layout still under 16 KB with every tile on.
  - `device_sim`: a value reaching the device and only changes being sent, bad then ok, a press polling again in a day-long interval, polling stopping after the device leaves and starting after it returns, a revoked session getting only `unauthorized` and no polls after it, a new layout re-sending every tile, and a source edit with no layout push.
- **Dependencies:** none added. `serde_json::Value::pointer` reads the JSON pointer.
- **Choices made in S6:**
  - A tile polls on its own task, which polls, waits `every_s` and polls again, so a tile cannot overlap itself. The interval runs from the end of a poll, so a slow source stretches it. A manager task starts and stops the pollers as the live config and the connected count change.
  - Values go through a `watch` channel. A poller never waits on a session, and a session reads the latest values at its own pace and sends only the ones that differ from what it sent. Every session sends each tile after its layout, because a device rebuilds its grid and the tiles come back blank. The session sends from its own loop, so a stalled device blocks only its own session, for at most the 5 s write timeout, as for a result or a layout.
  - A tile that is changed or removed drops its value, and idle drops all of them. Each value also carries the tile definition it was read under, and a session sends it only while that is still the definition in the live config, so no device is shown the old source's reading after an edit applies.
  - The session counts live in the registry under its lock and publish on a `watch`, so the count cannot disagree with who is connected. A revoked or replaced session leaves the count at once, and the kick is checked between tile messages like it is between icon frames.
  - Tile polls use the runners and their caps, timeouts, process-group kill and redaction. They claim the key `tile:<id>`, which no button id can have, so a poll never makes a press "already running" and the other way round. They share the 8 slots, and a poll that finds none is skipped.
  - A press polls its tile again after the action ends, from the press task, so a run that outlives its session still does.
  - The runners read up to 64 KB for a poll against 1 KB for a press, because a JSON body is parsed whole. The text for a good poll is the first non-blank line of output, or the value at `json_pointer`, for a shell source as well as an HTTP one. A bad poll shows a shell source's first line (so a check can print `DOWN`) or else the summary, and an HTTP source's summary always.
  - The wire `state` is `ok` or `bad`. The device draws anything else as unknown, which is also how a tile looks before its first update. The config has no state color: a tile is grey until its first value, keeps the button's `bg` while good and turns red while bad.
  - `every_s` is 2 to 86400. `json_pointer` must start with `/`.
  - The firmware shrinks a tile button's label to one line of 24 pt with the value under it (32 pt, or 24 pt in cells under 80 px tall). With an icon on 4 or 5 rows there is room for the icon and the value only, so the label is hidden.
  - The layout carries `tile: true`, so adding or removing a tile redraws the grid.
- **Review and audit fixes (2026-10-11):**
  - **An edited tile skipped its immediate poll (major).** Dropping the old poller only aborted it. On the multi-thread runtime its future, which holds the tile's claim, was dropped later, so the replacement was refused its first poll and slept `every_s`. The manager now aborts the replaced pollers and awaits their handles before it clears values and spawns replacements. A multi-thread test with a claim-holding stand-in fails without the await.
  - **A JSON pointer decoded a redacted secret (major).** `redact` matches literally, so a reply that escaped a secret (`\/`, `\u0042`) kept it, and the pointer then decoded it into the real value for the device and the log. A poll now returns the raw output and the secrets its request used (`Ran`). The tile parses the raw output, picks, redacts the picked value, and only then makes it printable. Tests cover both escapes, a secret inside a longer value, and the plain echo. This also lets a pointer match non-ASCII keys, which the printable filter used to mangle.
  - **Stale value window.** Readings carry their tile definition, and a session withholds one that doesn't match the live config. The poll loop writes only when the reading changed.
  - **Poke cooldown.** A tile never starts a poll within 2 s (`MIN_EVERY_S`) of its last start, whether a press, a reconnect or a short interval asks. The last start times outlive the pollers, so reconnect cycling can't bypass it. An edit clears the tile's time and polls at once.
  - **Starvation.** After 3 polls in a row that couldn't start, the tile publishes `bad` with the text `stale`. That needs no protocol or firmware change.
  - **Slot fairness.** `Actions::start_tile` allows at most 6 tile polls at once (`MAX_TILE_RUNNING`), checked under the same lock as the claim, so presses always find 2 of the 8 slots.
  - **Tests that didn't discriminate.** The `device_sim` test for unchanged values now waits for a poll that finds the same value and listens for 500 ms. A new test with two tiles shows that one change sends one message. The label-and-source edit test uses a slow new source, so the old reading would show. A unit test asserts an unchanged reading doesn't wake the watch. Each check was mutated away to see its test fail: the await on replaced pollers, the poller's change check, the cooldown, the redaction after the pick, the tile cap, the stale count, the session's sent check, and the definition check.
  - **Pre-existing flaky test (S3b).** `assert_port_closed` asserted `ConnectionRefused` on a port the pairing listener had just released, and a parallel test could bind it first. It now asserts that the pairing window advertises no port and that nothing at the address answers a valid `pair_req` with a `pair_offer`. Ten full `device_sim` runs after the change: 10 of 10 passed (51 tests each).
  - Docs: a pointer over a shell source with chatty stderr (such as `curl`'s progress meter) finds the output isn't JSON, so use `curl -s`.
- **Surprises:**
  - `prek run --all-files` skips untracked files, so typos flagged a word in the new `tiles.rs` only under `--files`.
  - Opening the serial port reset the board this time.

### S7 results (2026-10-11)

- **Backend changes:**
  - `ConfigStatus.revision` is now the SHA-256 of the file as last read, live or not (empty if missing or unreadable). `save` has one rule: the revision must equal the file's hash now. That repairs a file that was invalid at startup (the window passes the revision `get_config` returns), and a hand edit made after load is still refused. A save over a file that isn't live keeps the old text as `config.toml.bak.<UTC YYYYMMDDTHHMMSSZ>` (0600), never over an existing one, newest 5 kept. `ConfigStatus.loaded` says whether any good config has loaded since start, so the window can tell an empty startup grid from a last good config. A deleted file is recreated by a save naming "".
  - `DaemonEvent::LayoutPushed(device)`, sent after a config change reaches a connected device as a new layout (not for the first layout of a session). `lib.rs` forwards it as `layout-pushed`. The device sends no acknowledgement, so "pushed" means the daemon wrote it.
  - `tile_values` command and `tile-values` event (`TileView`: button, tile definition, text, state), so the preview shows real tile values. The window ignores a reading whose tile definition differs from the draft's.
  - `tauri-plugin-dialog` 2.8 (plan §6), used from Rust only: `import_icon` opens the native file dialog itself (PNG, JPEG, WebP filter), takes no path from the window, and returns the icon name or none on cancel. No JS dialog package and no `dialog:allow-open` capability, since that would widen the asset scope. Tauri's `protocol-asset` feature lets the preview load icon files.
- **CSP:** `default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' asset: http://asset.localhost; connect-src 'self' ipc: http://ipc.localhost; object-src 'none'; base-uri 'none'`. `devCsp` adds `'unsafe-inline'` to script-src (the Vite React preamble) and style-src, and `ws://localhost:1420` to connect-src. Asset protocol scope: `$APPCONFIG/icons/*`. No capability was added. Running under the CSP in the webview is not yet seen.
- **Preview fidelity:** geometry (`preview.ts`), icon box (`box_px`, pinned by the same table as the Rust test), label line limits, tile layout and colors mirror `ui.c`. Line breaking and "..." (`lvtext.ts`) port LVGL 9's `lv_text_get_next_line` and the DOTS long mode with the firmware's break characters, using Montserrat 24 and 32 advance widths read from the LVGL font sources. Kerning is left out: about 1 px mean and 3 px at p95 per label (measured on 3000 sample labels). Glyph shapes are the system font. Not compared with LVGL running; the port follows its source.
- **Tests:** 22 TS tests (`pnpm test`, Node's runner, no new package; Vitest is not installed), 4 new config tests, and `device_sim` asserts `LayoutPushed`. Totals: 144 unit and 51 `device_sim`.
- **Review and audit fixes (2026-10-11):**
  - Icon import moved into Rust (above). Replacing a not-live file needs an in-page confirmation that names what is written (an empty grid if the file never loaded) and where the backup goes. A note by Save says comments are not kept, and `site/src/config.md` documents both.
  - Errors: Rust's `\u{..}` escapes in ids are read back. A message about a button the draft lacks goes to a general list, a refused save always shows a "Save refused" banner with every message, and out-of-grid or any buttons show as chips (marked when they have problems) when the grid size is invalid. An invalid header name shows under both header tables.
  - Duplicate header rows block a save with an inline error, and the forms start over from the draft after Revert or Reload.
  - The page listens before it reads, ignores the `config-status` event of its own save, and does not reload during a save.
  - Pairing resumes on the Devices tab. A hidden icon can show again. Tabs have `aria-controls` and arrow keys, and the hex input, Remove, Replace and Forget buttons are labelled.
  - `pnpm build` type-checks `tests/` (`tsconfig.test.json`, with a small ambient shim for the two Node modules), and CI runs `pnpm test`. The save and dirty logic is in `saveflow.ts`, with tests.
- **Gaps:** no secret delete command; no `list_icons` (the picker offers icons the config uses plus ones imported this session); no `test_action`; the device never acknowledges a layout; a config deleted while running is recreated only by a save.

## 8. Testing

- **Rust unit tests:**
  - protocol fixtures, shared with `site/src/protocol.md`
  - the SAS vector
  - config validation
  - runners against real processes
  - HTTP against a local tokio responder
  - icon bytes
  - tile timing on a paused clock
- **Rust integration:**
  - `tests/device_sim.rs` is a fake device: a tungstenite client plus a pinned test cert.
  - It drives pair → hello → layout → icons → press → result, plus every negative case.
  - It runs without mDNS, so CI needs no multicast.
- **Firmware:** CI builds it. Each firmware slice has a device-in-the-loop checklist with serial-log evidence.

## 9. Risks

| Risk | Mitigation |
|---|---|
| mbedTLS 4.1 ↔ rustls handshake fails | The S3a spike ran first and passed. TLS 1.2 ECDHE-ECDSA-AES256-GCM-SHA384 was negotiated |
| Internal heap pressure (TLS + LVGL + Hosted) | mbedTLS allocates in PSRAM. Log the min free internal heap in every slice |
| avahi-daemon also owns UDP 5353 | The daemon registers through avahi's D-Bus API, so avahi is the only responder |
| Desktop firewall blocks 47820/47821 | Document the ports. The tray status shows "no device" |
| Tray support varies by desktop environment | GNOME needs the AppIndicator extension |
| Docker/VPN addresses get advertised | The device's same-subnet preference. avahi answers on every interface, so `deny-interfaces` in `avahi-daemon.conf` is the host-side fix |
| GT911 pins or orientation wrong | S1 verifies, with `#define` knobs |
| Bug in the custom pairing protocol | Standard commit/reveal shape, a shared test vector, a security audit on S3 |
| Autostart env lacks PATH or the SSH agent | Document it. stderr shows in results |

Rollback: each slice is its own commit on `main` (see the status table), so `git revert` undoes one. Firmware rolls back by flashing the previous build, since the partition table doesn't change. Forget pairing wipes NVS.

## Open questions

| # | Question | Default |
|---|---|---|
| 1 | Pairing strength | Settled in S3b: commit/reveal SAS, built and paired on hardware. TOFU was simpler but open to an active MITM while pairing |
| 2 | UI design direction (look, references, palette, type) | Settled 2026-10-11 by the user: a dark control-room look (always dark, dense, monospace accents, status colors), a live device-accurate preview, palette left to Claude. Built in S7 |
| 3 | Desktop environment | KDE or another StatusNotifierItem tray. GNOME needs an extension |
| 4 | Desktop firewall active? | Yes, ufw on the dev desktop. The user added `ufw allow from 192.168.0.0/24 to any port 47820,47821 proto tcp`. The daemon can't add rules itself without root |
| 5 | HTTPS to self-signed services (e.g. Home Assistant) | Unsupported in v1. A per-action `insecure_tls` opt-in later |
| 6 | SVG icons | PNG, JPEG and WebP only. SVG via resvg later |
| 7 | Non-ASCII labels | Rejected; the built-in fonts lack the glyphs |
| 8 | Tile polling | Settled in S6: only while a device is connected, as built |
| 9 | Plain HTTP headers such as `Content-Type` | Settled in S4 by the user: a per-action `headers` table beside `secret_headers`. A name appears once across both, ignoring case |

## Backlog

- **Sentry error reporting.** Research, then plan as its own slice.
  - Firmware: [getsentry/sentry-micro](https://github.com/getsentry/sentry-micro). First check ESP-IDF v6.1 and ESP32-P4 support, maturity, footprint, and transport (it needs TLS to the Sentry host). If upstream falls short, forking it and adding what watchdeck needs is acceptable.
  - App: the Sentry SDK for the Rust daemon, and probably the React frontend.
  - Config: a DSN setting, off by default. On the desktop it goes in `config.toml`. Decide how the device gets its DSN, for example pushed by the daemon over the session or stored in NVS. Keep DSNs and any PII (device ids, IPs, labels) out of events unless the user opts in.
