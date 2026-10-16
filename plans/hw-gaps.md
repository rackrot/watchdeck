# Hardware gaps: bad frames at the device parsers, desktop IP change

Closes the two items in `plans/desktop-app-v1.md:20`.

## Goal and done

- The icon, tile and layout parsers have host unit tests that run in CI.
- A fault harness has sent each malformed case below to the real board. Each result is recorded against what the spec expects.
- No case crashes, overruns, leaks or wedges the board. The one exception is the live-peer stall (F1), unless the user chooses the vendored fix.
- The board has recovered from a desktop IP change on hardware in both phases of the procedure below, without a reboot. The measured times are recorded.
- `plans/desktop-app-v1.md:20` is updated.

## Assumptions

- ESP-IDF v6.1's linux target builds `log`, `mbedtls` (PSA) and `unity` ("Running ESP-IDF Applications on Host", component table). The managed `espressif/cjson` is plain C. S1's first build confirms this.
- The `espressif/idf:v6.1` container has a native gcc. If it doesn't, the CI step installs `gcc` with apt. UNCONFIRMED.
- The host libc provides `strlcpy` (glibc 2.38+). If it doesn't, the test defines its own; `proto.c` doesn't change. UNCONFIRMED.
- The ufw rule for 47820/47821 matches on source `192.168.0.0/24`, not on a destination address. The operator checks `sudo ufw status numbered` first.

## Findings

| # | Where | What | Severity |
|---|---|---|---|
| F1 | `esp_websocket_client.c:1076-1111` (v1.8.0, same on upstream main), `:1387`, `:553`; `link.c:558` | A peer that sends a frame header and part of its body, then goes quiet, keeps the websocket task looping with the client lock held. Pings stop. The next `drop_client` (a press whose send fails, Wi-Fi down, pairing timeout) blocks the link task forever. A desktop that vanishes mid-transfer causes it, and so does a hostile peer on the plain pairing socket | High |
| F2 | `ui.c:381`, `ui.c:202-219`, `ui.c:581,593`, `ui.c:704`, daemon list names; `pairing.c:197` | External text reaches LVGL without `copy_printable`. `get_str` (`proto.c:159`) can cut a multibyte character and leave its lead byte. `lv_text_utf8_get_byte_id` (`lv_text.c:836-839`) then steps by the size that lead byte claims and can pass the NUL. Whether this over-reads or corrupts memory is UNCONFIRMED | Medium |
| F3 | `link.c:1044-1047`, `1165` | `ST_UP` has no deadline for `welcome`. A pinned peer that never sends it holds the board indefinitely | Low |
| F4 | `link.c:181` | FIN is never checked. A first fragment is treated as a whole message, and continuations are dropped. Validation still applies, so the risk is wrong parses, not memory | Low |
| F5 | `link.c:197` | `EV_WS_BAD` for an oversize frame uses `post` with no wait. With a full queue the close is lost, and the frame is skipped instead of closing the link | Low |
| — | `proto.c:182-190`, `proto.c:322` | `get_int` accepts fractions (`rev` 1.5 becomes 1). Trailing bytes after the JSON value are accepted. `\u0000` cuts a string at the NUL. All of these are memory-safe. The tests pin this behavior, and the policy stays as is | Note |

## Approach

- **CI part.** Write host (linux-target) Unity tests of `proto.c`, which is pure apart from cJSON, `esp_log` and PSA SHA-256. The test project compiles `../../main/proto.c` directly, so production code isn't refactored.
- **Hardware part.** Write a Rust example, `fault_daemon`, that stands in for the daemon. It uses the crate's public `tls::Identity`, `store::Store` and `mdns::advertise`, and runs scripted cases against the paired board. `cargo test` builds examples, so CI keeps it compiling.
- **Run order.** Run the harness on the current firmware first, then fix F1 (keepalive), F2 to F5, and run it again.
- **IP change.** Add a secondary address and use an nft reject rule. The primary address and routes are never touched.

### Rejected

| Option | Why not |
|---|---|
| On-device Unity test app | Needs flashing, can't run in CI, and repeats the host tests |
| Debug fault hook in the production daemon | Widens production code just for tests |
| Python fake daemon | Adds a toolchain, and would handle `identity.key` outside the crate |
| Moving `proto.c` into its own component | A refactor nobody needs. Compiling the file directly is enough |
| DHCP release/renew, or moving the primary IP | Disrupts the desktop's network |
| macvlan plus a temporary avahi `allow-interfaces` edit | Edits system config |

## Slices

| Slice | Agent | Depends on |
|---|---|---|
| S1 Host parser tests in CI | coder | — |
| S2 Fault harness | coder-high | — |
| S3 Baseline run on current firmware | coder + operator | S2 |
| S4 UI text sanitising (F2) | coder | S3 |
| S5 Link hardening (F1 keepalive, F3, F4, F5) | coder-high | S3 |
| S6 Rerun the harness, IP-change procedure | operator + coder | S4, S5 |
| S7 Upstream issue, plan and doc updates | scribe | S6 |

### S1 Host parser tests in CI

- **Files**
  - `firmware/test/host_proto/CMakeLists.txt`
  - `firmware/test/host_proto/sdkconfig.defaults` (`CONFIG_CJSON_NESTING_LIMIT=8`)
  - `firmware/test/host_proto/main/CMakeLists.txt` (compiles `test_proto.c` and `../../../main/proto.c`; `PRIV_REQUIRES unity mbedtls log`)
  - `firmware/test/host_proto/main/idf_component.yml` (`espressif/cjson: "^1.7.19"`)
  - `firmware/test/host_proto/main/test_proto.c`
  - `mise.toml` (task `fw:test`)
  - `.github/workflows/ci.yml` (a step in the `firmware` job)
- **`app_main` behavior**
  - Calls `psa_crypto_init()`.
  - Runs Unity.
  - Calls `exit()` with the failure count, because returning from `app_main` on linux doesn't end the process.
- **Icon cases**
  - Lengths 0, 23 and 24
  - Bad magic
  - w or h of 0 or 257
  - Length one byte over and one byte under
  - One pixel byte flipped (hash mismatch)
  - Valid 1x1 and 256x256 frames
- **Tile cases**
  - `btn` missing, 63 characters (accepted), 64 characters (refused)
  - `text` as a number, longer than 64 (cut to 64), empty
  - `state` absent, `"weird"` or a number (all unknown)
  - `"a\u0000x"` as `btn` (becomes `a`)
- **General cases**
  - Unknown `t`, `t` that isn't a string, root that isn't an object
  - Truncated JSON, trailing garbage (accepted), two objects back to back
- **Layout cases**
  - `cols` 0 or 9, `rows` 6, 41 buttons
  - `col` of `cols`
  - `icon` as `null`, 31 hex digits, uppercase hex
  - `tile` as a string
  - `rev` of `1e400` or -1
  - Nesting depth 9
  - A label of 46 `A` plus `é`: assert that the stored label ends in a lone `0xC3`, which documents F2
- **CI step**: `. $IDF_PATH/export.sh && cd firmware/test/host_proto && idf.py --preview set-target linux build && timeout 60 build/host_proto.elf`
- **Acceptance**
  - The first build succeeds, and a deliberately failing case makes the process exit non-zero (check the exit status).
  - CI is green.
- **Cheapest check**: `mise run fw:test`

### S2 Fault harness

- **Files**: `app/src-tauri/examples/fault_daemon.rs`, and `mise.toml` (task `app:fault`).
- **Safety: refuse to start unless all of these hold**
  - `identity.pem` and `identity.key` both exist. The harness checks this before calling `load_or_create`, so it never creates a new identity.
  - `state.json` exists and holds `--device <id>`.
  - Port 47820 is free. That port is the daemon's lock, so this proves the tray app has quit.
- **Safety while running**
  - Never send `error`. On any auth mismatch, close and abort, because `unauthorized` makes the board erase its pairing (`link.c:984-988`).
  - Never print the token.
- **Flow**
  1. Advertise with the real `fp`.
  2. Accept wss and check the hello with `Store::verify`.
  3. Send `welcome` and the base layout L0: one tile button `t1`, one icon button with icon `X`, one plain button.
  4. Serve `icon_get`.
  5. Run the cases.
- **Markers**: before each case, send `{"t":"mark","case":"<id>"}`. The board logs it on its `rx:` line (`link.c:969`), which ties each case to the serial log.
- **Checks on the board's state**
  - Icons: send the bad frame, then the correct frame for the same requested id without another layout, then L0 again with the same content. Expect no `icon_get`, which proves X is cached and the `want` list is intact.
  - Liveness after each case: send a layout naming a fresh id `Y`, and expect `icon_get` for exactly `[Y]` within 5 s.
  - Tiles: the board sends nothing back, so check its serial log for the marker plus the expected line.
- **Raw frames**: use tungstenite `Message::Frame` for fragments and invalid UTF-8. For the stall case, write bytes straight to the TLS stream through `get_mut()`. Both API details are UNCONFIRMED.
- **Serial capture**: use pyserial from the IDF Python env, writing to a file. `--check <log>` grades each case from that file.
- **Leak check**
  1. Warm up.
  2. Apply L0 with X cached, close, and record `heap before connect` (internal and PSRAM).
  3. Run three rounds of every case that keeps the link up, 20 times each.
  4. Re-apply L0, close, and record again.
  5. Pass if neither number keeps falling from round to round. Each sample comes after the queue has drained.
- **Pairing mode**: run only if the user agrees (open question 3). It needs Forget pairing on the board.
  - Advertise `pair=<port>` on plain ws.
  - Cases: a 47-byte name ending in a cut multibyte character, oversize text, a binary frame, a stall mid-frame, a forged `pair_result` ok.
  - After the user taps No on the forged result, the board must still be unpaired after a reboot.
- **Acceptance**: `cargo build --example fault_daemon` succeeds, clippy is clean, and the startup guards are unit-tested where they're plain functions.

### Cases, with expectations on the current firmware

| Id | Send | Expect |
|---|---|---|
| I1 | Binary frame of 10 bytes | Dropped, "shorter than its header", link stays up |
| I2 | Bad magic | Dropped |
| I3 / I4 | w = 0 / w = 257 | Dropped, "outside 1..256" |
| I5 / I6 | Length + 1 / length − 1 | Dropped, "should be N bytes" |
| I7 | Hash mismatch | Dropped, "does not match its pixels" |
| I8 | Valid frame for an unrequested id | Dropped, "was not requested" |
| I9 | Valid frame for a cached id | Ignored, "already cached" |
| I10 | Binary frame of 262,169 bytes | Link closes, new hello within 20 s |
| I11 | 20 unrequested frames back to back, then I10 | Today the close may be lost (F5). After S5 the link closes |
| I12 | First fragment (FIN = 0) holding the full valid frame for X | Today X is cached (F4). After S5 it's dropped |
| I13 | Header claiming 262,168 bytes, 1,000 bytes sent, then a 60 s pause, during which the operator presses a button | Today the link task wedges (F1), so reboot with an RTS pulse. After S5 a live peer still wedges it (fixed only by the vendored option) |
| I13b | Same as I13, then an nft drop of all board ↔ harness traffic | After S5, keepalive aborts within about 20 s and the board reconnects once the rule is removed |
| T1 | `tile` for an unknown button | "no such tile button" |
| T2 | `tile` for a button that isn't a tile | Ignored |
| T3 | `text` as a number | "dropped unparsable message", the previous value stays |
| T4 | `text` of 200 characters | Shows the first 64 |
| T5 | `state` absent, `"weird"`, or 5 | Grey |
| T6 | `btn` of 64 characters | Dropped |
| T7 | Non-ASCII `text` | `?` on screen |
| T8 | `tile` before the first layout | Ignored |
| T9 | Trailing garbage after a valid tile | Applied (pinned behavior) |
| L1 | Label of 46 `A` plus `é` | Today: watch for a crash or garbage (F2). After S4: `?` |
| L2 | Raw text frame whose label ends in byte `0xF0` | Same as L1 |
| L3 | Non-ASCII `welcome` name of 47 bytes | Same as L1, on the status line |
| L4 | No `welcome` for 60 s after the hello | Today the board waits forever (F3). After S5 it retries |

### S3 Baseline run

- The operator quits the tray app and starts the serial capture. The coder runs the harness with all cases except pairing mode.
- I13 wedges the board, so reboot it with an RTS pulse afterward.
- **Acceptance**: a results table filled in against the table above, with F1 to F5 confirmed or refuted on hardware.

### S4 UI text sanitising (F2)

- **File**: `firmware/main/ui.c`.
- Run `copy_printable` at every place external text reaches LVGL:
  - the label and `s_labels` in `build_grid`
  - the result title from `button_name`
  - `ui_set_daemon_text`
  - the pairing code and confirm views
  - the names in the daemon list
- **Acceptance**: `mise run fw:build` passes, and L1 to L3 show `?` on hardware.

### S5 Link hardening

- **File**: `firmware/main/link.c`.
- **F1**: add `keep_alive_enable`, `keep_alive_idle = 5`, `keep_alive_interval = 5`, `keep_alive_count = 3` to `open_client`. The fields exist in `esp_websocket_client.h:154`, and `LWIP_TCP_KEEPALIVE = 1`.
- **F3**: set `connect_deadline_ms` in `ST_UP` until `welcome` arrives, and check it in `link_task` with `candidate_failed("no welcome")`.
- **F4**: drop any frame where `!d->fin`. The `fin` field is in `esp_websocket_client.h:87`.
- **F5**: change `post(&bad)` to `post_wait(&bad, pdMS_TO_TICKS(RX_POST_WAIT_MS))`.
- **Acceptance**: `fw:build` passes, and on the rerun I11, I12, I13b and L4 match their after-S5 expectations.

### S6 Rerun and IP change

- Rerun the harness on the new firmware.
- Restart the tray app and run the IP-change procedure below.
- **Acceptance**: every case matches its expectation, and both IP phases pass within their targets, with measured times recorded.

### S7 Docs

- File an upstream issue with esp-protocols (`esp_websocket_client_recv` loops mid-frame without checking `run` or a stall limit).
- Update `plans/desktop-app-v1.md:20`.
- Add a line to `CLAUDE.md` Hardware facts about F1.
- In `site/src/protocol.md`, note that the device drops fragmented frames.

## IP-change procedure

The primary address `192.168.0.80` and its routes are never touched. Nothing below survives a reboot.

### Before you start

- The tray app is running, and the board is paired and connected.
- Start the serial capture.
- Find the board's IP with `ss -tn state established '( sport = :47820 )'`.
- Run `sudo ufw status numbered` and confirm the 47820 rule matches on source only.
- Pick a free address `NEW` outside the DHCP pool (default `192.168.0.250`), and confirm `ping -c2 -W1 NEW` gets no reply.

### Phase A: failover by reset

1. Run `sudo ip addr add NEW/24 dev enp11s0`.
2. Install the reject rule:
   - `sudo nft add table inet wdtest`
   - `sudo nft add chain inet wdtest in '{ type filter hook input priority -10; }'`
   - `sudo nft add rule inet wdtest in ip saddr BOARD ip daddr 192.168.0.80 tcp dport 47820 counter reject with tcp reset`
3. Within about 10 s the board's next ping gets a reset. Check all of these:
   - The board log shows `candidate … at NEW` (`link.c:430`), then `connecting to NEW` (`link.c:628`), then `connected to`.
   - `ss` shows local address `NEW:47820`.
   - `sudo nft list table inet wdtest` shows a counter above 0.
   - A press shows its result.
4. The target is 30 s from adding the rule.

### Phase B: the address the session uses goes away

1. Make sure the board is idle: the layout is applied, no `icon_get` is outstanding, and at least 15 s have passed since it connected.
2. Run `sudo nft delete table inet wdtest`.
3. Run `sudo ip addr del NEW/24 dev enp11s0`. Record the time.
4. Within about 35 s the board detects the loss through its ping timeout. Expect "link lost", then discovery, then `connecting to 192.168.0.80`. A stale `NEW` candidate may fail first, which costs up to 5 s.
5. Check that `ss` shows `192.168.0.80:47820`, a press works, and tiles update.
6. The target is 90 s from deleting the address. The worst case is about 60 s: 35 s to detect, 1.2 s of backoff, 13 s of discovery, 5 s per stale candidate, plus the TLS handshake.

Report Phase A and Phase B separately. Phase B mid-transfer is harness case I13b; don't run it with the real daemon.

### Rollback

- `sudo nft delete table inet wdtest`
- `sudo ip addr del NEW/24 dev enp11s0`

## Risks

| Risk | Mitigation |
|---|---|
| The harness makes the board erase its pairing | It never sends `error`, and it checks `state.json` before advertising |
| I13 wedges the board | Expected. Reboot with an RTS pulse |
| PSA hashing or `strlcpy` fails on the linux target | S1's first acceptance step catches it. Fallbacks are in Assumptions |
| A Phase B result is really F1 | Phase B requires the board to be idle |
| Keepalive misses a peer that is alive but stalled | That's open question 2 |

## Open questions

Settled 2026-10-15 by the user: (1) `192.168.0.250`; (2) keepalive only, no vendored patch and no upstream issue, since the stall needs an unauthenticated peer on the pairing socket and a vanished desktop is covered by keepalive; (3) run pairing mode once; (4) the baseline may wedge the board and reboot it with an RTS pulse. S7 drops the upstream issue.

1. **Spare address for the IP-change test.** `192.168.0.250` if it's outside the router's DHCP pool (recommended), otherwise another free address outside the pool. Check with `ping -c2 -W1` first.
2. **F1 fix.**
   - (a, recommended) TCP keepalive plus an upstream issue. Covers a desktop that vanishes mid-transfer (dropped in about 20 s). A live hostile peer on the pairing socket can still wedge the board while pairing is open; a reboot clears it.
   - (b) Keepalive plus a vendored, patched `esp_websocket_client` whose receive loop gives up on a stall or when `run` clears. Closes every case, at the cost of a fork to keep in sync.
   - (c) Upstream issue only.
3. **Harness pairing mode.** Run it once (recommended): about 2 minutes of the user's time to hold Forget pairing, tap No on the forged result, and pair again. Or skip it and leave the pairing path untested on hardware.
4. **May the baseline run reboot the board?** Yes, with an RTS pulse (recommended): no pairing or config is lost, only the icon cache. Or skip I13 and leave F1 unconfirmed on hardware.
