/// TLS handling for the Android Auto phone connection.
///
/// Android Auto uses TLS 1.2 where the phone acts as the TLS CLIENT and
/// we act as the TLS SERVER.  The car side (T-Dongle tunnel) is plain TCP —
/// the T-Dongle forwards raw AAP bytes without any additional framing.
///
/// On first run a self-signed ECDSA P-256 certificate + private key are
/// generated via `rcgen` and persisted to disk so the phone can be paired
/// once and reuse the same identity.  If the files already exist they are
/// loaded instead of regenerating.
///
/// Note: Android Auto negotiates TLS cipher suites dynamically and accepts
/// ECDSA P-256 certificates (TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256).
use std::path::{Path, PathBuf};
use std::sync::Arc;

use anyhow::{Context, Result};
use log::{info, warn};
use rcgen::{CertificateParams, DistinguishedName, DnType, KeyPair};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use rustls::ServerConfig;
use time::OffsetDateTime;
use tokio_rustls::TlsAcceptor;

/// Default paths for the certificate and private key.
const DEFAULT_CERT_PATH: &str = "/etc/aa-proxy/aa-proxy.crt";
const DEFAULT_KEY_PATH: &str = "/etc/aa-proxy/aa-proxy.key";

/// Fallback to a local directory when /etc is not writable (e.g., dev).
const FALLBACK_CERT_PATH: &str = "/tmp/aa-proxy.crt";
const FALLBACK_KEY_PATH: &str = "/tmp/aa-proxy.key";

/// Locate writable certificate paths.  Tries the preferred /etc/ location
/// first; falls back to /tmp/ if that directory does not exist or is not
/// writable.
fn cert_paths() -> (PathBuf, PathBuf) {
    let preferred_dir = Path::new("/etc/aa-proxy");
    if preferred_dir.exists() {
        (
            PathBuf::from(DEFAULT_CERT_PATH),
            PathBuf::from(DEFAULT_KEY_PATH),
        )
    } else {
        (
            PathBuf::from(FALLBACK_CERT_PATH),
            PathBuf::from(FALLBACK_KEY_PATH),
        )
    }
}

/// Generate a self-signed ECDSA P-256 certificate suitable for Android Auto
/// TLS.
///
/// The certificate is valid for 10 years; Android Auto does not verify the
/// certificate against a known CA — it just needs a valid TLS handshake.
fn generate_self_signed() -> Result<(Vec<u8>, Vec<u8>)> {
    info!("Generating new self-signed ECDSA P-256 certificate for AA TLS identity");

    // KeyPair::generate() defaults to ECDSA P-256 with SHA-256 (ring backend).
    let key_pair = KeyPair::generate().context("Failed to generate ECDSA key pair")?;

    let mut params = CertificateParams::default();

    params.distinguished_name = {
        let mut dn = DistinguishedName::new();
        dn.push(DnType::CommonName, "aa-proxy");
        dn.push(DnType::OrganizationName, "aa-proxy");
        dn
    };

    // Valid from 2024-01-01 to 2034-01-01 (10 years).
    params.not_before = OffsetDateTime::from_unix_timestamp(1_704_067_200) // 2024-01-01 00:00:00 UTC
        .context("Invalid not_before timestamp")?;
    params.not_after = OffsetDateTime::from_unix_timestamp(2_019_686_400) // 2034-01-01 00:00:00 UTC
        .context("Invalid not_after timestamp")?;

    let cert = params
        .self_signed(&key_pair)
        .context("Failed to self-sign certificate")?;

    let cert_pem = cert.pem().into_bytes();
    let key_pem = key_pair.serialize_pem().into_bytes();

    Ok((cert_pem, key_pem))
}

/// Load PEM certificate and key from disk.  Returns `(cert_pem, key_pem)`.
fn load_from_disk(cert_path: &Path, key_path: &Path) -> Result<(Vec<u8>, Vec<u8>)> {
    let cert_pem = std::fs::read(cert_path)
        .with_context(|| format!("Failed to read cert from {}", cert_path.display()))?;
    let key_pem = std::fs::read(key_path)
        .with_context(|| format!("Failed to read key from {}", key_path.display()))?;
    Ok((cert_pem, key_pem))
}

/// Write PEM data to disk, creating parent directories as needed.
fn write_to_disk(
    cert_path: &Path,
    key_path: &Path,
    cert_pem: &[u8],
    key_pem: &[u8],
) -> Result<()> {
    if let Some(parent) = cert_path.parent() {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("Failed to create directory {}", parent.display()))?;
    }
    std::fs::write(cert_path, cert_pem)
        .with_context(|| format!("Failed to write cert to {}", cert_path.display()))?;
    std::fs::write(key_path, key_pem)
        .with_context(|| format!("Failed to write key to {}", key_path.display()))?;
    // Restrict key file permissions to owner read-only on Unix.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let perms = std::fs::Permissions::from_mode(0o600);
        std::fs::set_permissions(key_path, perms)
            .with_context(|| format!("Failed to chmod key file {}", key_path.display()))?;
    }
    Ok(())
}

/// Parse PEM cert + key bytes into DER structures suitable for `rustls`.
fn parse_pem(
    cert_pem: &[u8],
    key_pem: &[u8],
) -> Result<(Vec<CertificateDer<'static>>, PrivateKeyDer<'static>)> {
    let certs: Vec<CertificateDer<'static>> = {
        let mut cursor = std::io::Cursor::new(cert_pem);
        rustls_pemfile::certs(&mut cursor)
            .collect::<std::result::Result<Vec<_>, _>>()
            .context("Failed to parse certificate PEM")?
    };

    if certs.is_empty() {
        anyhow::bail!("No certificates found in PEM data");
    }

    // rcgen serialises private keys as PKCS#8 PEM blocks.
    let key: PrivateKeyDer<'static> = {
        let mut cursor = std::io::Cursor::new(key_pem);
        let mut keys: Vec<PrivatePkcs8KeyDer<'static>> =
            rustls_pemfile::pkcs8_private_keys(&mut cursor)
                .collect::<std::result::Result<Vec<_>, _>>()
                .context("Failed to parse PKCS#8 private key PEM")?;

        if keys.is_empty() {
            anyhow::bail!(
                "No PKCS#8 private key found in PEM data. \
                 If the key is in a different format (SEC1/EC, PKCS#1/RSA) \
                 regenerate the certificate by deleting the key file."
            );
        }
        PrivateKeyDer::Pkcs8(keys.remove(0))
    };

    Ok((certs, key))
}

/// Build a `TlsAcceptor` for the phone-facing side of the proxy.
///
/// Loads an existing certificate + key from disk if present; generates and
/// persists a new self-signed pair otherwise.
///
/// # TLS configuration notes
/// - Server-side TLS (we are the server, phone is client)
/// - Client authentication is NOT required — AA handshake is handled at the
///   AAP protocol layer, not the TLS layer
/// - Cipher suite: TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 (negotiated by AA)
pub fn build_tls_acceptor() -> Result<TlsAcceptor> {
    let (cert_path, key_path) = cert_paths();

    let (cert_pem, key_pem) = if cert_path.exists() && key_path.exists() {
        info!(
            "Loading existing TLS identity from {} and {}",
            cert_path.display(),
            key_path.display()
        );
        match load_from_disk(&cert_path, &key_path) {
            Ok(pair) => pair,
            Err(e) => {
                warn!(
                    "Failed to load existing TLS identity ({}); regenerating",
                    e
                );
                let pair = generate_self_signed()?;
                if let Err(e) = write_to_disk(&cert_path, &key_path, &pair.0, &pair.1) {
                    warn!("Could not persist TLS identity to disk: {}", e);
                }
                pair
            }
        }
    } else {
        let pair = generate_self_signed()?;
        if let Err(e) = write_to_disk(&cert_path, &key_path, &pair.0, &pair.1) {
            warn!("Could not persist TLS identity to disk: {}", e);
        }
        pair
    };

    let (certs, key) = parse_pem(&cert_pem, &key_pem)?;

    let config = ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(certs, key)
        .context("Failed to build TLS server config")?;

    info!("TLS acceptor built successfully");
    Ok(TlsAcceptor::from(Arc::new(config)))
}
