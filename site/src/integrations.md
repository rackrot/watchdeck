# Integrations and the push API

A right swipe on page 1 of the board shows the feed page: notes (notifications) and cards (single values). They come from the `[[integration]]` tables in `config.toml`, which [Buttons and actions](config.md#integrations) describes:

- A `stat` integration polls a command or a URL and shows the value as a card.
- A `push` integration takes notes and cards that scripts, cron jobs and Home Assistant send to the daemon's push API, over HTTP or with the `watchdeck` command.

This page covers the push API, the `watchdeck` command and the LAN option.

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
| `413` | The body is over 8 KB (8192 bytes) |
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

## Adding a source

For developers: every source is a `kind` in `IntegrationConfig` (`daemon/config.rs`) with its rules in `check`, and one implementation of `Connector` (a polled source) or `Receiver` (a push source) in `daemon/integrations/`, built by one arm of `build`. A `Receiver` names the first path segments it answers under `/v1/` in `routes()`, and the listener refuses a token on any other route. The `stat` source (`integrations/stat.rs`) is the smallest polled example, and `push` (`integrations/push.rs`) the receiving one. Nothing in the feed store, the sessions or the board changes for a new source.
