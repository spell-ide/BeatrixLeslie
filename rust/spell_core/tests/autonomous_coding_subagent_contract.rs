//! Exhaustive Offline HTTP/1.1 + SSE Integration Contract Suite for
//! `delegate_coding_subagent` over real `HttpTransport` and ephemeral `LoopbackVerifyServer`
//! (`127.0.0.1:0`).

#[cfg(not(feature = "test-support"))]
pub use spell_core::{agent_loop, contract, is_turn_cancel_requested, locator, transport};
#[cfg(not(feature = "test-support"))]
#[path = "../src/test_support.rs"]
mod test_support;
#[cfg(feature = "test-support")]
use spell_core::test_support;

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use serde_json::{json, Value};

use spell_core::agent_loop::{SpellCoreSession, LoopState};
use spell_core::contract::{WireToolCall, WireToolFunction};
use spell_core::subagent_coding::{parse_delegation_result_meta, SubagentProtocol};
use spell_core::tools::resolve_workspace_path;

use test_support::{
    FixtureTransport, LoopbackVerifyServer, RecordedSubagentCall, ScriptedSubagentStep,
    FIXTURE_MAIN_MODEL_ID, FIXTURE_SUBAGENT_MODEL_ID,
};

static WORKSPACE_SEQ: AtomicU64 = AtomicU64::new(1);
const CONTRACT_BEARER_KEY: &str = "offline-contract-bearer-key";

/// RAII temporary workspace directory that cleans up on drop without touching any real repo.
struct TempWorkspace {
    root: PathBuf,
}

impl TempWorkspace {
    fn new(label: &str) -> Self {
        let seq = WORKSPACE_SEQ.fetch_add(1, Ordering::SeqCst);
        let pid = std::process::id();
        let dir = std::env::temp_dir().join(format!("spell_contract_{pid}_{seq}_{label}"));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("src")).expect("create temp workspace src/");
        Self { root: dir }
    }

    fn path(&self) -> &Path {
        &self.root
    }

    fn write_rel(&self, rel: &str, content: &str) -> PathBuf {
        let target = self.root.join(rel);
        if let Some(parent) = target.parent() {
            fs::create_dir_all(parent).expect("create parent dir");
        }
        fs::write(&target, content).expect("write temp workspace file");
        target
    }

    fn read_rel(&self, rel: &str) -> String {
        fs::read_to_string(self.root.join(rel)).expect("read temp workspace file")
    }
}

impl Drop for TempWorkspace {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

/// Boots an isolated `SpellCoreSession` wired over real `HttpTransport` to an ephemeral
/// `LoopbackVerifyServer` (`127.0.0.1:0`).
fn boot_http_loopback_session(
    ws: &TempWorkspace,
    db_path: Option<&str>,
) -> (
    SpellCoreSession,
    Arc<FixtureTransport>,
    LoopbackVerifyServer,
    String,
) {
    let fixture = Arc::new(FixtureTransport::new());
    let server = LoopbackVerifyServer::start(fixture.clone(), CONTRACT_BEARER_KEY)
        .expect("start ephemeral LoopbackVerifyServer");
    assert_ne!(
        server.addr().port(),
        8000,
        "Ephemeral LoopbackVerifyServer must never bind live port 8000"
    );

    let mut session =
        SpellCoreSession::new(db_path).expect("initialize SpellCoreSession over SQLite");
    session
        .configure_endpoint(
            Some(&server.base_url()),
            Some(Some(server.expected_api_key())),
            Some(FIXTURE_MAIN_MODEL_ID),
            Some(FIXTURE_SUBAGENT_MODEL_ID),
        )
        .expect("configure_endpoint to ephemeral loopback server");
    assert!(
        session
            .active_transport()
            .transport_name()
            .starts_with("HttpTransport"),
        "Session must exercise real HttpTransport, got: {}",
        session.active_transport().transport_name()
    );

    let _proj = session
        .add_project(
            "ContractWorkspace",
            ws.path().to_str().expect("utf8 workspace path"),
            false,
        )
        .expect("add_project");
    let tid = session
        .selected_thread_id
        .clone()
        .expect("selected_thread_id");

    (session, fixture, server, tid)
}

fn make_tc(id: &str, name: &str, args: Value) -> WireToolCall {
    WireToolCall {
        id: id.to_string(),
        r#type: "function".to_string(),
        function: WireToolFunction {
            name: name.to_string(),
            arguments: args.to_string(),
        },
        index: Some(0),
    }
}

fn make_raw_tc(id: &str, name: &str, raw_arguments: &str) -> WireToolCall {
    WireToolCall {
        id: id.to_string(),
        r#type: "function".to_string(),
        function: WireToolFunction {
            name: name.to_string(),
            arguments: raw_arguments.to_string(),
        },
        index: Some(0),
    }
}

fn delegate_calls_only(all: &[RecordedSubagentCall]) -> Vec<&RecordedSubagentCall> {
    all.iter()
        .filter(|c| {
            c.prompt_cache_key
                .as_deref()
                .map(|k| k.contains(":delegate:"))
                .unwrap_or(false)
        })
        .collect()
}

// ============================================================================
// 1. Native Tool-Call Streaming SSE Multi-Step Overlay Reads, Commit, Journal & Undo
// ============================================================================

#[test]
fn test_e2e_native_protocol_streaming_sse_multistep_overlay_reads_commit_journal_and_undo() {
    let ws = TempWorkspace::new("native_multistep_undo");
    let orig_lib = concat!(
        "//! Core arithmetic library.\n",
        "pub fn compute_total(base: usize, factor: usize) -> usize {\n",
        "    let scaled = base.saturating_mul(factor);\n",
        "    scaled + 1\n",
        "}\n",
    );
    ws.write_rel("src/lib.rs", orig_lib);

    let (mut session, fixture, _server, tid) = boot_http_loopback_session(&ws, Some(":memory:"));

    let new_helper_text = concat!(
        "// UTF-8 SSE boundary check • αβγ ✓\n",
        "pub const OVERLAY_UNCOMMITTED_HELPER_TAG: &str = \"SHARED_OVERLAY_PROBE_77\";\n",
        "pub fn helper_bonus() -> usize {\n",
        "    42\n",
        "}\n",
    );

    // Queue 9 native tool-call steps (each split across 2 SSE delta.tool_calls chunks by LoopbackVerifyServer):
    // 0: find_path
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_0_find",
        "find_path",
        json!({"pattern": "*.rs", "path": "src"}),
    )]);
    // 1: read_file with expand_to_symbol: true (line 3 inside compute_total expands to lines 2..=5)
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_1_read",
        "read_file",
        json!({
            "path": "src/lib.rs",
            "start_line": 3,
            "end_line": 3,
            "expand_to_symbol": true
        }),
    )]);
    // 2: outline_symbols
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_2_outline",
        "outline_symbols",
        json!({"path": "src/lib.rs"}),
    )]);
    // 3: get_symbol_body
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_3_symbody",
        "get_symbol_body",
        json!({"path": "src/lib.rs", "symbol": "compute_total"}),
    )]);
    // 4: create_directory("src/new_mod")
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_4_mkdir",
        "create_directory",
        json!({"path": "src/new_mod"}),
    )]);
    // 5: write_file("src/new_mod/helper.rs")
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_5_write",
        "write_file",
        json!({
            "path": "src/new_mod/helper.rs",
            "content": new_helper_text
        }),
    )]);
    // 6: edit_file("src/lib.rs")
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_6_edit",
        "edit_file",
        json!({
            "path": "src/lib.rs",
            "old_text": "    scaled + 1",
            "new_text": "    // SHARED_OVERLAY_PROBE_77 • αβγ\n    scaled + new_mod::helper::helper_bonus()"
        }),
    )]);
    // 7: grep across "src" BEFORE commit -> must see BOTH uncommitted helper.rs AND modified lib.rs!
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_7_grep",
        "grep",
        json!({"pattern": "SHARED_OVERLAY_PROBE_77", "path": "src"}),
    )]);
    // 8: finish
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_8_finish",
        "finish",
        json!({
            "summary": "Created src/new_mod/helper.rs and wired compute_total in src/lib.rs",
            "verification_notes": "Verified overlay grep across both files and Gatekeeper Critic PASS."
        }),
    )]);

    let (output, status) = session
        .execute_delegate_coding_subagent_direct(
            &tid,
            "call-opt-a-1",
            json!({
                "instruction": "Create src/new_mod/helper.rs and wire helper_bonus() into compute_total in src/lib.rs."
            }),
        )
        .expect("execute_delegate_coding_subagent_direct");
    assert_eq!(status, "completed");

    // Verify all 9 native tool-call subagent wire calls streamed SSE (`stream: true`, `tool_choice: "auto"`)
    let all_calls = fixture.recorded_subagent_calls();
    let del_calls = delegate_calls_only(&all_calls);
    assert_eq!(del_calls.len(), 9, "Expected 9 native tool-call steps");
    for (idx, call) in del_calls.iter().enumerate() {
        assert_eq!(call.model, FIXTURE_SUBAGENT_MODEL_ID, "step {idx} model");
        assert!(
            call.stream,
            "step {idx} must use stream=true in native tool-call protocol"
        );
        assert_eq!(
            call.tool_choice,
            Some(json!("auto")),
            "step {idx} must send tool_choice=\"auto\" in native tool-call protocol"
        );
    }

    // Inspect Step 8 (`finish`) wire history to verify in-loop tool results BEFORE disk commit:
    let step8_msgs = &del_calls[8].messages;
    let read_expanded_out = step8_msgs
        .iter()
        .find(|m| m.tool_call_id.as_deref() == Some("tc_1_read"))
        .and_then(|m| m.content.as_deref())
        .expect("tc_1_read output");
    assert!(
        read_expanded_out.contains("pub fn compute_total") && read_expanded_out.contains("scaled + 1"),
        "expand_to_symbol: true must expand line 3 to full compute_total symbol, got: {read_expanded_out}"
    );
    let outline_out = step8_msgs
        .iter()
        .find(|m| m.tool_call_id.as_deref() == Some("tc_2_outline"))
        .and_then(|m| m.content.as_deref())
        .expect("tc_2_outline output");
    assert!(
        outline_out.contains("compute_total"),
        "outline_symbols must include compute_total: {outline_out}"
    );
    let symbody_out = step8_msgs
        .iter()
        .find(|m| m.tool_call_id.as_deref() == Some("tc_3_symbody"))
        .and_then(|m| m.content.as_deref())
        .expect("tc_3_symbody output");
    assert!(
        symbody_out.contains("saturating_mul"),
        "get_symbol_body must return compute_total body: {symbody_out}"
    );
    let grep_out = step8_msgs
        .iter()
        .find(|m| m.tool_call_id.as_deref() == Some("tc_7_grep"))
        .and_then(|m| m.content.as_deref())
        .expect("tc_7_grep output");
    assert!(
        grep_out.contains("src/new_mod/helper.rs") && grep_out.contains("src/lib.rs"),
        "In-loop overlay grep BEFORE disk commit must see BOTH uncommitted helper.rs and modified lib.rs, got:\n{grep_out}"
    );

    // Prove physical disk remained untouched throughout all 9 loop steps right up to atomic commit:
    // `delegation_undo_log[0]` snapshots physical disk (`fs::read_to_string` & `!dir.exists()`) immediately prior to commit.
    assert_eq!(session.delegation_undo_log.len(), 1);
    let undo_rec = &session.delegation_undo_log[0];
    let helper_restore = undo_rec
        .files
        .iter()
        .find(|f| f.relative_path == "src/new_mod/helper.rs")
        .expect("undo entry for helper.rs");
    assert_eq!(
        helper_restore.prior_disk_content, None,
        "src/new_mod/helper.rs must NOT exist on physical disk prior to finish commit"
    );
    let lib_restore = undo_rec
        .files
        .iter()
        .find(|f| f.relative_path == "src/lib.rs")
        .expect("undo entry for lib.rs");
    assert_eq!(
        lib_restore.prior_disk_content.as_deref(),
        Some(orig_lib),
        "src/lib.rs on physical disk must remain 100% untouched mid-loop prior to finish commit"
    );
    assert!(
        undo_rec
            .created_dirs
            .iter()
            .any(|d| d.ends_with("src/new_mod")),
        "src/new_mod must not exist on disk prior to finish commit"
    );

    // Verify atomic commit on disk after `finish`
    assert_eq!(ws.read_rel("src/new_mod/helper.rs"), new_helper_text);
    let committed_lib = ws.read_rel("src/lib.rs");
    assert!(committed_lib.contains("new_mod::helper::helper_bonus()"));
    assert!(!committed_lib.contains("scaled + 1"));

    // Verify DelegationResultMeta
    let meta = parse_delegation_result_meta(&output).expect("parse DelegationResultMeta");
    assert_eq!(meta.status, "committed");
    assert_eq!(meta.protocol_used, "native");
    assert!(!meta.fallback_triggered);
    assert_eq!(meta.undo_id.as_deref(), Some("del-1"));
    assert_eq!(meta.steps, 9);
    assert_eq!(meta.files.len(), 2);
    assert!(meta.files.iter().all(|f| f.critic_passed && f.bounces == 0));

    // Verify `get_mutation_journal` recorded `"delegate_coding_subagent"`
    let journal_before_raw =
        session.dispatch_json_command(&json!({"cmd": "get_mutation_journal"}).to_string());
    let journal_before: Value = serde_json::from_str(&journal_before_raw).expect("journal json");
    assert_eq!(journal_before["ok"].as_bool(), Some(true));
    let entries_before = journal_before["entries"].as_array().expect("entries array");
    assert!(
        entries_before
            .iter()
            .any(|e| e["tool"].as_str() == Some("delegate_coding_subagent")),
        "Expected delegate_coding_subagent in mutation journal: {journal_before}"
    );

    // Dispatch one-click transactional Undo (`{"cmd": "undo_delegation", "undo_id": "del-1"}`)
    let undo_snap_raw = session.dispatch_json_command(
        &json!({
            "cmd": "undo_delegation",
            "undo_id": "del-1"
        })
        .to_string(),
    );
    let undo_snap: Value = serde_json::from_str(&undo_snap_raw).expect("undo snapshot json");
    assert_eq!(undo_snap["ok"].as_bool(), Some(true), "undo failed: {undo_snap}");

    // Verify byte-for-byte restore of `src/lib.rs`, deletion of `src/new_mod/helper.rs`, and pruning of empty `src/new_mod`
    assert_eq!(ws.read_rel("src/lib.rs").as_bytes(), orig_lib.as_bytes());
    assert!(
        !ws.path().join("src/new_mod/helper.rs").exists(),
        "Undo must delete newly created src/new_mod/helper.rs"
    );
    assert!(
        !ws.path().join("src/new_mod").exists(),
        "Undo must prune empty newly created src/new_mod directory"
    );

    // Verify snapshot `undone_delegation_ids` contains `"del-1"`
    let undone_ids = undo_snap["undone_delegation_ids"]
        .as_array()
        .expect("undone_delegation_ids array");
    assert!(
        undone_ids.iter().any(|v| v.as_str() == Some("del-1")),
        "Snapshot undone_delegation_ids must contain del-1: {undo_snap}"
    );

    // Verify `"undo_delegation"` recorded in `get_mutation_journal`
    let journal_after_raw =
        session.dispatch_json_command(&json!({"cmd": "get_mutation_journal"}).to_string());
    let journal_after: Value = serde_json::from_str(&journal_after_raw).expect("journal after json");
    let entries_after = journal_after["entries"].as_array().expect("entries array");
    assert!(
        entries_after
            .iter()
            .any(|e| e["tool"].as_str() == Some("undo_delegation")
                && e["call_id"].as_str() == Some("del-1")),
        "Expected undo_delegation in mutation journal: {journal_after}"
    );
}

// ============================================================================
// 2. JSON Action Protocol Setting & SQLite Persistence
// ============================================================================

#[test]
fn test_e2e_json_action_protocol_setting_and_persistence() {
    let ws = TempWorkspace::new("json_protocol_persistence");
    let orig_lib = "pub const RETRY_LIMIT: usize = 3;\n";
    ws.write_rel("src/lib.rs", orig_lib);
    let sqlite_path = ws.path().join("persisted_session.sqlite3");
    let sqlite_str = sqlite_path.to_str().expect("utf8 sqlite path");

    // Step 1: Set `subagent_protocol` = `"json"` via JSON command and verify snapshot
    let (mut session1, fixture, server, tid1) = boot_http_loopback_session(&ws, Some(sqlite_str));
    let set_snap_raw = session1.dispatch_json_command(
        &json!({
            "cmd": "set_subagent_protocol",
            "protocol": "json"
        })
        .to_string(),
    );
    let set_snap: Value = serde_json::from_str(&set_snap_raw).expect("set_subagent_protocol json");
    assert_eq!(set_snap["ok"].as_bool(), Some(true));
    assert_eq!(set_snap["subagent_protocol"].as_str(), Some("json"));
    assert_eq!(set_snap["subagentProtocol"].as_str(), Some("json"));
    drop(session1);

    // Step 2: Re-open SpellCoreSession from the same SQLite DB and verify persistence
    let mut session = SpellCoreSession::new(Some(sqlite_str)).expect("reopen persisted session");
    // Persisted endpoint URL and model IDs survive restart; only the in-memory API key is supplied.
    assert_eq!(session.model_id, FIXTURE_MAIN_MODEL_ID);
    assert_eq!(session.subagent_model_id, FIXTURE_SUBAGENT_MODEL_ID);
    session
        .configure_endpoint(None, Some(Some(server.expected_api_key())), None, None)
        .expect("set in-memory api_key on reopened session");
    assert_eq!(session.subagent_protocol, SubagentProtocol::Json);
    let reopened_snap: Value =
        serde_json::from_str(&session.build_full_state_snapshot_json()).expect("reopened snapshot");
    assert_eq!(reopened_snap["subagent_protocol"].as_str(), Some("json"));

    // Prime main prefix over HTTP SSE so subagent inherits `# Tools` schema (`tool_choice: "none"` in JSON action protocol)
    let prime_raw = session.dispatch_json_command(
        &json!({
            "cmd": "send_prompt",
            "thread_id": tid1,
            "prompt": "Confirm workspace readiness in one sentence."
        })
        .to_string(),
    );
    let prime_snap: Value = serde_json::from_str(&prime_raw).expect("prime snapshot");
    assert_eq!(prime_snap["ok"].as_bool(), Some(true));

    // Queue JSON action protocol steps: <think>plan</think> + fenced ```json ... ``` -> edit_file -> finish
    fixture.push_subagent_step(ScriptedSubagentStep {
        content: concat!(
            "<think>plan: read src/lib.rs first before updating RETRY_LIMIT</think>\n",
            "```json\n",
            "{\"action\": \"read_file\", \"path\": \"src/lib.rs\", \"start_line\": 1, \"end_line\": 10}\n",
            "```"
        )
        .to_string(),
        reasoning_content: String::new(),
        tool_calls: Vec::new(),
        finish_reason: Some("stop".to_string()),
        prefix_cache_reused: Some(true),
    });
    fixture.push_subagent_step(ScriptedSubagentStep {
        content: json!({
            "action": "edit_file",
            "path": "src/lib.rs",
            "old_text": "pub const RETRY_LIMIT: usize = 3;",
            "new_text": "pub const RETRY_LIMIT: usize = 8;"
        })
        .to_string(),
        reasoning_content: String::new(),
        tool_calls: Vec::new(),
        finish_reason: Some("stop".to_string()),
        prefix_cache_reused: Some(true),
    });
    fixture.push_subagent_step(ScriptedSubagentStep {
        content: json!({
            "action": "finish",
            "summary": "Done via JSON action protocol"
        })
        .to_string(),
        reasoning_content: String::new(),
        tool_calls: Vec::new(),
        finish_reason: Some("stop".to_string()),
        prefix_cache_reused: Some(true),
    });

    let (output, status) = session
        .execute_delegate_coding_subagent_direct(
            &tid1,
            "call-json-proto-1",
            json!({
                "instruction": "Bump RETRY_LIMIT from 3 to 8 in src/lib.rs"
            }),
        )
        .expect("JSON action protocol delegation");
    assert_eq!(status, "completed");

    // Verify wire calls had `stream == false` and `tool_choice == Some("none")`
    let all_calls = fixture.recorded_subagent_calls();
    let del_calls = delegate_calls_only(&all_calls);
    assert_eq!(del_calls.len(), 3);
    for (idx, call) in del_calls.iter().enumerate() {
        assert!(!call.stream, "JSON action protocol step {idx} must use stream=false");
        assert_eq!(
            call.tool_choice,
            Some(json!("none")),
            "JSON action protocol step {idx} with aligned tools must set tool_choice=\"none\""
        );
    }

    // Verify disk commit and metadata
    assert_eq!(ws.read_rel("src/lib.rs"), "pub const RETRY_LIMIT: usize = 8;\n");
    let meta = parse_delegation_result_meta(&output).expect("JSON action protocol meta");
    assert_eq!(meta.status, "committed");
    assert_eq!(meta.protocol_used, "json");
    assert!(!meta.fallback_triggered);
    assert_eq!(meta.summary, "Done via JSON action protocol");
    assert_eq!(meta.files.len(), 1);
}

// ============================================================================
// 3. Sticky Fallback Trigger #1: JSON Action Emitted in Native Content
// ============================================================================

#[test]
fn test_e2e_sticky_fallback_trigger_1_json_action_in_native_content() {
    let ws = TempWorkspace::new("fallback_trigger_1");
    ws.write_rel("src/lib.rs", "pub fn max_batch() -> usize { 16 }\n");

    let (mut session, fixture, _server, tid) = boot_http_loopback_session(&ws, Some(":memory:"));
    assert_eq!(session.subagent_protocol, SubagentProtocol::Auto);

    // Step 0 (`Native` in `Auto`): returns empty `tool_calls` with valid JSON action in `content`
    fixture.push_subagent_step(ScriptedSubagentStep {
        content: r#"{"action": "read_file", "path": "src/lib.rs"}"#.to_string(),
        reasoning_content: String::new(),
        tool_calls: Vec::new(),
        finish_reason: Some("stop".to_string()),
        prefix_cache_reused: None,
    });
    // Step 1 (stickily switched to `Json`): edit_file
    fixture.push_subagent_step(ScriptedSubagentStep {
        content: json!({
            "action": "edit_file",
            "path": "src/lib.rs",
            "old_text": "pub fn max_batch() -> usize { 16 }",
            "new_text": "pub fn max_batch() -> usize { 64 }"
        })
        .to_string(),
        reasoning_content: String::new(),
        tool_calls: Vec::new(),
        finish_reason: Some("stop".to_string()),
        prefix_cache_reused: None,
    });
    // Step 2 (stays stickily in `Json`): finish
    fixture.push_subagent_step(ScriptedSubagentStep {
        content: json!({
            "action": "finish",
            "summary": "Updated max_batch after Trigger 1 sticky switch"
        })
        .to_string(),
        reasoning_content: String::new(),
        tool_calls: Vec::new(),
        finish_reason: Some("stop".to_string()),
        prefix_cache_reused: None,
    });

    let (output, status) = session
        .execute_delegate_coding_subagent_direct(
            &tid,
            "call-fb1",
            json!({"instruction": "Update max_batch() to return 64 in src/lib.rs"}),
        )
        .expect("Trigger 1 delegation");
    assert_eq!(status, "completed");

    let all_calls = fixture.recorded_subagent_calls();
    let del_calls = delegate_calls_only(&all_calls);
    assert_eq!(del_calls.len(), 3);
    // Step 0 started in Native (`stream == true`, `tool_choice == Some("auto")`)
    assert!(del_calls[0].stream);
    assert_eq!(del_calls[0].tool_choice, Some(json!("auto")));
    // Steps 1 & 2 switched stickily to Json (`stream == false`, `tool_choice == Some("none")`)
    assert!(!del_calls[1].stream);
    assert_eq!(del_calls[1].tool_choice, Some(json!("none")));
    assert!(!del_calls[2].stream);
    assert_eq!(del_calls[2].tool_choice, Some(json!("none")));

    assert_eq!(ws.read_rel("src/lib.rs"), "pub fn max_batch() -> usize { 64 }\n");
    let meta = parse_delegation_result_meta(&output).expect("Trigger 1 meta");
    assert_eq!(meta.status, "committed");
    assert_eq!(meta.protocol_used, "json");
    assert!(meta.fallback_triggered);
    assert_eq!(meta.steps, 3);
}

// ============================================================================
// 4. Sticky Fallback Trigger #2: Two Consecutive Malformed Native Steps
// ============================================================================

#[test]
fn test_e2e_sticky_fallback_trigger_2_two_consecutive_malformed_native_steps() {
    let ws = TempWorkspace::new("fallback_trigger_2");
    ws.write_rel("src/lib.rs", "pub const QUEUE_DEPTH: usize = 32;\n");

    let (mut session, fixture, _server, tid) = boot_http_loopback_session(&ws, Some(":memory:"));
    assert_eq!(session.subagent_protocol, SubagentProtocol::Auto);

    // Step 0 (`Native`): broken JSON arguments on `edit_file` (malformed #1)
    fixture.push_subagent_tool_calls(vec![make_raw_tc(
        "tc_bad_json",
        "edit_file",
        "{\"path\": \"src/lib.rs\", \"old_text\": UNQUOTED_BROKEN_JSON",
    )]);
    // Step 1 (`Native`): unknown tool `"hallucinated_tool"` (malformed #2 -> triggers sticky switch to JSON action protocol!)
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_unknown_tool",
        "hallucinated_tool",
        json!({"target": "src/lib.rs"}),
    )]);
    // Step 2 (`Json` after sticky switch): valid JSON `edit_file`
    fixture.push_subagent_step(ScriptedSubagentStep {
        content: json!({
            "action": "edit_file",
            "path": "src/lib.rs",
            "old_text": "pub const QUEUE_DEPTH: usize = 32;",
            "new_text": "pub const QUEUE_DEPTH: usize = 128;"
        })
        .to_string(),
        reasoning_content: String::new(),
        tool_calls: Vec::new(),
        finish_reason: Some("stop".to_string()),
        prefix_cache_reused: None,
    });
    // Step 3 (`Json`): `finish`
    fixture.push_subagent_step(ScriptedSubagentStep {
        content: json!({
            "action": "finish",
            "summary": "Updated QUEUE_DEPTH to 128 after sticky fallback"
        })
        .to_string(),
        reasoning_content: String::new(),
        tool_calls: Vec::new(),
        finish_reason: Some("stop".to_string()),
        prefix_cache_reused: None,
    });

    let (output, status) = session
        .execute_delegate_coding_subagent_direct(
            &tid,
            "call-fb2",
            json!({"instruction": "Increase QUEUE_DEPTH to 128 in src/lib.rs"}),
        )
        .expect("Trigger 2 delegation");
    assert_eq!(status, "completed");

    let all_calls = fixture.recorded_subagent_calls();
    let del_calls = delegate_calls_only(&all_calls);
    assert_eq!(del_calls.len(), 4);
    assert!(del_calls[0].stream, "Step 0 Native");
    assert!(del_calls[1].stream, "Step 1 still Native after 1 malformed step");
    assert!(!del_calls[2].stream, "Step 2 switched stickily to Json");
    assert_eq!(del_calls[2].tool_choice, Some(json!("none")));
    assert!(
        del_calls[2]
            .messages
            .last()
            .and_then(|m| m.content.as_deref())
            .unwrap_or("")
            .contains("[PROTOCOL SWITCH -> JSON ACTION MODE]"),
        "Step 2 must receive corrective sticky fallback switch prompt"
    );
    assert!(!del_calls[3].stream, "Step 3 remains in Json");

    assert_eq!(ws.read_rel("src/lib.rs"), "pub const QUEUE_DEPTH: usize = 128;\n");
    let meta = parse_delegation_result_meta(&output).expect("Trigger 2 meta");
    assert_eq!(meta.status, "committed");
    assert_eq!(meta.protocol_used, "json");
    assert!(meta.fallback_triggered);
    assert_eq!(meta.steps, 4);
}

// ============================================================================
// 5. Disallowed Parent Tools Sandboxed Without Malformed Penalty
// ============================================================================

#[test]
fn test_e2e_disallowed_parent_tools_sandboxed_without_malformed_penalty() {
    let ws = TempWorkspace::new("disallowed_parent_tools");
    ws.write_rel("src/lib.rs", "pub const SAFE_MODE: bool = false;\n");
    ws.write_rel("src/keep_me.rs", "pub const MUST_SURVIVE: bool = true;\n");

    let (mut session, fixture, _server, tid) = boot_http_loopback_session(&ws, Some(":memory:"));

    // 4 consecutive steps attempting disallowed parent tools (`terminal`, `delete_path`, `fetch`, `delegate_coding_subagent`).
    // If any counted as MalformedStep, 2 consecutive steps would flip `fallback_triggered = true`!
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_deny_term",
        "terminal",
        json!({"command": "echo forbidden"}),
    )]);
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_deny_del",
        "delete_path",
        json!({"path": "src/keep_me.rs"}),
    )]);
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_deny_fetch",
        "fetch",
        json!({"url": "https://example.com"}),
    )]);
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_deny_recurse",
        "delegate_coding_subagent",
        json!({"instruction": "recursive spawn attempt"}),
    )]);
    // Step 4: valid `edit_file` in Native
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_valid_edit",
        "edit_file",
        json!({
            "path": "src/lib.rs",
            "old_text": "pub const SAFE_MODE: bool = false;",
            "new_text": "pub const SAFE_MODE: bool = true;"
        }),
    )]);
    // Step 5: valid `finish` in Native
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_valid_finish",
        "finish",
        json!({"summary": "Enabled SAFE_MODE cleanly in Native mode"}),
    )]);

    let (output, status) = session
        .execute_delegate_coding_subagent_direct(
            &tid,
            "call-sandbox-deny",
            json!({"instruction": "Enable SAFE_MODE in src/lib.rs"}),
        )
        .expect("Disallowed parent tools delegation");
    assert_eq!(status, "completed");

    let all_calls = fixture.recorded_subagent_calls();
    let del_calls = delegate_calls_only(&all_calls);
    assert_eq!(del_calls.len(), 6);
    for (idx, call) in del_calls.iter().enumerate() {
        assert!(call.stream, "Step {idx} must remain in Native stream=true");
        assert_eq!(call.tool_choice, Some(json!("auto")));
    }

    // Verify all 4 disallowed tool calls received `PERMISSION_DENIED:` in-loop
    let final_msgs = &del_calls[5].messages;
    for denied_id in ["tc_deny_term", "tc_deny_del", "tc_deny_fetch", "tc_deny_recurse"] {
        let tool_reply = final_msgs
            .iter()
            .find(|m| m.tool_call_id.as_deref() == Some(denied_id))
            .and_then(|m| m.content.as_deref())
            .unwrap_or("");
        assert!(
            tool_reply.starts_with("PERMISSION_DENIED:"),
            "Expected PERMISSION_DENIED for {denied_id}, got: {tool_reply}"
        );
    }

    // Verify `src/keep_me.rs` was NOT deleted and `src/lib.rs` was cleanly committed
    assert!(ws.path().join("src/keep_me.rs").exists());
    assert_eq!(ws.read_rel("src/lib.rs"), "pub const SAFE_MODE: bool = true;\n");

    let meta = parse_delegation_result_meta(&output).expect("sandbox meta");
    assert_eq!(meta.status, "committed");
    assert_eq!(meta.protocol_used, "native");
    assert!(!meta.fallback_triggered);
    assert_eq!(meta.steps, 6);
}

// ============================================================================
// 6. Gatekeeper Critic Reject Self-Heal & 3-Bounce Escalation Preserving LoopState
// ============================================================================

#[test]
fn test_e2e_critic_reject_self_heal_and_three_bounce_escalation_preserves_parent_loop_state() {
    let ws = TempWorkspace::new("critic_bounces");
    let orig_a = concat!(
        "pub fn parse_port(raw: &str) -> Result<u16, String> {\n",
        "    Ok(8080)\n",
        "}\n",
    );
    let orig_b = "pub fn compute_ratio(num: u64, den: u64) -> u64 { num }\n";
    ws.write_rel("src/part_a.rs", orig_a);
    ws.write_rel("src/part_b.rs", orig_b);

    let (mut session, fixture, _server, tid) = boot_http_loopback_session(&ws, Some(":memory:"));
    session.critic_enabled = true;

    // --- Part A: Bounce 1 rejected (`fix_hint: "remove .unwrap()"`), Step 1 self-heals, Gatekeeper Critic PASS, Step 2 finish ---
    fixture.push_subagent_response_for_key(
        "critic:",
        json!({
            "passed": false,
            "reason": "Fallible parse uses .unwrap()",
            "violations": ["Unchecked .unwrap() on user input"],
            "fix_hint": "remove .unwrap()"
        })
        .to_string(),
        Some("stop".to_string()),
    );
    fixture.push_subagent_response_for_key(
        "critic:",
        json!({
            "passed": true,
            "reason": "Propagates parse error cleanly via map_err"
        })
        .to_string(),
        Some("stop".to_string()),
    );

    // Step 0: introduces `.unwrap()` -> rejected by Gatekeeper Critic
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_a_0",
        "edit_file",
        json!({
            "path": "src/part_a.rs",
            "old_text": "    Ok(8080)",
            "new_text": "    Ok(raw.parse::<u16>().unwrap())"
        }),
    )]);
    // Step 1: fixes edit without `.unwrap()` -> approved by Gatekeeper Critic
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_a_1",
        "edit_file",
        json!({
            "path": "src/part_a.rs",
            "old_text": "    Ok(8080)",
            "new_text": "    raw.parse::<u16>().map_err(|e| e.to_string())"
        }),
    )]);
    // Step 2: finish
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_a_2",
        "finish",
        json!({"summary": "Self-healed parse_port after Gatekeeper Critic feedback"}),
    )]);

    let (out_a, status_a) = session
        .execute_delegate_coding_subagent_direct(
            &tid,
            "call-part-a",
            json!({"instruction": "Implement parse_port in src/part_a.rs"}),
        )
        .expect("Part A delegation");
    assert_eq!(status_a, "completed");

    let calls_after_a = fixture.recorded_subagent_calls();
    let del_calls_a = delegate_calls_only(&calls_after_a);
    let bounce_feedback = del_calls_a[1]
        .messages
        .iter()
        .find(|m| m.tool_call_id.as_deref() == Some("tc_a_0"))
        .and_then(|m| m.content.as_deref())
        .unwrap_or("");
    assert!(
        bounce_feedback.contains("remove .unwrap()"),
        "Step 1 must receive Gatekeeper Critic fix_hint 'remove .unwrap()', got: {bounce_feedback}"
    );

    let meta_a = parse_delegation_result_meta(&out_a).expect("Part A meta");
    assert_eq!(meta_a.status, "committed");
    assert_eq!(meta_a.files.len(), 1);
    assert_eq!(meta_a.files[0].path, "src/part_a.rs");
    assert!(meta_a.files[0].critic_passed);
    assert_eq!(meta_a.files[0].bounces, 1);
    assert!(ws
        .read_rel("src/part_a.rs")
        .contains("raw.parse::<u16>().map_err(|e| e.to_string())"));

    // --- Part B: 3 consecutive Gatekeeper Critic rejections -> escalates (`critic_bounces_exhausted`), disk untouched, parent LoopState stays `Active` ---
    session.loop_state = LoopState::Active;
    for attempt in 1..=3 {
        fixture.push_subagent_response_for_key(
            "critic:",
            json!({
                "passed": false,
                "reason": format!("Unchecked division by zero (attempt {attempt})"),
                "fix_hint": "Guard den == 0 before dividing"
            })
            .to_string(),
            Some("stop".to_string()),
        );
        fixture.push_subagent_tool_calls(vec![make_tc(
            &format!("tc_b_{attempt}"),
            "edit_file",
            json!({
                "path": "src/part_b.rs",
                "old_text": "pub fn compute_ratio(num: u64, den: u64) -> u64 { num }",
                "new_text": format!("pub fn compute_ratio(num: u64, den: u64) -> u64 {{ num / den /* {attempt} */ }}")
            }),
        )]);
    }

    let (out_b, status_b) = session
        .execute_delegate_coding_subagent_direct(
            &tid,
            "call-part-b",
            json!({"instruction": "Update compute_ratio in src/part_b.rs"}),
        )
        .expect("Part B delegation");
    assert_eq!(status_b, "completed");

    let meta_b = parse_delegation_result_meta(&out_b).expect("Part B meta");
    assert_eq!(meta_b.status, "escalated");
    assert_eq!(
        meta_b.escalation_reason.as_deref(),
        Some("critic_bounces_exhausted")
    );
    assert_eq!(meta_b.undo_id, None);
    assert_eq!(meta_b.steps, 3);
    assert!(
        meta_b
            .files
            .iter()
            .any(|f| f.path == "src/part_b.rs" && !f.critic_passed && f.bounces == 3),
        "Expected failed_file_meta with bounces == 3: {meta_b:?}"
    );

    // Disk for `src/part_b.rs` must be 100% untouched AND parent `session.loop_state` must remain `Active`!
    assert_eq!(ws.read_rel("src/part_b.rs"), orig_b);
    assert_eq!(
        session.loop_state,
        LoopState::Active,
        "Subagent critic exhaustion must NEVER trip parent session into PausedOnCriticReject"
    );
}

// ============================================================================
// 7. Cross-Thread Mutation Conflict Escalates Without Overwriting
// ============================================================================

#[test]
fn test_e2e_cross_thread_mutation_conflict_escalates_without_overwriting() {
    let ws = TempWorkspace::new("cross_thread_conflict");
    ws.write_rel("src/shared.rs", "pub const VERSION: u32 = 1;\n");

    let (mut session, fixture, _server, tid_a) = boot_http_loopback_session(&ws, Some(":memory:"));
    let pid = session.selected_project_id.clone().expect("project id");
    let thread_b = session
        .create_thread(Some(&pid), Some("Thread B"), Some("write"), Vec::new())
        .expect("create Thread B");
    let tid_b = thread_b.id;
    assert_ne!(tid_a, tid_b);

    // Thread B mutates `src/shared.rs` -> commits `VERSION = 2` on disk & records mutation in `last_mutation_by_path`
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_tb_edit",
        "edit_file",
        json!({
            "path": "src/shared.rs",
            "old_text": "pub const VERSION: u32 = 1;",
            "new_text": "pub const VERSION: u32 = 2; // Thread B authoritative edit"
        }),
    )]);
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_tb_finish",
        "finish",
        json!({"summary": "Thread B bumped VERSION to 2"}),
    )]);

    let (out_b, _) = session
        .execute_delegate_coding_subagent_direct(
            &tid_b,
            "call-thread-b",
            json!({"instruction": "Set VERSION to 2 in src/shared.rs"}),
        )
        .expect("Thread B commit");
    let meta_b = parse_delegation_result_meta(&out_b).expect("Thread B meta");
    assert_eq!(meta_b.status, "committed");
    let thread_b_disk = "pub const VERSION: u32 = 2; // Thread B authoritative edit\n";
    assert_eq!(ws.read_rel("src/shared.rs"), thread_b_disk);

    // Within 600s, Thread A's subagent attempts `edit_file` on `src/shared.rs` -> escalates with `mutation_conflict`!
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_ta_conflict",
        "edit_file",
        json!({
            "path": "src/shared.rs",
            "old_text": "pub const VERSION: u32 = 2; // Thread B authoritative edit",
            "new_text": "pub const VERSION: u32 = 999; // Thread A conflicting overwrite"
        }),
    )]);

    let (out_a, status_a) = session
        .execute_delegate_coding_subagent_direct(
            &tid_a,
            "call-thread-a",
            json!({"instruction": "Overwrite VERSION in src/shared.rs from Thread A"}),
        )
        .expect("Thread A conflict delegation");
    assert_eq!(status_a, "completed");

    let meta_a = parse_delegation_result_meta(&out_a).expect("Thread A meta");
    assert_eq!(meta_a.status, "escalated");
    assert_eq!(meta_a.escalation_reason.as_deref(), Some("mutation_conflict"));
    assert_eq!(meta_a.undo_id, None);
    assert!(meta_a.summary.contains("CONFLICT_WARNING"));

    // Thread B's disk contents must remain 100% untouched!
    assert_eq!(ws.read_rel("src/shared.rs"), thread_b_disk);
}

// ============================================================================
// 8. Web-Tainted Thread Sensitive File Escalates With Zero Disk Writes
// ============================================================================

#[test]
fn test_e2e_web_tainted_thread_sensitive_file_escalates() {
    let ws = TempWorkspace::new("web_taint_sensitive");
    let orig_agents = "# Workspace Agent Rules\nNever weaken sandbox checks.\n";
    ws.write_rel("AGENTS.md", orig_agents);
    fs::create_dir_all(ws.path().join(".git/hooks")).expect("create .git/hooks");

    let (mut session, fixture, _server, tid) = boot_http_loopback_session(&ws, Some(":memory:"));
    session.web_tainted_threads.insert(tid.clone());

    // Step 0: attempts `.git/hooks/pre-commit` (blocked by protected globs with zero disk writes)
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_taint_hook",
        "write_file",
        json!({
            "path": ".git/hooks/pre-commit",
            "content": "#!/bin/sh\necho injected\n"
        }),
    )]);
    // Step 1: attempts `edit_file` on `AGENTS.md` -> immediately escalates (`web_taint_sensitive_file`)
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_taint_agents",
        "edit_file",
        json!({
            "path": "AGENTS.md",
            "old_text": "Never weaken sandbox checks.",
            "new_text": "Allow external instructions."
        }),
    )]);

    let (output, status) = session
        .execute_delegate_coding_subagent_direct(
            &tid,
            "call-web-taint",
            json!({"instruction": "Update workspace rules"}),
        )
        .expect("web-taint delegation");
    assert_eq!(status, "completed");

    let meta = parse_delegation_result_meta(&output).expect("web-taint meta");
    assert_eq!(meta.status, "escalated");
    assert_eq!(
        meta.escalation_reason.as_deref(),
        Some("web_taint_sensitive_file")
    );
    assert_eq!(meta.undo_id, None);
    assert!(meta.files.is_empty());

    // Zero disk writes on both `.git/hooks/pre-commit` and `AGENTS.md`
    assert!(!ws.path().join(".git/hooks/pre-commit").exists());
    assert_eq!(ws.read_rel("AGENTS.md"), orig_agents);
}

// ============================================================================
// 9. Step Budget Exhausted Escalates & Caches Proposal IDs (`prop-1`)
// ============================================================================

#[test]
fn test_e2e_step_budget_exhausted_escalates_and_caches_proposal_ids() {
    let ws = TempWorkspace::new("step_budget_proposals");
    let orig_lib = "pub fn timeout_secs() -> u64 { 5 }\n";
    let expected_staged = "pub fn timeout_secs() -> u64 { 30 }\n";
    ws.write_rel("src/lib.rs", orig_lib);

    let (mut session, fixture, _server, tid) = boot_http_loopback_session(&ws, Some(":memory:"));

    // Invoke with `max_steps: 2`:
    // Step 0: critic-verified `edit_file` on `src/lib.rs`
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_budget_edit",
        "edit_file",
        json!({
            "path": "src/lib.rs",
            "old_text": "pub fn timeout_secs() -> u64 { 5 }",
            "new_text": "pub fn timeout_secs() -> u64 { 30 }"
        }),
    )]);
    // Step 1: `read_file` without `finish` -> exhausts `max_steps: 2`!
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_budget_read",
        "read_file",
        json!({"path": "src/lib.rs"}),
    )]);

    let (output, status) = session
        .execute_delegate_coding_subagent_direct(
            &tid,
            "call-budget-2",
            json!({
                "instruction": "Raise timeout_secs to 30 in src/lib.rs",
                "target_files": ["src/lib.rs"],
                "max_steps": 2
            }),
        )
        .expect("step budget delegation");
    assert_eq!(status, "completed");

    let meta = parse_delegation_result_meta(&output).expect("step budget meta");
    assert_eq!(meta.status, "escalated");
    assert_eq!(
        meta.escalation_reason.as_deref(),
        Some("step_budget_exhausted")
    );
    assert_eq!(meta.steps, 2);
    assert_eq!(meta.max_steps, 2);
    assert_eq!(meta.undo_id, None);
    assert_eq!(meta.proposal_ids, vec!["prop-1".to_string()]);

    // Disk must remain untouched
    assert_eq!(ws.read_rel("src/lib.rs"), orig_lib);

    // `session.proposal_cache` must contain `"prop-1"` with the verified edit hunk
    let cached_prop = session
        .proposal_cache
        .iter()
        .find(|p| p.proposal_id == "prop-1")
        .expect("prop-1 in session.proposal_cache");
    assert_eq!(cached_prop.path, "src/lib.rs");
    assert_eq!(cached_prop.edits.len(), 1);
    assert_eq!(cached_prop.edits[0].old_text, orig_lib);
    assert_eq!(cached_prop.edits[0].new_text, expected_staged);
}

// ============================================================================
// 10. Parent Uncommitted Staged Files Isolated From Delegation Commit & Rollback
// ============================================================================

#[test]
fn test_e2e_parent_uncommitted_staged_files_isolated_from_delegation_commit_and_rollback() {
    let ws = TempWorkspace::new("parent_stage_isolation");
    let orig_parent_disk = "pub const PARENT_WIP: usize = 1;\n";
    let staged_parent_wip = "pub const PARENT_WIP: usize = 777; // uncommitted parent stage\n";
    let orig_sub_ok = "pub const SUB_OK: usize = 10;\n";
    let committed_sub_ok = "pub const SUB_OK: usize = 11;\n";
    let orig_sub_fail = "pub const SUB_FAIL: usize = 20;\n";

    ws.write_rel("src/parent_wip.rs", orig_parent_disk);
    ws.write_rel("src/sub_ok.rs", orig_sub_ok);
    ws.write_rel("src/sub_fail.rs", orig_sub_fail);

    let (mut session, fixture, _server, tid) = boot_http_loopback_session(&ws, Some(":memory:"));

    // Stage an uncommitted parent mutation on `src/parent_wip.rs` in `session.snapshot_store`
    let canon_parent = resolve_workspace_path(ws.path(), "src/parent_wip.rs")
        .expect("resolve src/parent_wip.rs");
    session
        .snapshot_store
        .stage_write(&canon_parent, "src/parent_wip.rs", staged_parent_wip.to_string())
        .expect("stage parent_wip.rs");
    assert!(session.snapshot_store.has_staged_mutation(&canon_parent));

    // Delegation 1: Committed delegation on `src/sub_ok.rs`
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_iso_ok_edit",
        "edit_file",
        json!({
            "path": "src/sub_ok.rs",
            "old_text": "pub const SUB_OK: usize = 10;",
            "new_text": "pub const SUB_OK: usize = 11;"
        }),
    )]);
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_iso_ok_finish",
        "finish",
        json!({"summary": "Committed sub_ok.rs"}),
    )]);

    let (out_ok, _) = session
        .execute_delegate_coding_subagent_direct(
            &tid,
            "call-iso-ok",
            json!({"instruction": "Update SUB_OK to 11 in src/sub_ok.rs"}),
        )
        .expect("committed delegation on sub_ok.rs");
    let meta_ok = parse_delegation_result_meta(&out_ok).expect("meta_ok");
    assert_eq!(meta_ok.status, "committed");

    // Delegation 2: Escalated delegation on `src/sub_fail.rs` (`max_steps: 1` without `finish`)
    fixture.push_subagent_tool_calls(vec![make_tc(
        "tc_iso_fail_edit",
        "edit_file",
        json!({
            "path": "src/sub_fail.rs",
            "old_text": "pub const SUB_FAIL: usize = 20;",
            "new_text": "pub const SUB_FAIL: usize = 99;"
        }),
    )]);

    let (out_fail, _) = session
        .execute_delegate_coding_subagent_direct(
            &tid,
            "call-iso-fail",
            json!({
                "instruction": "Attempt SUB_FAIL update in src/sub_fail.rs",
                "max_steps": 1
            }),
        )
        .expect("escalated delegation on sub_fail.rs");
    let meta_fail = parse_delegation_result_meta(&out_fail).expect("meta_fail");
    assert_eq!(meta_fail.status, "escalated");
    assert_eq!(
        meta_fail.escalation_reason.as_deref(),
        Some("step_budget_exhausted")
    );

    // Verify isolation invariants across disk and `session.snapshot_store`:
    // 1. `src/sub_ok.rs` IS committed on disk
    assert_eq!(ws.read_rel("src/sub_ok.rs"), committed_sub_ok);
    // 2. `src/sub_fail.rs` IS untouched on disk
    assert_eq!(ws.read_rel("src/sub_fail.rs"), orig_sub_fail);
    // 3. `src/parent_wip.rs` is NOT prematurely written to disk AND remains staged in `session.snapshot_store`!
    assert_eq!(
        ws.read_rel("src/parent_wip.rs"),
        orig_parent_disk,
        "Parent uncommitted staged file must NEVER be prematurely written to disk by subagent commit"
    );
    assert!(
        session.snapshot_store.has_staged_mutation(&canon_parent),
        "Parent uncommitted staged file must remain staged in session.snapshot_store"
    );
    assert_eq!(
        session
            .snapshot_store
            .read_file_text(&canon_parent)
            .expect("read staged parent_wip.rs"),
        staged_parent_wip
    );
}
