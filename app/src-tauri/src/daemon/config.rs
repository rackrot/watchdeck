//! `config.toml`: the grid and what each button does. Rust is the authority on what is valid. A
//! config that breaks any rule is never applied or pushed, and the last good one stays live. The
//! daemon polls the file, and `secrets.toml` whose names the rules check, and reloads on a change.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs::{self, File};
use std::io::{self, Read};
use std::net::IpAddr;
use std::ops::Deref;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, SystemTime};

use reqwest::header::HeaderName;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use tokio::sync::watch;

use super::icons::{self, IconSet, Icons};
use super::protocol::{encode_hex, Button, IconFrame, Message};
use super::secrets::Secrets;
use super::store::write_private;

pub const FILE: &str = "config.toml";
pub const MAX_COLS: u32 = 8;
pub const MAX_ROWS: u32 = 5;
/// The device keeps an id in 64 bytes and a label in 48, each with its NUL.
pub const MAX_ID_BYTES: usize = 63;
pub const MAX_LABEL_BYTES: usize = 47;
pub const MAX_TIMEOUT_S: u64 = 600;
/// A tile polls at most this often, and at least this rarely (a day).
pub const MIN_EVERY_S: u64 = 2;
pub const MAX_EVERY_S: u64 = 24 * 60 * 60;
/// How many `config.toml.bak.*` files a repairing save leaves behind.
const KEEP_BACKUPS: usize = 5;
const DEFAULT_TIMEOUT_S: u64 = 10;
const MAX_FILE_BYTES: u64 = 1024 * 1024;
const MAX_BODY_BYTES: usize = 64 * 1024;
const METHODS: [&str; 6] = ["GET", "HEAD", "POST", "PUT", "PATCH", "DELETE"];
/// How often the files are checked for changes.
const POLL: Duration = Duration::from_millis(500);

/// Written on first run, when there is no `config.toml`.
pub const DEFAULT: &str = r##"# watchdeck: the button grid and what each button does.
# The app reloads this file when it changes. A version that breaks a rule is
# not applied, and the app logs why. Reference:
# https://rackrot.github.io/watchdeck/config.html
version = 1

[layout]
cols = 5 # 1 to 8
rows = 3 # 1 to 5

# One [[button]] per cell. pos = [column, row], counted from 0 at the top left.
# Labels are printable ASCII, at most 47 characters.
[[button]]
id = "hello"
pos = [0, 0]
label = "Hello"
bg = "#1e88e5"
fg = "#ffffff"
# argv runs without a shell, in your home directory. timeout_s is 1 to 600.
action = { kind = "shell", argv = ["notify-send", "watchdeck", "Hello from watchdeck"], timeout_s = 10 }
"##;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub version: u32,
    pub layout: Grid,
    #[serde(default, rename = "button")]
    pub buttons: Vec<ButtonConfig>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Grid {
    pub cols: u32,
    pub rows: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ButtonConfig {
    pub id: String,
    /// `[column, row]`, from 0.
    pub pos: [u32; 2],
    pub label: String,
    #[serde(default = "default_bg")]
    pub bg: String,
    #[serde(default = "default_fg")]
    pub fg: String,
    /// A file in `icons/`, shown above the label.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub icon: Option<String>,
    pub action: Action,
    /// Makes the button show a live value, polled from `source`. The tile table goes last
    /// because TOML wants tables after plain values.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tile: Option<TileConfig>,
}

/// A live value on a button: `source` runs every `every_s` seconds while a device is connected.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TileConfig {
    pub every_s: u64,
    /// Picks the value out of the JSON the source prints or returns (RFC 6901, e.g. `/state`).
    /// Without it the value is the first line of output.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub json_pointer: Option<String>,
    pub source: Action,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum Action {
    Shell {
        argv: Vec<String>,
        #[serde(default = "default_timeout")]
        timeout_s: u64,
    },
    Http {
        method: String,
        url: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        body: Option<String>,
        /// Header name -> value, sent as written. Not secret.
        #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
        headers: BTreeMap<String, String>,
        /// Header name -> the name of a secret in `secrets.toml`.
        #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
        secret_headers: BTreeMap<String, String>,
        /// Lets `secret_headers` cross the network over plain http.
        #[serde(default, skip_serializing_if = "std::ops::Not::not")]
        insecure_http: bool,
        #[serde(default = "default_timeout")]
        timeout_s: u64,
    },
}

fn default_bg() -> String {
    "#1e88e5".into()
}

fn default_fg() -> String {
    "#ffffff".into()
}

fn default_timeout() -> u64 {
    DEFAULT_TIMEOUT_S
}

/// What a device draws, which is all of a config except the actions.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Layout {
    pub cols: u32,
    pub rows: u32,
    pub buttons: Vec<Button>,
    /// The pictures the buttons name, for answering `icon_get`.
    pub icons: Vec<Arc<IconFrame>>,
}

impl Layout {
    pub fn message(&self, rev: u32) -> Message {
        Message::Layout {
            rev,
            cols: self.cols,
            rows: self.rows,
            buttons: self.buttons.clone(),
        }
    }
}

/// `#rrggbb` as `0xRRGGBB`.
fn color(text: &str) -> Option<u32> {
    let hex = text.strip_prefix('#')?;
    if hex.len() != 6 || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    u32::from_str_radix(hex, 16).ok()
}

fn printable_ascii(text: &str) -> bool {
    text.bytes().all(|b| (b' '..=b'~').contains(&b))
}

/// Whether `url` names this machine: a loopback address, or `localhost`.
fn loopback(url: &reqwest::Url) -> bool {
    match url.host_str() {
        Some("localhost") => true,
        Some(host) => host
            .trim_start_matches('[')
            .trim_end_matches(']')
            .parse::<IpAddr>()
            .is_ok_and(|ip| ip.is_loopback()),
        None => false,
    }
}

/// A config that passed every check, with the icons it names rendered. This is what goes live,
/// so a layout and the icons it names always come from the same load.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Live {
    pub config: Config,
    /// One entry for each icon file `config` names.
    pub icons: IconSet,
}

impl Deref for Live {
    type Target = Config;

    fn deref(&self) -> &Config {
        &self.config
    }
}

impl Live {
    /// No buttons, which is what runs until a valid file loads.
    pub fn empty() -> Self {
        Self {
            config: Config::empty(),
            icons: IconSet::new(),
        }
    }

    pub fn layout(&self) -> Layout {
        let rgb = |text: &str| color(text).expect("colors are checked before a config goes live");
        let mut icons = Vec::new();
        let buttons = self
            .config
            .buttons
            .iter()
            .map(|b| Button {
                id: b.id.clone(),
                col: b.pos[0],
                row: b.pos[1],
                label: b.label.clone(),
                bg: rgb(&b.bg),
                fg: rgb(&b.fg),
                tile: b.tile.is_some(),
                icon: b.icon.as_ref().map(|name| {
                    let icon = self
                        .icons
                        .get(name)
                        .expect("icons are rendered before a config goes live");
                    icons.push(icon.clone());
                    encode_hex(&icon.id)
                }),
            })
            .collect();
        Layout {
            cols: self.config.layout.cols,
            rows: self.config.layout.rows,
            buttons,
            icons,
        }
    }
}

impl Config {
    /// No buttons, which is what runs until a valid file loads.
    pub fn empty() -> Self {
        Self {
            version: 1,
            layout: Grid { cols: 5, rows: 3 },
            buttons: Vec::new(),
        }
    }

    pub fn button(&self, id: &str) -> Option<&ButtonConfig> {
        self.buttons.iter().find(|b| b.id == id)
    }

    /// The ids of the buttons whose action or tile source sends secret `name`.
    pub fn secret_users(&self, name: &str) -> Vec<String> {
        let uses = |action: &Action| {
            matches!(action, Action::Http { secret_headers, .. }
                if secret_headers.values().any(|s| s == name))
        };
        self.buttons
            .iter()
            .filter(|b| uses(&b.action) || b.tile.as_ref().is_some_and(|t| uses(&t.source)))
            .map(|b| b.id.clone())
            .collect()
    }

    /// Every rule this config breaks. `secrets` holds the names in `secrets.toml`, and is
    /// consulted only for buttons that use a secret.
    pub fn check(&self, secrets: &io::Result<Vec<String>>) -> Vec<String> {
        let mut errors = Vec::new();
        if self.version != 1 {
            errors.push(format!("version is {}, expected 1", self.version));
        }
        let Grid { cols, rows } = self.layout;
        let grid_ok = (1..=MAX_COLS).contains(&cols) && (1..=MAX_ROWS).contains(&rows);
        if !(1..=MAX_COLS).contains(&cols) {
            errors.push(format!("layout.cols is {cols}, expected 1 to {MAX_COLS}"));
        }
        if !(1..=MAX_ROWS).contains(&rows) {
            errors.push(format!("layout.rows is {rows}, expected 1 to {MAX_ROWS}"));
        }

        let mut ids = HashSet::new();
        let mut cells = HashMap::new();
        for b in &self.buttons {
            let mut error = |problem: String| errors.push(format!("button {:?}: {problem}", b.id));
            if b.id.is_empty()
                || b.id.len() > MAX_ID_BYTES
                || !b
                    .id
                    .bytes()
                    .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'_' | b'-' | b'.'))
            {
                error(format!(
                    "id must be 1 to {MAX_ID_BYTES} letters, digits, '_', '-' or '.'"
                ));
            }
            if !ids.insert(b.id.as_str()) {
                error("another button has this id".into());
            }
            let [col, row] = b.pos;
            if grid_ok && (col >= cols || row >= rows) {
                error(format!(
                    "pos [{col}, {row}] is outside the {cols}x{rows} grid"
                ));
            } else if let Some(other) = cells.insert((col, row), b.id.as_str()) {
                error(format!("pos [{col}, {row}] is taken by button {other:?}"));
            }
            if b.label.len() > MAX_LABEL_BYTES {
                error(format!(
                    "label is {} bytes, at most {MAX_LABEL_BYTES}",
                    b.label.len()
                ));
            }
            if !printable_ascii(&b.label) {
                error(
                    "label must be printable ASCII, the only characters the device can draw".into(),
                );
            }
            if let Some(problem) = b.icon.as_deref().and_then(icons::name_error) {
                error(problem);
            }
            for (field, value) in [("bg", &b.bg), ("fg", &b.fg)] {
                if color(value).is_none() {
                    error(format!("{field} {value:?} is not a #rrggbb color"));
                }
            }
            for problem in b.action.check(secrets) {
                error(problem);
            }
            if let Some(tile) = &b.tile {
                for problem in tile.check(secrets) {
                    error(problem);
                }
            }
        }
        errors
    }
}

impl TileConfig {
    fn check(&self, secrets: &io::Result<Vec<String>>) -> Vec<String> {
        let mut errors = Vec::new();
        if !(MIN_EVERY_S..=MAX_EVERY_S).contains(&self.every_s) {
            errors.push(format!(
                "tile.every_s is {}, expected {MIN_EVERY_S} to {MAX_EVERY_S}",
                self.every_s
            ));
        }
        // "" names the whole document, which is never a short value; RFC 6901 pointers that
        // select something start with '/'.
        if self
            .json_pointer
            .as_deref()
            .is_some_and(|p| !p.starts_with('/'))
        {
            errors.push("tile.json_pointer must start with '/', like \"/state\"".into());
        }
        errors.extend(
            self.source
                .check(secrets)
                .into_iter()
                .map(|problem| format!("tile.source: {problem}")),
        );
        errors
    }
}

impl Action {
    pub fn timeout(&self) -> Duration {
        match self {
            Action::Shell { timeout_s, .. } | Action::Http { timeout_s, .. } => {
                Duration::from_secs(*timeout_s)
            }
        }
    }

    /// Whether this sends secret headers over plain http to another machine, where anyone on the
    /// way, or anyone who answers for the host name, can read them.
    pub fn secrets_in_clear(&self) -> bool {
        let Action::Http {
            url,
            secret_headers,
            ..
        } = self
        else {
            return false;
        };
        !secret_headers.is_empty()
            && reqwest::Url::parse(url).is_ok_and(|u| u.scheme() == "http" && !loopback(&u))
    }

    fn check(&self, secrets: &io::Result<Vec<String>>) -> Vec<String> {
        let mut errors = Vec::new();
        let timeout_s = self.timeout().as_secs();
        if !(1..=MAX_TIMEOUT_S).contains(&timeout_s) {
            errors.push(format!(
                "timeout_s is {timeout_s}, expected 1 to {MAX_TIMEOUT_S}"
            ));
        }
        match self {
            Action::Shell { argv, .. } => {
                match argv.first() {
                    None => errors.push("argv is empty".into()),
                    Some(program) if program.is_empty() => {
                        errors.push("argv[0], the program, is empty".into())
                    }
                    Some(_) => {}
                }
                for (i, arg) in argv.iter().enumerate() {
                    if arg.contains('\0') {
                        errors.push(format!("argv[{i}] contains a NUL character"));
                    }
                }
            }
            Action::Http {
                method,
                url,
                body,
                headers,
                secret_headers,
                insecure_http,
                ..
            } => {
                if !METHODS.contains(&method.as_str()) {
                    errors.push(format!(
                        "method {method:?} is not one of {}",
                        METHODS.join(", ")
                    ));
                }
                // The URL stays out of errors, in case it carries a token of its own.
                match reqwest::Url::parse(url) {
                    Err(e) => errors.push(format!("url does not parse: {e}")),
                    Ok(u) if !matches!(u.scheme(), "http" | "https") => {
                        errors.push("url is not http:// or https://".into())
                    }
                    Ok(u) if u.host_str().is_none_or(str::is_empty) => {
                        errors.push("url has no host".into())
                    }
                    Ok(_) => {}
                }
                if self.secrets_in_clear() && !insecure_http {
                    errors.push(
                        "secret_headers would cross the network in the clear over http://; \
                         use https://, or set insecure_http = true"
                            .into(),
                    );
                }
                if body.as_ref().is_some_and(|b| b.len() > MAX_BODY_BYTES) {
                    errors.push(format!("body is over {} KB", MAX_BODY_BYTES / 1024));
                }
                let mut names = HashSet::new();
                for header in headers.keys().chain(secret_headers.keys()) {
                    if HeaderName::from_bytes(header.as_bytes()).is_err() {
                        errors.push(format!("{header:?} is not a valid header name"));
                    } else if !names.insert(header.to_ascii_lowercase()) {
                        errors.push(format!("header {header:?} is set twice; names ignore case"));
                    }
                }
                for (header, value) in headers {
                    if !printable_ascii(value) {
                        errors.push(format!(
                            "header {header:?} must have a value of printable ASCII on one line"
                        ));
                    }
                }
                for (header, secret) in secret_headers {
                    match secrets {
                        Ok(names) if names.contains(secret) => {}
                        Ok(_) => errors.push(format!(
                            "header {header:?} uses secret {secret:?}, which is not in secrets.toml"
                        )),
                        Err(e) => errors.push(format!(
                            "header {header:?} uses secret {secret:?}, but secrets.toml can't be read: {e}"
                        )),
                    }
                }
            }
        }
        errors
    }
}

/// Parses `text` and checks it against every rule.
pub fn parse(text: &str, secrets: &io::Result<Vec<String>>) -> Result<Config, Vec<String>> {
    let config: Config = toml::from_str(text).map_err(|e| vec![toml_error(text, &e)])?;
    let errors = config.check(secrets);
    if errors.is_empty() {
        Ok(config)
    } else {
        Err(errors)
    }
}

/// Where the TOML went wrong and why, without toml's copy of the line, which may hold a URL. The
/// message itself can still quote a value of the wrong type.
fn toml_error(text: &str, e: &toml::de::Error) -> String {
    let message = e.message().trim_end();
    let Some(span) = e.span() else {
        return message.to_string();
    };
    let before = text.get(..span.start).unwrap_or(text);
    let line = before.matches('\n').count() + 1;
    let column = before.rsplit('\n').next().unwrap_or("").chars().count() + 1;
    format!("line {line}, column {column}: {message}")
}

/// What the window needs to know about the file.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ConfigStatus {
    /// SHA-256 of the file text as the last reload read it, live or not. It is empty when the file
    /// was missing or unreadable. A save must name it, and is refused if the file has changed
    /// since.
    pub revision: String,
    /// Why the file on disk is not live, or empty when it is.
    pub errors: Vec<String>,
    /// Whether a good config has loaded since the app started. If not, the live config is the
    /// empty grid and not any version of the file.
    pub loaded: bool,
}

/// The live config, and the file it comes from.
pub struct Configs {
    path: PathBuf,
    secrets: Arc<Secrets>,
    live: watch::Sender<Arc<Live>>,
    status: watch::Sender<ConfigStatus>,
    icons_dir: PathBuf,
    icons: Icons,
    /// The icon files the file on disk names, for the poller to stamp. Kept even when the config
    /// is refused, so a file that is still being copied in is read again once it settles.
    watched: Mutex<Vec<String>>,
    /// The files as the last reload read them. Holding it serializes reloads and saves, so a
    /// save's check and its write see the same file.
    read: Mutex<Stamps>,
}

impl Configs {
    /// Loads `config.toml` from `dir`, writing [`DEFAULT`] there first if there is none. If the
    /// file can't be read or breaks a rule, an empty grid goes live and `status` says why.
    pub fn open(dir: &Path, secrets: Arc<Secrets>) -> io::Result<Arc<Self>> {
        let path = dir.join(FILE);
        let icons_dir = dir.join(icons::DIR);
        if !path.try_exists()? {
            write_private(&path, DEFAULT.as_bytes())?;
            log::info!("wrote the default {}", path.display());
        }
        let configs = Arc::new(Self {
            path,
            secrets,
            live: watch::channel(Arc::new(Live::empty())).0,
            status: watch::channel(ConfigStatus {
                revision: String::new(),
                errors: Vec::new(),
                loaded: false,
            })
            .0,
            icons: Icons::new(icons_dir.clone()),
            icons_dir,
            watched: Mutex::default(),
            // Nothing read yet, which differs from any file that exists.
            read: Mutex::new(Stamps::default()),
        });
        // reload logs its own outcome.
        let _ = configs.reload();
        Ok(configs)
    }

    pub fn subscribe(&self) -> watch::Receiver<Arc<Live>> {
        self.live.subscribe()
    }

    pub fn status(&self) -> watch::Receiver<ConfigStatus> {
        self.status.subscribe()
    }

    pub fn current(&self) -> Arc<Live> {
        self.live.borrow().clone()
    }

    fn locked(&self) -> MutexGuard<'_, Stamps> {
        self.read.lock().expect("config lock poisoned")
    }

    /// Reads and checks the file. If it passes, it goes live (devices get its layout if that
    /// changed) and `Ok` says whether anything changed. If not, the live config stays. A file
    /// that changes while it is read counts as not read, and the poller reads it again.
    pub fn reload(&self) -> Result<bool, Vec<String>> {
        self.prewarm();
        let mut read = self.locked();
        let before = self.stamp();
        let mut icons_before = None;
        let mut revision = String::new();
        let loaded = read_capped(&self.path).and_then(|bytes| {
            revision = sha256_hex(&bytes);
            let text = String::from_utf8(bytes).map_err(|_| vec!["not UTF-8".to_string()])?;
            let config = parse(&text, &self.secrets.names())?;
            self.watch(&config);
            icons_before = Some(self.stamp().icons);
            let icons = self.icons.load(&config)?;
            Ok(Live { config, icons })
        });
        let now = self.stamp();
        if now.files != before.files || icons_before.is_some_and(|icons| icons != now.icons) {
            log::debug!("{} changed while being read", self.path.display());
            return Ok(false);
        }
        *read = now;
        let live = match loaded {
            Ok(live) => live,
            Err(errors) => {
                log::error!(
                    "{} not applied, keeping the live config: {}",
                    self.path.display(),
                    errors.join("; ")
                );
                self.status.send_if_modified(|status| {
                    let changed = status.errors != errors || status.revision != revision;
                    status.errors = errors.clone();
                    status.revision = revision;
                    changed
                });
                return Err(errors);
            }
        };
        let changed = self.go_live(live);
        if changed {
            log::info!("applied {}", self.path.display());
        }
        self.set_status(revision);
        Ok(changed)
    }

    /// Checks `config` and writes it in place of the file, which must still be the one at
    /// `revision`, the text the window saw. Returns the new revision. Nothing is written unless
    /// every check passes. When the file on disk is not live (it breaks a rule), this is how the
    /// window repairs it, and the text it replaces is kept as `config.toml.bak.<UTC time>`, newest 5 kept.
    pub fn save(&self, config: Config, revision: &str) -> Result<String, Vec<String>> {
        let errors = config.check(&self.secrets.names());
        if !errors.is_empty() {
            return Err(errors);
        }
        // Render before the lock, so a slow decode doesn't hold it, then again under the lock.
        // That pass is a cache hit unless a file changed in between, as in `reload`.
        self.icons.load(&config)?;
        let _read = self.locked();
        // Again under the lock, since a secret may have been deleted since the first check.
        let errors = config.check(&self.secrets.names());
        if !errors.is_empty() {
            return Err(errors);
        }
        let icons = self.icons.load(&config)?;
        let on_disk = self.read_file()?;
        if on_disk.as_deref().map_or_else(String::new, sha256_hex) != revision {
            return Err(vec![format!(
                "{FILE} changed since it was loaded; load it again before saving"
            )]);
        }
        if let Some(bytes) = on_disk.filter(|_| !self.status.borrow().errors.is_empty()) {
            let backup = self.backup(&bytes)?;
            log::warn!("kept the replaced {FILE} as {}", backup.display());
        }
        let text = format!(
            "# Written by the watchdeck app, which does not keep comments.\n{}",
            toml::to_string(&config).expect("a config serializes")
        );
        write_private(&self.path, text.as_bytes())
            .map_err(|e| vec![format!("Could not save {FILE}: {e}")])?;
        let revision = sha256_hex(text.as_bytes());
        if self.go_live(Live { config, icons }) {
            log::info!("saved and applied {}", self.path.display());
        }
        self.set_status(revision.clone());
        Ok(revision)
    }

    /// Writes `bytes` to a new `config.toml.bak.<UTC YYYYMMDDTHHMMSSZ>` beside the file, never over
    /// an existing one, then drops all but the newest [`KEEP_BACKUPS`].
    fn backup(&self, bytes: &[u8]) -> Result<PathBuf, Vec<String>> {
        let dir = self.path.parent().unwrap_or(Path::new("."));
        let prefix = format!("{FILE}.bak.");
        let mut old = self.backups(dir, &prefix);
        let newest = old
            .last()
            .and_then(|p| p.file_name())
            .map(|n| n.to_os_string());
        let mut at = time::OffsetDateTime::now_utc();
        // The stamp sorts after every backup there is, so name order is age order even when
        // saves land in one second or the clock steps back.
        let path = loop {
            let stamp = format!(
                "{:04}{:02}{:02}T{:02}{:02}{:02}Z",
                at.year(),
                u8::from(at.month()),
                at.day(),
                at.hour(),
                at.minute(),
                at.second()
            );
            let name = std::ffi::OsString::from(format!("{prefix}{stamp}"));
            if newest.as_ref().is_none_or(|n| name > *n) {
                break dir.join(name);
            }
            at += time::Duration::seconds(1);
        };
        write_private(&path, bytes)
            .map_err(|e| vec![format!("Could not keep {} first: {e}", path.display())])?;
        old.push(path.clone());
        for stale in old.iter().take(old.len().saturating_sub(KEEP_BACKUPS)) {
            if let Err(e) = fs::remove_file(stale) {
                log::warn!("could not remove the old backup {}: {e}", stale.display());
            }
        }
        Ok(path)
    }

    /// The backups in `dir`, oldest first. Only regular files named exactly
    /// `<prefix><YYYYMMDDTHHMMSSZ>` count, so pruning never touches the user's own files and a
    /// stray name can't sort above every stamp.
    fn backups(&self, dir: &Path, prefix: &str) -> Vec<PathBuf> {
        let mut found: Vec<PathBuf> = fs::read_dir(dir)
            .map(|entries| {
                entries
                    .filter_map(Result::ok)
                    .filter(|e| e.file_type().is_ok_and(|t| t.is_file()))
                    .filter(|e| {
                        e.file_name()
                            .to_str()
                            .and_then(|n| n.strip_prefix(prefix))
                            .is_some_and(is_backup_stamp)
                    })
                    .map(|e| e.path())
                    .collect()
            })
            .unwrap_or_default();
        found.sort();
        found
    }

    /// The file's bytes, or `None` if there is no file.
    fn read_file(&self) -> Result<Option<Vec<u8>>, Vec<String>> {
        match self.path.try_exists() {
            Ok(false) => Ok(None),
            Ok(true) => read_capped(&self.path).map(Some),
            Err(e) => Err(vec![format!("{}: {e}", self.path.display())]),
        }
    }

    /// Copies the image at `src` into `icons/` and returns the file name for a button's `icon`.
    pub fn import_icon(&self, src: &Path) -> Result<String, String> {
        self.icons.import(src)
    }

    /// The files in `icons/` that a button's `icon` could name, sorted.
    pub fn list_icons(&self) -> Result<Vec<String>, String> {
        self.icons.list()
    }

    /// Removes secret `name`, unless the live config still uses it, since the next reload would
    /// then refuse that config. The error names the buttons. The config lock is held so a save
    /// can't go live with the secret between the check and the removal.
    pub fn delete_secret(&self, name: &str) -> Result<(), String> {
        let read = self.locked();
        let users = self.current().secret_users(name);
        if !users.is_empty() {
            return Err(format!(
                "Secret {name:?} is used by button {}. Change {} first.",
                users.join(", "),
                if users.len() == 1 { "it" } else { "them" }
            ));
        }
        self.secrets.remove(name)?;
        drop(read);
        // Applies the change now instead of at the next poll. reload logs its own outcome.
        let _ = self.reload();
        Ok(())
    }

    /// Renders the icons that the file on disk names, before `reload` takes the lock, so that
    /// decoding mostly stays out of it. The locked pass then finds them cached, and decodes only
    /// what changed since, which can briefly hold up a save or another reload.
    /// Whatever is wrong with the file, the locked pass reports it, so errors are dropped here.
    fn prewarm(&self) {
        let Ok(bytes) = read_capped(&self.path) else {
            return;
        };
        let Ok(text) = String::from_utf8(bytes) else {
            return;
        };
        if let Ok(config) = parse(&text, &self.secrets.names()) {
            let _ = self.icons.load(&config);
        }
    }

    fn watch(&self, config: &Config) {
        let mut names: Vec<String> = config
            .buttons
            .iter()
            .filter_map(|b| b.icon.clone())
            .collect();
        names.sort();
        names.dedup();
        *self.watched.lock().expect("watched icons poisoned") = names;
    }

    /// Reloads whenever `config.toml`, `secrets.toml` or the `icons/` directory differ from what
    /// the last reload read, until the future is dropped. A change counts once they look the same
    /// on two polls in a row, so a write in progress is not read half done.
    pub async fn watch_files(self: Arc<Self>) {
        let mut last = self.stamp();
        loop {
            tokio::time::sleep(POLL).await;
            let now = self.stamp();
            let read = self.locked().clone();
            if now == last && now != read {
                // Decoding icons would stall a worker, so it runs on the blocking pool. reload
                // logs its own outcome.
                let this = self.clone();
                if let Err(e) = tokio::task::spawn_blocking(move || this.reload()).await {
                    log::error!("config reload failed: {e}");
                }
            }
            last = now;
        }
    }

    fn stamp(&self) -> Stamps {
        let watched = self.watched.lock().expect("watched icons poisoned");
        Stamps {
            files: (
                stamp(&self.path),
                stamp(&self.secrets.path()),
                stamp(&self.icons_dir),
            ),
            icons: watched
                .iter()
                .map(|name| stamp(&self.icons_dir.join(name)))
                .collect(),
        }
    }

    /// Callers hold the lock.
    fn go_live(&self, next: Live) -> bool {
        self.live.send_if_modified(|live| {
            let changed = **live != next;
            if changed {
                let sources = |b: &ButtonConfig| {
                    [Some(&b.action), b.tile.as_ref().map(|t| &t.source)]
                        .into_iter()
                        .flatten()
                        .any(Action::secrets_in_clear)
                };
                for b in next.buttons.iter().filter(|b| sources(b)) {
                    log::warn!(
                        "button {:?} sends secret headers over plain http, as insecure_http allows",
                        b.id
                    );
                }
                *live = Arc::new(next);
            }
            changed
        })
    }

    fn set_status(&self, revision: String) {
        let next = ConfigStatus {
            revision,
            errors: Vec::new(),
            loaded: true,
        };
        self.status.send_if_modified(|status| {
            let changed = *status != next;
            *status = next;
            changed
        });
    }
}

/// When a file last changed, as far as polling can tell: mtime, size and inode, since editors
/// often save by renaming a new file into place, then mode and owner, so the chmod or chown that
/// makes `secrets.toml` readable takes effect at once.
type Stamp = (SystemTime, u64, u64, u32, u32);
/// What the poller watches.
#[derive(Clone, Default, PartialEq)]
struct Stamps {
    /// `config.toml`, `secrets.toml`, then the `icons/` directory, whose stamp changes when a
    /// file is added, renamed or removed.
    files: (Option<Stamp>, Option<Stamp>, Option<Stamp>),
    /// The icon files `config.toml` names, which also change when one is overwritten in place or
    /// is still being written.
    icons: Vec<Option<Stamp>>,
}

fn stamp(path: &Path) -> Option<Stamp> {
    let meta = fs::metadata(path).ok()?;
    Some((
        meta.modified().ok()?,
        meta.len(),
        meta.ino(),
        meta.mode(),
        meta.uid(),
    ))
}

fn read_capped(path: &Path) -> Result<Vec<u8>, Vec<String>> {
    let fail = |e: io::Error| vec![format!("{}: {e}", path.display())];
    let mut bytes = Vec::new();
    File::open(path)
        .and_then(|file| file.take(MAX_FILE_BYTES + 1).read_to_end(&mut bytes))
        .map_err(fail)?;
    if bytes.len() as u64 > MAX_FILE_BYTES {
        return Err(vec![format!(
            "{} is over {} KB",
            path.display(),
            MAX_FILE_BYTES / 1024
        )]);
    }
    Ok(bytes)
}

fn sha256_hex(bytes: &[u8]) -> String {
    encode_hex(&Sha256::digest(bytes))
}

/// `YYYYMMDDTHHMMSSZ`, the stamp `backup` writes.
fn is_backup_stamp(s: &str) -> bool {
    let b = s.as_bytes();
    b.len() == 16
        && b[8] == b'T'
        && b[15] == b'Z'
        && b[..8].iter().chain(&b[9..15]).all(u8::is_ascii_digit)
}

#[cfg(test)]
mod tests {
    use std::fs::Permissions;
    use std::os::unix::fs::PermissionsExt;

    use super::*;
    use crate::daemon::protocol::MAX_MESSAGE_BYTES;

    /// The plan's example, without its icon and tile (`TILE` adds the tile).
    const EXAMPLE: &str = r##"
version = 1
[layout]
cols = 5
rows = 3

[[button]]
id = "plex"
pos = [0, 0]
label = "Plex"
bg = "#1e88e5"
fg = "#ffffff"
action = { kind = "shell", argv = ["ssh", "nas", "docker", "restart", "plex"], timeout_s = 30 }

[[button]]
id = "office-light"
pos = [1, 0]
label = "Office"
action = { kind = "http", method = "POST", url = "https://ha.lan:8123/api/services/light/toggle",
           body = '{"entity_id":"light.office"}', secret_headers = { Authorization = "ha" } }
"##;

    const EXAMPLE_URL: &str = "url = \"https://ha.lan:8123/api/services/light/toggle\"";

    fn names() -> io::Result<Vec<String>> {
        Ok(vec!["ha".into()])
    }

    /// The errors from `text`, which must parse as TOML and as a config.
    fn errors(text: &str) -> Vec<String> {
        parse(text, &names()).expect_err("the config must be refused")
    }

    /// The example with its first button's `from` replaced by `to`.
    fn example_with(from: &str, to: &str) -> String {
        assert!(EXAMPLE.contains(from), "{from}");
        EXAMPLE.replacen(from, to, 1)
    }

    fn assert_refused(text: &str, expected: &str) {
        let errors = errors(text);
        assert!(
            errors.iter().any(|e| e.contains(expected)),
            "expected an error containing {expected:?}, got {errors:?}"
        );
    }

    #[test]
    fn the_example_and_the_default_pass() {
        let config = parse(EXAMPLE, &names()).unwrap();
        assert_eq!(config.buttons.len(), 2);
        let office = config.button("office-light").unwrap();
        assert_eq!(
            (office.bg.as_str(), office.fg.as_str()),
            ("#1e88e5", "#ffffff")
        );
        assert_eq!(office.action.timeout(), Duration::from_secs(10));
        let layout = live(config).layout();
        assert_eq!((layout.cols, layout.rows), (5, 3));
        assert_eq!(
            (layout.buttons[0].bg, layout.buttons[0].fg),
            (0x1e88e5, 0xffffff)
        );

        let default = parse(DEFAULT, &Ok(Vec::new())).unwrap();
        assert_eq!(default.buttons[0].id, "hello");
        assert!(matches!(
            &default.buttons[0].action,
            Action::Shell { argv, .. } if argv[0] == "notify-send"
        ));
    }

    #[test]
    fn a_toml_error_names_its_line() {
        assert_refused("version = 1\n[layout]\ncols = x\n", "line 3");
    }

    #[test]
    fn unknown_fields_are_refused_tiles_included() {
        assert_refused(
            &example_with("label = \"Plex\"", "name = \"Plex\""),
            "unknown field `name`",
        );
        assert_refused(
            &with_tile("every_s = 15, source = SHELL, extra = 1"),
            "unknown field `extra`",
        );
        assert_refused(&with_tile("every_s = 15"), "missing field `source`");
        assert_refused(&with_tile("source = SHELL"), "missing field `every_s`");
        assert_refused(
            &example_with("timeout_s = 30", "timeout_s = 30, shell = true"),
            "shell",
        );
        assert_refused(&example_with("kind = \"shell\"", "kind = \"exec\""), "exec");
    }

    /// A shell source that prints a number.
    const SHELL: &str = r#"{ kind = "shell", argv = ["uptime"] }"#;

    /// The example with `tile = { <fields> }` on its first button, `SHELL` standing for the source.
    fn with_tile(fields: &str) -> String {
        example_with(
            "label = \"Plex\"",
            &format!(
                "label = \"Plex\"\ntile = {{ {} }}",
                fields.replace("SHELL", SHELL)
            ),
        )
    }

    #[test]
    fn a_tile_with_a_shell_or_http_source_passes() {
        let config = parse(&with_tile("every_s = 15, source = SHELL"), &names()).unwrap();
        let tile = config.buttons[0].tile.as_ref().unwrap();
        assert_eq!((tile.every_s, tile.json_pointer.as_deref()), (15, None));
        assert!(config.buttons[1].tile.is_none());

        let http = with_tile(
            r#"every_s = 2, json_pointer = "/state",
               source = { kind = "http", method = "GET", url = "https://ha.lan:8123/api/states/light.office",
                          secret_headers = { Authorization = "ha" }, timeout_s = 5 }"#,
        );
        let config = parse(&http, &names()).unwrap();
        let tile = config.buttons[0].tile.as_ref().unwrap();
        assert_eq!(tile.json_pointer.as_deref(), Some("/state"));
        assert_eq!(tile.source.timeout(), Duration::from_secs(5));

        let layout = live(config).layout();
        assert_eq!(
            layout.buttons.iter().map(|b| b.tile).collect::<Vec<_>>(),
            [true, false]
        );
    }

    #[test]
    fn a_tile_polls_every_2_s_to_a_day() {
        for (every_s, ok) in [
            (0, false),
            (1, false),
            (2, true),
            (86_400, true),
            (86_401, false),
        ] {
            let text = with_tile(&format!("every_s = {every_s}, source = SHELL"));
            match parse(&text, &names()) {
                Ok(_) => assert!(ok, "every_s = {every_s} must be refused"),
                Err(errors) => {
                    assert!(!ok, "every_s = {every_s}: {errors:?}");
                    assert!(
                        errors[0]
                            .contains(&format!("tile.every_s is {every_s}, expected 2 to 86400")),
                        "{errors:?}"
                    );
                }
            }
        }
        assert_refused(&with_tile("every_s = -1, source = SHELL"), "invalid value");
    }

    #[test]
    fn a_json_pointer_must_start_with_a_slash() {
        for pointer in ["state", "", "#/state"] {
            assert_refused(
                &with_tile(&format!(
                    "every_s = 5, json_pointer = {pointer:?}, source = SHELL"
                )),
                "tile.json_pointer must start with '/'",
            );
        }
    }

    #[test]
    fn a_tile_source_obeys_every_action_rule() {
        let refused = |source: &str, expected: &str| {
            assert_refused(
                &with_tile(&format!("every_s = 5, source = {source}")),
                expected,
            )
        };
        refused(
            r#"{ kind = "shell", argv = [] }"#,
            "tile.source: argv is empty",
        );
        refused(
            r#"{ kind = "shell", argv = ["x"], timeout_s = 601 }"#,
            "tile.source: timeout_s is 601",
        );
        refused(
            r#"{ kind = "http", method = "GET", url = "ftp://h/" }"#,
            "tile.source: url is not http:// or https://",
        );
        refused(
            r#"{ kind = "http", method = "FETCH", url = "https://h/" }"#,
            "tile.source: method",
        );
        refused(
            r#"{ kind = "http", method = "GET", url = "https://h/", secret_headers = { Authorization = "nope" } }"#,
            "tile.source: header \"Authorization\" uses secret \"nope\"",
        );
        refused(
            r#"{ kind = "http", method = "GET", url = "http://nas.lan/", secret_headers = { Authorization = "ha" } }"#,
            "tile.source: secret_headers would cross the network in the clear",
        );
        refused(r#"{ kind = "exec" }"#, "exec");
        refused(r#"{ kind = "shell", argv = ["x"], shell = true }"#, "shell");
        // The same rules apply when the daemon checks a config the window saves.
        let errors = errors(&with_tile(
            "every_s = 1, json_pointer = \"x\", source = SHELL",
        ));
        assert_eq!(errors.len(), 2, "{errors:?}");
        assert!(errors
            .iter()
            .all(|e| e.starts_with("button \"plex\": tile.")));
    }

    #[test]
    fn an_icon_must_be_a_plain_file_name() {
        let with_icon = |icon: &str| {
            example_with(
                "label = \"Plex\"",
                &format!("label = \"Plex\"\nicon = {icon:?}"),
            )
        };
        let config = parse(&with_icon("3f2a9c.png"), &names()).unwrap();
        assert_eq!(config.buttons[0].icon.as_deref(), Some("3f2a9c.png"));
        assert_eq!(config.buttons[1].icon, None);
        for bad in ["", "../a.png", "a/b.png", ".hidden.png", "a b.png"] {
            assert_refused(&with_icon(bad), "icon must be a file name");
        }
    }

    #[test]
    fn the_version_must_be_1() {
        assert_refused(
            &EXAMPLE.replacen("version = 1", "version = 2", 1),
            "version is 2",
        );
    }

    #[test]
    fn the_grid_is_1_to_8_columns_by_1_to_5_rows() {
        for (from, to, expected) in [
            ("cols = 5", "cols = 0", "layout.cols is 0"),
            ("cols = 5", "cols = 9", "layout.cols is 9"),
            ("rows = 3", "rows = 0", "layout.rows is 0"),
            ("rows = 3", "rows = 6", "layout.rows is 6"),
        ] {
            assert_refused(&EXAMPLE.replacen(from, to, 1), expected);
        }
        let big = EXAMPLE
            .replacen("cols = 5", "cols = 8", 1)
            .replacen("rows = 3", "rows = 5", 1);
        parse(&big, &names()).unwrap();
    }

    #[test]
    fn ids_are_unique_short_and_plain() {
        assert_refused(
            &example_with("id = \"plex\"", "id = \"office-light\""),
            "another button has this id",
        );
        for bad in ["", "has space", "x\u{e9}y", &"a".repeat(64)] {
            assert_refused(
                &example_with("id = \"plex\"", &format!("id = {bad:?}")),
                "id must be",
            );
        }
        let longest = example_with("id = \"plex\"", &format!("id = {:?}", "a".repeat(63)));
        parse(&longest, &names()).unwrap();
    }

    #[test]
    fn buttons_sit_inside_the_grid_one_per_cell() {
        assert_refused(
            &example_with("pos = [0, 0]", "pos = [5, 0]"),
            "outside the 5x3 grid",
        );
        assert_refused(
            &example_with("pos = [0, 0]", "pos = [0, 3]"),
            "outside the 5x3 grid",
        );
        assert_refused(
            &example_with("pos = [0, 0]", "pos = [1, 0]"),
            "taken by button",
        );
    }

    #[test]
    fn labels_are_printable_ascii_up_to_47_bytes() {
        assert_refused(
            &example_with("label = \"Plex\"", "label = \"Pl\u{e9}x\""),
            "printable ASCII",
        );
        assert_refused(
            &example_with("label = \"Plex\"", "label = \"a\\tb\""),
            "printable ASCII",
        );
        let long = format!("label = {:?}", "a".repeat(48));
        assert_refused(&example_with("label = \"Plex\"", &long), "at most 47");
        let longest = format!("label = {:?}", "~".repeat(47));
        parse(&example_with("label = \"Plex\"", &longest), &names()).unwrap();
    }

    #[test]
    fn colors_are_hash_and_six_hex_digits() {
        for bad in ["red", "#12345", "#1234567", "#gggggg", "1e88e5"] {
            assert_refused(
                &example_with("bg = \"#1e88e5\"", &format!("bg = {bad:?}")),
                "is not a #rrggbb color",
            );
        }
        assert_refused(
            &example_with("fg = \"#ffffff\"", "fg = \"white\""),
            "fg \"white\"",
        );
        parse(
            &example_with("bg = \"#1e88e5\"", "bg = \"#ABCdef\""),
            &names(),
        )
        .unwrap();
    }

    #[test]
    fn shell_argv_is_non_empty_with_a_program_and_no_nul() {
        let argv = "argv = [\"ssh\", \"nas\", \"docker\", \"restart\", \"plex\"]";
        assert_refused(&example_with(argv, "argv = []"), "argv is empty");
        assert_refused(
            &example_with(argv, "argv = [\"\", \"x\"]"),
            "argv[0], the program, is empty",
        );
        assert_refused(
            &example_with(argv, "argv = [\"echo\", \"a\\u0000b\"]"),
            "argv[1] contains a NUL",
        );
        assert_refused(&example_with(argv, "argv = \"ssh nas\""), "invalid type");
    }

    #[test]
    fn timeouts_are_1_to_600_seconds() {
        assert_refused(
            &example_with("timeout_s = 30", "timeout_s = 0"),
            "timeout_s is 0",
        );
        assert_refused(
            &example_with("timeout_s = 30", "timeout_s = 601"),
            "timeout_s is 601",
        );
        assert_refused(
            &example_with("timeout_s = 30", "timeout_s = -1"),
            "invalid value",
        );
        let http = EXAMPLE.replacen(
            "Authorization = \"ha\" }",
            "Authorization = \"ha\" }, timeout_s = 0",
            1,
        );
        assert_refused(&http, "timeout_s is 0");
        parse(&example_with("timeout_s = 30", "timeout_s = 600"), &names()).unwrap();
    }

    #[test]
    fn http_urls_are_http_or_https_with_a_host_and_stay_out_of_errors() {
        for (bad, expected) in [
            ("ftp://ha.lan/x?token=abc", "is not http:// or https://"),
            ("file:///etc/passwd?token=abc", "is not http:// or https://"),
            ("ha.lan/x?token=abc", "does not parse"),
            ("http://?token=abc", "does not parse"),
        ] {
            let errors = errors(&EXAMPLE.replacen(EXAMPLE_URL, &format!("url = {bad:?}"), 1));
            assert!(errors.iter().any(|e| e.contains(expected)), "{errors:?}");
            assert!(
                errors.iter().all(|e| !e.contains("token=abc")),
                "{errors:?}"
            );
        }
        parse(
            &EXAMPLE.replacen(EXAMPLE_URL, "url = \"https://ha.lan/x\"", 1),
            &names(),
        )
        .unwrap();
    }

    #[test]
    fn secret_headers_need_https_unless_the_host_is_this_machine() {
        let plain = EXAMPLE.replacen(EXAMPLE_URL, "url = \"http://ha.lan:8123/x\"", 1);
        assert_refused(&plain, "in the clear");
        for here in [
            "http://127.0.0.1:8123/x",
            "http://127.9.9.9/x",
            "http://[::1]:8123/x",
            "http://localhost:8123/x",
            "http://LOCALHOST/x",
        ] {
            let text = EXAMPLE.replacen(EXAMPLE_URL, &format!("url = {here:?}"), 1);
            let config = parse(&text, &names()).unwrap();
            assert!(!config.buttons[1].action.secrets_in_clear(), "{here}");
        }
        let opted_in = plain.replacen(
            "secret_headers = {",
            "insecure_http = true, secret_headers = {",
            1,
        );
        let config = parse(&opted_in, &names()).unwrap();
        assert!(config.buttons[1].action.secrets_in_clear());
        let without_secrets =
            plain.replacen(", secret_headers = { Authorization = \"ha\" }", "", 1);
        parse(&without_secrets, &names()).unwrap();
    }

    #[test]
    fn plain_headers_are_checked_and_no_name_repeats_ignoring_case() {
        let secret = "secret_headers = { Authorization = \"ha\" }";
        let with = |headers: &str| EXAMPLE.replacen(secret, &format!("{headers}, {secret}"), 1);
        let config = parse(
            &with("headers = { \"Content-Type\" = \"application/json\" }"),
            &names(),
        )
        .unwrap();
        let Action::Http { headers, .. } = &config.buttons[1].action else {
            panic!("expected an http action");
        };
        assert_eq!(headers["Content-Type"], "application/json");

        for (headers, expected) in [
            (
                "headers = { \"X-A\" = \"1\\r\\nX-Injected: 1\" }",
                "printable ASCII",
            ),
            ("headers = { \"X-A\" = \"tab\\there\" }", "printable ASCII"),
            (
                "headers = { \"Bad Header\" = \"1\" }",
                "not a valid header name",
            ),
            (
                "headers = { authorization = \"Basic x\" }",
                "\"Authorization\" is set twice",
            ),
        ] {
            assert_refused(&with(headers), expected);
        }
        assert_refused(
            &EXAMPLE.replacen(
                secret,
                "secret_headers = { Authorization = \"ha\", authorization = \"ha\" }",
                1,
            ),
            "is set twice",
        );
    }

    #[test]
    fn a_toml_error_keeps_its_place_but_not_the_line() {
        let errors = errors("version = 1\nurl = \"http://x/?token=abc\n");
        assert!(errors[0].starts_with("line 2, column "), "{errors:?}");
        assert!(!errors[0].contains("token=abc"), "{errors:?}");
    }

    #[test]
    fn http_methods_bodies_and_headers_are_checked() {
        assert_refused(
            &EXAMPLE.replacen("method = \"POST\"", "method = \"post\"", 1),
            "is not one of",
        );
        assert_refused(
            &EXAMPLE.replacen("method = \"POST\"", "method = \"TRACE\"", 1),
            "is not one of",
        );
        let body = format!("body = '{}'", "a".repeat(MAX_BODY_BYTES + 1));
        assert_refused(
            &EXAMPLE.replacen("body = '{\"entity_id\":\"light.office\"}'", &body, 1),
            "body is over",
        );
        let header = "secret_headers = { Authorization = \"ha\" }";
        assert_refused(
            &EXAMPLE.replacen(header, "secret_headers = { \"Bad Header\" = \"ha\" }", 1),
            "not a valid header name",
        );
    }

    #[test]
    fn secret_headers_name_secrets_that_exist() {
        let errors = parse(EXAMPLE, &Ok(vec!["other".into()])).unwrap_err();
        assert!(
            errors[0].contains("secret \"ha\", which is not in secrets.toml"),
            "{errors:?}"
        );
        let unreadable = Err(io::Error::new(io::ErrorKind::PermissionDenied, "mode 0644"));
        let errors = parse(EXAMPLE, &unreadable).unwrap_err();
        assert!(errors[0].contains("can't be read: mode 0644"), "{errors:?}");
        // A config without secrets doesn't care whether secrets.toml reads.
        let shell_only = EXAMPLE
            .split("[[button]]")
            .take(2)
            .collect::<Vec<_>>()
            .join("[[button]]");
        parse(&shell_only, &Err(io::Error::other("unreadable"))).unwrap();
    }

    #[test]
    fn every_problem_is_reported_at_once() {
        let text = example_with("pos = [0, 0]", "pos = [9, 9]").replacen(
            "bg = \"#1e88e5\"",
            "bg = \"blue\"",
            1,
        );
        let errors = parse(&text, &Ok(Vec::new())).unwrap_err();
        assert_eq!(errors.len(), 3, "{errors:?}");
    }

    #[test]
    fn the_fullest_layout_fits_in_one_message() {
        let mut config = Config::empty();
        config.layout = Grid {
            cols: MAX_COLS,
            rows: MAX_ROWS,
        };
        for i in 0..MAX_COLS * MAX_ROWS {
            config.buttons.push(ButtonConfig {
                id: format!("{i:0>63}"),
                pos: [i % MAX_COLS, i / MAX_COLS],
                label: "\"\\".repeat(MAX_LABEL_BYTES / 2) + "\"",
                bg: "#ffffff".into(),
                fg: "#ffffff".into(),
                icon: Some("i.png".into()),
                action: Action::Shell {
                    argv: vec!["true".into()],
                    timeout_s: 1,
                },
                tile: Some(TileConfig {
                    every_s: MIN_EVERY_S,
                    json_pointer: None,
                    source: Action::Shell {
                        argv: vec!["true".into()],
                        timeout_s: 1,
                    },
                }),
            });
        }
        assert!(config.check(&Ok(Vec::new())).is_empty());
        let mut live = live(config);
        live.icons
            .insert("i.png".into(), Arc::new(IconFrame::new(1, 1, vec![0; 4])));
        let json = serde_json::to_string(&live.layout().message(u32::MAX)).unwrap();
        assert!(json.len() <= MAX_MESSAGE_BYTES, "{} bytes", json.len());
    }

    /// `config` as it goes live when it names no icons.
    fn live(config: Config) -> Live {
        Live {
            config,
            icons: IconSet::new(),
        }
    }

    // ---- The file ----

    fn open(dir: &Path) -> Arc<Configs> {
        Configs::open(dir, Arc::new(Secrets::new(dir))).unwrap()
    }

    fn mode(path: &Path) -> u32 {
        fs::metadata(path).unwrap().permissions().mode() & 0o777
    }

    #[test]
    fn a_missing_file_gets_the_default_written_0600_and_loaded() {
        let tmp = tempfile::tempdir().unwrap();
        let configs = open(tmp.path());
        let path = tmp.path().join(FILE);
        assert_eq!(fs::read_to_string(&path).unwrap(), DEFAULT);
        assert_eq!(mode(&path), 0o600);
        assert_eq!(configs.current().buttons[0].id, "hello");
        let status = configs.status().borrow().clone();
        assert!(status.errors.is_empty());
        assert_eq!(status.revision, sha256_hex(DEFAULT.as_bytes()));
    }

    #[test]
    fn an_invalid_file_at_start_runs_an_empty_grid_and_says_why() {
        let tmp = tempfile::tempdir().unwrap();
        fs::write(
            tmp.path().join(FILE),
            "version = 2\n[layout]\ncols = 5\nrows = 3\n",
        )
        .unwrap();
        let configs = open(tmp.path());
        assert_eq!(configs.current().config, Config::empty());
        let status = configs.status().borrow().clone();
        assert!(status.errors[0].contains("version is 2"), "{status:?}");
        assert!(!status.loaded, "no good config has loaded");
        assert_eq!(
            status.revision,
            sha256_hex(b"version = 2\n[layout]\ncols = 5\nrows = 3\n"),
            "the revision names the file that was refused, so a save can replace it"
        );
    }

    #[test]
    fn a_save_repairs_a_file_that_was_invalid_at_start_and_keeps_the_old_text() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join(FILE);
        let broken = "version = 2\n[layout]\ncols = 5\nrows = 3\n";
        fs::write(&path, broken).unwrap();
        let configs = open(tmp.path());
        let status = configs.status().borrow().clone();
        assert!(!status.errors.is_empty());

        let mut config = Config::empty();
        config.layout.cols = 4;
        let revision = configs.save(config.clone(), &status.revision).unwrap();
        assert_eq!(fs::read_to_string(&backups(tmp.path())[0]).unwrap(), broken);
        assert_eq!(mode(&backups(tmp.path())[0]), 0o600);
        assert_eq!(configs.current().config, config);
        let status = configs.status().borrow().clone();
        assert!(status.errors.is_empty() && status.loaded);
        assert_eq!(status.revision, revision);
        assert_eq!(
            parse(&fs::read_to_string(&path).unwrap(), &Ok(Vec::new())).unwrap(),
            config
        );
    }

    /// The `config.toml.bak.*` files in `dir`, oldest first.
    fn backups(dir: &Path) -> Vec<PathBuf> {
        let mut found: Vec<PathBuf> = fs::read_dir(dir)
            .unwrap()
            .map(|e| e.unwrap().path())
            .filter(|p| {
                p.file_name()
                    .unwrap()
                    .to_string_lossy()
                    .starts_with("config.toml.bak.")
            })
            .collect();
        found.sort();
        found
    }

    #[test]
    fn repeated_repairs_keep_each_old_text_in_its_own_timestamped_backup_and_the_newest_five() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join(FILE);
        fs::write(&path, "broken 0").unwrap();
        let configs = open(tmp.path());
        let mut config = Config::empty();
        for n in 1..=7 {
            let revision = configs.status().borrow().revision.clone();
            configs.save(config.clone(), &revision).unwrap();
            // A hand edit breaks the file again.
            fs::write(&path, format!("broken {n}")).unwrap();
            configs.reload().unwrap_err();
        }
        let kept = backups(tmp.path());
        assert_eq!(kept.len(), KEEP_BACKUPS);
        let name = kept[0].file_name().unwrap().to_string_lossy().into_owned();
        let stamp = name.strip_prefix("config.toml.bak.").unwrap();
        assert!(
            stamp.len() == 16 && stamp.ends_with('Z') && stamp.as_bytes()[8] == b'T',
            "{name}"
        );
        // Saves in the same second never overwrite, so the newest five texts are 2..=6.
        let texts: Vec<String> = kept
            .iter()
            .map(|p| fs::read_to_string(p).unwrap())
            .collect();
        assert_eq!(
            texts,
            ["broken 2", "broken 3", "broken 4", "broken 5", "broken 6"].map(String::from)
        );
        config.layout.cols = 3;
    }

    #[test]
    fn backup_pruning_ignores_files_that_only_look_like_backups() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join(FILE);
        // A name that sorts above every stamp used to stall the stamp search, and a user's own
        // file or a directory used to be pruned.
        let others = ["config.toml.bak.zzz", "config.toml.bak.manual"];
        for name in others {
            fs::write(tmp.path().join(name), "mine").unwrap();
        }
        fs::create_dir(tmp.path().join("config.toml.bak.20000101T000000Z")).unwrap();
        fs::write(&path, "broken 0").unwrap();
        let configs = open(tmp.path());
        for n in 1..=KEEP_BACKUPS + 2 {
            let revision = configs.status().borrow().revision.clone();
            configs.save(Config::empty(), &revision).unwrap();
            fs::write(&path, format!("broken {n}")).unwrap();
            configs.reload().unwrap_err();
        }
        for name in others {
            assert_eq!(fs::read_to_string(tmp.path().join(name)).unwrap(), "mine");
        }
        assert!(tmp.path().join("config.toml.bak.20000101T000000Z").is_dir());
        assert_eq!(
            configs.backups(tmp.path(), "config.toml.bak.").len(),
            KEEP_BACKUPS
        );
    }

    #[test]
    fn a_repair_never_overwrites_a_newer_hand_edit() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join(FILE);
        fs::write(&path, "version = 2\n").unwrap();
        let configs = open(tmp.path());
        let seen = configs.status().borrow().revision.clone();

        // The window loaded the first broken file; then the user edited it again.
        fs::write(&path, "version = 3\n").unwrap();
        let errors = configs.save(Config::empty(), &seen).unwrap_err();
        assert!(errors[0].contains("changed since"), "{errors:?}");
        assert_eq!(fs::read_to_string(&path).unwrap(), "version = 3\n");
        assert!(backups(tmp.path()).is_empty());
    }

    #[test]
    fn a_hand_edit_that_breaks_a_live_config_is_replaced_only_by_name() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join(FILE);
        let configs = open(tmp.path());
        let loaded = configs.status().borrow().revision.clone();
        let config = configs.current().config.clone();

        fs::write(&path, "not toml [").unwrap();
        configs.reload().unwrap_err();
        // The window still holds the old revision, which no longer matches the file.
        assert!(configs.save(config.clone(), &loaded).is_err());
        let refused = configs.status().borrow().revision.clone();
        assert_ne!(refused, loaded);
        configs.save(config, &refused).unwrap();
        assert_eq!(
            fs::read_to_string(&backups(tmp.path())[0]).unwrap(),
            "not toml ["
        );
    }

    #[test]
    fn a_deleted_file_is_recreated_by_a_save_naming_no_revision() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join(FILE);
        let configs = open(tmp.path());
        let config = configs.current().config.clone();
        fs::remove_file(&path).unwrap();
        configs.reload().unwrap_err();
        assert_eq!(configs.status().borrow().revision, "");
        assert!(configs.save(config.clone(), "0123").is_err());
        configs.save(config, "").unwrap();
        assert!(path.exists());
        assert!(backups(tmp.path()).is_empty());
    }

    #[test]
    fn a_bad_reload_keeps_the_live_config_and_a_good_one_replaces_it() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join(FILE);
        let configs = open(tmp.path());
        let mut live = configs.subscribe();
        live.mark_unchanged();

        fs::write(
            &path,
            DEFAULT.replacen("label = \"Hello\"", "label = \"H\u{e9}llo\"", 1),
        )
        .unwrap();
        let errors = configs.reload().unwrap_err();
        assert!(errors[0].contains("printable ASCII"), "{errors:?}");
        assert!(
            !live.has_changed().unwrap(),
            "an invalid config must not go live"
        );
        assert_eq!(configs.current().buttons[0].label, "Hello");
        assert_eq!(configs.status().borrow().errors, errors);

        fs::write(
            &path,
            DEFAULT.replacen("label = \"Hello\"", "label = \"Hi\"", 1),
        )
        .unwrap();
        assert_eq!(configs.reload(), Ok(true));
        assert!(live.has_changed().unwrap());
        assert_eq!(configs.current().buttons[0].label, "Hi");
        assert!(configs.status().borrow().errors.is_empty());

        // A comment-only edit is a new revision of the same config, so nothing is pushed.
        live.mark_unchanged();
        fs::write(
            &path,
            format!("# note\n{}", fs::read_to_string(&path).unwrap()),
        )
        .unwrap();
        assert_eq!(configs.reload(), Ok(false));
        assert!(!live.has_changed().unwrap());
    }

    #[test]
    fn a_missing_secret_holds_the_config_back_until_it_is_set() {
        let tmp = tempfile::tempdir().unwrap();
        fs::write(tmp.path().join(FILE), EXAMPLE).unwrap();
        let secrets = Arc::new(Secrets::new(tmp.path()));
        let configs = Configs::open(tmp.path(), secrets.clone()).unwrap();
        assert!(configs.current().buttons.is_empty());
        secrets.set("ha", "Bearer x").unwrap();
        assert_eq!(configs.reload(), Ok(true));
        assert_eq!(configs.current().buttons.len(), 2);
    }

    #[test]
    fn save_writes_0600_and_goes_live_but_only_from_the_revision_on_disk() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join(FILE);
        let configs = open(tmp.path());
        let loaded = configs.status().borrow().revision.clone();
        let mut config = configs.current().config.clone();
        config.buttons[0].label = "Saved".into();

        let stale = configs.save(config.clone(), "0123").unwrap_err();
        assert!(
            stale[0].contains("changed since it was loaded"),
            "{stale:?}"
        );
        let revision = configs.save(config.clone(), &loaded).unwrap();
        assert_ne!(revision, loaded);
        assert_eq!(mode(&path), 0o600);
        assert_eq!(configs.current().buttons[0].label, "Saved");
        assert_eq!(
            parse(&fs::read_to_string(&path).unwrap(), &Ok(Vec::new())).unwrap(),
            config
        );
        assert_eq!(
            configs.reload(),
            Ok(false),
            "the poller finds nothing new after a save"
        );

        // A hand edit that isn't live yet is never overwritten.
        fs::write(&path, "version = 1\n").unwrap();
        let held = configs.save(config, &revision).unwrap_err();
        assert!(held[0].contains("changed since"), "{held:?}");
        assert_eq!(fs::read_to_string(&path).unwrap(), "version = 1\n");
    }

    #[test]
    fn an_invalid_save_writes_nothing() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join(FILE);
        let configs = open(tmp.path());
        let revision = configs.status().borrow().revision.clone();
        let mut config = configs.current().config.clone();
        config.buttons[0].pos = [7, 0];
        let errors = configs.save(config, &revision).unwrap_err();
        assert!(errors[0].contains("outside"), "{errors:?}");
        assert_eq!(fs::read_to_string(&path).unwrap(), DEFAULT);
    }

    #[test]
    fn an_unreadable_or_huge_file_is_an_error_not_a_crash() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join(FILE);
        let configs = open(tmp.path());
        fs::write(&path, vec![b'#'; MAX_FILE_BYTES as usize + 1]).unwrap();
        assert!(configs.reload().unwrap_err()[0].contains("is over"));
        fs::write(&path, b"version = 1\xff").unwrap();
        assert_eq!(configs.reload().unwrap_err(), ["not UTF-8"]);
        fs::set_permissions(&path, Permissions::from_mode(0o000)).unwrap();
        // Root reads anything, so only check that a failure, if any, is reported.
        if fs::read(&path).is_err() {
            assert!(configs.reload().is_err());
        }
        fs::remove_file(&path).unwrap();
        assert!(configs.reload().is_err());
        assert_eq!(configs.current().buttons[0].id, "hello");
    }

    #[tokio::test(start_paused = true)]
    async fn the_poller_applies_an_edit_once_it_settles() {
        let tmp = tempfile::tempdir().unwrap();
        let path = tmp.path().join(FILE);
        let configs = open(tmp.path());
        let mut live = configs.subscribe();
        live.mark_unchanged();
        tokio::spawn(configs.clone().watch_files());
        tokio::time::sleep(POLL / 2).await;

        fs::write(
            &path,
            DEFAULT.replacen("label = \"Hello\"", "label = \"Polled\"", 1),
        )
        .unwrap();
        tokio::time::sleep(POLL).await;
        assert!(
            !live.has_changed().unwrap(),
            "applied before the file settled"
        );
        tokio::time::timeout(3 * POLL, live.changed())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(configs.current().buttons[0].label, "Polled");
    }

    #[tokio::test(start_paused = true)]
    async fn the_chmod_that_makes_secrets_readable_applies_the_config() {
        let tmp = tempfile::tempdir().unwrap();
        fs::write(tmp.path().join(FILE), EXAMPLE).unwrap();
        let secrets_path = tmp.path().join(crate::daemon::secrets::FILE);
        fs::write(&secrets_path, "ha = \"Bearer x\"\n").unwrap();
        fs::set_permissions(&secrets_path, Permissions::from_mode(0o644)).unwrap();
        let configs = open(tmp.path());
        let errors = configs.status().borrow().errors.clone();
        assert!(errors[0].contains("can't be read"), "{errors:?}");
        let mut live = configs.subscribe();
        live.mark_unchanged();
        tokio::spawn(configs.clone().watch_files());
        tokio::time::sleep(POLL / 2).await;

        // Only the mode changes, not the content, size or inode.
        fs::set_permissions(&secrets_path, Permissions::from_mode(0o600)).unwrap();
        tokio::time::timeout(4 * POLL, live.changed())
            .await
            .expect("the chmod was not picked up")
            .unwrap();
        assert_eq!(configs.current().buttons.len(), 2);
    }

    #[tokio::test(start_paused = true)]
    async fn an_edit_made_before_the_poller_starts_still_applies() {
        let tmp = tempfile::tempdir().unwrap();
        let configs = open(tmp.path());
        let mut live = configs.subscribe();
        live.mark_unchanged();
        fs::write(
            tmp.path().join(FILE),
            DEFAULT.replacen("label = \"Hello\"", "label = \"Early\"", 1),
        )
        .unwrap();

        tokio::spawn(configs.clone().watch_files());
        tokio::time::timeout(4 * POLL, live.changed())
            .await
            .expect("the edit was lost")
            .unwrap();
        assert_eq!(configs.current().buttons[0].label, "Early");
    }

    /// A 4x4 PNG, drawn as is on any grid.
    fn tiny_png() -> Vec<u8> {
        let picture = image::RgbaImage::from_pixel(4, 4, image::Rgba([9, 9, 9, 255]));
        let mut out = std::io::Cursor::new(Vec::new());
        picture.write_to(&mut out, image::ImageFormat::Png).unwrap();
        out.into_inner()
    }

    #[tokio::test(start_paused = true)]
    async fn a_config_waits_for_its_icon_and_the_poller_notices_the_file_arriving() {
        let tmp = tempfile::tempdir().unwrap();
        fs::write(
            tmp.path().join(FILE),
            DEFAULT.replacen(
                "label = \"Hello\"",
                "label = \"Hello\"\nicon = \"a.png\"",
                1,
            ),
        )
        .unwrap();
        let configs = open(tmp.path());
        let errors = configs.status().borrow().errors.clone();
        assert!(errors[0].contains("\"a.png\": not found"), "{errors:?}");
        assert!(configs.current().buttons.is_empty());
        let mut live = configs.subscribe();
        live.mark_unchanged();
        tokio::spawn(configs.clone().watch_files());
        tokio::time::sleep(POLL / 2).await;

        fs::create_dir(tmp.path().join(icons::DIR)).unwrap();
        fs::write(tmp.path().join(icons::DIR).join("a.png"), tiny_png()).unwrap();
        tokio::time::timeout(4 * POLL, live.changed())
            .await
            .expect("the new icon was not picked up")
            .unwrap();
        let layout = configs.current().layout();
        assert_eq!(layout.buttons[0].icon.as_ref().map(String::len), Some(32));
        assert_eq!((layout.icons[0].w, layout.icons[0].h), (4, 4));
    }

    #[tokio::test(start_paused = true)]
    async fn a_half_copied_icon_is_read_again_once_the_file_is_finished_in_place() {
        let tmp = tempfile::tempdir().unwrap();
        fs::write(
            tmp.path().join(FILE),
            DEFAULT.replacen(
                "label = \"Hello\"",
                "label = \"Hello\"\nicon = \"a.png\"",
                1,
            ),
        )
        .unwrap();
        let png = tiny_png();
        fs::create_dir(tmp.path().join(icons::DIR)).unwrap();
        let file = tmp.path().join(icons::DIR).join("a.png");
        fs::write(&file, &png[..png.len() / 2]).unwrap();
        let configs = open(tmp.path());
        assert!(!configs.status().borrow().errors.is_empty());
        let mut live = configs.subscribe();
        live.mark_unchanged();
        tokio::spawn(configs.clone().watch_files());
        tokio::time::sleep(POLL / 2).await;

        // The rest arrives in the same file, so the directory's stamp doesn't change.
        fs::write(&file, &png).unwrap();
        tokio::time::timeout(4 * POLL, live.changed())
            .await
            .expect("the finished icon was not picked up")
            .unwrap();
        assert_eq!(configs.current().buttons.len(), 1);
    }

    #[test]
    fn a_save_names_icons_that_exist_and_an_import_makes_one() {
        let tmp = tempfile::tempdir().unwrap();
        let configs = open(tmp.path());
        let revision = configs.status().borrow().revision.clone();
        let mut config = configs.current().config.clone();
        config.buttons[0].icon = Some("a.png".into());

        let errors = configs.save(config.clone(), &revision).unwrap_err();
        assert!(errors[0].contains("\"a.png\": not found"), "{errors:?}");
        assert_eq!(fs::read_to_string(tmp.path().join(FILE)).unwrap(), DEFAULT);

        let src = tmp.path().join("source.png");
        fs::write(&src, tiny_png()).unwrap();
        let name = configs.import_icon(&src).unwrap();
        config.buttons[0].icon = Some(name.clone());
        let revision = configs.save(config.clone(), &revision).unwrap();
        assert_eq!(configs.current().buttons[0].icon, Some(name));
        let text = fs::read_to_string(tmp.path().join(FILE)).unwrap();
        assert_eq!(parse(&text, &names()).unwrap(), config);
        assert_eq!(configs.reload(), Ok(false), "{revision}");
    }

    #[test]
    fn a_secret_in_use_by_a_button_or_a_tile_cant_be_deleted() {
        let tmp = tempfile::tempdir().unwrap();
        let secrets = Arc::new(Secrets::new(tmp.path()));
        for name in ["ha", "tile-only", "spare"] {
            secrets.set(name, "Bearer x").unwrap();
        }
        let http = |name: &str| Action::Http {
            method: "GET".into(),
            url: "https://ha.lan/api".into(),
            body: None,
            headers: BTreeMap::new(),
            secret_headers: BTreeMap::from([("Authorization".into(), name.into())]),
            insecure_http: false,
            timeout_s: 5,
        };
        let button = |id: &str, col, action: Action, tile: Option<TileConfig>| ButtonConfig {
            id: id.into(),
            pos: [col, 0],
            label: id.into(),
            bg: "#000000".into(),
            fg: "#ffffff".into(),
            icon: None,
            action,
            tile,
        };
        let shell = Action::Shell {
            argv: vec!["true".into()],
            timeout_s: 5,
        };
        let tile = TileConfig {
            every_s: 30,
            json_pointer: None,
            source: http("tile-only"),
        };
        let configs = Configs::open(tmp.path(), secrets.clone()).unwrap();
        let revision = configs.status().borrow().revision.clone();
        let mut config = configs.current().config.clone();
        config.buttons = vec![
            button("a", 0, http("ha"), None),
            button("b", 1, shell, Some(tile)),
            button("c", 2, http("ha"), None),
        ];
        configs.save(config, &revision).unwrap();

        let err = configs.delete_secret("ha").unwrap_err();
        assert!(err.contains("button a, c"), "{err}");
        let err = configs.delete_secret("tile-only").unwrap_err();
        assert!(err.contains("button b"), "{err}");
        assert_eq!(secrets.names().unwrap(), ["ha", "spare", "tile-only"]);

        configs.delete_secret("spare").unwrap();
        assert_eq!(secrets.names().unwrap(), ["ha", "tile-only"]);
        let mode = fs::metadata(secrets.path()).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600);
        assert!(configs
            .delete_secret("spare")
            .unwrap_err()
            .contains("no secret"));
        assert!(configs.status().borrow().errors.is_empty());
    }

    #[test]
    fn a_save_is_refused_when_its_secret_is_deleted_after_the_first_check() {
        let tmp = tempfile::tempdir().unwrap();
        let secrets = Arc::new(Secrets::new(tmp.path()));
        secrets.set("ha", "Bearer x").unwrap();
        let configs = Configs::open(tmp.path(), secrets.clone()).unwrap();
        let revision = configs.status().borrow().revision.clone();
        let mut config = configs.current().config.clone();
        config.buttons[0].action = Action::Http {
            method: "GET".into(),
            url: "https://ha.lan/api".into(),
            body: None,
            headers: BTreeMap::new(),
            secret_headers: BTreeMap::from([("Authorization".into(), "ha".into())]),
            insecure_http: false,
            timeout_s: 5,
        };
        // Hold the config lock so the save passes its first check, then waits. The secret goes
        // while it waits, as a delete that got the lock first would remove it.
        let errors = std::thread::scope(|s| {
            let held = configs.locked();
            let save = s.spawn(|| configs.save(config.clone(), &revision));
            std::thread::sleep(Duration::from_millis(300));
            secrets.remove("ha").unwrap();
            drop(held);
            save.join().unwrap().unwrap_err()
        });
        assert!(errors[0].contains("secret \"ha\""), "{errors:?}");
        assert_ne!(configs.current().config, config);
    }

    #[test]
    fn a_save_round_trips_http_actions_with_every_field() {
        let tmp = tempfile::tempdir().unwrap();
        let secrets = Arc::new(Secrets::new(tmp.path()));
        secrets.set("ha", "Bearer x").unwrap();
        let configs = Configs::open(tmp.path(), secrets).unwrap();
        let revision = configs.status().borrow().revision.clone();
        let http = |id: &str, col, url: &str, insecure_http| ButtonConfig {
            id: id.into(),
            pos: [col, 0],
            label: id.into(),
            bg: "#000000".into(),
            fg: "#ffffff".into(),
            icon: None,
            tile: None,
            action: Action::Http {
                method: "POST".into(),
                url: url.into(),
                body: Some("{\"a\": \"b\\nc\"}\n'quoted'".into()),
                headers: BTreeMap::from([("Content-Type".into(), "application/json".into())]),
                secret_headers: BTreeMap::from([("Authorization".into(), "ha".into())]),
                insecure_http,
                timeout_s: 5,
            },
        };
        let mut config = configs.current().config.clone();
        let mut web = http("web", 1, "https://ha.lan/api", false);
        web.tile = Some(TileConfig {
            every_s: 30,
            json_pointer: Some("/state".into()),
            source: web.action.clone(),
        });
        config.buttons.push(web);
        config
            .buttons
            .push(http("lan", 2, "http://ha.lan/api", true));

        configs.save(config.clone(), &revision).unwrap();
        let text = fs::read_to_string(tmp.path().join(FILE)).unwrap();
        assert_eq!(text.matches("insecure_http").count(), 1, "{text}");
        assert_eq!(parse(&text, &names()).unwrap(), config);
    }
}
