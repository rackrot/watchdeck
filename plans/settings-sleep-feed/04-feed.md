# The feed: a page at index -1, and integrations that fill it

Part of [`00-index.md`](00-index.md). Findings F1 to F44 are there. The feed slices are N1 to N5, so they never read as findings. Needs G4 and G5 from [`01-groundwork.md`](01-groundwork.md) and T1 from [`02-settings.md`](02-settings.md).

A right swipe on page 1 shows a page of **notifications** and **stat cards**. They come from `[[integration]]` sources in the daemon:

- **push**: a local HTTP API, so scripts, cron jobs and Home Assistant can post notifications and cards, with a CLI (`watchdeck notify ...`) for the scripts.
- **stat**: a polled value, exactly like a tile's source (shell or HTTP, with `json_pointer`), shown as a card.
- **Named connectors**, phase 2 (N5): `prometheus` (a query, polled, shown as a card) and `grafana` (alerts from Grafana's webhook contact point, shown as notifications).

## Decisions

### One interface in the daemon (D12)

```rust
/// What a source shows: its cards and its notes. A polled source returns all of them each time,
/// and the store replaces what it showed before.
pub struct Snapshot { pub cards: Vec<Card>, pub notes: Vec<Note> }

/// A configured [[integration]] that the daemon polls. One implementation per polled kind.
pub trait Poller: Send + Sync + 'static {
    fn every(&self) -> Duration;
    fn poll<'a>(&'a self, ctx: &'a PollCtx) -> BoxFuture<'a, Result<Snapshot, String>>;
}

/// A configured [[integration]] that receives. One implementation per push kind.
pub trait Receiver: Send + Sync + 'static {
    /// The routes it answers under /v1/, after the bearer token has picked this integration.
    fn accept(&self, req: &PushRequest) -> Result<Vec<Update>, PushError>;
}

/// The registry: one match arm per kind.
pub fn build(cfg: &IntegrationConfig, deps: &Deps) -> Source { ... }
pub enum Source { Polled(Arc<dyn Poller>), Push(Arc<dyn Receiver>) }
```

- `BoxFuture` is `futures_util`'s, already a dependency (F30).
- **Adding a source** is: a `kind` variant in `IntegrationConfig` with its `check()` rules, an implementation of `Poller` or `Receiver`, one arm in `build`, and a fixture test. Nothing in the store, the manager, the sessions or the board changes. `site/src/integrations.md` says so, with the `stat` source as the worked example.
- **The manager** (like `tiles::manage`, F27) starts and stops one task per polled source as the config changes, only while at least one session is open. It owns the schedule, the cooldown (`MIN_EVERY_S`), the `stale` mark after three polls that couldn't run, and the redaction of every string a source returns. A connector only reads and maps.
- **Slots**: polls take a tile slot through `Actions` (F27), at most 2 at a time, so tiles keep 4 of their 6 and presses keep their 2.
- **The store** (`feed.rs`) holds the cards in config order (up to 8) and the notes newest first (up to 32; the board gets the newest 12). Note ids are `<integration>/<id>`. A note expires after its `ttl_s` (default 1 day, at most 7). Dismissing a note from any board removes it for every board. The store lives in memory: a daemon restart starts empty, and polled cards fill again within a period.

### The push API (D13)

| Request | Body | Does |
|---|---|---|
| `POST /v1/notes` | `{"title":"Backup done","body":"...","level":"ok","id":"backup","ttl_s":3600}` (`title` required) | Adds a note, or replaces the one with the same `id`. 201 with `{"id":"ha/backup"}` |
| `DELETE /v1/notes/{id}` | — | Removes it. 204, or 404 |
| `PUT /v1/cards/{id}` | `{"label":"Washer","value":"12 min","state":"ok"}` | Sets a pushed card. 204. 409 when the 8 card slots are full |
| `DELETE /v1/cards/{id}` | — | 204 or 404 |
| `POST /v1/grafana` | Grafana's webhook payload | N5. Firing alerts become notes, resolved ones are removed |

- **Auth**: `Authorization: Bearer <token>`. The token is the value of the `token_secret` a push integration names in `secrets.toml` (0600, F29), at least 32 characters. The daemon compares it with every push integration's token in constant time (`aws_lc_rs::constant_time`, as `store.rs` and `pairing.rs` do) and the match picks the integration, so a token can only write as its own source. Anything else gets 401 `unauthorized`, with the same body for a missing, malformed or wrong token.
- **Where it listens**: `127.0.0.1:47822`, plain HTTP, whenever a push integration exists. With `[push] lan = true`, also `0.0.0.0:47823` over HTTPS with the daemon's existing TLS identity (the wss certificate), whose fingerprint the app shows for pinning. Never plain HTTP on the LAN (Q3). Neither port is advertised over mDNS.
- **Limits**: body at most 8 KB (413); headers within 5 s of connecting (a slow client is dropped); 32 connections at once; 10 requests a second per integration, bursts of 30 (429); a peer with 20 failed auths in a minute gets 429 for a minute; one request per connection.
- **Text**: every field is made printable ASCII and cut to the wire's limits (G4) by the daemon, not refused, so a script's emoji costs a `?`, not the note. Unknown JSON fields are refused (400) so a typo in a key shows.
- **Logging**: one line per accepted request with the integration id, the route and the status. Never the token, never a body.
- **The server**: `hyper` 1.12 with `server` and `http1`, and `hyper-util` (both in the lock through `reqwest`, F30). Turning on hyper's server feature adds `httpdate`, which isn't in the lock, so slice V1 vets it as status-bar-2 V0 vetted NVML. If vetting fails, the fallback is a minimal HTTP/1.1 reader on `httparse` (already in the lock): one request per connection, `Content-Length` only, no chunked bodies.

### The CLI (D14)

```text
watchdeck notify [--integration ID] [--level info|ok|warn|error] [--id ID] [--ttl SECONDS] TITLE [BODY]
watchdeck card   [--integration ID] [--state ok|bad] [--label LABEL] ID VALUE
watchdeck dismiss [--integration ID] ID
```

- A subcommand of the app's own binary, handled in `main.rs` before `watchdeck_lib::run()`, so Tauri, the window and the single-instance plugin never start. It's already installed wherever the app is.
- **Token**: `WATCHDECK_TOKEN` if set, else it reads `config.toml` and `secrets.toml` from the config dir (the same user, the same 0600 checks as the daemon) and takes the push integration named by `--integration`, or the only one.
- It posts to `127.0.0.1:47822` with `std::net` (no new dependency) and exits 0 on success, 1 with the daemon's reason on a refusal, 2 when the app isn't running.

### The -1 page on the board (D11)

- **Navigation**: `swipe_page` at page 0 going right (dir -1) shows the feed instead of doing nothing, when the board holds a snapshot with `sources` of 1 or more. On the feed, a left swipe (dir +1) shows page 0, and a right swipe does nothing. The model's shown page stays page 0's id, so a new set, a reconnect or a settings round trip keeps working as today; the UI keeps an `on_feed` flag.
- **It's not a page of the set**: no `proto_layout_t`, no `pageset` change, no change to "Page x of y", to the grid's geometry or to any icon box. A board with one page reaches the feed the same way.
- **Layout** (1024 x 556 under the bar), all in Montserrat chrome fonts:
  - Up to 8 cards in two rows of four (244 x 112 each): the label in 24 and the value in 32, on a card colored like a tile (ok: dark blue-grey; bad: dark red; unknown: grey).
  - Below, the newest notes that fit (about 5 rows of 56 px; without cards, about 9): a colored edge by level, the title, the source and its age (`5 min`, from the `stats` clock; no age before the first `stats`). "+N more" when some don't fit.
  - With no cards and no notes: "Nothing here yet".
  - A tap on a note opens it whole in a modal on the top layer: title, body, source, time, Dismiss and Close. Dismiss sends `feed_dismiss`. While the link is down Dismiss is off and the page is dimmed, like the grid.
- **Gestures**: the feed page takes `touch_track` and the gesture handler the grid uses, so the long swipe up (settings) and the long swipe down (sleep, P1) work there too. Note rows are pressed only by a tap under 20 px (`drag_consumed`).
- **Updates**: a new snapshot rebuilds the page's content in place (at most about 50 objects).
- **Storage**: the snapshot lives in the model under its lock (PSRAM, about 6 KB), written by the link task as tiles are (store under the model lock, release, then the UI callback takes the LVGL lock and then the model lock, F25's order).

### Phase 2 connectors (N5)

- **`prometheus`**: `url`, `query`, `every_s`, `label`, `decimals` (0 to 3), `suffix` (at most 7), `bad_above` and `bad_below` (optional), `secret_headers` like an HTTP action. It runs `GET <url>/api/v1/query?query=<encoded>` through the actions' HTTP client (so `insecure_tls`, timeouts and redaction behave as for actions), and reads `/data/result/0/value/1` for a vector or `/data/result/1` for a scalar. No result is a card reading "no data" in state bad.
- **`grafana`**: a push kind. Grafana's alerting has a webhook contact point that POSTs its alerts as JSON, with an optional Authorization header (scheme `Bearer`, credentials the token). The receiver maps each alert to a note: id `grafana/<fingerprint>`, title from `labels.alertname` and `annotations.summary`, body from `annotations.description`, level from `labels.severity` (`critical` to error, `warning` to warn, else info). Status `resolved` removes the note. N5 checks the payload against Grafana's current docs and keeps a captured payload as the fixture.
- Both are sugar over the two base kinds, so the interface above holds them without change. That's the test of the interface.

## Goal and done

- `watchdeck notify "Backup done"` on the desktop shows a note on the -1 page within 2 s on every connected board. `curl` with the token does the same, and without it gets 401.
- A Home Assistant `rest_command` to the LAN listener (with `lan = true`) shows a note, and one to the loopback port from another host can't connect.
- A `stat` integration shows its value as a card within its period, with `stale` after three skipped polls, as a tile does.
- A `prometheus` integration shows a query's value, and a Grafana test alert shows as a note and leaves on resolve (N5).
- Dismiss on one board removes the note on every board.
- The -1 page works offline (dimmed, the last snapshot), and doesn't exist with no integration configured.
- Pool with the feed page up, at 8 cards and 12 notes, is logged and leaves at least 20 KB free.

## Slices

### V1 Vet `httpdate` (before N2)

- In a throwaway worktree: turn on `hyper`'s `server` and `http1` features and `hyper-util`'s `server`, `tokio` and `server-auto` as needed, then `cargo tree -e features -i hyper` and the `Cargo.lock` diff. Expected: `httpdate` alone. Then, as V0 did: owners and publisher, license, advisories (osv.dev, RustSec), source against the repo's tag, no build script.
- **Stop** and go back to the user if anything else appears or a check fails; N2 then uses the `httparse` fallback.
- **Output**: a vetting record in this plan's results and N2's commit message.

### N1 Daemon feed, integration registry, polled stats

- **Files**: `daemon/feed.rs` (new: the store, TTL, dismiss, the snapshot for the wire with G4's limits), `daemon/integrations/mod.rs` (new: `Poller`, `Receiver`, `Source`, `build`, the manager), `integrations/stat.rs` (new: the `stat` poller, reusing `tiles::reading` and its `json_pointer` and redaction), `tiles.rs` (`reading` and `pick` made `pub(crate)`), `actions.rs` (a feed slot count under `MAX_TILE_RUNNING`), `server.rs` (`Shared` holds the store; the `feed` branch in `serve`, gated on `CAP_FEED`, coalesced to twice a second; the first snapshot after the first set, its tiles and `sleep`; `feed_dismiss`), `protocol.rs` (`CAPS` gains `CAP_FEED`), `lib.rs` (the window gets `feed-changed`), `commands.rs` (`feed_snapshot`), `tests/device_sim.rs`.
- **Tests**:
  - Store: 33 notes keep the newest 32; the wire snapshot has the newest 12; a note past its TTL leaves at the next sweep (paused clock); a replace by id keeps one note; a dismiss removes it; strings are cut and made printable.
  - The fullest wire snapshot (8 cards, 12 notes, every string at its limit and every character a `"`) is under 16 KB, printed.
  - `stat`: with a fake `Sample`, a value, a json pointer, a failed run (bad), three skips (stale); a secret in the output is redacted.
  - Manager: no polls with no session; one task per source; an edited source restarts and drops its old card; at most 2 polls at once.
  - `device_sim`: a hello with `feed` gets a snapshot with `sources` after the first set; a stat source's card arrives; a dismiss from one session updates both sessions; a session without `feed` gets none; every existing test passes unchanged.
- **Acceptance**: `cargo test`, clippy; no new crate.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml feed integrations` and `--test device_sim`.
- **Owner**: high complexity, daemon.

### N2 Push API and CLI

- **Precondition**: V1.
- **Files**: `daemon/push.rs` (new: the listeners, auth, limits, routing to `Receiver`s), `integrations/push.rs` (new: the notes and cards routes), `lib.rs` (start the listeners; the LAN one with the identity), `config.rs` (the token-length rule if G5 didn't add it), `src/cli.rs` (new) and `main.rs` (the subcommands), `commands.rs` (`create_push_token(id)`: 32 random bytes from `aws_lc_rs::rand` as 64 hex digits, stored with `Secrets::set` as `push-<id>`, returned once), `Cargo.toml` (the features), `site/src/integrations.md` (new: the API, the CLI, Home Assistant and cron examples, the LAN option and its certificate), `tests/push_api.rs` (new).
- **Tests** (`push_api.rs`, a real listener on a free port):
  - 401 for no header, `Basic`, a wrong token and another integration's token used on a route that integration lacks; the body is identical for all of them.
  - 201 and the note in the store; replace by `id`; 204 on delete; 404 on an unknown id.
  - 413 at 8 KB + 1; 400 for an unknown field and for a missing `title`; a 10 s slow header is dropped at 5 s.
  - 429 after the burst, and for a peer after 20 failed auths.
  - Loopback only without `lan`: the LAN port refuses; with `lan`, the LAN port speaks TLS with the daemon's certificate and plain HTTP to it fails.
  - No line of the captured log holds the token.
  - CLI: against the test listener, exit 0, 1 (with the reason) and 2 (nothing listening); token from `WATCHDECK_TOKEN` and from the files.
- **Acceptance**: the tests, clippy, `cargo test`. On this desktop, `watchdeck notify "hi"` with the dev app running reaches `feed-changed` in the window.
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml --test push_api`.
- **Owner**: high complexity, daemon, security. A reviewer reads the auth path and the limits.

### N3 The -1 page on the board

- **Files**: `firmware/main/feedpage.c/.h` (new: the layout, the note modal, the refresh), `model.c/.h` (the snapshot, `model_set_feed`, `model_feed`), `ui.c` (`swipe_page` at the ends, the `on_feed` state, the mode hooks from T1), `link.c` (dispatch `feed`, `link_feed_dismiss`, the hello names `feed` in this commit), `link.h`, `main.c` (`on_feed`), `feedfmt.c/.h` (new, pure: the age text and the "+N more" count from the rows that fit, host-tested), host `test_feedfmt.c`.
- **Host cases** (`feedfmt`): ages of 0 s, 59 s, 1 min, 59 min, 2 h, 3 d, and an unknown clock; how many note rows fit with 0, 4 and 8 cards.
- **Acceptance (batch A, against the N1 daemon with a `stat` source and notes added by N2's CLI or a test endpoint)**:
  - A right swipe on page 1 shows the feed; a left swipe returns; a right swipe on the feed does nothing; neither log shows a press.
  - With one page (no "Page x of y"), the same.
  - With no integration configured, a right swipe on page 1 does nothing.
  - The long swipe up and down work from the feed (settings, sleep), and closing settings returns to the feed.
  - A tap on a note opens it, Dismiss removes it on this board and a second board (or a second `device_sim` session watching).
  - Daemon stopped: the page dims, Dismiss is off, swipes still work.
  - Pool with 8 cards and 12 notes up is logged; 10 switches between page 1 and the feed leave "page built" within 200 bytes.
- **Cheapest check**: `mise run fw:test`, `mise run fw:build`, then batch A.
- **Owner**: high complexity, firmware LVGL. Merge after P1 (both change the gesture code).

### N4 App: Feed tab and integrations form

- **Files**: `PageTabs.tsx` (a "Feed" tab before the first page, shown when the config has an integration or once the user adds one), `FeedForm.tsx` (new: the integrations list and one form per kind), `FeedPreview.tsx` (new: the live snapshot from `feed-changed`, drawn as the board draws it), `ConfigPage.tsx`, `api.ts` (`feed_snapshot`, `create_push_token`), `errors.ts` (G5's bucket by integration id), `config.ts`, `app/tests/*.test.ts`.
- **Form**: add, remove and reorder integrations; per kind: `push` (label, Generate token, which stores the secret and shows the token once with copy buttons for a `curl` line, a CLI line and a Home Assistant `rest_command` block; the LAN switch and the certificate fingerprint), `stat` (label, every, json pointer, the source editor reused from `ActionForm.tsx`, and Test, which runs the source once as a tile's Test does), and in N5 `prometheus` and `grafana`.
- **Tests**: the error routing per integration id; `feedChanged` true and `layoutTargets` empty for an integrations-only edit; the preview's row count matches `test_feedfmt.c`'s table.
- **Acceptance (window)**: add a push integration, generate a token, run the shown `curl`, and the note appears in the preview and on the board; add a `stat` and see its card; the save bar's wording for an integrations-only save.
- **Cheapest check**: `pnpm test`, `pnpm build`, Biome from `app/`.
- **Owner**: design judgment, app.

### N5 Prometheus and Grafana connectors

- **Files**: `config.rs` (the two kinds and their rules), `integrations/prometheus.rs`, `integrations/grafana.rs` (new), `integrations/mod.rs` (two arms), `FeedForm.tsx` (two forms), `site/src/integrations.md` (both, with Grafana's contact-point setup), fixtures `prometheus_vector.json`, `prometheus_scalar.json`, `prometheus_empty.json`, `grafana_firing.json`, `grafana_resolved.json`.
- **Tests**: against F37's local listener pattern: a vector and a scalar give the value with `decimals` and `suffix`; empty gives "no data" (bad); `bad_above` 90 with 91 gives bad; a 500 gives bad with the summary; the query is URL-encoded; `secret_headers` are sent and redacted. Grafana: a firing payload adds notes with the mapped levels; a resolved one removes them; a payload with an unknown top-level field still parses (Grafana adds fields), but a missing `alerts` is 400.
- **Acceptance**: the tests; on this desktop, if a Prometheus or Grafana is reachable, one live reading each (recorded, or "not run" with the reason).
- **Cheapest check**: `cargo test --manifest-path app/src-tauri/Cargo.toml integrations::`.
- **Owner**: design judgment, daemon.

## Rejected alternatives

| Option | Why not |
|---|---|
| The -1 page as a fifth `[[page]]` | `MAX_PAGES` 4 and the paged geometry are part of the `pages` capability (`protocol.md` Pages); a fifth page would shrink every grid and change every icon id. The feed isn't a grid of buttons |
| Feed items as tiles on a grid page | Tiles belong to buttons and cells; notifications need a list, a body and a dismiss |
| A Unix socket for local pushes | No auth to manage, but Home Assistant is usually on another host, so a token-authenticated HTTP API is needed anyway. One mechanism, not two |
| The CLI talking D-Bus to the running app (single-instance args) | Cron jobs have no session bus, and it starts a webview process per call |
| A separate CLI crate or binary | Another artifact to build and install. A subcommand ships with the app |
| `axum` | A new framework for five routes; hyper is already in the tree |
| Plain HTTP on the LAN | The bearer token would cross the network in clear (Q3) |
| A new token store for push tokens | `secrets.toml` already has the permissions, the validation and the redaction (F29) |
| Polling Grafana's alert API | Needs a Grafana token with read access and a poll per period; the webhook contact point pushes exactly the transitions |
| A Prometheus client crate | One GET and one JSON pointer |
| Notes persisted across daemon restarts | A file of other people's messages to keep and expire. Pushers resend; polled sources refill |
| Toasting new notes over the grid | Not asked for, and it would cover buttons. A later option |

## Risks

| Risk | Mitigation |
|---|---|
| The push API is an unauthenticated or brute-forceable hole | Loopback by default; 32+ character tokens; constant-time compare; one 401 body; failed-auth throttling; N2's tests; a review of the auth path |
| A pusher floods the feed | 10 requests a second per integration, 32 notes kept, 12 sent, coalesced to two snapshots a second |
| A script's text breaks the board | The daemon makes every string printable ASCII and cuts it (G4); the board's `copy_printable` is the backstop; no recolor on feed labels |
| Integration polls starve tiles or presses | At most 2 at a time, inside the tile budget (F27) |
| `httpdate` fails vetting | V1's fallback: a minimal `httparse` reader |
| The feed page and the grid both handle gestures and drift apart | They share `touch_track` and the gesture handler (N3), so one fix covers both |
| Grafana's payload changes shape | Unknown fields are accepted; the fixture is captured from current docs; a missing `alerts` is a clear 400 |
| The fullest snapshot passes 16 KB | The size test at the limits (N1) |

## Memory estimates

| What | Pool | PSRAM |
|---|---|---|
| Feed page, 8 cards and 12 notes | about 12 KB | — |
| The note modal | about 2 KB | — |
| Snapshot in the model, and in the parse target | — | about 6 KB each |
