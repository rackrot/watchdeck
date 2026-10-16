//! The daemon's private files. `state.json` maps each paired device id to SHA-256 of its token.
//! The daemon never sees a token at pairing and keeps only the hash, so a leaked `state.json`
//! opens no session. Every private file is a 0600 regular file owned by the config dir's owner.

use std::collections::BTreeMap;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};

use aws_lc_rs::constant_time::verify_slices_are_equal;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use super::protocol::{decode_hex, encode_hex, valid_device_id, TOKEN_BYTES};

/// Public so the fault harness can check for the file before `load`, which treats a missing one as empty.
pub const STATE_FILE: &str = "state.json";
const STATE_VERSION: u32 = 1;

/// Paired devices, as loaded from `state.json` and saved back on every change. Only the process
/// that holds port 47820 writes it, so the port doubles as the cross-process lock.
pub struct Store {
    dir: PathBuf,
    devices: BTreeMap<String, [u8; TOKEN_BYTES]>,
}

#[derive(Serialize, Deserialize)]
struct StateFile {
    version: u32,
    devices: BTreeMap<String, Entry>,
}

#[derive(Serialize, Deserialize)]
struct Entry {
    token_sha256: String,
}

impl Store {
    /// Loads `state.json` from `dir`, or starts empty when there is none. A file that is not a
    /// 0600 regular file owned by the dir's owner, or that does not parse, is refused.
    pub fn load(dir: &Path) -> io::Result<Self> {
        let path = dir.join(STATE_FILE);
        let mut store = Self {
            dir: dir.to_path_buf(),
            devices: BTreeMap::new(),
        };
        if !path.try_exists()? {
            return Ok(store);
        }
        check_private(&path, fs::metadata(dir)?.uid())?;
        let invalid = |problem: String| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "{}: {problem}; delete it to forget every device",
                    path.display()
                ),
            )
        };
        let file: StateFile =
            serde_json::from_slice(&fs::read(&path)?).map_err(|e| invalid(e.to_string()))?;
        if file.version != STATE_VERSION {
            return Err(invalid(format!("unsupported version {}", file.version)));
        }
        for (device, entry) in file.devices {
            let hash = decode_hex(&entry.token_sha256)
                .filter(|_| valid_device_id(&device))
                .ok_or_else(|| invalid(format!("invalid entry for {device:?}")))?;
            store.devices.insert(device, hash);
        }
        Ok(store)
    }

    pub fn ids(&self) -> impl Iterator<Item = &str> {
        self.devices.keys().map(String::as_str)
    }

    /// Whether `token` is the one `device` paired with. The comparison is constant-time, and an
    /// unknown device goes through the same hash and compare as a wrong token.
    pub fn verify(&self, device: &str, token: &[u8; TOKEN_BYTES]) -> bool {
        let stored = self.devices.get(device);
        let presented = Sha256::digest(token);
        let expected = stored.copied().unwrap_or([0; TOKEN_BYTES]);
        verify_slices_are_equal(&presented, &expected).is_ok() && stored.is_some()
    }

    /// Pairs `device`, replacing any earlier pairing, and saves. A save that fails before the
    /// rename changes nothing.
    pub fn insert(&mut self, device: &str, token_sha256: [u8; TOKEN_BYTES]) -> io::Result<()> {
        let mut next = self.devices.clone();
        next.insert(device.to_string(), token_sha256);
        self.save(next)
    }

    /// Forgets `device` and saves. Returns whether it was paired. A save that fails before the
    /// rename changes nothing.
    pub fn remove(&mut self, device: &str) -> io::Result<bool> {
        let mut next = self.devices.clone();
        if next.remove(device).is_none() {
            return Ok(false);
        }
        self.save(next)?;
        Ok(true)
    }

    fn save(&mut self, devices: BTreeMap<String, [u8; TOKEN_BYTES]>) -> io::Result<()> {
        let file = StateFile {
            version: STATE_VERSION,
            devices: devices
                .iter()
                .map(|(device, hash)| {
                    let token_sha256 = encode_hex(hash);
                    (device.clone(), Entry { token_sha256 })
                })
                .collect(),
        };
        let mut json = serde_json::to_vec_pretty(&file).expect("the state serializes");
        json.push(b'\n');
        write_private(&self.dir.join(STATE_FILE), &json)?;
        // The rename has replaced the file, so memory follows it whatever the fsync below does.
        self.devices = devices;
        // Without the dir fsync a power cut could undo the rename, but nothing reads the old file
        // before then, so a failure is worth a warning, not an error.
        if let Err(e) = File::open(&self.dir).and_then(|dir| dir.sync_all()) {
            log::warn!(
                "{}: fsync after saving {STATE_FILE} failed: {e}",
                self.dir.display()
            );
        }
        Ok(())
    }
}

/// Refuses a file that is not a regular 0600 file owned by `uid`, the config dir's owner.
pub(super) fn check_private(path: &Path, uid: u32) -> io::Result<()> {
    let meta = fs::symlink_metadata(path)?;
    let mode = meta.mode() & 0o777;
    let refuse = |problem: String| {
        let display = path.display();
        Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            format!(
                "{display} {problem}; fix it with `chown $USER {display} && chmod 600 {display}`"
            ),
        ))
    };
    if !meta.is_file() {
        return refuse("is not a regular file".into());
    }
    if mode != 0o600 {
        return refuse(format!("has mode {mode:04o}, expected 0600"));
    }
    if meta.uid() != uid {
        return refuse(format!("is owned by uid {}, expected {uid}", meta.uid()));
    }
    Ok(())
}

/// Writes `bytes` to `path` as 0600 through a temporary file, so a crash never leaves a
/// partial file behind. The temp file is opened `create_new`, which (O_EXCL) fails on a planted
/// symlink instead of following it, so it needs no separate O_NOFOLLOW. A temp file left by a
/// crash is removed first, which is safe because the caller is the only writer: it holds the
/// identity lock, or port 47820 for `state.json`, `config.toml` and `secrets.toml`.
pub(super) fn write_private(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let tmp = path.with_added_extension("tmp");
    match fs::remove_file(&tmp) {
        Err(e) if e.kind() != io::ErrorKind::NotFound => return Err(e),
        _ => {}
    }
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&tmp)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    fs::rename(&tmp, path)
}

#[cfg(test)]
mod tests {
    use std::fs::Permissions;
    use std::os::unix::fs::PermissionsExt;

    use super::*;

    const A: &str = "wd-00000000000a";
    const B: &str = "wd-00000000000b";

    fn hash(token: &[u8; TOKEN_BYTES]) -> [u8; TOKEN_BYTES] {
        Sha256::digest(token).into()
    }

    #[test]
    fn a_missing_file_is_an_empty_store_and_nothing_is_written() {
        let tmp = tempfile::tempdir().unwrap();
        let store = Store::load(tmp.path()).unwrap();
        assert_eq!(store.ids().count(), 0);
        assert!(!store.verify(A, &[0; 32]));
        assert!(!tmp.path().join(STATE_FILE).exists());
    }

    #[test]
    fn pairings_survive_a_reload_as_hashes_in_a_0600_file() {
        let tmp = tempfile::tempdir().unwrap();
        let (token_a, token_b) = ([1u8; 32], [2u8; 32]);
        let mut store = Store::load(tmp.path()).unwrap();
        store.insert(A, hash(&token_a)).unwrap();
        store.insert(B, hash(&token_b)).unwrap();

        let path = tmp.path().join(STATE_FILE);
        let mode = fs::metadata(&path).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600);
        let text = fs::read_to_string(&path).unwrap();
        assert!(text.contains(&encode_hex(&hash(&token_a))), "{text}");
        assert!(
            !text.contains(&encode_hex(&token_a)),
            "a token reached the disk"
        );

        let store = Store::load(tmp.path()).unwrap();
        assert_eq!(store.ids().collect::<Vec<_>>(), [A, B]);
        assert!(store.verify(A, &token_a));
        assert!(store.verify(B, &token_b));
    }

    #[test]
    fn a_token_opens_only_its_own_device() {
        let tmp = tempfile::tempdir().unwrap();
        let mut store = Store::load(tmp.path()).unwrap();
        store.insert(A, hash(&[1; 32])).unwrap();
        assert!(!store.verify(A, &[2; 32]), "wrong token");
        assert!(!store.verify(B, &[1; 32]), "A's token for B");
        assert!(!store.verify("wd-unknown", &[1; 32]), "unknown device");
    }

    #[test]
    fn re_pairing_replaces_the_token_and_remove_forgets() {
        let tmp = tempfile::tempdir().unwrap();
        let mut store = Store::load(tmp.path()).unwrap();
        store.insert(A, hash(&[1; 32])).unwrap();
        store.insert(A, hash(&[2; 32])).unwrap();
        assert!(!store.verify(A, &[1; 32]));
        assert!(store.verify(A, &[2; 32]));

        assert!(store.remove(A).unwrap());
        assert!(!store.remove(A).unwrap());
        assert!(!store.verify(A, &[2; 32]));
        assert_eq!(Store::load(tmp.path()).unwrap().ids().count(), 0);
    }

    #[test]
    fn a_failed_save_changes_nothing() {
        let tmp = tempfile::tempdir().unwrap();
        let mut store = Store::load(tmp.path()).unwrap();
        store.insert(A, hash(&[1; 32])).unwrap();
        // A directory where the temp file goes makes the write fail.
        fs::create_dir(tmp.path().join("state.json.tmp")).unwrap();

        assert!(store.insert(B, hash(&[2; 32])).is_err());
        assert!(store.remove(A).is_err());
        assert!(store.verify(A, &[1; 32]));
        assert!(!store.verify(B, &[2; 32]));
    }

    #[test]
    fn a_failed_dir_fsync_after_the_rename_still_counts_as_saved() {
        let tmp = tempfile::tempdir().unwrap();
        let mut store = Store::load(tmp.path()).unwrap();
        // Without read permission the dir can't be opened for its fsync, while the temp file
        // and the rename still work.
        fs::set_permissions(tmp.path(), Permissions::from_mode(0o300)).unwrap();
        let inserted = store.insert(A, hash(&[1; 32]));
        fs::set_permissions(tmp.path(), Permissions::from_mode(0o700)).unwrap();

        inserted.unwrap();
        assert!(store.verify(A, &[1; 32]));
        assert!(Store::load(tmp.path()).unwrap().verify(A, &[1; 32]));
    }

    #[test]
    fn a_loose_or_corrupt_file_is_refused() {
        let tmp = tempfile::tempdir().unwrap();
        let mut store = Store::load(tmp.path()).unwrap();
        store.insert(A, hash(&[1; 32])).unwrap();
        let path = tmp.path().join(STATE_FILE);

        fs::set_permissions(&path, Permissions::from_mode(0o644)).unwrap();
        let err = Store::load(tmp.path())
            .err()
            .expect("a 0644 file must not load");
        assert_eq!(err.kind(), io::ErrorKind::PermissionDenied);

        fs::set_permissions(&path, Permissions::from_mode(0o600)).unwrap();
        for bad in [
            "not json",
            r#"{"version":2,"devices":{}}"#,
            r#"{"version":1,"devices":{"wd-1":{"token_sha256":"abc"}}}"#,
        ] {
            fs::write(&path, bad).unwrap();
            let err = Store::load(tmp.path()).err().expect(bad);
            assert_eq!(err.kind(), io::ErrorKind::InvalidData, "{bad}");
        }
    }
}
