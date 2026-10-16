# watchdeck: Sentry error reporting

## Status (2026-10-16)

S1 to S4 are built, reviewed and merged (S4 at 917a5ec, not yet flashed). The live-DSN checks are still to do, and they need the user's Sentry project.

S5 is on hold behind a live experiment, decided by the user 2026-10-16. Sentry's `symbolic` library has no RISC-V `Arch` (`EM_RISCV` maps to `Unknown`; `symbolic-debuginfo/src/elf.rs`, `symbolic-common/src/types.rs` on master, checked 2026-10-16), so resolving a P4 PC in Sentry is UNCONFIRMED. The experiment: upload one build-id-stamped `watchdeck.elf` with `sentry-cli debug-files upload`, send one hand-made event whose `debug_meta` names that build-id with `arch` omitted, and check whether a known PC resolves to file:line. Build S5 only if it does; otherwise drop it and keep `addr2line` as the way to read PCs. Decisions settled 2026-10-15 by the user: 1(a) device reports via the daemon, 2(b) `tauri-plugin-sentry` for the webview, 3(a), 4(a), 5(b) build S5 symbolication, 6(a). The `sentry` crate is approved. Build S1 to S5 in order.

### S1 results

Done: daemon SDK, config, scrubber, budget, log bridge and panic hook (`daemon/reporting.rs`, `[sentry]` in `Config`). Not done: the live-DSN check (a real `error!` and a release-build panic arriving scrubbed).

- The log plugin now registers in `setup` through `split`, first thing, so the Sentry wrapper is in place before any app log line. With no client bound the wrapper only logs.
- Log lines written before `setup` registers the log plugin are lost. That is accepted.
- `Warn` breadcrumbs follow a process-wide flag that `start` sets from `pii`.
- The scrubber drops the event if `secrets.toml` can't be read, since the values can't be removed.
- The key cap evicts the oldest key. The 30 per hour cap makes 256 keys unreachable today, so `Budget::with_limits` exists for the test. `Budget::admit` takes a TTL, which is where S3's per-device hourly cap plugs in.
- `Reporting::client()` hands S2 the same `Arc<Client>` for `tauri_plugin_sentry::init_with_options`.

### S2 results

Done: `tauri-plugin-sentry =0.8.0` (no default features), the `main` window created at the end of `setup`, `sentry:default` on the window's capability, the `component=ui` tag and `request` drop in `Filter::process`, a `TypedOnly` transport wrapper, and `window.Sentry?.captureException` at the three catch sites. Not done: the live check (a throw from devtools in a release build with a real DSN, under the unchanged CSP, so the CSP point above stays UNCONFIRMED).

- The window is built right after the plugin registers, then shown unless `--hidden`. Tray and single-instance callbacks only run once the event loop starts, after `setup` returns, so `show_window` always finds it. A second launch during `setup` would hit its existing "no main window" log line.
- The transport wrapper passes only `Event` and `ClientReport` items. It cuts attachments out, since the plugin's `envelope` command lets the page send an unscrubbed one. Raw envelopes, empty ones, and any with another item (transactions, sessions) are dropped.
- After tagging `component=ui`, `Filter::process` sets `platform` to `other`, because Sentry may infer the client IP for `javascript`.
- `tauri.conf.json` gives the window `"label": "main"` explicitly, since `tray.rs` looks it up by that label.

### S3 results

Done: `report` and `report_ack` (`daemon/protocol.rs`, `site/src/protocol.md`, `tests/fixtures/report*.json`), the session handler (`daemon/server.rs`), and `Reporting::device_report` (`daemon/reporting.rs`). Not done: the live-DSN check (a real report arriving as an event).

- `Server::bind` takes an `Option<Reporting>`, and `Reporting` is `Clone`. `lib.rs` hands the same one to the server and to Tauri state.
- A report parses leniently (missing fields become empty, and a mistyped one becomes an empty report that keeps its `id`) and `Report::check` validates it. Every report counts toward the session's two. One with a valid `id` that fails is logged and acked so the device erases it. One without a valid `id`, or past the second, is dropped unacked. The session always stays up.
- A reset's crash fields are cleared by `Report::normalized` before the event and the cap key are built. The exception type is scrubbed like the rest.
- `crash` requires `pc`, `ra`, `cause`, `addr` and `elf`. `reset` ignores them. S4 must send all of them for a crash. `task` and `cause` are also limited to printable ASCII, so they can't forge log lines.
- The per-device cap is `Budget::admit(device|elf|pc|reason, 1 h)` in `device_report`, against a device-report budget of 10 per hour across all devices that is apart from the shared 30, so a crash loop can't silence daemon and UI errors. `Filter::process` skips the 10-minute dedupe for `component=firmware` events so a slot isn't taken twice.
- The client fills in `release` and the daemon host's contexts on every event. `Filter::process` removes both from firmware events, keeping only a `watchdeck-firmware@<fw>` release. A `fw` outside `[A-Za-z0-9._+-]{1,64}` gets no release.
- The device id goes in `user.id`, only with `pii`.
- The frames are `[ra, pc]`, since Sentry reads the last frame as the crash site.
- `device_sim` with Sentry on uses a loopback DSN with nothing listening. It shows the ack doesn't depend on the budget, not that an event arrives.

## Goal and definition of done

- Sentry reports are opt-in. They cover daemon errors and panics, webview errors, and board crashes and abnormal resets.
- It's off by default. With no `[sentry]` table in `config.toml`, the app makes no Sentry client and no Sentry traffic, even when `SENTRY_DSN` is set in the environment.
- Personal data stays out by default. The defaults keep hostnames, home paths, IPs, device ids, button ids and labels, command output and secret values out of events. `pii = true` turns that filtering off.
- The work is done when all of these hold:
  - `cargo test` covers the config rules, the scrubber, the throttle, and how a device report becomes an event.
  - Manually, with a real DSN, one event of each kind arrives scrubbed: a daemon `error!`, a release-build panic, a webview error, and a board crash.
  - The board's report survives an older daemon, a dropped connection, and Forget pairing.
  - CI is green.

## Open questions

1. **Firmware path.**
   - (a, recommended) The device sends a crash summary and the daemon reports it. No DSN, TLS or SDK on the device.
   - (b) Fork sentry-micro and have the daemon relay its envelopes. You get its native event shape, metrics and logs, at the cost of ~15 vendored files, the DSN on the device, and the daemon parsing envelopes to enforce privacy.
   - (c) The device reports straight to Sentry. That adds a second TLS stack, a CA bundle and internet egress.
2. **Webview errors.**
   - (a, recommended) Route them through Rust with message-only events and no new dependencies.
   - (b) `tauri-plugin-sentry`, which gives breadcrumbs and browser context but adds a capability and injected JS.
   - (c) `@sentry/react`, which needs a CSP `connect-src` exception.
3. **Release symbols.**
   - (a, recommended) Keep `strip = true` and accept address-only Rust frames.
   - (b) `strip = "debuginfo"` keeps function names. The size cost hasn't been measured.
   - (c) Upload debug files from CI.
4. **Native crashes such as WebKitGTK.**
   - (a, recommended) Defer them.
   - (b) Add `sentry-minidump`, which runs a crash-reporter child process.
5. **S5 symbolication.**
   - (a, recommended) Defer it. The PC and the `elf` hash let you run `riscv32-esp-elf-addr2line` locally.
   - (b) Build it now. It needs sentry-cli and a CI token.
6. **Settings UI for `[sentry]`.**
   - (a, recommended) Hand-edit `config.toml` with docs for now.
   - (b) Add a form to the config UI.

## Research (all checked 2026-10-15)

### Firmware: getsentry/sentry-micro

| Fact | Source |
|---|---|
| Exists, MIT. Its description says "Hackweek prototype". Created 2026-08-17, last push 2026-08-24, 5 stars, no releases, `library.json` v0.1.0 | `gh repo view getsentry/sentry-micro`, `library.json` |
| Packaged for Arduino: `"frameworks": "arduino"`. The example builds with PlatformIO `espressif32@7.0.1`. There's no ESP-IDF component CMake | `library.json`, `examples/wifi_basic/platformio.ini` |
| The core (`src/core/`) is freestanding C99 and tested on a PC. The device layer (`sentry_device_esp32.c`, `sentry_coredump_esp32.c`, `sentry_storage_nvs.c`) is plain ESP-IDF and knows `CONFIG_IDF_TARGET_ESP32P4` | `platformio.ini`, `src/device/*` |
| Every transport is C++ under `#if defined(ARDUINO)`. The Wi-Fi one uses `HTTPClient`/`WiFiClientSecure` and turns TLS verification **off** unless you call `set_ca_cert()`. There's no `esp_http_client` transport, though a C `sentry_transport_t` vtable exists for writing one | `src/transport/sentry_transport_wifi.cpp`, `src/core/sentry_transport.h` |
| On RISC-V (P4), a crash yields only two frames, PC and RA. The IDF summary has a raw stack dump, not an unwound backtrace. Its own comment says server-side unwinding "is not done yet" | `sentry_coredump_esp32.c` `fill_arch_details`; IDF v6.1 `esp_core_dump_summary_port.h` |
| Crash path: `esp_core_dump_image_check()`, then `esp_core_dump_get_summary()`. The caller erases after delivery | `sentry_coredump_esp32.c:241-289`, `wifi_basic/main.cpp:391` |
| Its relay design has a companion app forward device-built envelopes. Its binary framing is aimed at BLE | `src/core/sentry_relay.h` |
| `-Wl,--build-id` broke an ESP32 boot (the note moved the vector table), so it stamps the build-id after linking as a non-ALLOC note | `scripts/stamp_build_id.py` |
| watchdeck today: no coredump (`CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y`), no build-id note in `build/watchdeck.elf` (`readelf -n` is empty), `CONFIG_APP_RETRIEVE_LEN_ELF_SHA=9` | `firmware/sdkconfig`, readelf |

**Verdict.** Upstream can't be used as is on ESP-IDF v6.1. A fork would mean vendoring the C core, writing an IDF transport and getting the DSN to the device. The crash data it gets on the P4 is the same two frames that ~150 lines of our own code would get.

### App: Rust

| Fact | Source |
|---|---|
| `sentry` 0.49.3 (2026-09-21), MSRV 1.88. mise pins Rust 1.99 | crates.io API |
| Default features include `transport` = reqwest + **native-tls (OpenSSL)**. Use `default-features = false` with `reqwest` + `rustls-no-provider` so it takes the aws-lc-rs provider that `lib.rs:25` installs | crates.io feature list |
| It requires `reqwest ^0.13.2`. The lockfile has 0.13.5, so there's one copy | crates.io deps, `Cargo.lock` |
| `sentry-panic` flushes the client inside the hook, so `panic = "abort"` still delivers | `sentry-panic/src/lib.rs:33-37` @0.49.3 |
| `sentry-panic` always depends on `sentry-backtrace`, so panic events carry stack frames even without the `backtrace` feature | `sentry-panic/Cargo.toml` @0.49.3 |
| `ContextIntegration` sets `server_name` to the hostname when it's unset | `sentry-contexts/src/integration.rs:74-76` |
| `apply_defaults` reads `SENTRY_DSN`, the proxy variables and **`SSL_VERIFY`**, and `SSL_VERIFY=false` turns certificate verification off | `sentry/src/defaults.rs:81-113` |
| Dropping `ClientInitGuard` closes the client | `sentry/src/init.rs:31-41` |
| `tauri-plugin-log` 2.10.1 (locked) has `Builder::split(&AppHandle)` and `attach_logger`, so `sentry_log::SentryLogger::with_dest` can wrap its logger | plugins-workspace tag `log-v2.10.1`, `lib.rs:873,918` |
| `Envelope::from_slice` and `Client::send_envelope` exist. Only the rejected relay option needs them | `sentry-types` envelope.rs, `sentry-core` client @0.49.3 |
| `tauri-plugin-sentry` 0.8.0 (2026-10-13). It injects `@sentry/browser` into every webview, needs the `sentry:default` capability, and its default `minidump` feature runs a crash-reporter child process | `timfish/sentry-tauri` README |
| `@sentry/react` and `@sentry/browser` 11.8.0 (2026-10-15) | npm registry |

### Config and privacy

- Sentry treats the DSN as write-only, not a secret, so it can live in `config.toml`. `get_config` will hand it to the webview.
- `save_config` serializes the `Config` struct, and the UI keeps `structuredClone(view.config)` (`ConfigPage.tsx:72`). A new optional field therefore survives a UI save once Rust and TS both declare it.
- `Configs::save` calls `Config::check` and never `parse` (`config.rs` around 660). New rules belong in `check`.
- These existing `error!` lines would become events:
  - `lib.rs` emit failures
  - `config.rs` "not applied" (carries user ids, labels and paths) and "reload failed"
  - `server.rs` accept and session failures
  - `pairing.rs` task failure
  - `mdns.rs` unavailable
  - `tiles.rs` poller panic
  - `tray.rs` autostart and window errors
- `warn!` lines carry peer IPs and device ids, and `info!` carries `DaemonEvent` debug output.
- Coredumps hold task stacks. The link task builds `hello`, token included, in a stack buffer (`link.c:694`).

## Design

```text
board ──wss: report {crash summary}──▶ daemon ──https──▶ Sentry ingest
webview ──tauri-plugin-sentry IPC─────▶ daemon ┘
daemon log::error!, panics ───────────────┘
```

### Daemon (`app/src-tauri/src/daemon/reporting.rs`)

The module is named `reporting` rather than `sentry` so it doesn't shadow the crate. It doesn't import tauri.

- **Config.** `Config.sentry: Option<SentryConfig>`, with `#[serde(default, skip_serializing_if = "Option::is_none")]` and `deny_unknown_fields`:
  - `dsn: String` is required, must parse as `sentry::types::Dsn`, and must be https unless loopback. `check()` enforces this.
  - `pii: bool` defaults to false.
- **Startup only.** After `Configs` loads in `setup`, a present `[sentry]` builds the client:
  1. `sentry::apply_defaults(opts)`.
  2. Force `dsn` from config and `accept_invalid_certs = false`, so `SENTRY_DSN` and `SSL_VERIFY` can't override them.
  3. `Client::from(opts)`, bound to `Hub::main()`.
  4. Keep the `Arc<Client>` in managed state. On `RunEvent::Exit`, call `client.close(Some(2 s))`.

  An absent table means no client and no `sentry::init`. Changing `[sentry]` takes effect on restart, and the app logs that once.
- **Release and environment.** Daemon events use `release = watchdeck@<CARGO_PKG_VERSION>`. The environment is `development` in debug builds and `production` in release builds.
- **Crate features.** `contexts, panic, log, reqwest, rustls-no-provider`. No `debug-images` (it sends library paths), no `release-health`, no `logs`.
- **Logger.** In `setup`, call `tauri_plugin_log::Builder…split(app.handle())`, wrap the result in `SentryLogger::with_dest`, then `attach_logger` and `app.handle().plugin(plugin)`.
  - Filter: `Error` becomes an Event, and everything else is ignored.
  - With `pii = true`, `Warn` becomes a breadcrumb too.
  - The config "not applied" line moves to `target: "watchdeck::config_rejected"`, which never reaches Sentry. It's user error, not an app bug.
- **Scrubber.** It runs in `before_send` while `pii = false`, over message, exception values, frame paths, breadcrumbs, tags and extra. It works in this order:
  1. Exact `secrets.toml` values become blank. This runs first.
  2. The home dir becomes `~`.
  3. The hostname becomes `<host>`.
  4. IPv4 and IPv6 literals become `<ip>`, parsed with std.
  5. `wd-<hex>` becomes `<device>`.
  6. Text inside `"…"` becomes `"…"` with nothing in it.

  It also clears `server_name` and `user`.
- **Budget.**
  - It's a bounded expiring map with at most 256 keys. The key is component + message (or fault PC) + build, and a repeat within 10 min is dropped.
  - The process allows at most 30 events per hour.
  - Firmware reports get a per-device cap across sessions: one per (elf, pc) per hour.
- **Device reports** become `level=fatal` events tagged `component=firmware`, `reset_reason`, `elf` and `cause`. Their exception frames are the PC and RA as `instruction_addr`. `release = watchdeck-firmware@<fw>` is set only when the crashed build matches the running one. The device id is attached only with `pii`. The daemon **always** writes one `warn!` line per report, whether Sentry is on or off.

### Webview (tauri-plugin-sentry, user decision 2(b); checked 2026-10-15 against the 0.8.0 crate, tauri 2.12.2, sentry-types 0.49.3)

- `init_with_options(&client, …{ inject: true, debug: false })` takes our S1 client (`lib.rs:128` @0.8.0). Events go through `sentry::capture_event`, so they get Rust breadcrumbs, `before_send` and the budget.
- Tauri builds config windows before `setup` (`app.rs:2691` @2.12.2), and a webview's init scripts are fixed when it's built. So `main` gets `"create": false`. The end of `setup` registers the plugin only if `[sentry]` exists, then calls `WebviewWindowBuilder::from_config(…)`. Without `[sentry]` there is no plugin and no `window.Sentry`.
- The 351 KB script (`@sentry/browser` 10.8.0) sets `window.Sentry`, catches global errors and rejections, and sends envelopes and breadcrumbs over `__TAURI_INTERNALS__` IPC. Its DSN is a dummy and is never fetched.
- The three catch sites (`hooks.ts:45,54`, `PairDialog.tsx:79`) call `window.Sentry?.captureException(e)`, typed in `errors.ts`. No npm dependency.
- `before_send` tags `platform == "javascript"` events `component=ui` and drops `request` (page URL, Referer). The plugin removes only User-Agent.
- Capability: `sentry:default`.
- CSP: no change. Init scripts are webview user scripts outside page CSP, like Tauri's IPC bootstrap, and IPC is already in `connect-src`. UNCONFIRMED until S2's check in a release build.

### Protocol (add to `site/src/protocol.md`)

```text
D→H report     {"t":"report","id":"<8 hex>","kind":"crash|reset","reason":"panic|int_wdt|task_wdt|wdt|brownout",
                "task":"link","pc":"0x4ff01234","ra":"0x4ff05678","cause":"LoadAccessFault","addr":"0x00000000",
                "elf":"<64 hex, crashed build>","same_build":true}
H→D report_ack {"t":"report_ack","id":"<8 hex>"}
```

- **Kinds.**
  - `crash` comes from a valid coredump summary and uses that dump's `elf`.
  - `reset` comes from `esp_reset_reason()` for this boot when there's no dump, such as a brownout or watchdog. It carries no `pc` or `elf`.
- **Delivery.** The device sends after `welcome` and resends every session until it gets `report_ack`. An older daemon ignores the unknown `t` (`server.rs:769`) and never acks, so nothing is lost. The daemon acks after it logs the report, even with Sentry off.
- **Validation.** The daemon checks the hex fields, an enum for `reason` and `kind`, `task` at 16 chars or fewer, and at most 2 reports per session. Adding these messages doesn't bump `v`.

### Firmware

- **sdkconfig.defaults:**
  - `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`
  - `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`
  - `CONFIG_APP_RETRIEVE_LEN_ELF_SHA=64`
  - Check the generated `sdkconfig` afterward, since Kconfig silently drops unknown names.
- **partitions.csv:** append `coredump, data, coredump, , 512K` after `factory`. The NVS offset doesn't change, so pairing survives, but a full flash is needed.
- **New `main/crash.c`, read at boot:**
  1. Run `esp_core_dump_image_check() == ESP_OK` before `esp_core_dump_get_summary()`. A corrupt dump is logged, erased and never parsed.
  2. Fill a static struct with reason, task, pc, ra, mcause name and mtval. The mcause name table is borrowed from sentry-micro (MIT, credited).
  3. Set `same_build` by comparing the dump's ELF hash with `esp_app_get_elf_sha256_str()`.
- **Message.** `proto.c` gains `proto_build_report`.
- **link.c.**
  - Sends the report after `welcome` and handles `report_ack` by erasing the dump.
  - **Forget pairing also erases the coredump**, because the dump may hold the token.
  - Only the summary leaves the device, never the raw dump.
- **Kconfig.** `WATCHDECK_TEST_CRASH` (default n) dereferences NULL 30 s after the first `welcome`. It's for manual checks only.
- **CMakeLists.** Add `crash.c` to SRCS and `espcoredump` to PRIV_REQUIRES.

### Rejected

| Option | Why it lost |
|---|---|
| sentry-micro as is | Arduino and PlatformIO only, a two-month-old prototype with no releases, and TLS unverified by default |
| Fork sentry-micro: device builds envelopes, daemon relays them | The DSN goes to the device. The daemon would have to parse and rewrite opaque envelopes to enforce privacy. It means ~15 vendored C files plus an IDF transport, an id derived from the eFuse MAC, and the same two P4 frames anyway. Its extras (metrics, logs, spans, an NVS queue) aren't requested, and the coredump partition already persists the report |
| Device posts to Sentry over `esp_http_client` | A second TLS context, a CA bundle, a DSN on the device and internet egress, all duplicating a pipe the device already has |
| Webview errors routed through a custom `report_ui_error` command | The plan's first recommendation. The user chose `tauri-plugin-sentry` instead, for breadcrumbs and browser context |
| `@sentry/react` directly | Needs `connect-src` to the ingest host, which loosens the S7 CSP, and a second DSN path |
| Rebinding the client live on config change | Restart-only is simpler. The setting rarely changes |
| `sentry-minidump` for WebKitGTK native crashes | Its re-exec'd crash-reporter process. Open question 4 |

## Slices

| # | Slice | Files | Depends on | Acceptance | Cheapest check | Agent |
|---|---|---|---|---|---|---|
| S1 | Daemon SDK, config, scrubber, budget, log bridge, panic | `Cargo.toml`, `Cargo.lock`, `lib.rs`, `daemon/mod.rs`, new `daemon/reporting.rs`, `daemon/config.rs`, `app/src/config.ts`, `site/src/config.md` | user approves `sentry` | No `[sentry]` means no client, even with `SENTRY_DSN` set. A bad or http DSN is refused on both reload and `save_config`. `SSL_VERIFY=false` doesn't disable verification. A UI save keeps `[sentry]`. Scrubber tests cover the secret-first ordering, quotes, IPs, home, hostname, `wd-` ids and frame paths. Budget tests cover distinct crashes, floods, and the 256-key cap. `cargo tree -i openssl-sys` and `cargo tree -i ring` are empty | `cargo test`, clippy. Manually, with a real DSN, a forced `error!` and a release-build panic arrive scrubbed | coder-high |
| S2 | Webview errors via tauri-plugin-sentry | `tauri.conf.json`, `lib.rs`, `capabilities/default.json`, `daemon/reporting.rs`, `errors.ts`, `hooks.ts`, `PairDialog.tsx` | S1 | With `[sentry]`, the page loads under the release CSP, and a thrown error, a rejection and the three catch sites arrive scrubbed as `component=ui`. Without it, no plugin and no `window.Sentry`. Tray and single-instance still show the window. `cargo tree -i sentry-minidump` is empty | `cargo test`, `pnpm build`, then throw from devtools in a release build | coder-high |
| S3 | `report`/`report_ack` and device-report events | `daemon/protocol.rs`, `daemon/server.rs`, `daemon/reporting.rs`, `site/src/protocol.md` | S1 | Parse and bounds tests pass. Captured-event tests (`sentry` with `features=["test"]` in dev-deps) show the event shape, `same_build` controlling release, no device id without pii, and the cross-session cap. Ack goes out with Sentry off. Over-limit reports are logged and acked but not sent | `cargo test` | coder-high |
| S4 | Firmware coredump and report | `firmware/partitions.csv`, `sdkconfig.defaults`, `main/crash.c`, `main/crash.h`, `main/proto.c`, `main/proto.h`, `main/link.c`, `main/main.c`, `main/CMakeLists.txt`, `main/Kconfig.projbuild`, `CLAUDE.md` hardware facts | S3 (spec) | Clean `fw:build`. The generated `sdkconfig` has all three keys. Manually, with `WATCHDECK_TEST_CRASH=y`: crash, reboot, then a daemon log line and Sentry event, then the dump is erased after ack. Against the current daemon (no ack) the dump survives. Forget pairing erases it. A brownout or WDT gives `kind=reset`. Pairing survives the reflash. The panic log shows the coredump written with no space error | `mise run fw:build`, then flash and check on the board | coder-high |
| S5 (optional) | Firmware symbolication in Sentry | stamp script (borrowed), `firmware/CMakeLists.txt`, `mise.toml` (sentry-cli), `ci.yml`, `protocol.rs`/`reporting.rs` (`build_id`, `debug_meta`) | S4, user decision | A known PC from the test crash resolves to `file:line` in Sentry against the uploaded ELF. Image address and size come from the ELF | Manual, on one build | coder-high |

Commit each slice once its checks pass. S2 and S3 can run in parallel after S1. S4 can be built against the S3 spec but checked only after S3 lands.

## New dependencies (user approval)

- `sentry = { version = "0.49.3", default-features = false, features = ["contexts", "panic", "log", "reqwest", "rustls-no-provider"] }`. This brings in sentry-core, -types, -panic, -backtrace, -contexts and -log.
- Dev: the same crate with `features = ["test"]`.
- `tauri-plugin-sentry = { version = "=0.8.0", default-features = false }`. It wants `sentry ^0.49` with default features off, so it shares our 0.49.3 and adds no TLS. Turning off default features also drops `minidump`. Its other deps are already locked.
- S5 only: `sentry-cli` pinned in mise, and a Sentry auth token as a CI secret.
- Firmware: none. `espcoredump` ships with IDF.

## Risks and rollback

| Risk | Mitigation |
|---|---|
| Personal data in free-text errors | Off by default, Error-level only, the scrubber, config rejections excluded, scrubber tests |
| Quota burn from a loop | The process cap plus the expiring map. Firmware reports are capped per device |
| The partition table change bricks a flash, or the coredump write fails in panic with PSRAM and ESP-Hosted running | The NVS offset is unchanged. The manual S4 check covers both. Rollback is the old table plus a full flash |
| A corrupt dump crashes boot | `image_check` before parsing. A corrupt dump is erased |
| OpenSSL or ring creeps in | `default-features = false`, plus the `cargo tree` check in S1 |
| `split()` ordering in `setup` misbehaves | The coder confirms early that the logger attaches before the first `log::info!` |
| Release frames are bare addresses (`strip = true`) | Accepted. The panic message and `file:line` still arrive. Open question 3 |
| Raw envelopes from the plugin (`commands.rs:30-34` @0.8.0, sent when parsing fails) and non-event envelopes (`:73`) skip `before_send` and the budget | Wrap the transport and drop envelopes with no typed items (`items()` is empty for raw, `envelope.rs:480`). Client reports (counts only) remain. Test it |
| The plugin's injected script fails under the release CSP | `init_with_no_injection`, plus `@sentry/browser` and the plugin's ~40-line `index.ts` bundled in (a new npm dependency, which needs the user's approval) |
| `tauri-plugin-sentry` 0.8.0 is two days old | Pin `=0.8.0`. Rollback is reverting S2 |
| The coredump partition is too small | 512K is an estimate, not measured. The S4 manual check reads the dump size from the panic log |

Rollback: remove `[sentry]` and reporting stops. Each slice is one revertible commit, and only S4's partition table needs a reflash to undo.

## Manual steps for the user

- Create the Sentry project and turn on "Prevent Storing of IP Addresses" and the server-side data scrubber.
- Add `[sentry] dsn = "…"` to `config.toml`.

## Critique log

| # | Point | Status |
|---|---|---|
| 1 | A corrupt dump is parsed without a checksum check | adopted: `image_check` first, erase if corrupt |
| 2 | Erase-on-send loses reports against older daemons | adopted: report id plus `report_ack`, and the device resends until acked |
| 3 | `APP_RETRIEVE_LEN_ELF_SHA=9` truncates the hash | adopted: set to 64 and check the generated sdkconfig (confirmed `sdkconfig:1176`) |
| 4 | The dump keeps the token after Forget | adopted: Forget erases the dump, and only the summary leaves the device (confirmed `link.c:694`) |
| 5 | Config validation errors leak user ids and labels | adopted: rejections go to their own excluded target, quote redaction, secrets matched first |
| 6 | `SSL_VERIFY` env disables TLS verification | adopted: apply defaults, then force strict TLS and the config DSN (confirmed `defaults.rs:112`) |
| 7 | Validating in `parse()` misses `save_config` | adopted: the rules go in `Config::check` (confirmed) |
| 8 | Guard lifetime and shutdown flush unspecified | adopted: the client lives in managed state, closed on `RunEvent::Exit` |
| 9 | Dump metadata may come from a different build or reset | adopted: `crash` and `reset` split, `same_build` controls release |
| 10 | S4 omits CMake and Kconfig changes | adopted |
| 11 | Panic events still carry backtraces | adopted: frames documented and their paths scrubbed (confirmed `sentry-panic/Cargo.toml`) |
| 12 | A stack string isn't a structured stacktrace | adopted: message-only events are an explicit, accepted limit |
| 13 | Handled UI failures bypass the global listeners | adopted: `reportError` at the three sites, no recursion |
| 14 | S5 lacks the build-id and image mapping | adopted: S5 is optional, and its acceptance is a known PC resolving |
| 15 | Throttles aren't a bounded budget | adopted in part: process cap, bounded keyed map, per-device cap across sessions. Event-level caps only, no budget per log site |

Second opinion: codex (gpt-6.1-sol, high effort, read-only sandbox), 15 points, all verified against code or upstream source.
