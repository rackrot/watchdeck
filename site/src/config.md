# Buttons and actions

The desktop app reads the button grid, and what each button does, from `config.toml`. HTTP actions can take header values from `secrets.toml`, which keeps tokens out of `config.toml`. Both files live in `$XDG_CONFIG_HOME/watchdeck/`, which is `~/.config/watchdeck/` when `XDG_CONFIG_HOME` is unset, empty or not an absolute path. Earlier versions kept them in `~/.config/dev.rackrot.watchdeck/`. On first start the app moves that whole folder to the new place, so paired devices stay paired. If the move fails, the app doesn't start and names both folders.

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
[2026-10-11T02:14:49Z ERROR watchdeck::config_rejected] /home/you/.config/watchdeck/config.toml not applied, keeping the live config: button "disk": pos [5, 0] is outside the 5x3 grid
```

- If the file breaks a rule when the app starts, the device shows an empty grid until you fix it.
- Deleting the file while the app runs keeps the last good config. The next Save in the window recreates the file, and the next start writes the default again.

### Saving from the window

The Layout page writes `config.toml` for you. A save rewrites the whole file from the config on screen, so **it does not keep your comments** (the file starts with a note saying so). A save is refused, and nothing is written, if the file changed on disk since the page loaded it.

If the file on disk breaks a rule, the page says so and offers **Replace config.toml**. It asks you to confirm what it will write, then saves the config on screen over the file. If the file was already broken when the app started, that config is an empty grid, not your file. The text it replaces is kept in the config folder as `config.toml.bak.<UTC time>`, for example `config.toml.bak.20261011T021449Z`. The newest 5 are kept, and each save makes a new one, so an earlier backup is never overwritten. Copy anything you want to keep out of the folder.

## Grid

| Key | Rule |
|---|---|
| `version` | `1` |
| `layout.cols` | 1 to 8 |
| `layout.rows` | 1 to 5 |
| `layout.font` | The default font of every button; see [Fonts](#fonts). Default `montserrat` |

## Buttons

Each `[[button]]` table is one cell.

| Key | Required | Rule |
|---|---|---|
| `id` | yes | 1 to 63 letters, digits, `_`, `-` or `.`, unique |
| `pos` | yes | `[column, row]`, counted from 0 at the top left. Inside the grid, one button per cell of its page |
| `label` | yes | Printable ASCII, at most 47 characters. The device's fonts have no other characters |
| `bg` | no | Background as `#rrggbb`. Default `#1e88e5` |
| `fg` | no | Label color as `#rrggbb`. Default `#ffffff` |
| `icon` | no | A picture in the `icons/` folder, such as `"3f2a9c1d0e44.png"`; see [Icons](#icons) |
| `font` | no | The font of the label, and of a tile's value unless the tile sets its own; see [Fonts](#fonts). Default: the grid's font |
| `page` | no | The `id` of a `[[page]]` table; see [Pages](#pages). Default: the first page |
| `action` | yes | A shell or HTTP action, below |
| `tile` | no | Makes the button show a live value; see [Tiles](#tiles) |

Unknown keys are errors, so a misspelled key is caught rather than ignored.

## Pages

A grid can have up to 4 pages, and the device shows one at a time. Declare them with `[[page]]` tables, and put each button on one with its `page` key:

```toml
[[page]]
id = "main"

[[page]]
id = "media"
label = "Media"

[[button]]
id = "plex"
page = "media"
pos = [0, 0]
label = "Plex"
action = { kind = "shell", argv = ["ssh", "nas", "docker", "restart", "plex"] }
```

| Key | Required | Rule |
|---|---|---|
| `id` | yes | 1 to 63 letters, digits, `_`, `-` or `.`, unique among pages |
| `label` | no | Printable ASCII, at most 24 characters. The config editor's tab shows it instead of the id. It is never sent to a device, and changing it changes nothing on one |

- With no `[[page]]` table the grid has one page, as before, and a button may not set `page`.
- With `[[page]]` tables there are 1 to 4 pages, in file order. The first is the home page, and a button without `page` goes on it.
- One button per cell *of a page*: the same `pos` on two pages is fine. A button without `page` counts as being on the first page, so it collides with a button that names that page.
- Button ids stay unique across all pages.
- At most 40 buttons across all pages have a `tile`, which keeps the polling load where it was. Every page's tiles poll, whether or not the page is on screen.
- The grid is the same size on every page (`layout.cols` by `layout.rows`).
- A layout of 2 or more pages gives the grid 31 px less height, for a "Page x of y" label under it, so icon boxes get smaller; see [Icons](#icons). Going from one page to two changes the box of every icon, so every SVG and each raster bigger than its new box gets a new id, and the device fetches it again. A smaller raster keeps its id. A single `[[page]]` table keeps the full-height grid.
- An error names the page, as `page "media": another page has this id`, or the button, as `button "plex": page "music" is not one of the [[page]] ids`. Other counts read `5 pages, at most 4` and `41 tiles across all pages, at most 40`.
- On a device with pages, a sideways swipe turns the page: left for the next, right for the previous. Past the first or last page a swipe does nothing. A swipe never presses a button, and nor does any touch that moves more than 20 px in any direction. A drag at least 40 px sideways, and more than twice as far sideways as up or down, also turns the page. Turning a page doesn't wait for the app, so it works while the device is offline. A reconnect keeps the page on screen, and a restart shows the first page.

### Older firmware and older apps (pages)

- A device on firmware without pages shows the first page only, whatever the config holds, and gets nothing for edits to the other pages. With 2 or more pages the icons are rendered for the shorter paged grid, so such a device shows them smaller than its full-height box (see the paged sizes under [Icons](#icons)).
- An app from before pages refuses a `config.toml` with a `[[page]]` table or a `page` key, because it rejects keys it doesn't know, and keeps its last good config (an empty grid on a fresh start). Before going back to an older app, remove the `[[page]]` tables and every `page` key, and keep the buttons you want on the first page.

## Status bar and settings

A paired device with a grid shows a bar 36 px high above it. From left to right the bar shows:

- `CPU 12% 64°C`, the desktop's processor use and temperature.
- `RAM 45%`, its memory in use as a percent.
- `GPU 23% 41°C`, the selected GPU's busy share and temperature. See [Which GPU](#which-gpu).
- `▼1.2M ▲340.0k`, network rates for the interface of the desktop's default route. `▼` is received and `▲` is sent. The unit is set in [Status bar settings](#status-bar-settings).
- `Wed 22 Oct 14:32`, the desktop's date and time in 24-hour form.

A value shows as `--` in its place when the desktop hasn't sent it, when the last frame is more than 5 s old, or when the GPU can't be read. With `gpu = "off"` or no readable GPU, both GPU values read `--`. The `°C` or `°F` after a temperature stays on the bar.

The desktop sends the host values once a second while the device is connected. While the link is down, the daemon's status text takes the place of the host values, and the clock keeps running on the device.

Swipe up on the grid to open settings. A swipe counts when it moves at least 200 px up and more than twice as far up as it moves sideways. Its touch doesn't press a button or turn a page. Settings shows the device's firmware, uptime and memory, the status of Wi-Fi, the desktop, the SD card, USB and touch, and the layout revision. Its buttons are Licenses, Forget pairing (the same 2 s hold as before) and Close. Settings closes with Close, with a swipe down, or after 60 s without a touch (the idle close waits while the license viewer is open), and then the grid comes back. An unpaired device closes settings too.

Until the first layout arrives, and while the device is unpaired, it shows the status screen instead: the Wi-Fi, SD, USB, touch and daemon lines, with Licenses and, while paired, Forget pairing.

### Status bar settings

An optional `[status_bar]` table chooses the GPU, the units the bar shows, and the colors values take past a threshold. It changes nothing in the grid: a save or edit that touches only `[status_bar]` pushes no layout to a device.

```toml
[status_bar]
gpu = "auto"        # "auto", "off", or a PCI address such as "0000:01:00.0"
temp_unit = "c"     # "c" or "f". The temperature steps below are in this unit
net_unit = "si"     # "si" (1.2M), "binary" (1.1Mi) or "bits" (9.6Mb). The rate steps are in MB/s, MiB/s or Mb/s

[status_bar.thresholds]
mem = [{ at = 80, color = "#ffb300" }, { at = 90, color = "#e53935" }]
cpu_temp = [{ at = 85, color = "#ffb300" }, { at = 95, color = "#e53935" }]
net_rx = []         # never colored
```

When the window saves, it writes `[status_bar]` in its own TOML layout, so the steps may read differently from the example above. The saved file parses back to the same settings.

| Key | Rule |
|---|---|
| `gpu` | `auto` (default), `off`, or a PCI address as `dddd:bb:dd.f` in lowercase hex, such as `0000:01:00.0` |
| `temp_unit` | `c` (default) or `f`. The bar shows `64°C` or `147°F`; the letter is always shown |
| `net_unit` | `si` (default): bytes with 1000-based prefixes, `1.2M`. `binary`: bytes with 1024-based prefixes, `1.1Mi`. `bits`: bits with 1000-based prefixes, `9.6Mb`. Bytes carry no `B`, and bits always carry their `b`, so the two never look alike |
| `thresholds` | A table of up to 3 steps for each metric, below |

#### Formats

The daemon sends temperatures in whole °C and rates in bytes per second. The device formats them in the units you chose. Each value takes a fixed number of cells and is right-aligned in them, so a changing number doesn't move the bar's other groups.

- **Percent** (CPU, RAM and GPU busy): a whole number and `%`, such as `12%`, in 4 cells. Above 100 it shows `100%`.
- **Temperature**: a whole degree in 3 cells, then `°C` or `°F`. The device rounds °F to the nearest degree: 0 °C is 32 °F, 64 °C is 147 °F and -40 °C is -40 °F.
- **`si`**: bytes per second, with no `B`. Under 1000 it's a whole number, such as `999`. Above that the number is divided by 1000 until its integer part is under 1000, then shows one decimal, cut and not rounded: `1.25 MB/s` shows `1.2M`, and `340,000 B/s` shows `340.0k`. A rate takes 6 cells.
- **`binary`**: as `si`, but divided by 1024 and shown with `Ki`, `Mi` and `Gi`. The prefix still changes at 1000, so `1000 KiB/s` shows `0.9Mi`. A rate takes 7 cells.
- **`bits`**: bits per second (bytes times 8), divided by 1000 and shown with `kb`, `Mb` and `Gb`, so `1.2 MB/s` shows `9.6Mb`. Under 1000 it's a whole number with `b`, such as `999b`. A rate takes 7 cells.

The largest rate shown is `999.9` with its prefix, such as `999.9G`.

#### Thresholds

Each metric takes `[{ at = N, color = "#rrggbb" }, ...]`, up to 3 steps with `at` rising. A value at or past a step's `at` is drawn in that step's color, and the highest step it reaches wins. The number is judged as the bar shows it, in the units you chose: with `temp_unit = "f"`, a step at 190 turns red exactly when the bar shows 190°F or more, and a RAM share is cut to a whole percent first.

| Key | Counted in | `at` | Default steps |
|---|---|---|---|
| `cpu` | % busy | 1 to 100 | 90 amber |
| `mem` (RAM on the bar) | % used | 1 to 100 | 80 amber, 90 red |
| `gpu` | % busy | 1 to 100 | none |
| `cpu_temp` | °C or °F | 1 to 150 (°C), 34 to 302 (°F) | 85 amber, 95 red (°C); 185, 203 (°F) |
| `gpu_temp` | °C or °F | 1 to 150 (°C), 34 to 302 (°F) | 84 amber, 90 red (°C); 183, 194 (°F) |
| `net_rx`, `net_tx` | MB/s (`si`), MiB/s (`binary`) or Mb/s (`bits`) | 1 to 100000 (`si`, `binary`), 1 to 800000 (`bits`) | none |

- Amber is `#ffb300` and red `#e53935`. The metric names are the wire's, so RAM is `mem`.
- A metric without a key takes its defaults for the configured unit. `[]` means it is never colored, which differs from leaving the key out.
- Hysteresis: once a step is reached it holds until the value falls below `at` minus a margin: 2 for percent and °C, 4 for °F (about 2 °C, since a °C is about 1.8 °F), and a tenth of `at` for rates. A value that blinks around a step doesn't flicker. A change to the units or steps, or the last device disconnecting, starts every metric afresh.
- An error names the key: `status_bar.thresholds.cpu_temp: at 400 is outside 34 to 302 °F`, `status_bar.thresholds.mem: 4 steps, at most 3`, `status_bar.thresholds.mem: steps must rise, 90 then 80`, `status_bar.thresholds.mem: color "red" is not #rrggbb`, `status_bar.temp_unit "k" is not c or f`. Unknown keys are errors, as everywhere in the file.
- **Editing a unit by hand changes what the steps mean.** The steps are counted in the unit the file names, and nothing converts them when you edit `temp_unit` or `net_unit` in a text editor. The ranges catch some of it (a 190 left behind after a switch back to `c` is outside 1 to 150), but not all: 85 is valid in both. The Status bar form in the window converts the steps when you change a unit there.

#### Which GPU

- `auto` picks the GPU the machine booted its console on (`boot_vga`), else the one with the lowest PCI address, among those the app can read (NVIDIA through its driver, AMD through `amdgpu`). On a desktop with a discrete card that is the discrete card.
- `off` shows `GPU --`.
- An address that names no GPU, or one that can't be read, is not a config error, since an external GPU can be unplugged. The bar shows `GPU --`, and the app logs why when the bar's sampling starts or the setting changes.
- On a laptop with a hybrid GPU, `auto` usually picks the integrated GPU. Naming the NVIDIA card by address keeps it awake while a device is connected, because reading it opens the driver.
- Reading an AMD dGPU's sysfs each second may wake it if it is runtime-suspended. The daemon makes those reads while a device is connected and the AMD GPU is the one picked, whether by `auto` or by address. A read that takes longer than 500 ms leaves the CPU temperature and that GPU's values as `--` for that tick, and the next tick waits for the same read rather than starting another.
- The daemon opens NVML only while a device is connected and the selected GPU is NVIDIA. While it is open, the daemon holds the driver's `/dev/nvidia*` files. They close when the last device disconnects, or when the selection moves to another GPU.

#### The Status bar form

The **Status bar** button in the Layout tab's grid panel opens the form in the editor beside the grid, in place of a button's form. The button reads "(has problems)" when the last save refused a bar setting. While the form is open, no button is selected. The form edits the same draft as the grid and saves with it. A save that changes only `[status_bar]` pushes no layout to a device.

- **Temperature** and **Network rates** set the units. Each option shows its example, such as `°F (shows 147°F)`. Switching converts the steps you have in the unit that changed. The form then says "Thresholds converted to °F.", or, when you had no steps of your own in that unit, that the defaults are now in that unit. A switch doesn't run while a step's `at` isn't a whole number, and asks you to fix the steps first.
- **GPU shown** is Auto, which names the GPU it picks now, or Off, or one GPU by its name and PCI address. A GPU the app can't read is listed but disabled, with its driver named. A saved address that no GPU matches is shown as "(not found)".
- **Colors past a threshold** has one block for each value: CPU, RAM, GPU, CPU temperature, GPU temperature, Download and Upload. Each block says which unit its steps are in. A value on its defaults has a "defaults" tag and shows its steps as text, or "Never colored." when it has none. Edit steps copies the defaults into the file so you can change them. Add step adds a step above the last one, up to 3. Remove deletes a step. Use defaults removes the value's own steps. Errors from the last save show under the value they belong to.

## Icons

A button can show a picture above its label. The picture is a PNG, JPEG, WebP or SVG file in `$XDG_CONFIG_HOME/watchdeck/icons/`, and `icon` names the file:

```toml
[[button]]
id = "plex"
pos = [0, 0]
label = "Plex"
icon = "plex.png"
action = { kind = "shell", argv = ["ssh", "nas", "docker", "restart", "plex"] }
```

To add one, either copy the file into `icons/` yourself (make the folder if it's missing) and name it in `config.toml`, or import it, which copies it there under a name made from its content, such as `3f2a9c1d0e44.png`. The config editor's Import button does the import. Its icon picker lists the files in `icons/` that the app would accept (a valid name, a regular file inside the folder, and a readable PNG, JPEG, WebP or SVG), and reads the folder again after an import.

- `icon` is a plain file name: 1 to 63 letters, digits, `_`, `-` or `.`, not starting with a dot. It can't point outside `icons/`.
- Only PNG, JPEG, WebP and SVG are read. The kind comes from the file's contents, not its name: a file that starts with `<` is read as SVG. Compressed SVG (`.svgz`) isn't supported.
- A file must be a regular file of at most 2 MB (an SVG at most 256 KB), and a raster picture at most 4096 × 4096 pixels. Bigger ones are refused, not scaled.
- An SVG is drawn by the app at the size of the box (see below), scaled up or down to fill it, keeping its shape, and its transparency is kept. An SVG file must be named `*.svg` (and a `.svg` file must hold an SVG), because the preview picks the file's type from its name. The app treats an SVG as untrusted and reads only the strict subset below. Nothing is fetched or read from disk, and no fonts are loaded. A file that uses anything else is refused, naming what it used, like any other bad icon. Lucide and Material Symbols icons fit the subset as they are downloaded, and so should other sets that use plain shapes and presentation attributes. A file saved by Inkscape or Illustrator usually doesn't, because of the editor's own elements and attributes, a `<style>` element, or properties such as `font-variation-settings` in a `style` attribute. Export it as plain SVG and clean it with an optimizer that turns styles into attributes and drops editor data first.
  - **Namespaces:** the root must be an `svg` element in the SVG namespace (`xmlns="http://www.w3.org/2000/svg"`), and so must every other element. An element in no namespace (under `xmlns=""`) or in another namespace is refused, not ignored, and that includes an editor's metadata. An attribute may have no namespace, or be `xlink:href` or `xml:space`. Any other namespaced attribute is refused, even one in the SVG namespace.
  - **Elements allowed:** `svg` (the root only), `g`, `defs`, `symbol`, `path`, `rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`, `linearGradient`, `radialGradient`, `stop`, `use`, `title`, `desc` and `metadata`. Everything else is refused, including `pattern`, `mask`, `clipPath`, `marker`, `filter` and its `fe*` primitives, `image`, `text` (convert text to paths), `switch`, `foreignObject`, `script`, `style`, animation and a nested `svg`, as are a DOCTYPE, entities and gzip-compressed SVG.
  - **Attributes allowed:** `id`, `class`, `style`, `version`, `baseProfile`, `role`, `focusable`, `enable-background`, any `aria-*` or `data-*`, `viewBox`, `preserveAspectRatio`, `x`, `y`, `width`, `height`, `d`, `points`, `rx`, `ry`, `cx`, `cy`, `r`, `x1`, `y1`, `x2`, `y2`, `fx`, `fy`, `fr`, `transform`, `href`, `offset`, `gradientUnits`, `gradientTransform`, `spreadMethod`, and these presentation attributes: `fill`, `fill-opacity`, `fill-rule`, `stroke`, `stroke-width`, `stroke-linecap`, `stroke-linejoin`, `stroke-miterlimit`, `stroke-opacity`, `stroke-dashoffset`, `opacity`, `color`, `display`, `visibility`, `stop-color`, `stop-opacity`, `overflow`, `paint-order`, `shape-rendering` and `clip-rule`. Any other attribute is refused, such as `filter`, `mask`, `clip-path`, `marker-start`, `stroke-dasharray`, `mix-blend-mode` and the font and text attributes.
  - **CSS:** a `style` attribute may set only the presentation attributes above. A `<style>` element is refused, so use attributes or `style` instead. `class` is allowed because icon sets put it on the root, but with no stylesheet it does nothing.
  - **Links:** an `href` (or `xlink:href`, but not both on one element) must be `#` and an id in the file, with no spaces. An id may be used only once. A `<use>` may not reach another `<use>`, and gradients that inherit from each other with `href` may be at most 8 deep.
  - **Limits on the file:** at most 2,000 elements, counting those in `defs` and counting again everything each `<use>` copies; nested at most 64 deep; at most 64 attributes on one element; at most 16 `xmlns` declarations; at most 64 `<use>` elements; at most 32 stops in a gradient and 256 in all; and at most 256 KB of path data (`d` and `points`), counting again what each `<use>` copies.
  - **Limits on the drawing:** before it draws an SVG, the app estimates what drawing it into the largest box (256 px) would cost, from the area each shape and stroke covers, the paint (a gradient costs about 16 times a plain color), the segments of each path (a path whose segments all cross each other costs the square of their number), and the layers that `opacity` and a sized `<symbol>` add, up to 5 times the box a side for a drawing that reaches past its `viewBox`. It refuses an SVG whose estimate is over about half a second, or whose layers would take over 32 MB. The estimate errs high, so a refused file is one that is far bigger or busier than an icon needs to be, for example hundreds of full-size gradient shapes or a single path of over 10,000 crossing segments.
  - **Size:** the `viewBox`, or the `width` and `height`, must give between 0.001 and 4096 units a side. A file with neither is refused.
- Transparency is kept, so a PNG or WebP with a see-through background shows the button's color behind it.
- The app shrinks a picture that is bigger than its box, keeping its shape, and leaves a smaller one as it is. It never scales a raster picture up. A JPEG is turned upright by its EXIF orientation first.
- The box is a square above the label. Its side is what the button has left after 4 px of padding on each side, a 2 px gap and the label, and it never exceeds 256 px or the button's width less the padding. The label gets 2 lines (35 px each), or 1 line where 2 lines would leave the icon under 24 px. A longer label ends in dots. So the box is 256 px on 1 row, 190 on 2 rows, 97 on 3 rows (the default 5×3 grid, labels up to 2 lines), 51 on 4 rows and 58 on 5 rows (8×5, labels of 1 line). On a single row of 4 or more columns the button's width limits it instead, for example 238 px on 4 columns.
- A layout of 2 or more pages gives the grid 31 px less height, for a "Page x of y" label under it, so the boxes are smaller: 256 px on 1 row, 174 on 2 rows, 87 on 3 rows, 43 on 4 rows and 52 on 5 rows, and the same width limit applies. A layout of one page keeps the sizes above.
- A WebP's declared sizes are checked before it is decoded, because the decoder would trust them. The app reads the headers (the canvas, each animation frame and the picture data) and refuses a picture over 4096 pixels on a side, one whose headers disagree, or one that breaks off.
- Buttons that name the same file share one picture on the device.
- A missing, unreadable or invalid file holds back the whole config, like any other broken rule, and the app logs which button and file.
- Adding or removing a file in `icons/`, or changing one a button names (its modified time or size), reloads the config once the change has held for two polls. That covers replacing a picture in place, and a file that is still being copied is read again when it finishes. A different picture gets a new id, so the device fetches it and drops the old one.

## Fonts

The device can draw a button's text in Montserrat or in one of six other fonts. Pick the font at three levels:

```toml
[layout]
cols = 5
rows = 3
font = "inter"            # the default for every button. Absent: montserrat

[[button]]
id = "nas-load"
pos = [2, 0]
label = "NAS load"
font = "orbitron"         # this button's label. Absent: the grid's font
action = { kind = "shell", argv = ["true"] }
tile = { every_s = 30, font = "fira-code", source = { kind = "shell", argv = ["cat", "/proc/loadavg"] } }
#                          ^ the value only. Absent: the button's font
```

| Id | Font |
|---|---|
| `montserrat` | Montserrat, the device's built-in font and the default |
| `inter` | Inter |
| `roboto` | Roboto |
| `fira-code` | Fira Code |
| `roboto-mono` | Roboto Mono |
| `orbitron` | Orbitron |
| `oxanium` | Oxanium |

- A label's font is the button's `font`, else `layout.font`, else `montserrat`. A tile's value uses `tile.font`, else the label's font. A button has only those two texts, so any pair of fonts is `font` for the label and `tile.font` for the value.
- `montserrat` is valid at every level, so a button can keep Montserrat under another grid font. Naming the font a level would inherit anyway is allowed, and pins it: a later change to `layout.font` leaves that button alone.
- An unknown name is an error like any other, and the last good config stays live: `layout.font "comic" is not one of montserrat, inter, roboto, fira-code, roboto-mono, orbitron, oxanium`. Names are lowercase.
- Only button labels and tile values change. The status lines, settings, the pairing screens and the result screens always use Montserrat. The status bar uses its own font, Roboto Mono.
- A font never changes the size of a button, its icon box or how many lines its label gets: a label still takes 1 or 2 lines and ends in dots where it doesn't fit, and a tile value is still one line. A wide font shows fewer characters before the dots, though. Orbitron is the widest, and it cuts a long value sooner than Montserrat or Inter. The config editor's preview breaks lines and places the dots the way the device does.
- Every font covers printable ASCII, the only characters labels and values may use. Orbitron has no `^` of its own, so the device draws that one character from Oxanium.

### Font licenses

All six fonts are under the SIL Open Font License 1.1. The firmware carries the license texts, each under its font's name and copyright line. On the device, tap **Licenses** in settings (swipe up on the grid to open it), or on the status screen, to the right of the USB line. Drag to scroll and tap **Close** to leave. The same texts are in the app bundle under `fonts/licenses/`, and in the firmware image as plain text, so `strings` on a `.bin` finds them.

### Older firmware and older apps

- A device on firmware from before fonts tells the app so when it connects, and the app sends it no font information. It draws everything in Montserrat, and the Devices page says so while it is connected. A change to fonts alone sends it nothing. Update its firmware to see the fonts.
- An app from before fonts refuses a `config.toml` with a `font` key, because it rejects keys it doesn't know, and keeps its last good config (an empty grid on a fresh start). Before going back to an older app, remove the `font` keys from `config.toml`.

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
| `insecure_tls` | no | `true` skips certificate verification for this action, for a self-signed `https://` service. Refused on a `http://` url. Default `false` |
| `timeout_s` | no | 1 to 600 seconds for the whole exchange. Default 10 |

- A header name may appear only once across `headers` and `secret_headers`, ignoring case.
- Redirects aren't followed, so a 3xx reply is a failure.
- Proxy settings are ignored, so a request goes only to the host in its URL. Over `https://`, only a server that proves it is that host gets it.
- HTTPS checks the server's certificate against the system's trusted roots, unless the action sets `insecure_tls`.
- The app reads at most 64 KB of the reply.

### Secrets over plain http

The app refuses a button whose `secret_headers` would go over `http://` to another machine. Over plain http, anyone on the network path can read the header. So can anyone who answers for the host name first, which is easy on a LAN, above all for `.local` names. Use `https://`, or a URL on this machine (`localhost`, `127.0.0.1` or `[::1]`), which needs no setting.

To send them anyway, set `insecure_http = true` on the action. The app then logs a warning whenever a config with that button goes live.

### Self-signed certificates

A service with a self-signed certificate, such as Home Assistant on its own `https://`, fails with a certificate error. Set `insecure_tls = true` on that action to accept any certificate from the server. The setting covers that action only, and also applies to a tile whose `source` sets it.

This turns off the protection https gives. The app can no longer tell the service from an impostor, so anyone on the network can answer in its place or sit in the middle, then read or change the request and the reply. That includes the `secret_headers` the request carries. Use it only for a service you trust on a network you trust. Redirects, the 64 KB reply limit and the timeout work as before. The app logs a warning naming the button whenever a config with this setting goes live.

## Tiles

A tile puts a live value on a button, such as a load average, a disk percentage or whether a service is up. Pressing the button still runs its `action`. The tile has its own `source`, which can be any shell or HTTP action:

| Key | Required | Rule |
|---|---|---|
| `every_s` | yes | Seconds between polls, 2 to 86400 |
| `json_pointer` | no | An [RFC 6901](https://www.rfc-editor.org/rfc/rfc6901) pointer such as `/state` or `/attributes/brightness`. It must start with `/` |
| `font` | no | The value's font; see [Fonts](#fonts). Default: the button's font |
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

- The file must have mode 0600, or the app won't read it and logs the fix: `chmod 600 "${XDG_CONFIG_HOME:-$HOME/.config}"/watchdeck/secrets.toml`.
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
