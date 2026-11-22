//! Cross-platform OS primitives for BeatrixLeslie IDE (`spell_core`).
//!
//! Provides 100% OS-portable implementations across macOS, Linux, and Windows for:
//! 1. **OS Keyring** ([`get_secret`], [`set_secret`], [`delete_secret`]) backed by `keyring`
//!    (Keychain on macOS, Credential Manager on Windows, Secret Service on Linux) with an
//!    in-memory-only process-local fallback when the OS keyring daemon is unavailable.
//! 2. **System Info** ([`SystemInfoSnapshot`], [`collect_system_info`]) backed by `sysinfo`.
//! 3. **PDF Text Extraction** ([`PdfExtractResult`], [`extract_pdf_text`]) backed by `lopdf`.

use std::collections::HashMap;
use std::path::Path;

use once_cell::sync::Lazy;
use parking_lot::Mutex;
use serde::{Deserialize, Serialize};

// ============================================================================
// 1. Cross-Platform OS Keyring (with Memory-Only Fallback)
// ============================================================================

/// Default OS keyring service identifier for Spell IDE credentials.
pub const DEFAULT_KEYRING_SERVICE: &str = "com.spell.SpellIDE.credentials";

const ENDPOINT_API_KEY_ACCOUNT: &str = "endpoint.api_key";

/// Process-local memory-only secret fallback used strictly when the OS keyring
/// daemon/store is unavailable (e.g. headless CI/test environments).
/// Secrets stored here are NEVER persisted to disk.
static MEMORY_SECRET_STORE: Lazy<Mutex<HashMap<(String, String), String>>> =
    Lazy::new(|| Mutex::new(HashMap::new()));

/// Retrieves a secret from the OS keyring (or process-local memory fallback if the
/// OS keyring daemon is unavailable).
///
/// - Rejects an empty or whitespace-only `account`.
/// - Defaults an empty or whitespace-only `service` to [`DEFAULT_KEYRING_SERVICE`].
/// - Returns `Ok(None)` when no entry exists.
/// - When `account == "endpoint.api_key"` and no keyring/fallback secret is found,
///   checks `SPELL_API_KEY` then `OPENAI_API_KEY` environment variables.
fn use_ephemeral_keyring() -> bool {
    std::env::var("SPELL_EPHEMERAL_KEYRING")
        .map(|v| {
            let t = v.trim();
            t == "1" || t.eq_ignore_ascii_case("true")
        })
        .unwrap_or(false)
}

pub fn get_secret(service: &str, account: &str) -> Result<Option<String>, String> {
    let (svc, acct) = normalize_service_and_account(service, account)?;
    if use_ephemeral_keyring() {
        let mem = memory_store_get(svc, acct);
        if mem.is_some() {
            return Ok(mem);
        }
        if acct == ENDPOINT_API_KEY_ACCOUNT {
            return Ok(resolve_env_api_key(|key| std::env::var(key).ok()));
        }
        return Ok(None);
    }
    let entry_res = keyring::Entry::new(svc, acct);
    get_secret_from_entry(svc, acct, entry_res.as_ref(), |key| std::env::var(key).ok())
}

/// Stores a secret in the OS keyring (falling back to a process-local memory-only
/// store if the OS keyring daemon is unavailable; never writes secrets to disk).
///
/// - Rejects an empty or whitespace-only `account`.
/// - Defaults an empty or whitespace-only `service` to [`DEFAULT_KEYRING_SERVICE`].
pub fn set_secret(service: &str, account: &str, value: &str) -> Result<(), String> {
    let (svc, acct) = normalize_service_and_account(service, account)?;
    if use_ephemeral_keyring() {
        memory_store_set(svc, acct, value);
        return Ok(());
    }
    let entry_res = keyring::Entry::new(svc, acct);
    set_secret_in_entry(svc, acct, value, entry_res.as_ref())
}

/// Deletes a secret from the OS keyring (and process-local memory fallback).
///
/// - Rejects an empty or whitespace-only `account`.
/// - Defaults an empty or whitespace-only `service` to [`DEFAULT_KEYRING_SERVICE`].
/// - Succeeds (`Ok(())`) if the entry does not exist (`keyring::Error::NoEntry`).
pub fn delete_secret(service: &str, account: &str) -> Result<(), String> {
    let (svc, acct) = normalize_service_and_account(service, account)?;
    if use_ephemeral_keyring() {
        memory_store_remove(svc, acct);
        return Ok(());
    }
    let entry_res = keyring::Entry::new(svc, acct);
    delete_secret_in_entry(svc, acct, entry_res.as_ref())
}

fn normalize_service_and_account<'a>(
    service: &'a str,
    account: &'a str,
) -> Result<(&'a str, &'a str), String> {
    let acct = account.trim();
    if acct.is_empty() {
        return Err("Keyring account name must not be empty".to_string());
    }
    let svc = service.trim();
    let svc = if svc.is_empty() {
        DEFAULT_KEYRING_SERVICE
    } else {
        svc
    };
    Ok((svc, acct))
}

fn is_keyring_unavailable(err: &keyring::Error) -> bool {
    matches!(
        err,
        keyring::Error::NoStorageAccess(_) | keyring::Error::PlatformFailure(_)
    )
}

fn memory_store_get(service: &str, account: &str) -> Option<String> {
    MEMORY_SECRET_STORE
        .lock()
        .get(&(service.to_string(), account.to_string()))
        .cloned()
}

fn memory_store_set(service: &str, account: &str, value: &str) {
    MEMORY_SECRET_STORE
        .lock()
        .insert((service.to_string(), account.to_string()), value.to_string());
}

fn memory_store_remove(service: &str, account: &str) {
    MEMORY_SECRET_STORE
        .lock()
        .remove(&(service.to_string(), account.to_string()));
}

fn resolve_env_api_key<F>(env_lookup: F) -> Option<String>
where
    F: Fn(&str) -> Option<String>,
{
    for env_key in ["SPELL_API_KEY", "OPENAI_API_KEY"] {
        if let Some(val) = env_lookup(env_key) {
            let trimmed = val.trim();
            if !trimmed.is_empty() {
                return Some(trimmed.to_string());
            }
        }
    }
    None
}

fn get_secret_from_entry<F>(
    service: &str,
    account: &str,
    entry_result: Result<&keyring::Entry, &keyring::Error>,
    env_lookup: F,
) -> Result<Option<String>, String>
where
    F: Fn(&str) -> Option<String>,
{
    let secret = match entry_result {
        Ok(entry) => match entry.get_password() {
            Ok(val) => Some(val),
            Err(keyring::Error::NoEntry) => memory_store_get(service, account),
            Err(ref err) if is_keyring_unavailable(err) => memory_store_get(service, account),
            Err(err) => {
                return Err(format!("Failed to read secret from OS keyring: {err}"));
            }
        },
        Err(err) if is_keyring_unavailable(err) => memory_store_get(service, account),
        Err(err) => {
            return Err(format!("Failed to initialize OS keyring entry: {err}"));
        }
    };

    if secret.is_some() {
        return Ok(secret);
    }

    if account == ENDPOINT_API_KEY_ACCOUNT {
        return Ok(resolve_env_api_key(env_lookup));
    }

    Ok(None)
}

fn set_secret_in_entry(
    service: &str,
    account: &str,
    value: &str,
    entry_result: Result<&keyring::Entry, &keyring::Error>,
) -> Result<(), String> {
    match entry_result {
        Ok(entry) => match entry.set_password(value) {
            Ok(()) => {
                memory_store_remove(service, account);
                Ok(())
            }
            Err(ref err) if is_keyring_unavailable(err) => {
                memory_store_set(service, account, value);
                Ok(())
            }
            Err(err) => Err(format!("Failed to store secret in OS keyring: {err}")),
        },
        Err(err) if is_keyring_unavailable(err) => {
            memory_store_set(service, account, value);
            Ok(())
        }
        Err(err) => Err(format!("Failed to initialize OS keyring entry: {err}")),
    }
}

fn delete_secret_in_entry(
    service: &str,
    account: &str,
    entry_result: Result<&keyring::Entry, &keyring::Error>,
) -> Result<(), String> {
    memory_store_remove(service, account);
    match entry_result {
        Ok(entry) => match entry.delete_credential() {
            Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
            Err(ref err) if is_keyring_unavailable(err) => Ok(()),
            Err(err) => Err(format!("Failed to delete secret from OS keyring: {err}")),
        },
        Err(err) if is_keyring_unavailable(err) => Ok(()),
        Err(err) => Err(format!("Failed to initialize OS keyring entry: {err}")),
    }
}

// ============================================================================
// 2. Cross-Platform System Info Snapshot
// ============================================================================

/// Cross-platform snapshot of host OS, CPU, memory, and Spell IDE directories.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SystemInfoSnapshot {
    pub os: String,
    pub os_version: String,
    pub arch: String,
    pub cpu_brand: String,
    pub cpu_cores: u32,
    pub total_memory_bytes: u64,
    pub available_memory_bytes: u64,
    pub hostname: String,
    pub data_dir: String,
    pub logs_dir: String,
}

/// Collects a cross-platform [`SystemInfoSnapshot`] via `sysinfo`.
pub fn collect_system_info(data_dir: &Path, logs_dir: &Path) -> SystemInfoSnapshot {
    let sys = sysinfo::System::new_all();
    let cpus = sys.cpus();

    let os = sysinfo::System::name()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| std::env::consts::OS.to_string());

    let os_version = sysinfo::System::long_os_version()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .or_else(|| {
            sysinfo::System::os_version()
                .map(|s| s.trim().to_string())
                .filter(|s| !s.is_empty())
        })
        .unwrap_or_else(|| "unknown".to_string());

    let arch = std::env::consts::ARCH.to_string();

    let cpu_brand = cpus
        .iter()
        .map(|cpu| cpu.brand().trim())
        .find(|brand| !brand.is_empty())
        .unwrap_or("unknown")
        .to_string();

    let cpu_cores_usize = if !cpus.is_empty() {
        cpus.len()
    } else {
        std::thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(0)
    };
    let cpu_cores = u32::try_from(cpu_cores_usize).unwrap_or(u32::MAX);

    let total_memory_bytes = sys.total_memory();
    let available_memory_bytes = sys.available_memory();

    let hostname = sysinfo::System::host_name()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "unknown".to_string());

    SystemInfoSnapshot {
        os,
        os_version,
        arch,
        cpu_brand,
        cpu_cores,
        total_memory_bytes,
        available_memory_bytes,
        hostname,
        data_dir: data_dir.to_string_lossy().into_owned(),
        logs_dir: logs_dir.to_string_lossy().into_owned(),
    }
}

// ============================================================================
// 3. Cross-Platform PDF Text Extraction
// ============================================================================

/// Extracted text and page count returned by [`extract_pdf_text`].
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct PdfExtractResult {
    pub text: String,
    pub pages: u32,
}

/// Loads a PDF file at `path` via `lopdf` and extracts per-page text joined with
/// `--- Page N ---` page separators.
///
/// Handles missing, unreadable, encrypted, and corrupt PDF files with clear error messages.
pub fn extract_pdf_text(path: &Path) -> Result<PdfExtractResult, String> {
    let meta = std::fs::metadata(path).map_err(|err| {
        if err.kind() == std::io::ErrorKind::NotFound {
            format!("PDF file not found: {}", path.display())
        } else {
            format!("Cannot access PDF file '{}': {err}", path.display())
        }
    })?;
    if meta.is_dir() {
        return Err(format!(
            "Path '{}' is a directory, expected a PDF file",
            path.display()
        ));
    }

    let mut doc = lopdf::Document::load(path).map_err(|err| format_lopdf_error(path, &err))?;

    if doc.is_encrypted() {
        // Attempt empty-password decryption for owner-permission-only PDFs.
        if let Err(err) = doc.decrypt("") {
            return Err(format!(
                "PDF '{}' is encrypted and requires a password: {err}",
                path.display()
            ));
        }
    } else if doc.trailer.has(b"Encrypt") {
        return Err(format!(
            "PDF '{}' is encrypted and cannot be decrypted",
            path.display()
        ));
    }

    let pages = doc.get_pages();
    if pages.is_empty() && doc.catalog().is_err() {
        return Err(format!(
            "Corrupt or invalid PDF '{}': missing document catalog",
            path.display()
        ));
    }

    let total_pages = u32::try_from(pages.len()).unwrap_or(u32::MAX);
    let mut page_sections: Vec<String> = Vec::with_capacity(pages.len());
    let mut extracted_any_ok = false;
    let mut last_extract_err: Option<String> = None;

    for &page_num in pages.keys() {
        match doc.extract_text(&[page_num]) {
            Ok(raw_text) => {
                extracted_any_ok = true;
                let cleaned = raw_text.trim();
                if !cleaned.is_empty() {
                    page_sections.push(format!("--- Page {page_num} ---\n{cleaned}"));
                }
            }
            Err(lopdf::Error::Decryption(dec_err)) => {
                return Err(format!(
                    "PDF '{}' is encrypted and could not be decrypted on page {page_num}: {dec_err}",
                    path.display()
                ));
            }
            Err(err) => {
                last_extract_err = Some(format!("page {page_num}: {err}"));
            }
        }
    }

    if !extracted_any_ok {
        if let Some(err_detail) = last_extract_err {
            return Err(format!(
                "Failed to extract text from PDF '{}' ({err_detail})",
                path.display()
            ));
        }
    }

    Ok(PdfExtractResult {
        text: page_sections.join("\n\n"),
        pages: total_pages,
    })
}

fn format_lopdf_error(path: &Path, err: &lopdf::Error) -> String {
    match err {
        lopdf::Error::IO(io_err) if io_err.kind() == std::io::ErrorKind::NotFound => {
            format!("PDF file not found: {}", path.display())
        }
        lopdf::Error::IO(io_err) => {
            format!("Failed to read PDF file '{}': {io_err}", path.display())
        }
        lopdf::Error::Decryption(dec_err) => {
            format!(
                "PDF '{}' is encrypted and cannot be decrypted: {dec_err}",
                path.display()
            )
        }
        lopdf::Error::InvalidPassword | lopdf::Error::UnsupportedSecurityHandler(_) => {
            format!(
                "PDF '{}' is encrypted and cannot be decrypted: {err}",
                path.display()
            )
        }
        lopdf::Error::Xref(_)
        | lopdf::Error::MissingXrefEntry
        | lopdf::Error::Parse(_)
        | lopdf::Error::Syntax(_)
        | lopdf::Error::InvalidOffset(_)
        | lopdf::Error::IndirectObject { .. }
        | lopdf::Error::InvalidStream(_)
        | lopdf::Error::InvalidObjectStream(_)
        | lopdf::Error::Decompress(_)
        | lopdf::Error::ObjectType { .. }
        | lopdf::Error::DictType { .. }
        | lopdf::Error::DictKey(_)
        | lopdf::Error::ObjectNotFound(_)
        | lopdf::Error::ObjectIdMismatch
        | lopdf::Error::ReferenceCycle(_)
        | lopdf::Error::ReferenceLimit
        | lopdf::Error::RecursionLimit => {
            format!("Corrupt or invalid PDF '{}': {err}", path.display())
        }
        other => format!("Failed to load PDF '{}': {other}", path.display()),
    }
}

// ============================================================================
// Unit Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};

    static TEST_COUNTER: AtomicU64 = AtomicU64::new(0);

    fn unique_temp_path(name: &str) -> PathBuf {
        let seq = TEST_COUNTER.fetch_add(1, Ordering::Relaxed);
        std::env::temp_dir().join(format!(
            "spell_platform_test_{}_{}_{}",
            std::process::id(),
            seq,
            name
        ))
    }

    fn write_test_pdf(path: &Path, page_texts: &[&str]) {
        use lopdf::content::{Content, Operation};
        use lopdf::{Dictionary, Document, Object, Stream};

        let mut doc = Document::with_version("1.5");
        let pages_id = doc.new_object_id();

        let mut font_dict = Dictionary::new();
        font_dict.set("Type", "Font");
        font_dict.set("Subtype", "Type1");
        font_dict.set("BaseFont", "Helvetica");
        font_dict.set("Encoding", "WinAnsiEncoding");
        let font_id = doc.add_object(font_dict);

        let mut fonts_map = Dictionary::new();
        fonts_map.set("F1", font_id);
        let mut resources_dict = Dictionary::new();
        resources_dict.set("Font", fonts_map);
        let resources_id = doc.add_object(resources_dict);

        let mut kids: Vec<Object> = Vec::new();
        for text in page_texts {
            let content = Content {
                operations: vec![
                    Operation::new("BT", vec![]),
                    Operation::new("Tf", vec!["F1".into(), Object::Integer(12)]),
                    Operation::new("Td", vec![Object::Integer(72), Object::Integer(720)]),
                    Operation::new("Tj", vec![Object::string_literal(*text)]),
                    Operation::new("ET", vec![]),
                ],
            };
            let encoded = content.encode().expect("encode PDF page content");
            let content_id = doc.add_object(Stream::new(Dictionary::new(), encoded));

            let mut page_dict = Dictionary::new();
            page_dict.set("Type", "Page");
            page_dict.set("Parent", pages_id);
            page_dict.set("Contents", content_id);
            let page_id = doc.add_object(page_dict);
            kids.push(page_id.into());
        }

        let mut pages_dict = Dictionary::new();
        pages_dict.set("Type", "Pages");
        pages_dict.set("Kids", kids);
        pages_dict.set("Count", Object::Integer(page_texts.len() as i64));
        pages_dict.set("Resources", resources_id);
        pages_dict.set(
            "MediaBox",
            vec![
                Object::Integer(0),
                Object::Integer(0),
                Object::Integer(612),
                Object::Integer(792),
            ],
        );
        doc.objects.insert(pages_id, Object::Dictionary(pages_dict));

        let mut catalog_dict = Dictionary::new();
        catalog_dict.set("Type", "Catalog");
        catalog_dict.set("Pages", pages_id);
        let catalog_id = doc.add_object(catalog_dict);

        doc.trailer.set("Root", catalog_id);
        doc.save(path).expect("save test PDF");
    }

    #[test]
    fn test_collect_system_info_populates_fields_and_roundtrips_json() {
        let data_dir = Path::new("/tmp/spell_data_test");
        let logs_dir = Path::new("/tmp/spell_logs_test");

        let snap = collect_system_info(data_dir, logs_dir);
        assert!(!snap.os.is_empty());
        assert!(!snap.os_version.is_empty());
        assert_eq!(snap.arch, std::env::consts::ARCH);
        assert!(!snap.cpu_brand.is_empty());
        assert!(snap.cpu_cores >= 1);
        assert!(snap.total_memory_bytes > 0);
        assert!(!snap.hostname.is_empty());
        assert_eq!(snap.data_dir, data_dir.to_string_lossy());
        assert_eq!(snap.logs_dir, logs_dir.to_string_lossy());

        let json = serde_json::to_string(&snap).expect("serialize SystemInfoSnapshot");
        let parsed: SystemInfoSnapshot =
            serde_json::from_str(&json).expect("deserialize SystemInfoSnapshot");
        assert_eq!(parsed, snap);
    }

    #[test]
    fn test_extract_pdf_text_missing_file_returns_clear_error() {
        let missing = unique_temp_path("does_not_exist.pdf");
        let _ = std::fs::remove_file(&missing);

        let err = extract_pdf_text(&missing).unwrap_err();
        assert!(
            err.to_ascii_lowercase().contains("not found"),
            "expected 'not found' error, got: {err}"
        );
    }

    #[test]
    fn test_extract_pdf_text_corrupt_file_returns_clear_error() {
        let corrupt = unique_temp_path("corrupt.pdf");
        std::fs::write(&corrupt, b"this is definitely not a valid PDF document").unwrap();

        let err = extract_pdf_text(&corrupt).unwrap_err();
        let _ = std::fs::remove_file(&corrupt);

        let low = err.to_ascii_lowercase();
        assert!(
            low.contains("corrupt") || low.contains("invalid"),
            "expected corrupt/invalid PDF error, got: {err}"
        );
    }

    #[test]
    fn test_extract_pdf_text_encrypted_pdf_returns_clear_error() {
        use lopdf::{Dictionary, Document, Object};

        let encrypted_path = unique_temp_path("encrypted.pdf");
        let mut doc = Document::with_version("1.5");
        let pages_id = doc.new_object_id();
        let mut pages_dict = Dictionary::new();
        pages_dict.set("Type", "Pages");
        pages_dict.set("Kids", Vec::<Object>::new());
        pages_dict.set("Count", Object::Integer(0));
        doc.objects.insert(pages_id, Object::Dictionary(pages_dict));

        let mut catalog_dict = Dictionary::new();
        catalog_dict.set("Type", "Catalog");
        catalog_dict.set("Pages", pages_id);
        let catalog_id = doc.add_object(catalog_dict);

        let mut encrypt_dict = Dictionary::new();
        encrypt_dict.set("Filter", "Standard");
        encrypt_dict.set("V", Object::Integer(2));
        encrypt_dict.set("R", Object::Integer(3));
        encrypt_dict.set("Length", Object::Integer(128));
        encrypt_dict.set("O", Object::string_literal("01234567890123456789012345678901"));
        encrypt_dict.set("U", Object::string_literal("01234567890123456789012345678901"));
        encrypt_dict.set("P", Object::Integer(-4));
        let encrypt_id = doc.add_object(encrypt_dict);

        doc.trailer.set("Root", catalog_id);
        doc.trailer.set("Encrypt", encrypt_id);
        doc.trailer.set(
            "ID",
            vec![
                Object::string_literal("1234567890123456"),
                Object::string_literal("1234567890123456"),
            ],
        );
        doc.save(&encrypted_path).unwrap();

        let err = extract_pdf_text(&encrypted_path).unwrap_err();
        let _ = std::fs::remove_file(&encrypted_path);

        assert!(
            err.to_ascii_lowercase().contains("encrypted"),
            "expected encrypted PDF error, got: {err}"
        );
    }

    #[test]
    fn test_extract_pdf_text_multipage_pdf_extracts_with_separators() {
        let pdf_path = unique_temp_path("multipage.pdf");
        write_test_pdf(&pdf_path, &["Alpha page content", "Beta page content"]);

        let res = extract_pdf_text(&pdf_path).expect("extract text from valid test PDF");
        let _ = std::fs::remove_file(&pdf_path);

        assert_eq!(res.pages, 2);
        assert!(res.text.contains("--- Page 1 ---"), "text: {}", res.text);
        assert!(res.text.contains("Alpha page content"), "text: {}", res.text);
        assert!(res.text.contains("--- Page 2 ---"), "text: {}", res.text);
        assert!(res.text.contains("Beta page content"), "text: {}", res.text);

        let json = serde_json::to_string(&res).expect("serialize PdfExtractResult");
        let parsed: PdfExtractResult =
            serde_json::from_str(&json).expect("deserialize PdfExtractResult");
        assert_eq!(parsed, res);
    }

    #[test]
    fn test_secret_validation_rejects_empty_account_and_defaults_service() {
        assert!(get_secret("", "").is_err());
        assert!(get_secret(DEFAULT_KEYRING_SERVICE, "   ").is_err());
        assert!(set_secret("", "", "secret-val").is_err());
        assert!(set_secret("custom.service", "   ", "secret-val").is_err());
        assert!(delete_secret("", "").is_err());
        assert!(delete_secret("custom.service", "\t\n").is_err());

        let (svc, acct) = normalize_service_and_account("", "endpoint.api_key").unwrap();
        assert_eq!(svc, DEFAULT_KEYRING_SERVICE);
        assert_eq!(acct, "endpoint.api_key");

        let (svc_ws, acct_ws) = normalize_service_and_account("   ", "  my.token  ").unwrap();
        assert_eq!(svc_ws, DEFAULT_KEYRING_SERVICE);
        assert_eq!(acct_ws, "my.token");

        let (svc_custom, acct_custom) =
            normalize_service_and_account("com.example.service", "user.key").unwrap();
        assert_eq!(svc_custom, "com.example.service");
        assert_eq!(acct_custom, "user.key");
    }

    #[test]
    fn test_secret_mock_roundtrip_and_no_entry_handling() {
        let svc = "com.spell.test.mock.roundtrip";
        let acct = "token.alpha";
        let entry = keyring::Entry::new_with_credential(Box::new(
            keyring::mock::MockCredential::default(),
        ));

        // Missing entry -> Ok(None) on get, Ok(()) on delete
        assert_eq!(
            get_secret_from_entry(svc, acct, Ok(&entry), |_| None).unwrap(),
            None
        );
        assert!(delete_secret_in_entry(svc, acct, Ok(&entry)).is_ok());

        // Set -> Get -> Delete -> Get
        set_secret_in_entry(svc, acct, "super-secret-value", Ok(&entry)).unwrap();
        assert_eq!(
            get_secret_from_entry(svc, acct, Ok(&entry), |_| None).unwrap(),
            Some("super-secret-value".to_string())
        );
        delete_secret_in_entry(svc, acct, Ok(&entry)).unwrap();
        assert_eq!(
            get_secret_from_entry(svc, acct, Ok(&entry), |_| None).unwrap(),
            None
        );
    }

    #[test]
    fn test_secret_memory_fallback_when_keyring_unavailable() {
        let svc = "com.spell.test.fallback.memory";
        let acct = "token.headless";
        let entry = keyring::Entry::new_with_credential(Box::new(
            keyring::mock::MockCredential::default(),
        ));
        let mock: &keyring::mock::MockCredential = entry
            .get_credential()
            .downcast_ref()
            .expect("downcast MockCredential");

        mock.set_error(keyring::Error::NoStorageAccess(Box::new(
            std::io::Error::new(std::io::ErrorKind::PermissionDenied, "keychain locked"),
        )));
        set_secret_in_entry(svc, acct, "headless-secret", Ok(&entry)).unwrap();

        mock.set_error(keyring::Error::PlatformFailure(Box::new(
            std::io::Error::new(std::io::ErrorKind::NotConnected, "dbus unavailable"),
        )));
        assert_eq!(
            get_secret_from_entry(svc, acct, Ok(&entry), |_| None).unwrap(),
            Some("headless-secret".to_string())
        );

        mock.set_error(keyring::Error::NoStorageAccess(Box::new(
            std::io::Error::new(std::io::ErrorKind::PermissionDenied, "keychain locked"),
        )));
        delete_secret_in_entry(svc, acct, Ok(&entry)).unwrap();

        assert_eq!(
            get_secret_from_entry(svc, acct, Ok(&entry), |_| None).unwrap(),
            None
        );
    }

    #[test]
    fn test_secret_env_fallback_only_for_endpoint_api_key_when_keyring_none() {
        let svc = "com.spell.test.env.fallback";
        let entry = keyring::Entry::new_with_credential(Box::new(
            keyring::mock::MockCredential::default(),
        ));

        let both_env = |key: &str| match key {
            "SPELL_API_KEY" => Some("  sk-spell-primary  ".to_string()),
            "OPENAI_API_KEY" => Some("sk-openai-secondary".to_string()),
            _ => None,
        };
        let openai_only_env = |key: &str| match key {
            "SPELL_API_KEY" => Some("   ".to_string()),
            "OPENAI_API_KEY" => Some("sk-openai-only".to_string()),
            _ => None,
        };

        // Other account never falls back to env vars.
        assert_eq!(
            get_secret_from_entry(svc, "other.account", Ok(&entry), both_env).unwrap(),
            None
        );

        // `endpoint.api_key` falls back to SPELL_API_KEY, then OPENAI_API_KEY, when keyring is None.
        assert_eq!(
            get_secret_from_entry(svc, ENDPOINT_API_KEY_ACCOUNT, Ok(&entry), both_env).unwrap(),
            Some("sk-spell-primary".to_string())
        );
        assert_eq!(
            get_secret_from_entry(svc, ENDPOINT_API_KEY_ACCOUNT, Ok(&entry), openai_only_env)
                .unwrap(),
            Some("sk-openai-only".to_string())
        );

        // When keyring has a value, keyring takes precedence over env vars.
        set_secret_in_entry(svc, ENDPOINT_API_KEY_ACCOUNT, "sk-from-keyring", Ok(&entry)).unwrap();
        assert_eq!(
            get_secret_from_entry(svc, ENDPOINT_API_KEY_ACCOUNT, Ok(&entry), both_env).unwrap(),
            Some("sk-from-keyring".to_string())
        );
        delete_secret_in_entry(svc, ENDPOINT_API_KEY_ACCOUNT, Ok(&entry)).unwrap();
    }
}
