# watchdeck v1: daemon, device protocol, screen config

Status: plan, 2026-10-10. S1 to S3a are built (see the slice table); S3b onward is not.

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
| `config.rs` | TOML schema, validation, atomic save (tmp + rename, 0600), refusal on a stale mtime |
| `store.rs` | `state.json` (device id → SHA-256(token)), `secrets.toml`, identity cert and key. Files 0600, dir 0700 |
| `tls.rs` | rcgen self-signed P-256 cert, rustls `ServerConfig` |
| `pairing.rs` | Pairing listener (open only while the pairing dialog is open) and the pairing state machine |
| `server.rs` | wss listener, per-session auth and dispatch, layout push on config change |
| `mdns.rs` | avahi-daemon registration over D-Bus (zbus) |
| `actions.rs` | Shell and HTTP runners |
| `tiles.rs` | Tile pollers |
| `icons.rs` | Decode, fit, ARGB8888, hash, cache |

- A `watch<Arc<Config>>` carries live config. A `broadcast<DaemonEvent>` carries connects, pairing requests and action runs.
- `lib.rs` builds Tauri, plugins and the tray, starts the daemon and mDNS, and forwards events to the UI and the tray status line.
- `commands.rs` exposes the Tauri commands:
  - config: `get_config`, `save_config`
  - devices and pairing: `list_devices`, `forget_device` (closes the live session too), `start_pairing`, `cancel_pairing`, `submit_pair_code`
  - secrets: `list_secrets`, `set_secret` (write-only)
  - tools: `test_action`, `import_icon`
- React UI in `app/src`: GridEditor, ButtonForm, PairDialog, Devices, Secrets, plus `api.ts` (typed invoke wrappers) and `config.ts` (types). Rust validation is the authority.

### Firmware: `firmware/main/`

| File | Owns |
|---|---|
| `main.c` | Boot order unchanged, then touch, ui, link |
| `display.c` | Panel and LVGL port. The hello labels move to `ui.c` |
| `touch.c` | GT911 via `esp_lcd_touch_gt911` + `lvgl_port_add_touch`, swap/mirror `#define` knobs |
| `ui.c` | Status screen, daemon list, pairing code, grid, result toast, tiles, Forget pairing |
| `proto.c` | cJSON parse/build, icon-frame parse. Pure functions |
| `pairing.c` | Pairing session, SAS, NVS blob |
| `link.c` | Discovery, backoff, websocket lifecycle, dispatch, icon cache |

- All LVGL calls run under `lvgl_port_lock`.
- The touch callback and the websocket event handler only post to the link task's queue. The link task does every send and every stop/destroy.
- `link.c` listens for `IP_EVENT` and `WIFI_EVENT` itself, so `wifi.c` doesn't change.

## 2. Discovery and connection

- **Advertising.** The daemon advertises `_watchdeck._tcp` on port 47820 (wss), with TXT `v=1`, `fp=<16 hex of SHA-256(cert PEM)>` and `pair=47821`. avahi follows IP changes and answers on every interface, including docker and virbr.
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
D→H hello    {"t":"hello","v":1,"device":"wd-a1b2c3","token":"<b64 32B>","fw":"0.2.0","screen":[1024,600]}
H→D welcome  {"t":"welcome","v":1,"name":"buck-desktop"}
H→D error    {"t":"error","code":"unauthorized|version|bad_request","msg":"…"}   then close
H→D layout   {"t":"layout","rev":7,"cols":5,"rows":3,"buttons":[
               {"id":"plex","col":0,"row":0,"label":"Plex","bg":1959605,"fg":16777215,
                "icon":"9f86d081884c7d659a2feaa0c55ad015","tile":false}]}
D→H icon_get {"t":"icon_get","ids":["9f86d081884c7d659a2feaa0c55ad015"]}
H→D <binary> "WDI1"(4) | id(16) | w u16le | h u16le | w*h*4 bytes ARGB8888 (LVGL order B,G,R,A)
D→H press    {"t":"press","btn":"plex","seq":17}
H→D result   {"t":"result","btn":"plex","seq":17,"ok":false,"code":3,"text":"<≤1 KB output tail>","ms":412}
H→D tile     {"t":"tile","btn":"nas-load","text":"0.42","state":"ok|bad|unknown"}
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

- **Why commit-then-reveal.** A man-in-the-middle sees the nonce only after committing its own inputs, so its best odds are 1 in 10^6. A single-round 6-digit code could be ground offline in milliseconds.
- **On the device,** after `ok`:
  - It writes one atomic NVS blob `{ver, fp, token, cert PEM}`.
  - It connects with wss, with the cert pinned via `cert_pem` and the common-name check skipped because the IP changes. Chain verification stays on.
- **On the daemon:** it stores only SHA-256(token), so a leaked `state.json` gives no session.
- **Device reactions:** `error{unauthorized}` wipes the blob. A TLS verify failure shows "daemon identity changed". The status screen always offers Forget pairing.

### TLS: use it

| | wss with the pinned cert (chosen) | Plain ws with the token |
|---|---|---|
| Protects | The token and output from sniffers, sessions from hijack, presses from a rogue daemon | Nothing. Sniffing the Wi-Fi gives the token and every action |
| Cost | mbedTLS heap in PSRAM, measured on 4.1: 4.4 KB internal + 22.9 KB PSRAM steady, 9.8 + 26.4 KB at the handshake peak. rustls + rcgen on the daemon | None |

### Desktop

- The TLS key, `state.json` and `secrets.toml` are 0600 in a 0700 config dir. No keyring: Secret Service doesn't protect against same-user processes and may be locked at login.
- Set a strict webview CSP. Today it's `null`, and the webview can call `save_config` and `test_action`.

### Shell runner

- The device supplies only a button id. Nothing device-supplied reaches argv, env, URL, headers or body.
- Spawn: `Command::new(argv[0]).args(&argv[1..])`, never through a shell. A user who wants a shell writes `["sh","-c",…]` in their own config. stdin is null.
- Timeout: 10 s by default, 600 s max. Each run gets its own process group. On timeout send `killpg(SIGTERM)`, then `SIGKILL` after 2 s, then reap. Never signal after the leader is reaped.
- Output: keep 64 KB per stream and drain the rest. The result text is the last 1 KB.
- Concurrency: one run per button ("already running" otherwise), and a global semaphore of 8 that includes tiles.
- The autostart environment may lack the interactive `PATH` or `SSH_AUTH_SOCK`. Document this.

### HTTP runner

- reqwest with redirects off, a timeout and a 64 KB body cap.
- `secret_headers` pull values from `secrets.toml`. Secret values never reach the UI, the device or the logs.

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
action = { kind = "http", method = "POST", url = "http://ha.lan:8123/api/services/light/toggle",
           body = '{"entity_id":"light.office"}', secret_headers = { Authorization = "ha" } }
tile = { every_s = 15, json_pointer = "/state",
         source = { kind = "http", method = "GET", url = "http://ha.lan:8123/api/states/light.office",
                    secret_headers = { Authorization = "ha" } } }
```

- **Tiles.**
  - Text is the first stdout line, the body, or the `json_pointer` value, capped at 64 chars.
  - State is `ok` on exit 0 or a 2xx response, otherwise `bad`.
  - Pollers run every `every_s` (at least 2 s) and only while a device is connected. They never overlap, and they poll again right after a press on the same button.
- **Validation** happens in Rust: unique ids, inside the grid, no overlaps, non-empty argv, sane timeouts, http(s) URLs, valid colors, ASCII labels, decodable icons, and secret names that exist. An invalid config is never saved or pushed.
- **Saving:**
  1. The UI edits a copy, then the user presses Save.
  2. Rust validates it and refuses if the file's mtime changed since load.
  3. Rust writes atomically, the watch channel fires, and the layout pushes.
  4. For hand edits, the tray has "Reload config". There is no file watcher.
- **Icons.** The daemon renders each icon into a box of min(cell) × 0.55, at most 256 px, as ARGB8888. The id is the first 16 bytes of SHA-256(w, h, pixels). Rejected: on-device PNG decode, which costs more CPU and code for no RAM saving.
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
| S3 | **Security gate.** S3a: TLS interop spike (rcgen + rustls ↔ mbedTLS). S3a done 2026-10-10: wss with the pinned cert works on hardware, and 47820 is wss-only (results below). S3b: pairing on both sides, Forget pairing | SAS test vector passes. A wrong code stores nothing. A bad or forgotten token gets `unauthorized`. Oversize messages close the link. Device rejects a regenerated cert. tcpdump: TLS only on 47820, no token on 47821 | `cargo test`, device, tcpdump, security review |
| S4 | Config and actions: `config.toml`, validation, shell and HTTP runners, secrets, live layout push, result modal | A timeout kills grandchildren. Output caps, exit codes, "already running". HTTP: secret headers, no redirects, caps. Live push needs no reconnect | `cargo test`, device, security review |
| S5 | Icons: import, render, hash, `icon_get`, PSRAM cache | Exact-bytes render test. An RGB + transparent quadrant icon shows correctly | `cargo test`, device |
| S6 | Tiles | Paused-clock tests: interval, no overlap, idle stop, poll after press | `cargo test`, device |
| S7 | Config UI (needs design direction first) | A save reaches the device within 2 s. Field errors write nothing. Secrets are never returned. Strict CSP | `pnpm build`, `app:dev` |
| S8 | App shape: tray menu, hidden start, close-to-tray, autostart, single-instance, logging | Tray-only after login. Closing the window keeps the device connected. A second launch focuses the window | `app:build`, manual check |

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
- **S3a seam:** the pinned cert compiles in from the gitignored `firmware/main/daemon_cert.pem.local`, a copy of the daemon's `identity.pem`. Without it the firmware builds but refuses to connect. Run `idf.py reconfigure` after creating it. S3b replaces it with the cert stored at pairing.

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

Rollback: each slice is its own PR. Firmware rolls back by flashing the previous build, since the partition table doesn't change. Forget pairing wipes NVS.

## Open questions

| # | Question | Default |
|---|---|---|
| 1 | Pairing strength | Commit/reveal SAS. TOFU is simpler but open to an active MITM while pairing |
| 2 | UI design direction (look, references, palette, type) | Needed before S7 |
| 3 | Desktop environment | KDE or another StatusNotifierItem tray. GNOME needs an extension |
| 4 | Desktop firewall active? | Ports TCP 47820 and 47821 |
| 5 | HTTPS to self-signed services (e.g. Home Assistant) | Unsupported in v1. A per-action `insecure_tls` opt-in later |
| 6 | SVG icons | PNG, JPEG and WebP only. SVG via resvg later |
| 7 | Non-ASCII labels | Rejected; the built-in fonts lack the glyphs |
| 8 | Tile polling | Only while a device is connected |
