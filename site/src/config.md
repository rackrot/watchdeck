# Buttons and actions

The desktop app reads the button grid, and what each button does, from `config.toml`. HTTP actions can take header values from `secrets.toml`, which keeps tokens out of `config.toml`. Both files live in `~/.config/dev.rackrot.watchdeck/`.

On its first run the app writes a commented `config.toml` with one button, Hello, which shows a desktop notification.

## Example

```toml
version = 1

[layout]
cols = 5
rows = 3

[[button]]
id = "plex"
pos = [0, 0]
label = "Restart Plex"
bg = "#e65100"
action = { kind = "shell", argv = ["ssh", "nas", "docker", "restart", "plex"], timeout_s = 60 }

[[button]]
id = "disk"
pos = [1, 0]
label = "Disk"
action = { kind = "shell", argv = ["sh", "-c", "df -h / | tail -n 1"] }

[[button]]
id = "office-light"
pos = [2, 0]
label = "Office"

[button.action]
kind = "http"
method = "POST"
url = "https://ha.lan:8123/api/services/light/toggle"
body = '{"entity_id":"light.office"}'
headers = { "Content-Type" = "application/json" }
secret_headers = { Authorization = "ha" }
```

The last button needs a secret named `ha`; see [Secrets](#secrets).

## Changing the config

Save the file and the change applies within about a second. The app checks `config.toml` and `secrets.toml` every half second. A paired device redraws its grid on the same connection, without reconnecting. An edit that changes only actions applies from the next press and leaves the screen alone.

A version that breaks any rule on this page doesn't apply. The last good config stays live, and the app logs every problem it found:

```text
[2026-10-11T02:14:49Z ERROR watchdeck_lib::daemon::config] /home/you/.config/dev.rackrot.watchdeck/config.toml not applied, keeping the live config: button "disk": pos [5, 0] is outside the 5x3 grid
```

- If the file breaks a rule when the app starts, the device shows an empty grid until you fix it.
- Deleting the file while the app runs keeps the last good config. The next start writes the default again.

## Grid

| Key | Rule |
|---|---|
| `version` | `1` |
| `layout.cols` | 1 to 8 |
| `layout.rows` | 1 to 5 |

## Buttons

Each `[[button]]` table is one cell.

| Key | Required | Rule |
|---|---|---|
| `id` | yes | 1 to 63 letters, digits, `_`, `-` or `.`, unique |
| `pos` | yes | `[column, row]`, counted from 0 at the top left. Inside the grid, one button per cell |
| `label` | yes | Printable ASCII, at most 47 characters. The device's fonts have no other characters |
| `bg` | no | Background as `#rrggbb`. Default `#1e88e5` |
| `fg` | no | Label color as `#rrggbb`. Default `#ffffff` |
| `action` | yes | A shell or HTTP action, below |

Unknown keys are errors, so a misspelled key is caught rather than ignored. Icons and live tiles aren't supported yet.

## Shell actions

| Key | Required | Rule |
|---|---|---|
| `kind` | yes | `"shell"` |
| `argv` | yes | The program, then its arguments. Not empty |
| `timeout_s` | no | 1 to 600 seconds. Default 10 |

- `argv` runs directly, without a shell, so `|`, `>`, `*` and `$HOME` have no special meaning. For shell syntax, run a shell yourself: `["sh", "-c", "df -h / | tail -n 1"]`.
- The command runs in your home directory, with the app's environment and stdin closed.
- When the app starts at login, its environment may lack your shell's `PATH` additions and `SSH_AUTH_SOCK`. Use absolute paths for programs outside the system `PATH`, and an SSH key that works without the agent.
- A button runs once at a time. A press while it runs gets "already running". At most 8 actions run at once, and a press beyond that gets "busy".
- On timeout the app sends SIGTERM to the command and every process it started, then SIGKILL 2 s later. A process that leaves the group, for example through `setsid`, escapes this.
- A command that starts a background job finishes when it exits, and the job keeps running. The job's output pipes close, so redirect them: `["sh", "-c", "backup.sh >/tmp/backup.log 2>&1 &"]`.

## HTTP actions

| Key | Required | Rule |
|---|---|---|
| `kind` | yes | `"http"` |
| `method` | yes | `GET`, `HEAD`, `POST`, `PUT`, `PATCH` or `DELETE` |
| `url` | yes | `http://` or `https://`, with a host |
| `body` | no | Sent as written, at most 64 KB. No `Content-Type` is added; set one in `headers` |
| `headers` | no | Header name → value, sent as written, e.g. `{ "Content-Type" = "application/json" }`. Values are printable ASCII |
| `secret_headers` | no | Header name → secret name, e.g. `{ Authorization = "ha" }` |
| `insecure_http` | no | `true` lets `secret_headers` go over `http://` to another machine. Default `false` |
| `timeout_s` | no | 1 to 600 seconds for the whole exchange. Default 10 |

- A header name may appear only once across `headers` and `secret_headers`, ignoring case.
- Redirects aren't followed, so a 3xx reply is a failure.
- Proxy settings are ignored, so a request goes only to the host in its URL. Over `https://`, only a server that proves it is that host gets it.
- HTTPS checks the server's certificate against the system's trusted roots. Self-signed certificates aren't supported yet.
- The app reads at most 64 KB of the reply.

### Secrets over plain http

The app refuses a button whose `secret_headers` would go over `http://` to another machine. Over plain http, anyone on the network path can read the header. So can anyone who answers for the host name first, which is easy on a LAN, above all for `.local` names. Use `https://`, or a URL on this machine (`localhost`, `127.0.0.1` or `[::1]`), which needs no setting.

To send them anyway, set `insecure_http = true` on the action. The app then logs a warning whenever a config with that button goes live.

## Secrets

`secrets.toml` holds one `name = "value"` line per secret:

```toml
ha = "Bearer paste-your-token-here"
```

- The file must have mode 0600, or the app won't read it and logs the fix: `chmod 600 ~/.config/dev.rackrot.watchdeck/secrets.toml`.
- A value goes out as an HTTP header, so it must be printable ASCII on one line, up to 4096 bytes. The app won't read a file with any other value, and the error names the secret.
- A button that names a missing secret holds back the whole config. Adding the secret applies it.
- Secret values never appear in logs or error messages. Errors name the secret, and a syntax error in `secrets.toml` gives only its line number.
- On the device, exact copies of a value in a reply show as `<secret>`, even one cut short by the 64 KB limit. An encoded copy, such as base64 or URL-encoded, isn't caught.
- The file is plain text. Anything running as your user can read it.

## Results

Every press shows a result on the device: the button's label with OK or Failed, a one-line summary with the time taken, and the end of the output.

| Outcome | Summary |
|---|---|
| The command exited | `exit 0` is OK, any other code fails |
| The command ran out of time | `timed out after 60 s` |
| A signal killed the command | `killed by signal 9` |
| The program wasn't found | `could not start ssh: No such file or directory (os error 2)` |
| The server answered | `HTTP 200`; only a 2xx status is OK |
| The request failed | `request failed: …`, without the URL |

- The output is the last 1 KB of stdout followed by stderr, or of the HTTP body. Characters other than printable ASCII show as `?`.
- An OK result closes after 4 s and a failure after 10 s. A tap closes it sooner.

## Security

- The device sends only a button id. Nothing it sends reaches a command line, URL, header or body.
- Anyone who can edit `config.toml` can run commands as you, so keep it as private as your shell startup files.
