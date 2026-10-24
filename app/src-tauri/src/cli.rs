//! `watchdeck notify`, `card` and `dismiss`: the push API from a script, as subcommands of the
//! app's own binary. `main` runs them before Tauri starts, so no window or single-instance check
//! is involved. The request goes to the loopback listener with `std::net`.
//!
//! Exit codes: 0 when the daemon took the request, 2 when nothing listens (the app isn't
//! running, or has no push integration), and 1 for anything else, with the reason on stderr.

use std::io::{self, Read, Write};
use std::net::{Ipv4Addr, SocketAddr, TcpStream};
use std::path::{Path, PathBuf};
use std::time::Duration;

use crate::daemon::config::{self, Config, IntegrationConfig};
use crate::daemon::push::LOOPBACK_PORT;
use crate::daemon::secrets::{valid_value, Secrets};

const COMMANDS: [&str; 3] = ["notify", "card", "dismiss"];
const CONNECT_TIMEOUT: Duration = Duration::from_secs(2);
const IO_TIMEOUT: Duration = Duration::from_secs(10);
/// The daemon's replies are a few dozen bytes.
const MAX_REPLY_BYTES: u64 = 64 * 1024;
const TOKEN_VAR: &str = "WATCHDECK_TOKEN";

const USAGE: &str = "\
usage:
  watchdeck notify  [--integration ID] [--level info|ok|warn|error] [--id ID] [--ttl SECONDS] TITLE [BODY]
  watchdeck card    [--integration ID] [--state ok|bad] [--label LABEL] ID VALUE
  watchdeck dismiss [--integration ID] ID

The token is WATCHDECK_TOKEN when set. Otherwise it is read from config.toml and
secrets.toml: the push integration --integration names, or the only one.";

/// Where a run sends to and finds its token.
pub struct Env {
    pub addr: SocketAddr,
    /// `WATCHDECK_TOKEN`, which wins over the files.
    pub token: Option<String>,
    /// The config dir, for the token when `token` is `None`.
    pub dir: Option<PathBuf>,
}

/// Runs the subcommand in the process's arguments, if they start with one, and returns its exit
/// code. `None` means the app should start as usual.
pub fn main() -> Option<i32> {
    let args: Vec<String> = (std::env::args_os().skip(1))
        .map(|a| a.to_string_lossy().into_owned())
        .collect();
    if !args.first().is_some_and(|a| COMMANDS.contains(&a.as_str())) {
        return None;
    }
    let env = Env {
        addr: (Ipv4Addr::LOCALHOST, LOOPBACK_PORT).into(),
        token: std::env::var(TOKEN_VAR).ok().filter(|t| !t.is_empty()),
        dir: std::env::home_dir()
            .map(|home| crate::config_dir(std::env::var_os("XDG_CONFIG_HOME"), &home)),
    };
    Some(run(&args, &env, &mut io::stdout(), &mut io::stderr()))
}

/// Runs `args` (the subcommand first) against `env`. Messages go to `out` and `err`, and never
/// hold the token.
pub fn run(args: &[String], env: &Env, out: &mut dyn Write, err: &mut dyn Write) -> i32 {
    // The writes are a script's stdout and stderr; there is no better place for their errors.
    let command = match parse(args) {
        Ok(Some(command)) => command,
        Ok(None) => {
            let _ = writeln!(out, "{USAGE}");
            return 0;
        }
        Err(why) => {
            let _ = writeln!(err, "watchdeck: {why}\n\n{USAGE}");
            return 1;
        }
    };
    let token = match token(env, command.integration.as_deref()) {
        Ok(token) => token,
        Err(why) => {
            let _ = writeln!(err, "watchdeck: {why}");
            return 1;
        }
    };
    match send(env.addr, &token, &command) {
        Err(Failure::NotListening(e)) => {
            let _ = writeln!(
                err,
                "watchdeck: nothing is listening on {} ({e}). Is the app running, with a push \
                 integration?",
                env.addr
            );
            2
        }
        Err(Failure::Other(why)) => {
            let _ = writeln!(err, "watchdeck: {why}");
            1
        }
        Ok(reply) if (200..300).contains(&reply.status) => {
            if let Some(id) = field(&reply.body, "id") {
                let _ = writeln!(out, "{id}");
            }
            0
        }
        Ok(reply) => {
            let reason = field(&reply.body, "error").unwrap_or_else(|| reply.body.clone());
            let _ = writeln!(err, "watchdeck: refused ({}): {reason}", reply.status);
            1
        }
    }
}

#[derive(Debug, Default, PartialEq, Eq)]
struct Command {
    integration: Option<String>,
    method: &'static str,
    /// Under `/v1/`, percent-encoded.
    path: String,
    /// JSON, or empty.
    body: String,
}

/// The request `args` asks for, `None` for `--help`, or why `args` are wrong.
fn parse(args: &[String]) -> Result<Option<Command>, String> {
    let (name, rest) = args.split_first().ok_or("no command")?;
    let mut options: Vec<(String, String)> = Vec::new();
    let mut positional = Vec::new();
    let mut rest = rest.iter();
    while let Some(arg) = rest.next() {
        if arg == "--" {
            positional.extend(rest.by_ref().cloned());
        } else if arg == "-h" || arg == "--help" {
            return Ok(None);
        } else if let Some(option) = arg.strip_prefix("--") {
            let (key, value) = match option.split_once('=') {
                Some((key, value)) => (key.to_string(), value.to_string()),
                None => {
                    let value = rest.next().ok_or(format!("--{option} needs a value"))?;
                    (option.to_string(), value.clone())
                }
            };
            options.push((key, value));
        } else {
            positional.push(arg.clone());
        }
    }
    let allowed: &[&str] = match name.as_str() {
        "notify" => &["integration", "level", "id", "ttl"],
        "card" => &["integration", "state", "label"],
        "dismiss" => &["integration"],
        other => return Err(format!("unknown command {other:?}")),
    };
    if let Some((key, _)) = options.iter().find(|(k, _)| !allowed.contains(&k.as_str())) {
        return Err(format!("{name} has no option --{key}"));
    }
    let option = |key: &str| {
        (options.iter().rev())
            .find(|(k, _)| k == key)
            .map(|(_, v)| v.clone())
    };
    let mut command = Command {
        integration: option("integration"),
        ..Command::default()
    };
    match (name.as_str(), positional.as_slice()) {
        ("notify", [title, extra @ ..]) if extra.len() <= 1 => {
            let mut body = serde_json::Map::new();
            body.insert("title".into(), title.clone().into());
            if let Some(text) = extra.first() {
                body.insert("body".into(), text.clone().into());
            }
            if let Some(level) = option("level") {
                body.insert("level".into(), level.into());
            }
            if let Some(id) = option("id") {
                body.insert("id".into(), id.into());
            }
            if let Some(ttl) = option("ttl") {
                let ttl: u64 = ttl.parse().map_err(|_| "--ttl is a number of seconds")?;
                body.insert("ttl_s".into(), ttl.into());
            }
            command.method = "POST";
            command.path = "notes".into();
            command.body = serde_json::Value::Object(body).to_string();
        }
        ("card", [id, value]) => {
            let mut body = serde_json::Map::new();
            body.insert("label".into(), option("label").unwrap_or(id.clone()).into());
            body.insert("value".into(), value.clone().into());
            if let Some(state) = option("state") {
                body.insert("state".into(), state.into());
            }
            command.method = "PUT";
            command.path = format!("cards/{}", encode(id));
            command.body = serde_json::Value::Object(body).to_string();
        }
        ("dismiss", [id]) => {
            command.method = "DELETE";
            command.path = format!("notes/{}", encode(id));
        }
        _ => return Err(format!("wrong arguments for {name}")),
    }
    Ok(Some(command))
}

/// `text` percent-encoded for one path segment.
fn encode(text: &str) -> String {
    text.bytes()
        .map(|b| match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'.' | b'_' | b'~' => {
                (b as char).to_string()
            }
            _ => format!("%{b:02X}"),
        })
        .collect()
}

/// The token from `env`, or from the files for push integration `integration` (or the only one).
fn token(env: &Env, integration: Option<&str>) -> Result<String, String> {
    let token = match (&env.token, &env.dir) {
        (Some(token), _) => token.clone(),
        (None, Some(dir)) => token_from_files(dir, integration)?,
        (None, None) => return Err(format!("{TOKEN_VAR} is not set and HOME is unknown")),
    };
    // It goes into a header line, so nothing may end that line early.
    if !valid_value(&token) {
        return Err("the token must be printable ASCII on one line".into());
    }
    Ok(token)
}

/// The token of a push integration, read as the daemon reads it: `secrets.toml` must be a 0600
/// file of the config dir's owner. Errors name files and secrets, never a value.
fn token_from_files(dir: &Path, integration: Option<&str>) -> Result<String, String> {
    let path = dir.join(config::FILE);
    let text = std::fs::read_to_string(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    // toml's message can quote a line, which may hold a URL, so only the file is named.
    let config: Config = toml::from_str(&text).map_err(|_| {
        format!(
            "{} doesn't parse, so set {TOKEN_VAR} instead",
            path.display()
        )
    })?;
    let pushes: Vec<(&str, &str)> = (config.integrations.iter())
        .filter_map(|i| match i {
            IntegrationConfig::Push {
                id, token_secret, ..
            } => Some((id.as_str(), token_secret.as_str())),
            IntegrationConfig::Stat { .. } => None,
        })
        .collect();
    let (id, secret) = match (integration, pushes.as_slice()) {
        (Some(want), _) => *(pushes.iter().find(|(id, _)| *id == want)).ok_or(format!(
            "there is no push integration {want:?} in {}",
            path.display()
        ))?,
        (None, [one]) => *one,
        (None, []) => {
            return Err(format!(
                "there is no push integration in {}",
                path.display()
            ))
        }
        (None, several) => {
            let ids: Vec<&str> = several.iter().map(|(id, _)| *id).collect();
            return Err(format!(
                "there are several push integrations ({}), so pick one with --integration",
                ids.join(", ")
            ));
        }
    };
    let secrets = Secrets::new(dir).load().map_err(|e| e.to_string())?;
    let token = secrets.get(secret).ok_or(format!(
        "integration {id:?}'s token_secret {secret:?} is not in secrets.toml"
    ))?;
    Ok(token.expose().to_string())
}

enum Failure {
    /// The connection was refused or never made.
    NotListening(io::Error),
    Other(String),
}

struct Reply {
    status: u16,
    body: String,
}

/// Sends `command` and reads the whole reply. The daemon closes after one.
fn send(addr: SocketAddr, token: &str, command: &Command) -> Result<Reply, Failure> {
    let mut stream =
        TcpStream::connect_timeout(&addr, CONNECT_TIMEOUT).map_err(Failure::NotListening)?;
    let failed = |e: io::Error| Failure::Other(format!("talking to {addr}: {e}"));
    stream.set_read_timeout(Some(IO_TIMEOUT)).map_err(failed)?;
    stream.set_write_timeout(Some(IO_TIMEOUT)).map_err(failed)?;
    let mut request = format!(
        "{} /v1/{} HTTP/1.1\r\nHost: {addr}\r\nAuthorization: Bearer {token}\r\n\
         Content-Length: {}\r\nConnection: close\r\n",
        command.method,
        command.path,
        command.body.len()
    );
    if !command.body.is_empty() {
        request.push_str("Content-Type: application/json\r\n");
    }
    request.push_str("\r\n");
    request.push_str(&command.body);
    stream.write_all(request.as_bytes()).map_err(failed)?;
    let mut raw = Vec::new();
    (&mut stream)
        .take(MAX_REPLY_BYTES)
        .read_to_end(&mut raw)
        .map_err(failed)?;
    let text = String::from_utf8_lossy(&raw);
    let (head, body) = text.split_once("\r\n\r\n").unwrap_or((&text, ""));
    let status = (head.split(' ').nth(1))
        .and_then(|code| code.parse().ok())
        .ok_or_else(|| Failure::Other(format!("{addr} did not answer in HTTP")))?;
    Ok(Reply {
        status,
        body: body.trim().to_string(),
    })
}

/// The string `key` of the JSON object `body`.
fn field(body: &str, key: &str) -> Option<String> {
    let value: serde_json::Value = serde_json::from_str(body).ok()?;
    Some(value.get(key)?.as_str()?.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(text: &[&str]) -> Vec<String> {
        text.iter().map(|s| s.to_string()).collect()
    }

    fn parsed(text: &[&str]) -> Command {
        parse(&args(text)).unwrap().unwrap()
    }

    #[test]
    fn notify_builds_a_note_with_only_the_given_fields() {
        let c = parsed(&["notify", "Backup done"]);
        assert_eq!((c.method, c.path.as_str()), ("POST", "notes"));
        assert_eq!(c.body, r#"{"title":"Backup done"}"#);
        let c = parsed(&[
            "notify",
            "--level=ok",
            "--id",
            "backup",
            "--ttl",
            "3600",
            "--integration",
            "ha",
            "T",
            "B",
        ]);
        assert_eq!(c.integration.as_deref(), Some("ha"));
        let body: serde_json::Value = serde_json::from_str(&c.body).unwrap();
        assert_eq!(
            body,
            serde_json::json!({"title": "T", "body": "B", "level": "ok", "id": "backup", "ttl_s": 3600})
        );
        assert_eq!(
            parsed(&["notify", "--", "--not-an-option"]).body,
            r#"{"title":"--not-an-option"}"#
        );
    }

    #[test]
    fn card_and_dismiss_put_the_id_in_the_path_encoded() {
        let c = parsed(&["card", "--state", "bad", "wash er", "12 min"]);
        assert_eq!((c.method, c.path.as_str()), ("PUT", "cards/wash%20er"));
        let body: serde_json::Value = serde_json::from_str(&c.body).unwrap();
        assert_eq!(
            body,
            serde_json::json!({"label": "wash er", "value": "12 min", "state": "bad"})
        );
        let c = parsed(&["dismiss", "a/b"]);
        assert_eq!(
            (c.method, c.path.as_str(), c.body.as_str()),
            ("DELETE", "notes/a%2Fb", "")
        );
    }

    #[test]
    fn wrong_arguments_are_refused() {
        for bad in [
            &["notify"][..],
            &["notify", "a", "b", "c"],
            &["notify", "--state", "ok", "t"],
            &["notify", "--ttl", "soon", "t"],
            &["notify", "--level"],
            &["card", "id"],
            &["dismiss"],
            &["dismiss", "a", "b"],
        ] {
            assert!(parse(&args(bad)).is_err(), "{bad:?}");
        }
        assert_eq!(parse(&args(&["notify", "--help"])), Ok(None));
    }

    #[test]
    fn a_token_that_could_break_the_header_is_refused() {
        let env = Env {
            addr: (Ipv4Addr::LOCALHOST, 1).into(),
            token: Some("abc\r\nX-Injected: 1".into()),
            dir: None,
        };
        let (mut out, mut err) = (Vec::new(), Vec::new());
        assert_eq!(run(&args(&["notify", "t"]), &env, &mut out, &mut err), 1);
        let err = String::from_utf8(err).unwrap();
        assert!(
            err.contains("printable ASCII") && !err.contains("Injected"),
            "{err}"
        );
    }
}
