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
tile = { every_s = 30, source = { kind = "shell", argv = ["sh", "-c", "df --output=pcent / | tail -n 1"] } }

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

[button.tile]
every_s = 15
json_pointer = "/state"

[button.tile.source]
kind = "http"
method = "GET"
url = "https://ha.lan:8123/api/states/light.office"
secret_headers = { Authorization = "ha" }
```

The Disk button shows how full `/` is, and the Office button shows whether the light is `on` or `off`; see [Tiles](#tiles). The last button needs a secret named `ha`; see [Secrets](#secrets).

## Changing the config

Save the file and the change applies within about a second. The app checks `config.toml` and `secrets.toml` every half second. A paired device redraws its grid on the same connection, without reconnecting. An edit that changes only actions applies from the next press and leaves the screen alone.

A version that breaks any rule on this page doesn't apply. The last good config stays live, and the app logs every problem it found:

```text
[2026-10-11T02:14:49Z ERROR watchdeck_lib::daemon::config] /home/you/.config/dev.rackrot.watchdeck/config.toml not applied, keeping the live config: button "disk": pos [5, 0] is outside the 5x3 grid
```

- If the file breaks a rule when the app starts, the device shows an empty grid until you fix it.
- Deleting the file while the app runs keeps the last good config. The next start writes the default again.

### Saving from the window

The Layout page writes `config.toml` for you. A save rewrites the whole file from the config on screen, so **it does not keep your comments** (the file starts with a note saying so). A save is refused, and nothing is written, if the file changed on disk since the page loaded it.

If the file on disk breaks a rule, the page says so and offers **Replace config.toml**. It asks you to confirm what it will write, then saves the config on screen over the file. If the file was already broken when the app started, that config is an empty grid, not your file. The text it replaces is kept in the config folder as `config.toml.bak.<UTC time>`, for example `config.toml.bak.20261011T021449Z`. The newest 5 are kept, and each save makes a new one, so an earlier backup is never overwritten. Copy anything you want to keep out of the folder.

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
| `icon` | no | A picture in the `icons/` folder, such as `"3f2a9c1d0e44.png"`; see [Icons](#icons) |
| `action` | yes | A shell or HTTP action, below |
| `tile` | no | Makes the button show a live value; see [Tiles](#tiles) |

Unknown keys are errors, so a misspelled key is caught rather than ignored.

## Icons

A button can show a picture above its label. The picture is a PNG, JPEG or WebP file in `~/.config/dev.rackrot.watchdeck/icons/`, and `icon` names the file:

```toml
[[button]]
id = "plex"
pos = [0, 0]
label = "Plex"
icon = "plex.png"
action = { kind = "shell", argv = ["ssh", "nas", "docker", "restart", "plex"] }
```

To add one, either copy the file into `icons/` yourself (make the folder if it's missing) and name it in `config.toml`, or import it, which copies it there under a name made from its content, such as `3f2a9c1d0e44.png`. The config editor's Import button does the import. Its icon picker lists the files in `icons/` that the app would accept (a valid name, a regular file inside the folder, and a readable PNG, JPEG or WebP), and reads the folder again after an import.

- `icon` is a plain file name: 1 to 63 letters, digits, `_`, `-` or `.`, not starting with a dot. It can't point outside `icons/`.
- Only PNG, JPEG and WebP are read. The kind comes from the file's contents, not its name. SVG isn't supported.
- A file must be a regular file of at most 2 MB, and the picture at most 4096 × 4096 pixels. Bigger ones are refused, not scaled.
- Transparency is kept, so a PNG or WebP with a see-through background shows the button's color behind it.
- The app shrinks a picture that is bigger than its box, keeping its shape, and leaves a smaller one as it is. It never scales a picture up. A JPEG is turned upright by its EXIF orientation first.
- The box is a square above the label. Its side is what the button has left after 4 px of padding on each side, a 2 px gap and the label, and it never exceeds 256 px or the button's width less the padding. The label gets 2 lines (35 px each), or 1 line where 2 lines would leave the icon under 24 px. A longer label ends in dots. So the box is 256 px on 1 row, 110 on 2 rows, 44 on 3 rows (the default 5×3 grid, labels up to 2 lines), 46 on 4 rows and 26 on 5 rows (8×5, labels of 1 line). On a single row of 4 or more columns the button's width limits it instead, for example 238 px on 4 columns.
- A WebP's declared sizes are checked before it is decoded, because the decoder would trust them. The app reads the headers (the canvas, each animation frame and the picture data) and refuses a picture over 4096 pixels on a side, one whose headers disagree, or one that breaks off.
- Buttons that name the same file share one picture on the device.
- A missing, unreadable or invalid file holds back the whole config, like any other broken rule, and the app logs which button and file.
- Adding or removing a file in `icons/`, or changing one a button names (its modified time or size), reloads the config once the change has held for two polls. That covers replacing a picture in place, and a file that is still being copied is read again when it finishes. A different picture gets a new id, so the device fetches it and drops the old one.

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

## Tiles

A tile puts a live value on a button, such as a load average, a disk percentage or whether a service is up. Pressing the button still runs its `action`. The tile has its own `source`, which can be any shell or HTTP action:

| Key | Required | Rule |
|---|---|---|
| `every_s` | yes | Seconds between polls, 2 to 86400 |
| `json_pointer` | no | An [RFC 6901](https://www.rfc-editor.org/rfc/rfc6901) pointer such as `/state` or `/attributes/brightness`. It must start with `/` |
| `source` | yes | A shell or HTTP action, with the keys above. Its `timeout_s` applies to each poll. A source that sends secret headers over plain http needs `insecure_http`, as for any action |

### When it polls

- Only while a device is connected. With no device there are no polls and no traffic. When the first device connects, every tile polls at once. When the last one leaves, polling stops and a poll under way is cancelled.
- A tile never starts a poll less than 2 s after its previous poll began, whatever asks for one: a press, a device that connects and drops again, or a short `every_s`. The app remembers when each tile last polled across those stops and starts. Editing a tile is the exception and polls at once.
- Each tile has one poll at a time. The next waits `every_s` seconds after the previous one ends, so a slow source stretches the interval instead of piling up.
- A press of the button polls its tile again once the press has finished, because the press probably changed what it shows, and once the 2 s above are up. A press that is refused ("already running", "busy") does not.
- Polls share the 8 action slots with presses, but at most 6 tile polls run at once, so a press always finds 2 slots free. A poll that gets no slot is skipped and tried again after `every_s`. After 3 skipped polls in a row the tile shows `stale` in red, instead of an old value that may no longer be true, until a poll runs. A poll and a press of the same button don't block each other.
- Editing a tile restarts it with a poll at once, and the first value from the new source replaces what the old one showed. A device never gets a value that the old source produced once the app has applied the edit.

### What it shows

- A good poll (exit 0, or a 2xx reply) shows its value. Without `json_pointer` that is the first line of output that isn't blank. With it, the output is read as JSON and the value is the string, number or boolean at the pointer. The pointer reads the output as it came, so keys with non-ASCII characters match. A missing pointer, a `null`, an object or an array, or output that isn't JSON, is a bad poll. A good poll with no value shows `ok`.
- A bad poll shows what went wrong: for a shell source its first line of output if it printed one, so a check can print `DOWN` and exit 1, and otherwise the summary (`exit 3`, `HTTP 503`, `timed out after 10 s`). For an HTTP source it is always the summary, never the reply.
- The text is one line of printable ASCII, cut at 64 characters. Other characters show as `?`. The device shows what fits and ends a longer value in dots.
- A shell source's output is its stdout followed by its stderr, and the app reads at most the last 64 KB of it. So a `json_pointer` over a command that writes progress to stderr, such as `curl`'s progress meter, finds the output isn't JSON. Use `curl -s`, or `2>/dev/null`. An HTTP reply is read up to 64 KB, and a JSON body longer than that doesn't parse.
- Secrets in a reply show as `<secret>`, as for any HTTP action. That includes a secret the reply escaped (`\/` or `\u0041`) which the pointer then decodes: the app blanks the picked value against the secrets the request used, before it goes to the device or the log.

### How the device draws it

- The label shrinks to one line and the value goes under it.
- A tile is grey until its first value arrives, then keeps the button's `bg` while polls are good and turns red while they are bad. If the app goes away, the grid greys out with the rest of the screen.
- A button with an icon and a tile on a grid of 4 or 5 rows has room for the icon and the value only, so its label is hidden. On 1 to 3 rows it keeps its label.
- The app logs a line at info level whenever a tile's value changes, and when polling starts and stops.

## Secrets

`secrets.toml` holds one `name = "value"` line per secret:

```toml
ha = "Bearer paste-your-token-here"
```

- The file must have mode 0600, or the app won't read it and logs the fix: `chmod 600 ~/.config/dev.rackrot.watchdeck/secrets.toml`.
- A value goes out as an HTTP header, so it must be printable ASCII on one line, up to 4096 bytes. The app won't read a file with any other value, and the error names the secret.
- A button that names a missing secret holds back the whole config. Adding the secret applies it.
- The Secrets page can remove a secret, after a confirmation. It refuses while the running config still names the secret in a button's action or a tile's source, and the message lists the buttons. Change those buttons first, then remove it.
- Secret values never appear in logs or error messages. Errors name the secret, and a syntax error in `secrets.toml` gives only its line number.
- On the device, exact copies of a value in a reply show as `<secret>`, even one cut short by the 64 KB limit. An encoded copy, such as base64 or URL-encoded, isn't caught.
- The file is plain text. Anything running as your user can read it.

## Results

Every press shows a result on the device: the button's label with OK or Failed, a one-line summary with the time taken, and the end of the output.

The config editor's Test button runs a button's saved action the same way, with the same limits and result text, and shows the result in the editor. No press result is sent to any device; a button with a tile polls it again, as a press does. It is off while the form differs from the saved button.

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
