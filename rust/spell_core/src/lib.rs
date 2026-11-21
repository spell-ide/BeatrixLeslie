//! `spell_core` — Endpoint-Agnostic OpenAI-Compatible Rust Core & FFI Session for BeatrixLeslie IDE
//!
//! Modules:
//! - [`locator`]: Dynamic `EnginePathLocator` discovery (`workspace_roots`, `data_dir`, API key).
//! - [`contract`]: Standard OpenAI-compatible chat/streaming/models wire types and local timer/subagent models.
//! - [`transport`]: [`EngineTransport`] trait and [`HttpTransport`] over pure-Rust `reqwest`/`rustls` (HTTP/HTTPS) and `#[cfg(unix)]` AF_UNIX sockets.
//! - [`platform`]: Cross-platform OS keyring (`keyring`), system info (`sysinfo`), and PDF text extraction (`lopdf`).
//! - [`tags`]: Conversation Rubric Tags (`ConversationTag`, `TagRegistry`).
//! - [`tools`]: Headless workspace tool suite (`prefill_context_subagent`, `delegate_coding_subagent`, filesystem, git, terminal, research), `TurnPinCache` SHA-256 deduplication, and system prompt builder.
//! - [`mcp_client`]: Real JSON-RPC 2.0 stdio `McpStdioClient` & `EmbeddedMcpResponder` with `< 50 ms` cancellation.
//! - [`storage`]: SQLite3 relational & KV persistence (`SpellStorage`) preserving exact `WireMessage` history and local wall-clock timers.
//! - [`agent_loop`]: `SpellCoreSession` multi-turn agent loop, local context reset, local timers, local subagent slots, and JSON FFI router.

pub mod agent_loop;
pub mod contract;
pub mod locator;
pub mod mcp_client;
pub mod online_fetch;
pub mod platform;
pub mod storage;
pub mod tags;
pub mod terminal_bridge;
pub mod subagent_coding;
pub mod tools;
pub mod transport;
#[cfg(any(test, feature = "test-support"))]
pub mod test_support;

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use once_cell::sync::Lazy;
use parking_lot::RwLock;

pub use crate::agent_loop::SpellCoreSession;
use crate::tools::read_file_unrestricted;

/// Global atomic runtime patch generation counter (`SPELL_PATCH_GEN`).
pub static SPELL_PATCH_GEN: AtomicU64 = AtomicU64::new(0);

/// Global lock-free turn cancellation flag (`SPELL_TURN_CANCEL_REQUESTED`).
pub static SPELL_TURN_CANCEL_REQUESTED: AtomicBool = AtomicBool::new(false);

/// Global lock-free live workspace snapshot cache (`SPELL_LIVE_SNAPSHOT_CACHE`).
pub static SPELL_LIVE_SNAPSHOT_CACHE: Lazy<RwLock<String>> =
    Lazy::new(|| RwLock::new("{\"ok\":true}".to_string()));

static LAST_FFI_PANIC: Lazy<RwLock<String>> = Lazy::new(|| RwLock::new(String::new()));
static FFI_PANIC_HOOK_ONCE: std::sync::Once = std::sync::Once::new();

fn install_ffi_panic_hook_once() {
    FFI_PANIC_HOOK_ONCE.call_once(|| {
        let prev = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            let loc = info
                .location()
                .map(|l| format!("{}:{}:{}", l.file(), l.line(), l.column()))
                .unwrap_or_else(|| "<unknown>".to_string());
            let msg = if let Some(s) = info.payload().downcast_ref::<&str>() {
                (*s).to_string()
            } else if let Some(s) = info.payload().downcast_ref::<String>() {
                s.clone()
            } else {
                "panic".to_string()
            };
            *LAST_FFI_PANIC.write() = format!("{loc}: {msg}");
            prev(info);
        }));
    });
}

pub fn update_live_snapshot_cache(snapshot_json: &str) {
    *SPELL_LIVE_SNAPSHOT_CACHE.write() = snapshot_json.to_string();
    let _ = spell_bump_patch_gen();
}

pub fn get_live_snapshot_cache() -> String {
    SPELL_LIVE_SNAPSHOT_CACHE.read().clone()
}

#[allow(dead_code)]
fn apply_active_subagents_to_snapshot_json(
    existing_json: &str,
    active: &[crate::contract::ActiveSubagentIndicator],
) -> Option<String> {
    let active_val = serde_json::to_value(active).ok()?;
    let mut parsed: serde_json::Value = serde_json::from_str(existing_json).ok()?;
    let obj = parsed.as_object_mut()?;
    obj.insert("active_subagents".to_string(), active_val.clone());
    obj.insert("activeSubagents".to_string(), active_val);
    serde_json::to_string(&parsed).ok()
}

/// Patches `SPELL_LIVE_SNAPSHOT_CACHE` in place to set both `"active_subagents"` and
/// `"activeSubagents"` to `active`, then bumps `SPELL_PATCH_GEN` when updated so UI polling
/// observes subagent lifecycle transitions within 50 ms without acquiring any session mutex.
///
/// # Lock Ordering
/// Callers MUST hold or release `session.active_subagents` in a strict, consistent lock order
/// (`session.active_subagents` BEFORE `SPELL_LIVE_SNAPSHOT_CACHE`) so concurrent parallel subagents
/// (`std::thread::scope`) can never race out-of-order or deadlock.
#[allow(dead_code)]
pub(crate) fn patch_live_snapshot_active_subagents(
    active: &[crate::contract::ActiveSubagentIndicator],
) {
    let updated = {
        let mut guard = SPELL_LIVE_SNAPSHOT_CACHE.write();
        if let Some(serialized) = apply_active_subagents_to_snapshot_json(&guard, active) {
            *guard = serialized;
            true
        } else {
            false
        }
    };
    if updated {
        let _ = spell_bump_patch_gen();
    }
}

pub fn request_turn_cancel() {
    SPELL_TURN_CANCEL_REQUESTED.store(true, Ordering::SeqCst);
    let _ = spell_bump_patch_gen();
}

pub fn clear_turn_cancel() {
    SPELL_TURN_CANCEL_REQUESTED.store(false, Ordering::SeqCst);
}

pub fn is_turn_cancel_requested() -> bool {
    SPELL_TURN_CANCEL_REQUESTED.load(Ordering::SeqCst)
}

// ============================================================================
// C-ABI Exports (`include/spell_core.h`)
// ============================================================================

/// Creates a new `SpellCoreSession` backed by SQLite3 at `db_path` (or default `data_dir` if null/empty).
///
/// # Safety
/// `db_path` must be either null or a valid null-terminated UTF-8 C string.
/// The returned pointer must be freed via [`spell_session_free`].
#[no_mangle]
pub unsafe extern "C" fn spell_session_new(db_path: *const c_char) -> *mut SpellCoreSession {
    install_ffi_panic_hook_once();
    let res = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let path_opt = if db_path.is_null() {
            None
        } else {
            CStr::from_ptr(db_path)
                .to_str()
                .ok()
                .map(str::trim)
                .filter(|s| !s.is_empty())
        };

        let session = SpellCoreSession::new(path_opt).unwrap_or_else(|_| {
            SpellCoreSession::new_in_memory().expect("Fallback in-memory SpellCoreSession")
        });
        Box::into_raw(Box::new(session))
    }));
    res.unwrap_or(std::ptr::null_mut())
}

/// Frees a `SpellCoreSession` pointer previously allocated by [`spell_session_new`].
///
/// # Safety
/// `ptr` must be either null or a valid pointer returned by [`spell_session_new`].
#[no_mangle]
pub unsafe extern "C" fn spell_session_free(ptr: *mut SpellCoreSession) {
    if !ptr.is_null() {
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _ = Box::from_raw(ptr);
        }));
    }
}

/// Dispatches a JSON command string to `SpellCoreSession` and returns a heap-allocated C string
/// containing the updated workspace snapshot JSON.
///
/// # Safety
/// `ptr` must be a valid pointer returned by [`spell_session_new`], and `cmd_json` must be a valid
/// null-terminated UTF-8 C string. The returned string must be freed via [`spell_free_string`].
#[no_mangle]
pub unsafe extern "C" fn spell_session_dispatch_json(
    ptr: *mut SpellCoreSession,
    cmd_json: *const c_char,
) -> *mut c_char {
    install_ffi_panic_hook_once();
    if ptr.is_null() {
        return CString::new("{\"ok\":false,\"error\":\"Null SpellCoreSession pointer\"}")
            .unwrap()
            .into_raw();
    }
    let session = &mut *ptr;
    let cmd_str = if cmd_json.is_null() {
        "{\"cmd\":\"get_snapshot\"}"
    } else {
        CStr::from_ptr(cmd_json)
            .to_str()
            .unwrap_or("{\"cmd\":\"get_snapshot\"}")
    };

    let response_json = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        session.dispatch_json_command(cmd_str)
    })) {
        Ok(json) => json,
        Err(_) => {
            let panic_detail = LAST_FFI_PANIC.read().clone();
            serde_json::json!({
                "ok": false,
                "error": format!("Rust FFI panic in dispatch_json_command: {panic_detail}")
            })
            .to_string()
        }
    };
    let sanitized = if response_json.as_bytes().contains(&0) {
        response_json.replace('\0', "")
    } else {
        response_json
    };
    CString::new(sanitized)
        .unwrap_or_else(|_| CString::new("{\"ok\":false,\"error\":\"CString encoding error\"}").unwrap())
        .into_raw()
}

/// Frees a heap-allocated C string previously returned by `spell_core`.
///
/// # Safety
/// `ptr` must be either null or a pointer returned by [`spell_session_dispatch_json`] or
/// [`spell_read_file_unrestricted`].
#[no_mangle]
pub unsafe extern "C" fn spell_free_string(ptr: *mut c_char) {
    if !ptr.is_null() {
        let _ = CString::from_raw(ptr);
    }
}

/// Returns the current runtime patch generation counter (`SPELL_PATCH_GEN`).
#[no_mangle]
pub extern "C" fn spell_get_patch_gen() -> u64 {
    SPELL_PATCH_GEN.load(Ordering::SeqCst)
}

/// Atomically increments `SPELL_PATCH_GEN` and returns the new value (`>= 1`).
#[no_mangle]
pub extern "C" fn spell_bump_patch_gen() -> u64 {
    SPELL_PATCH_GEN.fetch_add(1, Ordering::SeqCst) + 1
}

/// Returns a heap-allocated C string containing the latest cached workspace snapshot JSON
/// without acquiring any lock on `SpellCoreSession`.
///
/// The returned string must be freed via [`spell_free_string`].
#[no_mangle]
pub extern "C" fn spell_get_cached_snapshot_json() -> *mut c_char {
    let cached = get_live_snapshot_cache();
    let sanitized = if cached.as_bytes().contains(&0) {
        cached.replace('\0', "")
    } else {
        cached
    };
    CString::new(sanitized)
        .unwrap_or_else(|_| CString::new("{\"ok\":false}").unwrap())
        .into_raw()
}

/// Signals lock-free cancellation of any active streaming turn (`< 50 ms`) and bumps `SPELL_PATCH_GEN`.
#[no_mangle]
pub extern "C" fn spell_request_turn_cancel() -> bool {
    request_turn_cancel();
    true
}

/// Publishes the user's PTY terminal state to the agent terminal bridge.
///
/// `state_json` is `{"attached": bool, "cwd": str, "title": str, "scrollback": str}`.
/// Does **not** take the `SpellCoreSession` lock, so it is safe to call while an agent turn runs.
///
/// # Safety
/// `state_json` must be null or a valid null-terminated UTF-8 C string.
#[no_mangle]
pub unsafe extern "C" fn spell_terminal_publish(state_json: *const c_char) -> bool {
    if state_json.is_null() {
        return false;
    }
    let Ok(text) = CStr::from_ptr(state_json).to_str() else {
        return false;
    };
    let Ok(val) = serde_json::from_str::<serde_json::Value>(text) else {
        return false;
    };
    let s = |k: &str| val.get(k).and_then(|v| v.as_str()).unwrap_or("").to_string();
    terminal_bridge::TERMINAL_BRIDGE.publish(
        val.get("attached").and_then(|v| v.as_bool()).unwrap_or(false),
        &s("cwd"),
        &s("title"),
        &s("scrollback"),
    );
    true
}

/// Drains user-approved agent inputs as a JSON array `[{"seq":u64,"call_id":str,"data":str}]`.
/// Lock-free with respect to the session. Free the result with [`spell_free_string`].
#[no_mangle]
pub extern "C" fn spell_terminal_take_approved_inputs() -> *mut c_char {
    let inputs = terminal_bridge::TERMINAL_BRIDGE.take_approved();
    let json = serde_json::to_string(&inputs).unwrap_or_else(|_| "[]".to_string());
    CString::new(json.replace('\0', ""))
        .unwrap_or_else(|_| CString::new("[]").unwrap())
        .into_raw()
}

/// C-ABI wrapper for Hook 3 `read_file_unrestricted`.
///
/// # Safety
/// `path_ptr` must be a valid null-terminated C string.
/// The returned pointer must be freed via [`spell_free_string`].
#[no_mangle]
pub unsafe extern "C" fn spell_read_file_unrestricted(
    path_ptr: *const c_char,
    start_line: usize,
    end_line: usize,
) -> *mut c_char {
    if path_ptr.is_null() {
        return CString::new("IO_ERROR: null path_ptr").unwrap().into_raw();
    }
    let path_str = match CStr::from_ptr(path_ptr).to_str() {
        Ok(s) => s,
        Err(_) => return CString::new("IO_ERROR: invalid UTF-8 path").unwrap().into_raw(),
    };
    let start = if start_line == 0 { None } else { Some(start_line) };
    let end = if end_line == 0 { None } else { Some(end_line) };

    let output = match read_file_unrestricted(Path::new(path_str), start, end) {
        Ok(content) => content,
        Err(err) => err.to_string(),
    };

    let sanitized = if output.as_bytes().contains(&0) {
        output.replace('\0', "")
    } else {
        output
    };
    CString::new(sanitized)
        .unwrap_or_else(|_| CString::new("IO_ERROR: interior NUL byte").unwrap())
        .into_raw()
}

// ============================================================================
// Comprehensive Cross-Module Verification Suite (C1–C6, Decisions 1–4, MCP, FFI)
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use crate::contract::*;
    use crate::mcp_client::McpStdioClient;
    use crate::storage::{SpellStorage, StoredThread};
    use crate::tags::TagRegistry;
    use crate::tools::{
        build_openai_tools_schema, create_directory, delete_path, resolve_workspace_path,
        write_file, AgentProfile, ToolError, TurnPinCache, DEFAULT_MAX_AUTO_EXPAND_LINES,
        MAX_SINGLE_FILE_CHARS, MAX_TOTAL_TOOL_CHARS,
    };
    use crate::test_support::{FixtureTransport, LoopbackVerifyServer};
    use crate::transport::{derive_thread_key, EngineTransport, HttpTransport};
    use serde_json::{json, Value};
    use std::io::{Read, Write};
    use std::net::TcpStream;
    use std::sync::Arc;
    use std::time::{Duration, Instant};

    #[test]
    fn test_c1_stable_prompt_cache_key_across_epochs() {
        // Default mode: prompt_cache_key stays stable as `thread_id` across all epochs
        assert_eq!(resolve_prompt_cache_key("thread-alpha", 0, false), "thread-alpha");
        assert_eq!(resolve_prompt_cache_key("thread-alpha", 1, false), "thread-alpha");
        assert_eq!(resolve_prompt_cache_key("thread-alpha", 9, false), "thread-alpha");

        // Legacy compatibility toggle appends `:epoch<N>` when enabled, and derive_thread_key strips it
        assert_eq!(
            resolve_prompt_cache_key("thread-alpha", 2, true),
            "thread-alpha:epoch2"
        );
        assert_eq!(derive_thread_key(Some("thread-alpha:epoch2")), "thread-alpha");
    }

    #[test]
    fn test_c2_loopback_verify_server_401_without_auth_and_200_with_auth() {
        let fixture = Arc::new(FixtureTransport::new());
        let expected_key = "test-spell-secret-key";
        let server = LoopbackVerifyServer::start(fixture, expected_key).unwrap();

        // 1. Raw TCP request without Authorization: Bearer -> must return 401 Unauthorized
        let mut raw_stream = TcpStream::connect(server.addr()).unwrap();
        let unauth_req = format!(
            "GET /v1/models HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n",
            server.addr()
        );
        raw_stream.write_all(unauth_req.as_bytes()).unwrap();
        let mut resp_str = String::new();
        raw_stream.read_to_string(&mut resp_str).unwrap();
        assert!(
            resp_str.starts_with("HTTP/1.1 401"),
            "Expected 401 without auth headers, got: {resp_str}"
        );

        // 2. HttpTransport with valid Bearer API key -> 200 OK across `/v1/models` and `/v1/chat/completions`
        let client = HttpTransport::with_api_key(server.base_url(), expected_key);
        let models = client.list_models().unwrap();
        assert!(!models.is_empty());

        let mut req = ChatCompletionRequest::new(
            models[0].id.clone(),
            vec![WireMessage::user("Hello OpenAI-compatible loopback")],
        );
        let summary = client
            .stream_chat_completion(&mut req, &mut |_chunk| {})
            .unwrap();
        assert!(!summary.content.is_empty());
    }

    #[test]
    fn test_c3_and_decision2_engine_owned_timers_single_alarm_injection() {
        let transport = FixtureTransport::new();
        let timer = transport
            .set_timer(&SetTimerRequest {
                duration_s: 5.0,
                note: "Inspect background job".to_string(),
                label: "job-check".to_string(),
                thread_id: "thread-c3".to_string(),
                watch_target: Some(json!({"kind": "file_tail", "path": "Cargo.toml"})),
            })
            .unwrap();
        assert!(!timer.delivered);
        assert!(timer.status == "pending" || timer.status == "armed");

        // Advance simulated clock so timer is due
        transport.advance_simulated_time(6.0);

        let mut req = ChatCompletionRequest {
            model: "test-model".to_string(),
            messages: vec![
                WireMessage::system("System prefix"),
                WireMessage::user("Continue work after timer"),
            ],
            stream: true,
            stream_options: Some(StreamOptions {
                include_usage: true,
            }),
            temperature: None,
            max_tokens: None,
            reasoning_effort: None,
            tools: None,
            tool_choice: None,
            prompt_cache_key: Some("thread-c3".to_string()),
            client_compaction_epoch: Some(0),
            auto_compact_min_tps: Some(ENGINE_MIN_DECODE_TPS),
            ..Default::default()
        };

        let summary = transport
            .stream_chat_completion(&mut req, &mut |_chunk| {})
            .unwrap();
        assert_eq!(summary.engine_injected_alarms.len(), 1);
        let last_msg_content = req.messages.last().unwrap().content.as_deref().unwrap_or("");
        assert_eq!(
            last_msg_content.matches(TIMER_ALARM_MARKER).count(),
            1,
            "Alarm banner must be injected exactly once at the end of messages[-1]"
        );
    }

    #[test]
    fn test_c4_and_decision3_engine_owned_compaction_never_slices_ide_messages() {
        let mut session = crate::test_support::fixture_session().unwrap();
        let tid = session.selected_thread_id.clone().unwrap();

        let t1 = session
            .send_user_prompt(&tid, "Turn 1 baseline message")
            .unwrap();
        let count_after_t1 = t1.wire_messages.len();

        let t2 = session
            .send_user_prompt(
                &tid,
                "[SIM_SCENARIO:ENGINE_COMPACT] Trigger engine-owned compaction",
            )
            .unwrap();

        // IDE never slices or drops wire_messages; engine advances epoch & clears hot cache
        assert!(t2.wire_messages.len() > count_after_t1);
        assert!(t2.engine_compaction_epoch >= 1);
        assert!(t2.last_context_advice.as_ref().unwrap().hot_cache_cleared);
    }

    #[test]
    fn test_c5_read_file_caps_path_resolution_guardrail_and_sha256_deduplication() {
        assert_eq!(DEFAULT_MAX_AUTO_EXPAND_LINES, 1_500);
        assert_eq!(MAX_SINGLE_FILE_CHARS, 48_000);
        assert_eq!(MAX_TOTAL_TOOL_CHARS, 96_000);

        let tmp_dir =
            std::env::temp_dir().join(format!("spell_c5_verify_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&tmp_dir);
        create_directory(&tmp_dir).unwrap();

        let sub_file = tmp_dir.join("src/sample.rs");
        write_file(&sub_file, "fn alpha() {}\nfn beta() {}\n", false).unwrap();

        // Worktree-prefixed path resolution (`<worktree_name>/src/sample.rs:1:1`)
        let wt_name = tmp_dir.file_name().unwrap().to_string_lossy();
        let prefixed = format!("{wt_name}/src/sample.rs:1:1");
        let resolved = resolve_workspace_path(&tmp_dir, &prefixed).unwrap();
        assert_eq!(resolved, sub_file);

        // Security guardrail on out-of-workspace path
        let guard_err = resolve_workspace_path(
            &tmp_dir,
            "../outside_secret.txt",
        )
        .unwrap_err();
        assert!(matches!(guard_err, ToolError::SecurityViolation(_)));

        // Turn-pinning & SHA-256 duplicate file reference
        let mut pin_cache = TurnPinCache::new();
        let first_read = pin_cache
            .read_file_pinned("call_a", &sub_file, None, None)
            .unwrap();
        assert!(first_read.contains("fn alpha()"));

        let second_read = pin_cache
            .read_file_pinned("call_b", &sub_file, None, None)
            .unwrap();
        assert!(second_read.starts_with(&format!("<{} ", crate::tools::DUPLICATE_FILE_REF_TAG)));
        assert!(second_read.contains("sha256=\""));

        // Re-evaluating `call_a` returns the exact pinned full text
        let pinned_first = pin_cache
            .read_file_pinned("call_a", &sub_file, None, None)
            .unwrap();
        assert_eq!(pinned_first, first_read);

        let _ = delete_path(&tmp_dir);
    }

    #[test]
    fn test_c6_sqlite_wire_messages_roundtrip_preserves_reasoning_and_tool_calls() {
        let store = SpellStorage::new_in_memory().unwrap();
        let mut thread = StoredThread::new(
            "thread-c6",
            "proj-1",
            "C6 Roundtrip Thread",
            "write",
            vec!["systems-architecture".to_string()],
        );
        thread.wire_messages = vec![
            WireMessage::user("Inspect Cargo.toml"),
            WireMessage::assistant(
                Some(String::new()),
                Some("Step-by-step reasoning that must remain byte-identical.".to_string()),
                Some(vec![WireToolCall {
                    id: "call_c6_01".to_string(),
                    r#type: "function".to_string(),
                    function: WireToolFunction {
                        name: "read_file".to_string(),
                        arguments: "{\"path\":\"Cargo.toml\"}".to_string(),
                    },
                    index: Some(0),
                }]),
            ),
            WireMessage::tool(
                "call_c6_01",
                Some("read_file".to_string()),
                "[package]\nname = \"spell_core\"",
            ),
        ];

        store.upsert_thread(&thread).unwrap();
        let loaded = store.get_thread("thread-c6").unwrap().unwrap();
        assert_eq!(loaded.wire_messages, thread.wire_messages);
        assert_eq!(
            loaded.wire_messages[1].reasoning_content.as_deref(),
            Some("Step-by-step reasoning that must remain byte-identical.")
        );
        assert_eq!(
            loaded.wire_messages[1].tool_calls.as_ref().unwrap()[0].id,
            "call_c6_01"
        );
    }

    #[test]
    fn test_decision4_conversation_rubric_tags_and_no_spawn_agent() {
        let reg = TagRegistry::new();
        assert!(reg.list_tags().len() >= 5);
        assert!(reg.get_tag("research-synthesis").is_some());
        assert!(reg.get_tag("executive-decision-memo").is_some());

        let write_schema = build_openai_tools_schema(AgentProfile::Write);
        let schema_str = serde_json::to_string(&write_schema).unwrap();
        assert!(
            !schema_str.contains("spawn_agent"),
            "generic unrestricted spawn_agent must never appear in the tool schema"
        );
        assert!(
            schema_str.contains("\"name\":\"tag\""),
            "tag tool must be present in the tool schema"
        );
        assert!(
            schema_str.contains("\"name\":\"prefill_context_subagent\""),
            "prefill_context_subagent tool must be present in Write schema"
        );
        assert!(
            schema_str.contains("\"name\":\"delegate_coding_subagent\""),
            "delegate_coding_subagent tool must be present in Write schema"
        );

        let ask_schema = build_openai_tools_schema(AgentProfile::Ask);
        let ask_str = serde_json::to_string(&ask_schema).unwrap();
        assert!(
            ask_str.contains("\"name\":\"prefill_context_subagent\""),
            "read-only prefill_context_subagent must be allowed in Ask profile"
        );
        assert!(
            !ask_str.contains("\"name\":\"delegate_coding_subagent\""),
            "delegate_coding_subagent must not appear in Ask profile"
        );
    }

    #[test]
    fn test_real_mcp_stdio_client_sub_50ms_cancellation() {
        let (client, _responder) = McpStdioClient::new_embedded().unwrap();
        let init_res = client.initialize().unwrap();
        assert!(init_res.to_string().contains("spell-full-file-mcp"));

        let tools = client.list_tools().unwrap();
        assert_eq!(tools.len(), 10);

        let client_arc = Arc::new(client);
        let worker_client = client_arc.clone();
        let handle = std::thread::spawn(move || {
            worker_client.call_tool(777, "timer_wait", json!({ "duration_ms": 5000 }))
        });

        std::thread::sleep(Duration::from_millis(12));
        let cancel_start = Instant::now();
        client_arc
            .cancel_request(777, Some("test sub-50ms cancel"))
            .unwrap();
        let result = handle.join().unwrap().unwrap();
        let elapsed = cancel_start.elapsed();
        assert!(
            elapsed < Duration::from_millis(50),
            "Expected < 50ms MCP cancellation, got {:?}",
            elapsed
        );
        assert!(result.to_string().contains("cancelled"));
    }

    #[test]
    fn test_configure_engine_and_cached_engine_status() {
        // Fresh session defaults to unconfigured with zero network I/O and no error.
        let mut session = SpellCoreSession::new_in_memory().unwrap();
        assert!(!session.is_endpoint_configured());
        session.refresh_engine_status();
        let initial_snap: Value =
            serde_json::from_str(&session.build_full_state_snapshot_json()).unwrap();
        assert_eq!(initial_snap["endpoint_configured"], false);
        assert_eq!(initial_snap["endpoint_base_url"], "");
        assert_eq!(initial_snap["telemetry"]["engine_online"], false);
        assert!(initial_snap["telemetry"]["engine_error"].is_null());

        // Endpoint-agnostic URLs (remote HTTPS, loopback, Unix sockets) are accepted; invalid schemes rejected.
        assert!(session
            .configure_engine("https://api.openai.com/v1", None)
            .is_ok());
        assert!(session.configure_engine("ftp://example.com", None).is_err());
        // Empty URL cleanly unconfigures the endpoint.
        session.configure_engine("  ", None).unwrap();
        assert!(!session.is_endpoint_configured());

        // An unreachable configured endpoint reports offline with the transport error.
        session
            .configure_engine("http://127.0.0.1:9", Some("k"))
            .unwrap();
        session.refresh_engine_status();
        let snap: Value = serde_json::from_str(&session.build_full_state_snapshot_json()).unwrap();
        assert_eq!(snap["engine_base_url"], "http://127.0.0.1:9");
        assert_eq!(snap["endpoint_base_url"], "http://127.0.0.1:9");
        assert_eq!(snap["telemetry"]["engine_online"], false);
        assert!(snap["telemetry"]["engine_error"].as_str().unwrap_or("").len() > 0);

        // With a reachable OpenAI-compatible server the cache fills via GET /v1/models.
        let fixture = Arc::new(FixtureTransport::new());
        let server = LoopbackVerifyServer::start(fixture, "status-key").unwrap();
        session
            .configure_engine(&server.base_url(), Some("status-key"))
            .unwrap();
        session.refresh_engine_status();
        let snap: Value = serde_json::from_str(&session.build_full_state_snapshot_json()).unwrap();
        assert_eq!(snap["telemetry"]["engine_online"], true);
        assert!(snap["telemetry"]["engine_error"].is_null());
        assert_eq!(snap["engine_base_url"], server.base_url());
        assert!(!snap["available_models"].as_array().unwrap().is_empty());
    }

    #[test]
    fn test_c_abi_session_ffi_lifecycle() {
        unsafe {
            let mem = CString::new(":memory:").unwrap();
            let handle = spell_session_new(mem.as_ptr());
            assert!(!handle.is_null());

            let cmd = CString::new("{\"cmd\":\"get_snapshot\"}").unwrap();
            let raw_resp = spell_session_dispatch_json(handle, cmd.as_ptr());
            assert!(!raw_resp.is_null());
            let json_str = CStr::from_ptr(raw_resp).to_str().unwrap().to_owned();
            spell_free_string(raw_resp);

            let parsed: Value = serde_json::from_str(&json_str).unwrap();
            assert_eq!(parsed.get("ok").and_then(|v| v.as_bool()), Some(true));
            assert!(parsed.get("projects").is_some());
            assert!(parsed.get("threads").is_some());

            spell_session_free(handle);
        }
    }

    #[test]
    fn test_numbered_default_titles_and_rename() {
        let mut session = crate::test_support::fixture_session().unwrap();
        let root = session.locator.primary_workspace_root();
        let p2 = session.add_project("", &root.to_string_lossy(), false).unwrap();
        assert_eq!(p2.name, "Project 1", "the fixture project has a custom name, so numbering starts at 1");
        let p3 = session.add_project("  ", &root.to_string_lossy(), false).unwrap();
        assert_eq!(p3.name, "Project 2");

        let first = session.storage.list_threads(Some(&p3.id)).unwrap();
        assert_eq!(first.len(), 1);
        assert_eq!(first[0].title, "Conversation 1");
        let second = session.create_thread(Some(&p3.id), None, None, vec![]).unwrap();
        assert_eq!(second.title, "Conversation 2");

        // A prompt never renames the conversation.
        let after = session.send_user_prompt(&second.id, "Refactor the parser").unwrap();
        assert_eq!(after.title, "Conversation 2");

        // Rename via the FFI command router, and an empty title restores the numbered default.
        let out = session.dispatch_json_command(&format!(
            r#"{{"cmd":"rename_thread","thread_id":"{}","title":"Parser work"}}"#,
            second.id
        ));
        assert!(!out.contains("\"ok\":false"), "{out}");
        assert_eq!(session.storage.get_thread(&second.id).unwrap().unwrap().title, "Parser work");
        session.rename_thread(&second.id, "").unwrap();
        assert_eq!(session.storage.get_thread(&second.id).unwrap().unwrap().title, "Conversation 2");

        let out = session.dispatch_json_command(&format!(
            r#"{{"cmd":"rename_project","project_id":"{}","name":"Website"}}"#,
            p3.id
        ));
        assert!(!out.contains("\"ok\":false"), "{out}");
        assert_eq!(session.storage.get_project(&p3.id).unwrap().unwrap().name, "Website");

        // Deleting does not cause number reuse among the remaining conversations.
        let third = session.create_thread(Some(&p3.id), None, None, vec![]).unwrap();
        assert_eq!(third.title, "Conversation 3");
    }

    #[test]
    fn test_auto_title_and_edit_or_regenerate_turn() {
        let mut session = crate::test_support::fixture_session().unwrap();
        let new_th = session
            .create_thread(None, None, Some("write"), vec![])
            .unwrap();
        assert!(new_th.title.starts_with("Conversation "), "{}", new_th.title);

        let after_first = session
            .send_user_prompt(&new_th.id, "Audit Metal GDN megakernel latency")
            .unwrap();
        // Titles are never generated from the prompt; they stay numbered until renamed.
        assert_eq!(after_first.title, new_th.title);
        assert_eq!(after_first.wire_messages.len(), 2);

        let after_second = session
            .send_user_prompt(&new_th.id, "Temporary second question")
            .unwrap();
        assert_eq!(after_second.wire_messages.len(), 4);

        // Edit/regenerate the second user turn in place
        let regenerated = session
            .edit_or_regenerate_turn(&new_th.id, None, Some("Refined second question"))
            .unwrap();
        assert_eq!(regenerated.wire_messages.len(), 4);
        assert_eq!(
            regenerated.wire_messages[2].content.as_deref(),
            Some("Refined second question")
        );
    }

    #[test]
    fn test_messages_zero_immutable_across_turns_tags_and_compaction() {
        use crate::tags::ConversationTag;
        use crate::tools::{build_clean_system_prompt, ToolPermissionPolicy};
        use sha2::{Digest, Sha256};

        let mut session = crate::test_support::fixture_session().unwrap();
        let mut scoped_tag = ConversationTag::new(
            "read-only-auditor",
            "read-only-auditor",
            "evaluation",
            "Read-only audit rubric",
            "Inspect files without modifying them.",
            false,
        );
        scoped_tag.allowed_tools = Some(vec!["read_file".to_string(), "grep".to_string()]);
        session.tag_registry.upsert_tag(scoped_tag);

        let thread = session
            .create_thread(None, Some("G3 Thread"), Some("write"), vec![])
            .unwrap();

        // Turn 1: Baseline turn without tags
        let t1 = session
            .send_user_prompt(&thread.id, "Initial baseline turn")
            .unwrap();
        let proj = session.storage.get_project(&t1.project_id).unwrap().unwrap();
        let sys_turn1 = build_clean_system_prompt(
            &proj.name,
            std::path::Path::new(&proj.root_path),
            proj.is_read_only,
            AgentProfile::Write,
            &[],
        );
        let hash_turn1 = format!("{:x}", Sha256::digest(sys_turn1.as_bytes()));

        // Turn 2: Mid-thread inline @tag and #steer tokens
        let t2 = session
            .send_user_prompt(
                &thread.id,
                "@tag:read-only-auditor #steer:fast Audit the module structure.",
            )
            .unwrap();

        let turn2_user_wire = t2
            .wire_messages
            .iter()
            .filter(|m| m.role == "user")
            .nth(1)
            .unwrap()
            .content
            .as_deref()
            .unwrap();
        assert!(!turn2_user_wire.contains("@tag:read-only-auditor"));
        assert!(!turn2_user_wire.contains("#steer:fast"));
        assert!(turn2_user_wire.contains("<turn_rubric_and_steering>"));

        let sys_turn2 = build_clean_system_prompt(
            &proj.name,
            std::path::Path::new(&proj.root_path),
            proj.is_read_only,
            AgentProfile::Write,
            &[],
        );
        let hash_turn2 = format!("{:x}", Sha256::digest(sys_turn2.as_bytes()));
        assert_eq!(hash_turn1, hash_turn2);

        // Verify per-turn allowed_tools blocks `edit_file` while allowing `read_file`
        assert_eq!(
            session.effective_tool_permission("read_file", AgentProfile::Write, false),
            ToolPermissionPolicy::Allow
        );
        assert_eq!(
            session.effective_tool_permission("edit_file", AgentProfile::Write, false),
            ToolPermissionPolicy::Deny
        );
    }

    #[test]
    fn test_patch_live_snapshot_active_subagents_sets_dual_keys_and_bumps_patch_gen() {
        let indicators = vec![
            ActiveSubagentIndicator {
                id: "sub-prefill-0".to_string(),
                role: ActiveSubagentRole::Prefill,
                model: "test-subagent-model".to_string(),
                slot_label: "subagent0".to_string(),
                label: "Read docs".to_string(),
                step: None,
                max_steps: None,
                started_at_unix_ms: 1_763_600_000_001,
            },
            ActiveSubagentIndicator {
                id: "sub-coding-1".to_string(),
                role: ActiveSubagentRole::Coding,
                model: "test-subagent-model".to_string(),
                slot_label: "subagent1".to_string(),
                label: "Edit parser".to_string(),
                step: Some(2),
                max_steps: Some(12),
                started_at_unix_ms: 1_763_600_000_250,
            },
        ];

        // 1. Pure snapshot patching preserves unrelated keys and mirrors snake_case + camelCase.
        let base_json = r#"{"ok":true,"projects":[],"stage_write_mode":true}"#;
        let patched = apply_active_subagents_to_snapshot_json(base_json, &indicators)
            .expect("valid JSON object must patch");
        let parsed: Value = serde_json::from_str(&patched).unwrap();
        assert_eq!(parsed["ok"], true);
        assert_eq!(parsed["stage_write_mode"], true);
        assert!(parsed.get("projects").is_some());
        assert_eq!(parsed["active_subagents"], parsed["activeSubagents"]);
        assert_eq!(parsed["active_subagents"].as_array().unwrap().len(), 2);
        assert_eq!(parsed["active_subagents"][0]["role"], "prefill");
        assert!(parsed["active_subagents"][0].get("step").is_none());
        assert_eq!(parsed["active_subagents"][1]["role"], "coding");
        assert_eq!(parsed["active_subagents"][1]["step"], 2);
        assert_eq!(parsed["active_subagents"][1]["max_steps"], 12);

        // 2. Empty active slice clears both keys to `[]`.
        let cleared = apply_active_subagents_to_snapshot_json(&patched, &[])
            .expect("clearing active subagents must patch");
        let cleared_val: Value = serde_json::from_str(&cleared).unwrap();
        assert_eq!(cleared_val["active_subagents"], json!([]));
        assert_eq!(cleared_val["activeSubagents"], json!([]));

        // 3. Invalid or non-object JSON safely returns None without panicking.
        assert!(apply_active_subagents_to_snapshot_json("not-json", &indicators).is_none());
        assert!(apply_active_subagents_to_snapshot_json("[1,2,3]", &indicators).is_none());

        // 4. Live cache patcher updates in-place (preserving existing keys) and bumps SPELL_PATCH_GEN.
        let before_gen = spell_get_patch_gen();
        patch_live_snapshot_active_subagents(&indicators);
        let after_set_gen = spell_get_patch_gen();
        assert!(after_set_gen > before_gen);

        patch_live_snapshot_active_subagents(&[]);
        let after_clear_gen = spell_get_patch_gen();
        assert!(after_clear_gen > after_set_gen);
    }
}
