# Integrations and the push API

A right swipe on page 1 of the board shows the feed page: notes (notifications) and cards (single values). They come from the `[[integration]]` tables in `config.toml`, which [Buttons and actions](config.md#integrations) describes:

- A `stat` integration polls a command or a URL and shows the value as a card.
- A `push` integration takes notes and cards that scripts, cron jobs and Home Assistant send to the daemon's push API, over HTTP or with the `watchdeck` command.
- A `prometheus` integration runs a query against a Prometheus server and shows the value as a card.
- A `grafana` integration takes the alerts of a Grafana webhook contact point and shows them as notes.

This page covers the push API, the `watchdeck` command and the LAN option, then the [`prometheus`](#prometheus) and [`grafana`](#grafana) integrations.

## A push integration and its token

Each sender gets a push integration with its own token. The token is a secret in `secrets.toml` (a 0600 file next to `config.toml`) of at least 32 characters, and the integration names it in `token_secret`:

```toml
[[integration]]
id = "ha"
kind = "push"
label = "Home Assistant"
token_secret = "push-ha"
```

Add the token on the app's Secrets page, which keeps the file 0600, with 64 random hex digits from `openssl rand -hex 32` as its value. To add it by hand instead, keep the file private, since the daemon refuses a `secrets.toml` that isn't 0600:

```sh
(umask 077; printf 'push-ha = "%s"\n' "$(openssl rand -hex 32)" >> ~/.config/watchdeck/secrets.toml)
```

A token opens only its own integration: what it posts shows that integration's label, and it can't touch another integration's notes or cards, nor push them out. Each integration holds at most 16 notes and 4 cards. When one posts past that, or when all 32 notes are taken, only its own oldest note makes room. Two push integrations can't share a token: if two secrets hold the same value, it opens neither, and the daemon logs why. The daemon reads `secrets.toml` again within half a second of a change, so a new or changed token works at once.

## Where it listens

| Address | Scheme | When |
|---|---|---|
| `127.0.0.1:47822` | HTTP | While at least one push integration is configured |
| `0.0.0.0:47823` | HTTPS, with the daemon's own certificate | Also, with `[push] lan = true` (see [From another host](#from-another-host)) |

The listeners open and close as the config changes, with no restart. Neither port is advertised over mDNS. The loopback port can't be reached from another host, and the LAN port never speaks plain HTTP.

## Requests

Every request carries the token as `Authorization: Bearer <token>`. Bodies are JSON.

| Request | Body | Answer |
|---|---|---|
| `POST /v1/notes` | `{"title": "Backup done", "body": "4 min", "level": "ok", "id": "backup", "ttl_s": 3600}` | `201` with `{"id": "ha/backup"}`, the note's id on the board |
| `DELETE /v1/notes/{id}` | none | `204`, or `404` |
| `PUT /v1/cards/{id}` | `{"label": "Washer", "value": "12 min", "state": "ok"}` | `204`, or `409` when the integration has its 4 cards or the board's 8 slots are taken |
| `DELETE /v1/cards/{id}` | none | `204`, or `404` |

Note fields:

- `title` is required.
- `body` is optional, and the board shows it when the note is opened.
- `level` is `info` (the default), `ok`, `warn` or `error`.
- `id` is optional. A note with the same `id` as an earlier one replaces it. Without an `id`, the daemon numbers the note (`ha/1`, `ha/2`, ...).
- `ttl_s` is how long the note stays, from 1 s to 7 days. The default is 1 day.

Card fields: `label` and `value` are required, and `state` is `ok` (the default), `bad` or `unknown`. A card's id is in the path. An `{id}` in a path is percent-encoded, so `wash%20er` is the id `wash er`.

Text is never refused for its characters or length. The daemon turns anything that isn't printable ASCII into `?` (line breaks become spaces) and cuts each field to what the board shows: a title at 47 characters, a body at 159, a card label at 23 and a value at 15. An unknown field is refused, so a misspelled key shows up as a `400`.

The daemon keeps up to 32 notes, at most 16 from one integration. The board shows 12 of them and 8 cards, dealt to the integrations in turn so each gets its share, with the notes newest first. The daemon keeps notes in memory only, so a restart empties the feed until senders post again.

### Errors

Every error answer is JSON, `{"error": "<reason>"}`.

| Status | Why |
|---|---|
| `400` | The body isn't valid JSON for the route, has an unknown field or misses a required one, or the path's escapes are broken |
| `401` | `{"error":"unauthorized"}`, the same for every case: no `Authorization` header, a scheme other than `Bearer`, a wrong token, or a token used on a route its integration doesn't answer |
| `403` | The request came from a web page: it has an `Origin` or `Sec-Fetch-Site` header. curl, the `watchdeck` command and Home Assistant send neither |
| `404` | No such note or card, or a method the route doesn't take |
| `408` | The body took more than 5 s after the headers |
| `409` | The integration has its 4 cards, all 8 card slots are taken, or all 32 notes are other integrations' |
| `413` | The body is over 8 KB (8192 bytes), or over 64 KB on `/v1/grafana` |
| `429` | Too many requests (see the limits) |

### Limits

- One request per connection.
- The headers must arrive within 5 s of connecting (on 47823, the TLS handshake included), or the connection is dropped.
- At most 32 connections at once on each port, and 4 from one address. Past that, a new connection is closed at once. `127.0.0.1` has no per-address cap, since every sender on the desktop shares it.
- 10 requests a second with bursts of 30, per sender address and per integration.
- An address that presents 20 refused `Bearer` tokens within a minute is shut out for the next minute: its new connections are closed at once, whatever token they carry. Requests with no token or another scheme don't count. `127.0.0.1` is never shut out, so one broken script on the desktop can't block the others; it keeps its rate limit.

The daemon logs one line per accepted request: the integration, the route and the status, such as `push ha: POST /v1/notes 201`. It never logs a token, a header or a body.

## The watchdeck command

The app's own binary posts to the loopback port, so it works on the desktop where the app runs:

```text
watchdeck notify  [--integration ID] [--level info|ok|warn|error] [--id ID] [--ttl SECONDS] TITLE [BODY]
watchdeck card    [--integration ID] [--state ok|bad] [--label LABEL] ID VALUE
watchdeck dismiss [--integration ID] ID
```

- `notify` adds a note and prints its id on the board. `dismiss` takes the note's own id (`backup`, not `ha/backup`).
- `card` sets a card. Its label is the id unless `--label` names one.
- The token is the `WATCHDECK_TOKEN` environment variable when set. Otherwise the command reads `config.toml` and `secrets.toml` from the config folder, with the same 0600 check as the daemon, and takes the token of the push integration `--integration` names, or of the only one.
- Before it sends the token, the command checks (from `/proc/net/tcp`) that the socket listening on `127.0.0.1:47822` is yours, so another user on the machine can't collect the token while the app isn't running.
- Exit codes: `0` when the daemon took the request, `2` when nothing listens on `127.0.0.1:47822` (the app isn't running, or has no push integration) or another user's program does (`something else is listening on 127.0.0.1:47822`), and `1` for anything else, with the reason on stderr.

The command starts no window and doesn't need the app's window to be open.

```sh
watchdeck notify --level ok "Backup done" "4 min, 12 GB"
watchdeck card --state bad nas-disk "91%"
watchdeck dismiss backup
```

### cron

A cron job runs as you with `HOME` set, so the command finds the token in the config folder, as it does in a terminal. Set `XDG_CONFIG_HOME` in the crontab if the app runs with one. Cron's `PATH` is short, so name the binary by its full path (where the package installed the app):

```text
0 3 * * * /usr/bin/watchdeck notify --id backup --ttl 43200 Backup "$(/usr/local/bin/backup.sh 2>&1 | tail -n 1)"
```

With `--id backup`, each night's note replaces the last one. A crontab line can't hold a bare `%`, so keep those inside the script.

Don't write `WATCHDECK_TOKEN=...` into a crontab line: the crontab, cron's mail and your backups would all hold the token. Let the command read it from the config folder.

### curl

Give curl the header on stdin with `-K -`, so the token is never on a command line, where other users on the machine can read it (`ps`, `/proc/<pid>/cmdline`). `printf` is built into the shell, so it doesn't put the token on one either:

```sh
printf 'header = "Authorization: Bearer %s"\n' "$WATCHDECK_TOKEN" |
  curl -sS -K - \
    -H "Content-Type: application/json" \
    -d '{"title": "Deploy finished", "level": "ok"}' \
    http://127.0.0.1:47822/v1/notes
```

## From another host

To post from Home Assistant or anything else on the LAN, turn on the LAN listener in `config.toml`:

```toml
[push]
lan = true
```

The daemon then also serves HTTPS on port 47823 of every interface, with the same certificate the boards pin (`identity.pem` in the config folder). Open 47823/TCP in the desktop's firewall if one runs.

Plain HTTP to 47823 gets no answer. But a sender that used `http://` instead of `https://` has already sent its token across the LAN in clear, so replace that token. The daemon logs `plain HTTP on port 47823 from <ip>; replace that sender's token` when it happens.

The certificate is self-signed and names no host, so a client can't verify it the usual way. Check its public key instead. On the desktop:

```sh
openssl x509 -in ~/.config/watchdeck/identity.pem -pubkey -noout \
  | openssl pkey -pubin -outform der | openssl dgst -sha256 -binary | base64
```

Then from the other host, `-k` skips the name check and `--pinnedpubkey` still refuses any other key:

```sh
printf 'header = "Authorization: Bearer %s"\n' "$WATCHDECK_TOKEN" |
  curl -sS -K - -k --pinnedpubkey "sha256//<the base64 from above>" \
    -H "Content-Type: application/json" \
    -d '{"title": "From the LAN"}' \
    https://desktop.lan:47823/v1/notes
```

The `fp` the daemon logs at startup (`TLS identity in ..., fp=...`) and advertises over mDNS is something else: the first 16 hex digits of SHA-256 over the text of `identity.pem`, which `sha256sum ~/.config/watchdeck/identity.pem | cut -c1-16` prints. It tells you which identity the daemon has, and is not the value to pin.

The certificate changes only when `identity.pem` and `identity.key` are deleted, which also makes every board pair again.

### Home Assistant

If Home Assistant runs on the same machine as the app, leave `lan` off and point it at `http://127.0.0.1:47822` instead (drop `verify_ssl` from the examples below). The token then never leaves the machine.

From another host, Home Assistant's `rest_command` can't pin a key, so it needs `verify_ssl: false`. With that, anyone on the LAN who can intercept the traffic between Home Assistant and the desktop gets the token: they pose as the daemon, and Home Assistant hands it over. Use it only on a LAN you trust, and give Home Assistant its own push integration, so its token can be replaced alone.

In `secrets.yaml`:

```yaml
watchdeck_auth: "Bearer <the token>"
```

In `configuration.yaml`:

```yaml
rest_command:
  watchdeck_notify:
    url: "https://desktop.lan:47823/v1/notes"
    method: post
    verify_ssl: false
    headers:
      authorization: !secret watchdeck_auth
    content_type: "application/json"
    payload: >-
      {"title": {{ title | to_json }}, "body": {{ (message | default('')) | to_json }},
       "level": {{ (level | default('info')) | to_json }}}
  watchdeck_card:
    url: "https://desktop.lan:47823/v1/cards/{{ id | urlencode }}"
    method: put
    verify_ssl: false
    headers:
      authorization: !secret watchdeck_auth
    content_type: "application/json"
    payload: >-
      {"label": {{ label | to_json }}, "value": {{ value | string | to_json }},
       "state": {{ (state | default('ok')) | to_json }}}
```

An automation's or a script's action then calls it:

```yaml
- action: rest_command.watchdeck_notify
  data:
    title: "Washer done"
    message: "The laundry is ready"
    level: ok
```

## Prometheus

A `prometheus` integration runs one instant query every `every_s` seconds (2 to 86400) and shows the first value as a card. It sends `GET <url>/api/v1/query?query=<the query, URL-encoded>`, so `url` is the server's base, such as `http://prom.lan:9090` (a path prefix such as `http://host/prometheus` is kept), with no query or fragment of its own.

```toml
[[integration]]
id = "cpu"
kind = "prometheus"
label = "CPU"
url = "http://prom.lan:9090"
query = '100 - avg(rate(node_cpu_seconds_total{mode="idle"}[5m])) * 100'
every_s = 30
decimals = 1
suffix = "%"
bad_above = 90
```

| Key | Needed | Meaning |
|---|---|---|
| `url`, `query`, `every_s`, `label`, `id` | yes | The server's base URL, the PromQL expression (up to 2000 bytes), the period, and the card's name |
| `decimals` | no | 0 to 3 digits after the point. Without it, the value shows as Prometheus gives it |
| `suffix` | no | Up to 7 printable ASCII characters after the value, such as `%`, or a space and `MB` |
| `bad_above`, `bad_below` | no | The card is bad when the value is above the first or below the second. Whole or decimal numbers |
| `secret_headers` | no | Header name to the name of a secret in `secrets.toml`, as an HTTP action's, such as `{ Authorization = "prom-token" }` for a server behind a bearer token |
| `insecure_http`, `insecure_tls`, `timeout_s` | no | As an HTTP action's: `insecure_http` lets `secret_headers` cross the network over plain `http://`, `insecure_tls` accepts a self-signed certificate on `https://`, and `timeout_s` bounds the whole request: at most 30 and at most `every_s`, default 10 or `every_s` if less |

- The query must return one series, or a scalar. A `scalar` shows its value, and a `vector` its one series. A vector of several series is bad, with `N series, expected 1` (aggregate with `sum`, `avg` or `topk(1, ...)`). A `matrix` or a `string` result is bad, with a reason on the card.
- A value of `NaN` or `±Inf` shows as it is, in the unknown color, since no threshold can judge it. With `bad_above` or `bad_below` set, it is bad.
- The card is bad, with "no data", when the query returns no series. When the server answers with an error status, it is bad with the status and the reason from the body: Prometheus answers `400`, `422` and `503` with `{"status":"error","errorType":...,"error":...}`, so the card reads `HTTP 400: parse error at char 5 ...`. The board's card value holds 15 characters, so a long reason is cut. When the server can't be reached it is bad with the failure (`request failed: ...`, `timed out after 10 s`). Prometheus's `error` text also shows when it answers `200` with `"status":"error"`.
- Header values from `secret_headers` are blanked from anything the card shows. The URL never appears in an error.
- The polls run in the same slots as the other polled integrations and the tiles: at most 2 at a time, so a query that ran out its `timeout_s` holds one of the two until then, which is why the timeout can't pass the period. A poll that finds both taken is skipped. After three polls in a row that couldn't run, the card turns unknown.

## Grafana

A `grafana` integration shows Grafana-managed alerts as notes. It is a push kind: Grafana's webhook contact point POSTs its alerts to the push API, with the integration's token, and the daemon turns each alert into a note.

```toml
[[integration]]
id = "graf"
kind = "grafana"
label = "Grafana"
token_secret = "push-grafana"
levels = { page = "error" }       # optional
```

Add the token as for a push integration: a secret of at least 32 characters. It can't be shared with another push or grafana integration.

### The contact point

In Grafana, under Alerting, Contact points, add a contact point of type Webhook:

| Setting | Value |
|---|---|
| URL | `http://127.0.0.1:47822/v1/grafana` when Grafana runs on the same machine as the app (not in a container, where `127.0.0.1` is the container). Otherwise turn on [`lan`](#from-another-host) and use `https://desktop.lan:47823/v1/grafana` |
| HTTP method | `POST` (`PUT` works too) |
| Authorization header: scheme | `Bearer` (the default) |
| Authorization header: credentials | the token, the value of the secret `token_secret` names |

Grafana verifies the certificate on an `https://` URL, and the daemon's is self-signed and names no host. Use the loopback URL where you can. Otherwise use the contact point's TLS setting for a self-signed certificate if your Grafana version has one, knowing that anyone on the LAN who can intercept the traffic then gets the token.

Leave Grafana's "Max Alerts" at 0, or at a number you accept: alerts past it are never sent. Use the contact point's Test button: it sends a test alert, which shows as a note.

### What an alert becomes

- **A firing alert** adds a note with the id `graf/<fingerprint>`, so the notifications Grafana repeats while an alert fires replace one note and don't pile up. The note stays for the default 1 day, and Grafana's repeat keeps it fresh.
- **A resolved alert** removes the note. Resolving an alert whose note is gone already (dismissed on a board, expired, or lost to a daemon restart) is not an error.
- The **title** is `labels.alertname`, followed by a colon, a space and `annotations.summary` when there is one. The **body** is `annotations.description`.
- The **level** comes from the `severity` label, not case-sensitively: `critical` is `error`, `warning` is `warn`, and anything else, or none, is `info`. `levels` adds to this or changes it: each key is a severity, each value `info`, `ok`, `warn` or `error`. Two keys that differ only in case are refused.
- Only `alerts` is required in the payload. Other fields, at the top and in an alert, are ignored if the daemon doesn't know them, since Grafana adds fields. A body without `alerts`, with invalid JSON, or with an alert whose `status` isn't `firing` or `resolved` is a `400`. An alert with no usable fingerprint gets an id from a hash of its labels, so alerts of one rule with different labels stay separate notes. One bad alert refuses the whole payload.
- The route takes bodies up to 64 KB, not the 8 KB of the others, because a payload carries every alert of a group (about 1 KB each). The integration's limits apply: at most 16 notes, and an alert past that replaces the oldest of the integration's own, so a flood can't push out another integration's notes.

## Adding a source

For developers: every source is a `kind` in `IntegrationConfig` (`daemon/config.rs`) with its rules in `check`, and one implementation of `Connector` (a polled source) or `Receiver` (a push source) in `daemon/integrations/`, built by one arm of `build`. A `Receiver` names the first path segments it answers under `/v1/` in `routes()`, and the listener refuses a token on any other route. The `stat` source (`integrations/stat.rs`) is the smallest polled example, and `push` (`integrations/push.rs`) the receiving one. Nothing in the feed store, the sessions or the board changes for a new source.

The two interfaces have a few hooks beyond that, each with a default for a source that doesn't need it:

- A `Connector` that needs the whole reply of an HTTP request, not a tile's one value, takes the `Fetch` trait's `fetch` instead of `Sample`, which runs the request in a feed slot with the actions' client, timeouts and redaction. `prometheus` is the example: its config builds the request as an HTTP action (`IntegrationConfig::prometheus_action`).
- A `Receiver` that needs bodies bigger than 8 KB overrides `max_body_bytes`. The listener reads the bigger body only after the token has picked the integration. `grafana` is the example.
- A `Receiver` that reports the end of something, such as a resolved alert, returns `Update::ClearNote` instead of `Update::RemoveNote`, which removes the note without a `404` when it is already gone.
- A kind with a token names it in `IntegrationConfig::token_secret`, which is what the listener and the shared-token rule read.
