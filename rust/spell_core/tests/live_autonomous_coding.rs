//! Live Cross-Platform Rust/Cargo Autonomous Coding Integration Test (`spell_core`).
//!
//! Configured via environment variables:
//! - `SPELL_LIVE_ENDPOINT` (or `SPELL_LIVE_ENDPOINT_URL`): Base URL of any OpenAI-compatible endpoint
//! - `SPELL_API_KEY` (optional; falls back to `OPENAI_API_KEY` when unset)
//! - `SPELL_LIVE_MODEL` (optional; falls back to `SPELL_LIVE_MODEL_ID` or first model from `GET /models`)
//!
//! Run manually against a live endpoint:
//! ```bash
//! SPELL_LIVE_ENDPOINT=http://127.0.0.1:11434/v1 \
//!   SPELL_LIVE_MODEL=qwen2.5-coder:7b \
//!   cargo test --test live_autonomous_coding -- --ignored --nocapture
//! ```

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicU64, Ordering};

use serde_json::{json, Value};

use spell_core::agent_loop::SpellCoreSession;
use spell_core::locator::EnginePathLocator;
use spell_core::subagent_coding::parse_delegation_result_meta;
use spell_core::transport::{EngineTransport, HttpTransport};

static TEMP_SEQ: AtomicU64 = AtomicU64::new(1);

const SEED_CARGO_TOML: &str = r#"[package]
name = "spell_live_coding_fixture"
version = "0.1.0"
edition = "2021"
"#;

const SEED_LIB_RS: &str = r#"//! Minimal arithmetic fixture crate for autonomous coding verification.

pub fn add_checked(a: i64, b: i64) -> Option<i64> {
    a.checked_add(b)
}
"#;

#[derive(Clone, Debug, PartialEq, Eq)]
struct LiveCodingConfig {
    endpoint: String,
    api_key: Option<String>,
    model: Option<String>,
}

impl LiveCodingConfig {
    fn from_map(vars: &BTreeMap<String, String>) -> Option<Self> {
        let endpoint = vars
            .get("SPELL_LIVE_ENDPOINT")
            .or_else(|| vars.get("SPELL_LIVE_ENDPOINT_URL"))
            .map(|v| v.trim().to_string())
            .filter(|v| !v.is_empty())?;

        let api_key = vars
            .get("SPELL_API_KEY")
            .or_else(|| vars.get("OPENAI_API_KEY"))
            .map(|v| v.trim().to_string())
            .filter(|v| !v.is_empty());

        let model = vars
            .get("SPELL_LIVE_MODEL")
            .or_else(|| vars.get("SPELL_LIVE_MODEL_ID"))
            .map(|v| v.trim().to_string())
            .filter(|v| !v.is_empty());

        Some(Self {
            endpoint,
            api_key,
            model,
        })
    }

    fn from_env() -> Option<Self> {
        let mut vars = BTreeMap::new();
        for key in [
            "SPELL_LIVE_ENDPOINT",
            "SPELL_LIVE_ENDPOINT_URL",
            "SPELL_API_KEY",
            "OPENAI_API_KEY",
            "SPELL_LIVE_MODEL",
            "SPELL_LIVE_MODEL_ID",
        ] {
            if let Ok(val) = std::env::var(key) {
                vars.insert(key.to_string(), val);
            }
        }
        Self::from_map(&vars)
    }
}

struct TempCrateWorkspace {
    root: PathBuf,
    target_dir: PathBuf,
}

impl TempCrateWorkspace {
    fn seed(label: &str) -> Result<Self, String> {
        let seq = TEMP_SEQ.fetch_add(1, Ordering::SeqCst);
        let pid = std::process::id();
        let root = std::env::temp_dir().join(format!("spell_live_crate_{pid}_{seq}_{label}"));
        let target_dir =
            std::env::temp_dir().join(format!("spell_live_target_{pid}_{seq}_{label}"));
        let _ = fs::remove_dir_all(&root);
        let _ = fs::remove_dir_all(&target_dir);

        fs::create_dir_all(root.join("src")).map_err(|e| e.to_string())?;
        fs::write(root.join("Cargo.toml"), SEED_CARGO_TOML).map_err(|e| e.to_string())?;
        fs::write(root.join("src/lib.rs"), SEED_LIB_RS).map_err(|e| e.to_string())?;

        Ok(Self { root, target_dir })
    }

    fn path(&self) -> &Path {
        &self.root
    }

    fn verify_cargo_check(&self) -> Result<(), String> {
        let manifest = self.root.join("Cargo.toml");
        let out = Command::new("cargo")
            .arg("check")
            .arg("--offline")
            .arg("--manifest-path")
            .arg(&manifest)
            .arg("--target-dir")
            .arg(&self.target_dir)
            .output()
            .map_err(|e| format!("Failed to invoke cargo check: {e}"))?;
        if out.status.success() {
            Ok(())
        } else {
            let stderr = String::from_utf8_lossy(&out.stderr);
            Err(format!(
                "cargo check failed (status={:?}): {}",
                out.status.code(),
                stderr.trim()
            ))
        }
    }
}

impl Drop for TempCrateWorkspace {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
        let _ = fs::remove_dir_all(&self.target_dir);
    }
}

#[test]
fn live_coding_config_parsing_and_seed_crate_scaffold() {
    let empty = BTreeMap::new();
    assert_eq!(LiveCodingConfig::from_map(&empty), None);

    let mut blank = BTreeMap::new();
    blank.insert("SPELL_LIVE_ENDPOINT".to_string(), "   ".to_string());
    assert_eq!(LiveCodingConfig::from_map(&blank), None);

    let mut primary = BTreeMap::new();
    primary.insert(
        "SPELL_LIVE_ENDPOINT".to_string(),
        "  http://127.0.0.1:11434/v1  ".to_string(),
    );
    primary.insert("SPELL_API_KEY".to_string(), " sk-test-key ".to_string());
    primary.insert(
        "SPELL_LIVE_MODEL".to_string(),
        " qwen2.5-coder:7b ".to_string(),
    );
    let parsed = LiveCodingConfig::from_map(&primary).expect("parse primary env config");
    assert_eq!(parsed.endpoint, "http://127.0.0.1:11434/v1");
    assert_eq!(parsed.api_key.as_deref(), Some("sk-test-key"));
    assert_eq!(parsed.model.as_deref(), Some("qwen2.5-coder:7b"));

    let mut fallback = BTreeMap::new();
    fallback.insert(
        "SPELL_LIVE_ENDPOINT_URL".to_string(),
        "https://api.example.com/v1".to_string(),
    );
    fallback.insert("OPENAI_API_KEY".to_string(), "sk-fallback".to_string());
    fallback.insert(
        "SPELL_LIVE_MODEL_ID".to_string(),
        "gpt-4o-mini".to_string(),
    );
    let parsed_fb = LiveCodingConfig::from_map(&fallback).expect("parse fallback env config");
    assert_eq!(parsed_fb.endpoint, "https://api.example.com/v1");
    assert_eq!(parsed_fb.api_key.as_deref(), Some("sk-fallback"));
    assert_eq!(parsed_fb.model.as_deref(), Some("gpt-4o-mini"));

    let ws = TempCrateWorkspace::seed("unit_scaffold").expect("seed temporary Rust crate");
    let root_copy = ws.path().to_path_buf();
    assert!(root_copy.join("Cargo.toml").is_file());
    assert_eq!(
        fs::read_to_string(root_copy.join("src/lib.rs")).expect("read seed lib.rs"),
        SEED_LIB_RS
    );
    drop(ws);
    assert!(!root_copy.exists(), "TempCrateWorkspace must clean up on drop");
}

#[test]
#[ignore]
fn live_autonomous_coding_rust_crate_roundtrip() {
    let Some(cfg) = LiveCodingConfig::from_env() else {
        println!("Skipping live autonomous coding test: SPELL_LIVE_ENDPOINT is not set.");
        return;
    };

    let probe_transport = if let Some(ref key) = cfg.api_key {
        HttpTransport::with_api_key(cfg.endpoint.clone(), key)
    } else {
        HttpTransport::new(cfg.endpoint.clone(), EnginePathLocator::discover())
    };
    let discovered_models = probe_transport.list_models().unwrap_or_default();
    let model_id = cfg
        .model
        .clone()
        .or_else(|| discovered_models.first().map(|m| m.id.clone()))
        .expect("SPELL_LIVE_MODEL must be set or GET /models must return at least one model");

    let ws = TempCrateWorkspace::seed("live_roundtrip").expect("seed temporary Rust crate");
    ws.verify_cargo_check()
        .expect("baseline temporary Rust crate must pass cargo check");

    let mut session = SpellCoreSession::new_in_memory().expect("SpellCoreSession::new_in_memory");
    session
        .configure_endpoint(
            Some(&cfg.endpoint),
            Some(cfg.api_key.as_deref()),
            Some(&model_id),
            None,
        )
        .expect("configure_endpoint against live endpoint");

    let proj = session
        .add_project(
            "SpellLiveCodingFixture",
            ws.path().to_str().expect("utf8 workspace path"),
            false,
        )
        .expect("add_project");
    let thread = session
        .create_thread(
            Some(&proj.id),
            Some("Live Autonomous Coding Roundtrip"),
            Some("write"),
            vec!["systems-architecture".to_string()],
        )
        .expect("create_thread");

    let args = json!({
        "mode": "auto",
        "protocol": "auto",
        "target_files": ["src/lib.rs"],
        "context_notes": "You are executing inside `delegate_coding_subagent`. Edit ONLY `src/lib.rs` using `edit_file` (without line-number prefixes) and finish cleanly once Gatekeeper Critic passes.",
        "instruction": "Edit ONLY `src/lib.rs` to add `pub fn clamp_i64(value: i64, min: i64, max: i64) -> i64` returning `value.max(min).min(max)`. Preserve `add_checked` and do not modify `Cargo.toml`."
    });

    let (output, tool_status) = session
        .execute_delegate_coding_subagent_direct(&thread.id, "live_coding_call_1", args)
        .expect("execute_delegate_coding_subagent_direct");
    assert_eq!(tool_status, "completed", "unexpected tool_status: {output}");

    let meta = parse_delegation_result_meta(&output).expect("parse delegation result metadata");
    assert_eq!(meta.status, "committed", "expected committed status: {meta:?}");

    let updated_lib =
        fs::read_to_string(ws.path().join("src/lib.rs")).expect("read committed src/lib.rs");
    assert!(
        updated_lib.contains("pub fn clamp_i64("),
        "src/lib.rs missing `pub fn clamp_i64`: {updated_lib}"
    );
    assert!(
        updated_lib.contains("pub fn add_checked("),
        "src/lib.rs must preserve `pub fn add_checked`: {updated_lib}"
    );

    ws.verify_cargo_check()
        .expect("committed src/lib.rs must compile cleanly under cargo check");

    let undo_id = meta.undo_id.expect("committed delegation must emit undo_id");
    let undo_raw = session.dispatch_json_command(
        &json!({
            "cmd": "undo_delegation",
            "undo_id": undo_id,
        })
        .to_string(),
    );
    let undo_snap: Value = serde_json::from_str(&undo_raw).expect("parse undo snapshot JSON");
    assert_eq!(
        undo_snap["ok"].as_bool(),
        Some(true),
        "undo_delegation failed: {undo_snap}"
    );
    let restored_lib =
        fs::read_to_string(ws.path().join("src/lib.rs")).expect("read restored src/lib.rs");
    assert_eq!(
        restored_lib, SEED_LIB_RS,
        "undo_delegation must restore src/lib.rs byte-for-byte"
    );
}
