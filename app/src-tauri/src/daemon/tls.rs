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

    /// What a client pins with `curl --pinnedpubkey sha256//<this>`: SHA-256 over the cert's
    /// SubjectPublicKeyInfo, in base64. It is not [`Identity::fp`], which hashes the PEM text.
    pub fn spki_pin(&self) -> io::Result<String> {
        let spki = spki_of(self.cert.as_ref()).ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!("{CERT_FILE}: no public key"),
            )
        })?;
        Ok(base64(&Sha256::digest(spki)))
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

/// The first DER element of `der` and what follows it. Tags are one byte, as in every certificate.
fn der_element(der: &[u8]) -> Option<(&[u8], &[u8])> {
    let first = *der.get(1)?;
    let (header, len) = if first < 0x80 {
        (2, usize::from(first))
    } else {
        let n = usize::from(first & 0x7f);
        if n == 0 || n > 4 {
            return None;
        }
        let len = der
            .get(2..2 + n)?
            .iter()
            .fold(0, |len, b| len << 8 | usize::from(*b));
        (2 + n, len)
    };
    let end = header.checked_add(len)?;
    Some((der.get(..end)?, der.get(end..)?))
}

/// The inside of the DER element `element` (its header cut off).
fn der_content(element: &[u8]) -> Option<&[u8]> {
    let header = if element.get(1)? & 0x80 == 0 {
        2
    } else {
        2 + usize::from(element[1] & 0x7f)
    };
    element.get(header..)
}

/// The SubjectPublicKeyInfo element of the DER certificate `cert`, header included:
/// Certificate, then tbsCertificate, then [0] version (optional), serial, signature, issuer,
/// validity and subject, and the next element is the key.
fn spki_of(cert: &[u8]) -> Option<&[u8]> {
    let (cert, _) = der_element(cert)?;
    let (tbs, _) = der_element(der_content(cert)?)?;
    let mut rest = der_content(tbs)?;
    if *rest.first()? == 0xa0 {
        rest = der_element(rest)?.1;
    }
    for _ in 0..5 {
        rest = der_element(rest)?.1;
    }
    Some(der_element(rest)?.0)
}

/// `bytes` in standard base64 with padding.
fn base64(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let n = chunk
            .iter()
            .enumerate()
            .fold(0u32, |n, (i, b)| n | u32::from(*b) << (16 - 8 * i));
        for i in 0..4 {
            if i <= chunk.len() {
                out.push(char::from(ALPHABET[(n >> (18 - 6 * i) & 63) as usize]));
            } else {
                out.push('=');
            }
        }
    }
    out
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
    fn base64_pads_to_four_characters() {
        // Cases from RFC 4648 section 10, and bytes that reach the last two characters of the alphabet.
        for (plain, want) in [
            (&b""[..], ""),
            (b"foobar", "Zm9vYmFy"),
            (&[0xff], "/w=="),
            (&[0xff, 0xfe], "//4="),
            (&[0xff, 0xfe, 0xfd], "//79"),
            (&[0, 1, 2, 3], "AAECAw=="),
        ] {
            assert_eq!(base64(plain), want, "{plain:?}");
        }
    }

    #[test]
    fn the_pin_is_what_openssl_prints_for_the_certs_public_key() {
        // A P-256 cert made with openssl, and the value from the command in integrations.md:
        // openssl x509 -pubkey -noout | openssl pkey -pubin -outform der | openssl dgst -sha256 -binary | base64
        const CERT: &str = "308201863082012da00302010202140b2c68d2e5623e6817bc9fb64cff59b95b1d5a2d300a06082a8648ce3d04030230193117301506035504030c0e77617463686465636b2074657374301e170d3236313032343038353930335a170d3336313032313038353930335a30193117301506035504030c0e77617463686465636b20746573743059301306072a8648ce3d020106082a8648ce3d03010703420004e00175d1c5866bf9e86452de88b22b87a8fd440dfbd05285802fe94ee93e67200ae4adff6814beaf1d932a6510105bf9cdc5f1fb09f569a15f604e2e9b8f6a9aa3533051301d0603551d0e04160414a75c7f2b5d0e64a5ec9c6536ffb0c69b8ca761a5301f0603551d23041830168014a75c7f2b5d0e64a5ec9c6536ffb0c69b8ca761a5300f0603551d130101ff040530030101ff300a06082a8648ce3d040302034700304402204a575d64bacd1089a68ebc16bf55426ed7ec12adea49fab0dc088194a5539421022056d63263791d3996414ba68307c296f0e03b73344b58ddbc37a09a5805e3ca12";
        let cert: [u8; 394] = crate::daemon::protocol::decode_hex(CERT).unwrap();
        let spki = spki_of(&cert).unwrap();
        // 26 bytes of algorithm header and the 65-byte uncompressed point.
        assert_eq!(spki.len(), 91);
        assert_eq!(
            base64(&Sha256::digest(spki)),
            "AEbieqLeox6HTs7OZ6IBiNqljUizeJNR/17UJHurBI8="
        );
        // Cut anywhere, a certificate gives no key and never panics.
        for cut in 0..cert.len() {
            assert!(spki_of(&cert[..cut]).is_none(), "cut at {cut}");
        }
    }

    #[test]
    fn the_pin_of_a_generated_identity_is_the_hash_of_its_key() {
        use rcgen::PublicKeyData;
        let tmp = tempfile::tempdir().unwrap();
        let identity = Identity::load_or_create(tmp.path()).unwrap();
        let key =
            KeyPair::from_pem(&fs::read_to_string(tmp.path().join(KEY_FILE)).unwrap()).unwrap();
        let want = base64(&Sha256::digest(key.subject_public_key_info()));
        let pin = identity.spki_pin().unwrap();
        assert_eq!(pin, want);
        // 32 bytes of hash: 43 characters and one padding.
        assert!(pin.len() == 44 && pin.ends_with('='), "{pin}");
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
