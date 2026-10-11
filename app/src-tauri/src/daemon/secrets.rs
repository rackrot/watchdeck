//! `secrets.toml`: values that HTTP actions send as headers, looked up by name. The file is a flat
//! TOML table of strings, a 0600 file in the 0700 config dir, read afresh on every use so a hand
//! edit applies at once. Values leave this module only as request headers. The window and the
//! config check see names, and errors name a line or a secret, never a value.

use std::collections::BTreeMap;
use std::fmt;
use std::fs;
use std::io;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use super::store::{check_private, write_private};

pub const FILE: &str = "secrets.toml";
const MAX_NAME_BYTES: usize = 64;
const MAX_VALUE_BYTES: usize = 4096;

/// A secret's value, which always passes [`valid_value`]. Debug never prints it.
#[derive(Clone, PartialEq, Eq)]
pub struct Secret(String);

impl Secret {
    pub fn expose(&self) -> &str {
        &self.0
    }
}

impl fmt::Debug for Secret {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Secret(<redacted>)")
    }
}

pub struct Secrets {
    dir: PathBuf,
    /// Serializes `set`, which reads the file, changes it and writes it back.
    writing: Mutex<()>,
}

impl Secrets {
    pub fn new(dir: &Path) -> Self {
        Self {
            dir: dir.to_path_buf(),
            writing: Mutex::new(()),
        }
    }

    pub fn path(&self) -> PathBuf {
        self.dir.join(FILE)
    }

    /// Every secret, by name. With no file there are none. A file that is not a 0600 regular
    /// file owned by the dir's owner, that does not parse, or that holds a value failing
    /// [`valid_value`], is refused.
    pub fn load(&self) -> io::Result<BTreeMap<String, Secret>> {
        let path = self.path();
        if !path.try_exists()? {
            return Ok(BTreeMap::new());
        }
        check_private(&path, fs::metadata(&self.dir)?.uid())?;
        let text = fs::read_to_string(&path)?;
        let table: BTreeMap<String, String> = toml::from_str(&text).map_err(|e| {
            // toml's own message quotes the offending line, which may hold a value.
            let line = e.span().map_or(1, |span| {
                1 + text.as_bytes()[..span.start]
                    .iter()
                    .filter(|&&b| b == b'\n')
                    .count()
            });
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "{}: invalid at line {line}; each line must be name = \"value\"",
                    path.display()
                ),
            )
        })?;
        if let Some((name, _)) = table.iter().find(|(_, value)| !valid_value(value)) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "{}: secret {name:?} must be 1 to {MAX_VALUE_BYTES} bytes of printable ASCII \
                     on one line",
                    path.display()
                ),
            ));
        }
        Ok(table.into_iter().map(|(k, v)| (k, Secret(v))).collect())
    }

    pub fn names(&self) -> io::Result<Vec<String>> {
        Ok(self.load()?.into_keys().collect())
    }

    /// Adds or replaces `name`, whose value must pass [`valid_value`]. Errors never repeat the
    /// value.
    pub fn set(&self, name: &str, value: &str) -> Result<(), String> {
        if name.is_empty()
            || name.len() > MAX_NAME_BYTES
            || !name
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
        {
            return Err(format!(
                "A secret name is 1 to {MAX_NAME_BYTES} letters, digits, '_' or '-'."
            ));
        }
        if !valid_value(value) {
            return Err(format!(
                "The value must be 1 to {MAX_VALUE_BYTES} bytes of printable ASCII on one line, \
                 because it is sent as an HTTP header."
            ));
        }
        let _writing = self.writing.lock().expect("secrets lock poisoned");
        let mut all = self.load().map_err(|e| e.to_string())?;
        all.insert(name.to_string(), Secret(value.to_string()));
        let table: BTreeMap<&str, &str> =
            all.iter().map(|(k, v)| (k.as_str(), v.expose())).collect();
        let text = toml::to_string(&table).expect("a table of strings serializes");
        write_private(&self.path(), text.as_bytes())
            .map_err(|e| format!("Could not save {FILE}: {e}"))
    }
}

/// Whether `value` can be a secret: a header value of printable ASCII, which also keeps the
/// blanking of echoed secrets byte-exact.
pub fn valid_value(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_VALUE_BYTES
        && value.bytes().all(|b| (b' '..=b'~').contains(&b))
}

#[cfg(test)]
mod tests {
    use std::fs::Permissions;
    use std::os::unix::fs::PermissionsExt;

    use super::*;

    const VALUE: &str = "Bearer s3cr3t-value";

    #[test]
    fn set_secrets_load_back_from_a_0600_file_and_debug_redacts() {
        let tmp = tempfile::tempdir().unwrap();
        let secrets = Secrets::new(tmp.path());
        assert!(secrets.load().unwrap().is_empty());
        secrets.set("ha", VALUE).unwrap();
        secrets.set("other", "x").unwrap();
        secrets.set("ha", "replaced").unwrap();

        let mode = fs::metadata(secrets.path()).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600);
        assert_eq!(secrets.names().unwrap(), ["ha", "other"]);
        let all = secrets.load().unwrap();
        assert_eq!(all["ha"].expose(), "replaced");
        assert_eq!(format!("{:?}", all["ha"]), "Secret(<redacted>)");
    }

    #[test]
    fn bad_names_and_values_are_refused_without_echoing_the_value() {
        let tmp = tempfile::tempdir().unwrap();
        let secrets = Secrets::new(tmp.path());
        for name in ["", "has space", "dot.ted", &"n".repeat(65)] {
            assert!(secrets.set(name, "v").is_err(), "{name:?}");
        }
        for value in [
            String::new(),
            format!("{VALUE}\r\nX-Injected: 1"),
            format!("{VALUE}\u{7f}"),
            format!("{VALUE}\u{e9}"),
            format!("{VALUE}\t"),
            "v".repeat(MAX_VALUE_BYTES + 1),
        ] {
            let err = secrets.set("ok", &value).unwrap_err();
            assert!(!err.contains(VALUE), "{err}");
        }
        assert!(!secrets.path().exists(), "nothing was written");
    }

    #[test]
    fn a_malformed_file_is_refused_without_quoting_it() {
        let tmp = tempfile::tempdir().unwrap();
        let secrets = Secrets::new(tmp.path());
        for bad in [
            format!("ok = \"x\"\nha = \"{VALUE}"),
            format!("ha = \"{VALUE}\"\nha = \"{VALUE}\""),
            "pin = 123456\n".to_string(),
        ] {
            fs::write(secrets.path(), &bad).unwrap();
            fs::set_permissions(secrets.path(), Permissions::from_mode(0o600)).unwrap();
            let err = secrets.load().unwrap_err();
            assert_eq!(err.kind(), io::ErrorKind::InvalidData);
            let msg = err.to_string();
            assert!(msg.contains("line "), "{msg}");
            assert!(!msg.contains(VALUE) && !msg.contains("123456"), "{msg}");
        }
    }

    #[test]
    fn a_hand_edited_value_that_cant_be_a_header_is_refused_by_name() {
        let tmp = tempfile::tempdir().unwrap();
        let secrets = Secrets::new(tmp.path());
        for bad in [
            String::new(),
            format!("{VALUE}\t"),
            format!("{VALUE}\u{e9}"),
        ] {
            let table = BTreeMap::from([("ha", bad.as_str()), ("ok", "fine")]);
            fs::write(secrets.path(), toml::to_string(&table).unwrap()).unwrap();
            fs::set_permissions(secrets.path(), Permissions::from_mode(0o600)).unwrap();
            let err = secrets.load().unwrap_err();
            assert_eq!(err.kind(), io::ErrorKind::InvalidData);
            let msg = err.to_string();
            assert!(msg.contains("secret \"ha\""), "{msg}");
            assert!(!msg.contains(VALUE), "{msg}");
            assert!(secrets.names().is_err(), "the config check sees it too");
        }
    }

    #[test]
    fn a_loose_file_is_refused() {
        let tmp = tempfile::tempdir().unwrap();
        let secrets = Secrets::new(tmp.path());
        secrets.set("ha", VALUE).unwrap();
        fs::set_permissions(secrets.path(), Permissions::from_mode(0o644)).unwrap();
        let err = secrets.load().unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::PermissionDenied);
        assert!(
            secrets.set("ha", "new").is_err(),
            "a loose file is not rewritten"
        );
    }
}
