//! The daemon's TLS identity: a self-signed P-256 cert that devices pin, and the rustls config
//! that serves it. Devices compare the cert bytes and check neither names nor dates.

use std::fs::{self, File, OpenOptions, Permissions};
use std::io;
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::Path;
use std::sync::Arc;

use rcgen::{
    date_time_ymd, CertificateParams, DnType, ExtendedKeyUsagePurpose, KeyPair, KeyUsagePurpose,
    PKCS_ECDSA_P256_SHA256,
};
use sha2::{Digest, Sha256};
use time::{OffsetDateTime, Time};
use tokio_rustls::rustls::crypto::aws_lc_rs;
use tokio_rustls::rustls::pki_types::pem::PemObject;
use tokio_rustls::rustls::pki_types::{CertificateDer, PrivateKeyDer};
use tokio_rustls::rustls::{self, ServerConfig};

use super::store::{check_private, write_private};

/// The identity's file names, public so the fault harness can check for them before
/// `load_or_create`, which would otherwise create a new identity.
pub const CERT_FILE: &str = "identity.pem";
pub const KEY_FILE: &str = "identity.key";
const LOCK_FILE: &str = "identity.lock";
/// Hex digits in `fp`, the cert fingerprint advertised in mDNS TXT.
const FP_HEX_LEN: usize = 16;

pub struct Identity {
    /// The exact bytes of `identity.pem`, which devices pin and hash into `fp`.
    cert_pem: String,
    cert: CertificateDer<'static>,
    key: PrivateKeyDer<'static>,
}

impl Identity {
    /// Loads the identity stored in `dir`, or creates it there on first run. The dir is kept
    /// 0700 and both files are written 0600. An exclusive lock on `identity.lock` makes the
    /// check and the writes atomic against a second daemon starting at the same time. A key
    /// that is not 0600 or not owned by the dir's owner is refused, never repaired.
    pub fn load_or_create(dir: &Path) -> io::Result<Self> {
        fs::create_dir_all(dir)?;
        fs::set_permissions(dir, Permissions::from_mode(0o700))?;
        let lock = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .open(dir.join(LOCK_FILE))?;
        lock.lock()?;
        let cert_path = dir.join(CERT_FILE);
        let key_path = dir.join(KEY_FILE);
        match (cert_path.try_exists()?, key_path.try_exists()?) {
            (true, true) => {
                check_private(&key_path, fs::metadata(dir)?.uid())?;
                Self::parse(fs::read_to_string(&cert_path)?, &fs::read(&key_path)?)
            }
            (false, false) => {
                let (cert_pem, key_pem) = generate().map_err(io::Error::other)?;
                // The key goes first, so a crash between the writes leaves no cert to pin.
                write_private(&key_path, key_pem.as_bytes())?;
                write_private(&cert_path, cert_pem.as_bytes())?;
                File::open(dir)?.sync_all()?;
                Self::parse(cert_pem, key_pem.as_bytes())
            }
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "only one of {CERT_FILE} and {KEY_FILE} exists; delete it to generate a new \
                     identity, which every device must then pair with again"
                ),
            )),
        }
    }

    fn parse(cert_pem: String, key_pem: &[u8]) -> io::Result<Self> {
        let invalid = |file: &str, e: rustls::pki_types::pem::Error| {
            io::Error::new(io::ErrorKind::InvalidData, format!("{file}: {e}"))
        };
        let cert = CertificateDer::from_pem_slice(cert_pem.as_bytes())
            .map_err(|e| invalid(CERT_FILE, e))?;
        let key = PrivateKeyDer::from_pem_slice(key_pem).map_err(|e| invalid(KEY_FILE, e))?;
        Ok(Self {
            cert_pem,
            cert,
            key,
        })
    }

    /// The cert PEM, byte for byte what devices pin.
    pub fn cert_pem(&self) -> &str {
        &self.cert_pem
    }

    /// The first 16 hex digits of SHA-256 over the cert PEM. Devices match it in mDNS TXT `fp`.
    pub fn fp(&self) -> String {
        fingerprint(self.cert_pem.as_bytes())
    }

    /// A server config presenting this identity on the aws-lc-rs provider. Fails if the key
    /// does not match the cert.
    pub fn server_config(&self) -> Result<Arc<ServerConfig>, rustls::Error> {
        let config = ServerConfig::builder_with_provider(Arc::new(aws_lc_rs::default_provider()))
            .with_safe_default_protocol_versions()?
            .with_no_client_auth()
            .with_single_cert(vec![self.cert.clone()], self.key.clone_key())?;
        Ok(Arc::new(config))
    }
}

fn fingerprint(pem: &[u8]) -> String {
    Sha256::digest(pem)
        .iter()
        .take(FP_HEX_LEN / 2)
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// A new self-signed P-256 identity, as (cert PEM, PKCS#8 key PEM).
fn generate() -> Result<(String, String), rcgen::Error> {
    let key = KeyPair::generate_for(&PKCS_ECDSA_P256_SHA256)?;
    let mut params = CertificateParams::default();
    params
        .distinguished_name
        .push(DnType::CommonName, "watchdeck daemon");
    params.key_usages = vec![KeyUsagePurpose::DigitalSignature];
    params.extended_key_usages = vec![ExtendedKeyUsagePurpose::ServerAuth];
    params.not_before = OffsetDateTime::now_utc();
    // 99991231235959Z, RFC 5280's "no well-defined expiration date".
    params.not_after = date_time_ymd(9999, 12, 31)
        .replace_time(Time::from_hms(23, 59, 59).expect("23:59:59 is a valid time"));
    let cert = params.self_signed(&key)?;
    Ok((cert.pem(), key.serialize_pem()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mode(path: &Path) -> u32 {
        fs::metadata(path).unwrap().permissions().mode() & 0o777
    }

    #[test]
    fn the_identity_is_created_once_then_reused() {
        let tmp = tempfile::tempdir().unwrap();
        let dir = tmp.path().join("watchdeck");
        fs::create_dir(&dir).unwrap();
        fs::set_permissions(&dir, Permissions::from_mode(0o755)).unwrap();

        let first = Identity::load_or_create(&dir).unwrap();
        assert_eq!(mode(&dir), 0o700);
        assert_eq!(mode(&dir.join(CERT_FILE)), 0o600);
        assert_eq!(mode(&dir.join(KEY_FILE)), 0o600);
        let on_disk = fs::read(dir.join(CERT_FILE)).unwrap();
        assert_eq!(on_disk, first.cert_pem().as_bytes());
        assert_eq!(first.fp(), fingerprint(&on_disk));
        assert!(
            first.fp().len() == FP_HEX_LEN && first.fp().bytes().all(|b| b.is_ascii_hexdigit())
        );

        let second = Identity::load_or_create(&dir).unwrap();
        assert_eq!(second.fp(), first.fp());
        assert_eq!(second.cert_pem(), first.cert_pem());
        second.server_config().unwrap();
    }

    #[test]
    fn fp_is_the_first_16_hex_digits_of_sha256() {
        // printf abc | sha256sum. The firmware's PROTO_FP_LEN pins the 16.
        let digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        assert_eq!(fingerprint(b"abc"), digest[..16]);
    }

    #[test]
    fn half_an_identity_is_an_error_and_is_left_alone() {
        let tmp = tempfile::tempdir().unwrap();
        let cert_path = tmp.path().join(CERT_FILE);
        fs::write(&cert_path, "left alone").unwrap();

        let err = Identity::load_or_create(tmp.path())
            .err()
            .expect("half an identity must not load");
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
        assert_eq!(fs::read_to_string(&cert_path).unwrap(), "left alone");
        assert!(!tmp.path().join(KEY_FILE).exists());
    }

    #[test]
    fn a_loose_key_mode_is_refused_with_a_fix() {
        let tmp = tempfile::tempdir().unwrap();
        Identity::load_or_create(tmp.path()).unwrap();
        let key_path = tmp.path().join(KEY_FILE);
        fs::set_permissions(&key_path, Permissions::from_mode(0o640)).unwrap();

        let err = Identity::load_or_create(tmp.path())
            .err()
            .expect("a group-readable key must not load");
        assert_eq!(err.kind(), io::ErrorKind::PermissionDenied);
        let msg = err.to_string();
        assert!(msg.contains("0640") && msg.contains("chmod 600"), "{msg}");
        assert_eq!(mode(&key_path), 0o640, "the mode must be left alone");
    }

    #[test]
    fn a_key_owned_by_someone_else_is_refused() {
        let tmp = tempfile::tempdir().unwrap();
        Identity::load_or_create(tmp.path()).unwrap();
        let key_path = tmp.path().join(KEY_FILE);
        let uid = fs::metadata(&key_path).unwrap().uid();
        check_private(&key_path, uid).unwrap();
        let err = check_private(&key_path, uid + 1).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::PermissionDenied);
    }

    #[test]
    fn a_symlinked_key_is_refused() {
        let tmp = tempfile::tempdir().unwrap();
        Identity::load_or_create(tmp.path()).unwrap();
        let key_path = tmp.path().join(KEY_FILE);
        let real = tmp.path().join("real.key");
        fs::rename(&key_path, &real).unwrap();
        std::os::unix::fs::symlink(&real, &key_path).unwrap();

        let err = Identity::load_or_create(tmp.path())
            .err()
            .expect("a symlinked key must not load");
        assert_eq!(err.kind(), io::ErrorKind::PermissionDenied);
        assert!(err.to_string().contains("not a regular file"), "{err}");
    }

    #[test]
    fn a_stale_temp_file_or_planted_symlink_does_not_block_or_redirect_creation() {
        let tmp = tempfile::tempdir().unwrap();
        let victim = tmp.path().join("victim");
        fs::write(&victim, "untouched").unwrap();
        std::os::unix::fs::symlink(&victim, tmp.path().join("identity.key.tmp")).unwrap();

        Identity::load_or_create(tmp.path()).unwrap();
        assert_eq!(fs::read_to_string(&victim).unwrap(), "untouched");
        assert_eq!(mode(&tmp.path().join(KEY_FILE)), 0o600);
    }

    #[test]
    fn concurrent_starts_agree_on_one_identity() {
        let tmp = tempfile::tempdir().unwrap();
        let fps: Vec<String> = std::thread::scope(|s| {
            let starts: Vec<_> = (0..8)
                .map(|_| s.spawn(|| Identity::load_or_create(tmp.path()).unwrap().fp()))
                .collect();
            starts.into_iter().map(|h| h.join().unwrap()).collect()
        });
        assert!(fps.windows(2).all(|w| w[0] == w[1]), "{fps:?}");
    }
}
