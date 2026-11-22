//! Multi-Turn Agent Loop, Local Context Reset, Local SQLite Wakeup Timers &
//! Workspace Snapshot Orchestrator (`agent_loop`) for BeatrixLeslie IDE (`spell_core`).
//!
//! Implements:
//! - `SpellCoreSession`: Single Rust source of truth for Pane 1 (Projects),
//!   Pane 2 (Conversation History & Tags), and Pane 3 (Streaming Agent Chat,
//!   Tool Confirmation Cards, Client-Side Telemetry, and Local SQLite Timers).
//! - Endpoint-agnostic OpenAI-compatible transport configuration (`configure_endpoint`,
//!   `fetch_models`, `refresh_engine_status`).
//! - Local SQLite wall-clock timers (`schedule_engine_timer`, `poll_and_fire_due_engine_timers`,
//!   `cancel_engine_timer`, `list_timers`) with zero external control-plane dependencies.
//! - Conversation Rubric Tags (`tag`), headless tool execution, and local 4-slot subagent state.

use std::collections::{HashMap, HashSet, VecDeque};
use std::fs;
use std::path::Path;
use std::process::Command;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use crate::contract::*;
use crate::locator::EnginePathLocator;
use crate::mcp_client::McpStdioClient;
use crate::storage::*;
use crate::tags::*;
use crate::tools::{
    build_clean_system_prompt, build_openai_tools_schema,
    collect_numbered_subagent_context_with_roots, enforce_research_cache_write_guard,
    is_web_taint_sensitive_workspace_file, resolve_workspace_path,
    resolve_workspace_path_with_roots, AgentProfile, ToolPermissionPolicy, TurnPinCache,
};
use crate::transport::*;

/// Maximum multi-turn tool continuation steps per user prompt.
pub const DEFAULT_MAX_AGENT_STEPS: usize = 8;

/// Maximum cached `SubagentCodingProposal` items retained per `SpellCoreSession`.
pub const MAX_CACHED_SUBAGENT_PROPOSALS: usize = 64;

/// Canonical list of all 17 headless tools (generic `spawn_agent` denied; `skill` replaced by `tag`;
/// includes `prefill_context_subagent`, `delegate_coding_subagent`, and `apply_subagent_proposal`).
pub const ALL_HEADLESS_TOOL_NAMES: [&str; 17] = [
    "read_file",
    "edit_file",
    "write_file",
    "list_directory",
    "find_path",
    "grep",
    "create_directory",
    "copy_path",
    "move_path",
    "delete_path",
    "terminal",
    "fetch",
    "schedule_wakeup",
    "tag",
    "prefill_context_subagent",
    "delegate_coding_subagent",
    "apply_subagent_proposal",
];

static SESSION_REQ_SEQ: AtomicU64 = AtomicU64::new(100);

fn next_req_id() -> u64 {
    SESSION_REQ_SEQ.fetch_add(1, Ordering::SeqCst)
}

/// Pending tool call awaiting explicit user approval when its policy is `ToolPermissionPolicy::Confirm`
/// or when Online Research Domain Approval / Session Taint Guard / Web-Taint Rule Guard pauses execution.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct PendingToolConfirmation {
    pub call_id: String,
    pub thread_id: String,
    pub tool_name: String,
    pub arguments_json: String,
    pub remaining_tool_calls: Vec<WireToolCall>,
    pub created_at: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub confirmation_kind: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub target_domain: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}


/// Interactive Message Terminal CLI state.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct MessageTerminalState {
    pub is_open: bool,
    pub working_directory: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
    pub session_id: u64,
    #[serde(default)]
    pub last_output: String,
}

/// Returns the default tool permissions map (`Allow` for headless tools + `open_message_terminal`,
/// and `Confirm` for `apply_subagent_proposal` so patch application previews on the Ask/Confirm card).
pub fn default_tool_permissions() -> HashMap<String, ToolPermissionPolicy> {
    let mut tool_permissions = HashMap::new();
    for name in ALL_HEADLESS_TOOL_NAMES {
        let default_policy = if name == "apply_subagent_proposal" {
            ToolPermissionPolicy::Confirm
        } else {
            ToolPermissionPolicy::Allow
        };
        tool_permissions.insert(name.to_string(), default_policy);
    }
    tool_permissions.insert(
        "open_message_terminal".to_string(),
        ToolPermissionPolicy::Allow,
    );
    tool_permissions
}

/// Transient runtime snapshot of the recent main-turn wire prefix (`messages`, `tools`, `cache_key`)
/// shared cleanly with subagent tasks on the active thread.
#[derive(Clone, Debug)]
struct LastPrefixSnapshot {
    thread_id: String,
    cache_key: String,
    messages: Vec<WireMessage>,
    tools: Vec<Value>,
}

/// Non-secret persisted endpoint configuration stored in SQLite `kv_documents("settings", "endpoint_config")`.
/// API keys are NEVER stored in SQLite.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
struct PersistedEndpointConfig {
    #[serde(default)]
    base_url: String,
    #[serde(default)]
    model_id: String,
    #[serde(default)]
    subagent_model_id: String,
}

/// Local in-core subagent slot state (`slot 1..=4`) managed directly by `SpellCoreSession`.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct LocalSubagentSlotState {
    pub slot: usize,
    pub status: String,
    pub mode: String,
    pub model_id: String,
    pub lines: Vec<String>,
    pub decode_tok_s: f64,
    pub forked_from_slot: Option<usize>,
    pub updated_at_unix_s: u64,
    pub messages: Vec<WireMessage>,
}

impl LocalSubagentSlotState {
    pub fn new_idle(slot: usize) -> Self {
        Self {
            slot,
            status: "idle".to_string(),
            mode: "coding".to_string(),
            model_id: String::new(),
            lines: Vec::new(),
            decode_tok_s: 0.0,
            forked_from_slot: None,
            updated_at_unix_s: 0,
            messages: Vec::new(),
        }
    }

    fn append_lines_bounded(&mut self, text: &str) {
        const MAX_SLOT_LINES: usize = 2_000;
        for line in text.lines() {
            self.lines.push(line.to_string());
        }
        if self.lines.len() > MAX_SLOT_LINES {
            let drain_count = self.lines.len() - MAX_SLOT_LINES;
            self.lines.drain(..drain_count);
        }
    }
}

/// Client-side endpoint status, model catalog, and measured throughput telemetry.
#[derive(Clone, Debug, Default)]
pub struct EngineStatusCache {
    /// `None` until the first successful probe/turn, or after the endpoint stopped answering.
    pub telemetry: Option<TelemetryResponse>,
    pub subagents_status: Option<SubagentPoolStatusResponse>,
    pub timers: Vec<TimerRecord>,
    /// Models discovered via `GET /models`.
    pub available_models: Vec<ModelDescriptor>,
    /// Last measured `/models` or turn latency in milliseconds.
    pub endpoint_latency_ms: Option<u64>,
    /// Last measured client-side main turn decode speed (`tok/s`).
    pub decode_tok_s: f64,
    /// Exponentially weighted moving average of decode speed (`tok/s`).
    pub decode_tok_s_ewma: f64,
    /// Cumulative prompt tokens across turns in this session.
    pub total_prompt_tokens: u64,
    /// Cumulative completion tokens across turns in this session.
    pub total_completion_tokens: u64,
    /// Cumulative cached prompt tokens reported by providers in this session.
    pub total_cached_tokens: u64,
    /// Transport error from the last failed probe (shown in the IDE instead of stale numbers).
    pub last_error: Option<String>,
    pub refreshed_at_unix_s: f64,
    /// Sticky positive subagent decode throughput (`tok/s`).
    pub subagent_decode_tok_s: f64,
    /// Monotonic prefix-cache reuse counter observed across subagent completions.
    pub prefix_cache_reuses: u64,
    /// Whether at least one subagent completion succeeded in this session.
    pub saw_live_subagent_completion: bool,
}

/// Subagent status snapshot exported in Rust FFI JSON.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct EngineStatusSnapshot {
    pub subagent_model_id: String,
    pub subagent_inference_backed: bool,
    pub subagent_decode_tok_s: f64,
    pub prefix_cache_reuses: u64,
}

impl Default for EngineStatusSnapshot {
    fn default() -> Self {
        Self {
            subagent_model_id: String::new(),
            subagent_inference_backed: false,
            subagent_decode_tok_s: 0.0,
            prefix_cache_reuses: 0,
        }
    }
}

/// Typed workspace snapshot returned by [`SpellCoreSession::workspace_snapshot`].
#[derive(Clone, Debug, Default)]
pub struct WorkspaceSnapshot {
    pub telemetry: TelemetryResponse,
    pub subagent_telemetry: EngineStatusSnapshot,
    pub subagent_pool_status: Option<SubagentPoolStatusResponse>,
    pub selected_project_id: Option<String>,
    pub selected_thread_id: Option<String>,
    pub active_timers: Vec<TimerRecord>,
    pub engine_error: Option<String>,
}

/// Central Rust session state driving Spell IDE.
pub struct SpellCoreSession {
    pub locator: EnginePathLocator,
    pub storage: SpellStorage,
    pub tag_registry: TagRegistry,
    pub turn_pin_cache: TurnPinCache,
    /// OpenAI-compatible HTTP transport (endpoint + API key set by `configure_endpoint`).
    pub http_transport: Arc<HttpTransport>,
    /// Transport used for every completion/model call. Defaults to `http_transport`; tests
    /// may inject a test double via [`SpellCoreSession::new_with_transport`].
    pub transport: Arc<dyn EngineTransport>,
    /// `true` when constructed with a custom injected [`EngineTransport`] test double.
    uses_custom_transport: bool,
    /// Primary model ID configured by the user (empty until configured).
    pub model_id: String,
    /// Optional dedicated subagent model ID (falls back to `model_id` when empty).
    pub subagent_model_id: String,
    /// Local 4-slot in-core subagent runner state (`slot 1..=4`).
    pub local_subagent_slots: [LocalSubagentSlotState; 4],
    /// Client-side telemetry, discovered models, and latency cache.
    pub engine_status: parking_lot::Mutex<EngineStatusCache>,
    /// Defaults to `false` (keeps `prompt_cache_key = thread_id` stable); switchable for legacy comparison.
    pub epoch_cache_key_suffix: bool,
    /// Per-tool permission policies (`Allow` by default, with per-tool override to `Confirm` or `Deny`).
    pub tool_permissions: HashMap<String, ToolPermissionPolicy>,
    /// Paused tool calls awaiting user confirmation (`approve_tool_call(call_id, approved)`).
    pub pending_confirmations: Vec<PendingToolConfirmation>,
    /// Currently selected project ID in Pane 1.
    pub selected_project_id: Option<String>,
    /// Currently selected thread ID in Pane 2 / Pane 3.
    pub selected_thread_id: Option<String>,
    /// Optional embedded or subprocess MCP client for `spell-full-file-mcp` tools.
    pub mcp_client: Option<Arc<McpStdioClient>>,
    /// Active in-flight MCP request ID so `cancel_active_turn` can abort `< 50 ms`.
    pub active_mcp_req_id: Option<u64>,
    /// Most recent turn's preprocessed `@tag:` / `#steer:` directive state.
    pub last_turn_preprocessed: Option<crate::tags::PreprocessedPrompt>,
    /// Pre-indexed file snapshot & `Stage-Write` shadow overlay store.
    pub snapshot_store: crate::tools::WorkspaceSnapshotStore,
    /// Bounded compiler diagnostics ring buffer.
    pub diagnostics_bus: crate::tools::DiagnosticsRingBuffer,
    /// Current autonomous loop state.
    pub loop_state: LoopState,
    /// Completed or active autonomous loop cycle count.
    pub loop_cycle_count: u64,
    /// Consecutive Gatekeeper Critic rejection bounce count.
    pub critic_bounce_count: u8,
    /// Maximum consecutive critic rejections before circuit-breaker pause (default `3`).
    pub max_critic_bounces: u8,
    /// Most recent Gatekeeper Critic verdict.
    pub last_critic_verdict: Option<CriticVerdict>,
    /// Most recent non-destructive Git stash snapshot SHA.
    pub last_git_snapshot_sha: Option<String>,
    /// Whether live `edit_file` and `write_file` tool calls stage into `snapshot_store` (`Stage-Write` shadow overlay) instead of writing directly to disk.
    pub stage_write_mode: bool,
    /// Monotonic background timer wakeup tick sequence counter.
    pub timer_tick_seq: u64,
    /// Active workspace root directory.
    pub workspace_root: std::path::PathBuf,
    /// Interactive Message Terminal CLI state.
    pub message_terminal: MessageTerminalState,
    /// Active steering directive IDs toggled on in Settings (`#steer:<id>`).
    pub active_steering_ids: Vec<String>,
    /// Whether optional LLM-backed Gatekeeper Critic evaluation is enabled (defaults to `false`).
    pub critic_enabled: bool,
    /// Monotonic sequence number for Gatekeeper Critic reviews (`critic:<tid>:review_<seq>`).
    pub critic_review_seq: u64,
    /// Bounded cache (`<= 64`) of coding proposals emitted by `delegate_coding_subagent` (`prop-1`, `prop-2`, ...).
    pub proposal_cache: VecDeque<SubagentCodingProposal>,
    /// Monotonic sequence counter for `proposal_id` assignment (`prop-1`, `prop-2`, ...).
    pub next_proposal_seq: u64,
    /// Transient runtime snapshot of the recent wire prefix on the active thread.
    last_prefix_snapshot: Option<LastPrefixSnapshot>,
    /// Gated Online Research configuration (`OnlineResearchMode::Offline` fail-closed default).
    pub online_research_config: crate::online_fetch::OnlineResearchConfig,
    /// Thread IDs that have ingested untrusted external web/research content.
    pub web_tainted_threads: HashSet<String>,
    /// Thread IDs that have read local workspace files outside `.spell/research_cache`.
    pub workspace_read_threads: HashSet<String>,
    /// Rich citation metadata keyed by `tool_call_id` and URL for UI tool-call strips.
    pub last_fetch_citations: HashMap<String, crate::online_fetch::CitationMetadata>,
    /// Extra readable directories (e.g. fallback `research_cache` roots on read-only workspaces).
    pub extra_readable_roots: Vec<std::path::PathBuf>,
    /// Optional pluggable HTTPS fetcher override for deterministic offline tests.
    pub online_fetcher_override: Option<Arc<dyn crate::online_fetch::HttpsFetcher>>,
    /// Ephemeral call IDs explicitly approved by the user during `confirm_tool_call`.
    pub(crate) confirmed_tool_call_ids: HashSet<String>,
    /// Bounded journal of workspace mutations performed by tools (newest last).
    pub mutation_journal: VecDeque<MutationJournalEntry>,
    /// Last mutation per canonical path, used for cross-conversation conflict warnings.
    pub last_mutation_by_path: HashMap<String, MutationJournalEntry>,
    /// Default action protocol for autonomous `delegate_coding_subagent` runs (`Auto` | `Native` | `Json`).
    pub subagent_protocol: crate::subagent_coding::SubagentProtocol,
    /// Bounded (`<= MAX_DELEGATION_UNDO_RECORDS`) undo history for committed autonomous coding delegations.
    pub delegation_undo_log: VecDeque<crate::subagent_coding::DelegationUndoRecord>,
    /// Monotonic sequence counter for `undo_id` assignment (`"del-1"`, `"del-2"`, ...).
    pub next_delegation_undo_seq: u64,
    /// IDs of delegations that have been undone in this session (surfaced to UI cards).
    pub undone_delegation_ids: Vec<String>,
    /// Whether explicit `set_stage_write_mode(true)` (or per-call override) holds autonomous delegation commits in `snapshot_store`.
    pub(crate) delegation_stage_write_hold: bool,
    /// Canonical paths committed or staged by earlier sibling delegations in the active parallel subagent batch.
    pub(crate) active_batch_committed_paths: HashSet<std::path::PathBuf>,
    /// Canonical paths committed or held by the most recent delegation in the current turn.
    pub(crate) last_delegation_touched_paths: Vec<std::path::PathBuf>,
}

fn default_delegation_undo_next_seq() -> u64 {
    1
}

#[derive(Clone, Debug, Serialize, Deserialize)]
struct PersistedDelegationUndoState {
    #[serde(default = "default_delegation_undo_next_seq")]
    next_seq: u64,
    #[serde(default)]
    records: Vec<crate::subagent_coding::DelegationUndoRecord>,
    #[serde(default)]
    undone_ids: Vec<String>,
}

/// Maximum entries retained in [`SpellCoreSession::mutation_journal`].
pub const MUTATION_JOURNAL_CAPACITY: usize = 1_000;
/// Window in which a mutation by another conversation triggers a conflict warning.
pub const MUTATION_CONFLICT_WINDOW_SECS: u64 = 600;

/// One workspace mutation recorded by a tool call.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MutationJournalEntry {
    pub timestamp: u64,
    pub thread_id: String,
    pub call_id: String,
    pub tool: String,
    pub paths: Vec<String>,
    pub status: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub conflict_warning: Option<String>,
}

impl Default for SpellCoreSession {
    fn default() -> Self {
        Self::new(Some(":memory:")).expect("in-memory SpellCoreSession initialization")
    }
}

impl SpellCoreSession {
    /// Creates a new `SpellCoreSession` backed by SQLite at `db_path` (or default `data_dir` path if `None`).
    pub fn new(db_path: Option<&str>) -> Result<Self, String> {
        let locator = EnginePathLocator::discover();
        let storage = match db_path {
            Some(p) if !p.trim().is_empty() => SpellStorage::open_str(p)?,
            _ => {
                let default_db = locator.data_dir.join("spell_ide.sqlite3");
                SpellStorage::open(&default_db).or_else(|_| SpellStorage::new_in_memory())?
            }
        };

        let primary_root = locator.primary_workspace_root();
        let tag_registry = TagRegistry::with_project_root(&primary_root);

        // Sync built-in and persisted tags with SQLite
        for tag in tag_registry.list_tags() {
            let _ = storage.upsert_tag(&tag);
        }
        if let Ok(saved_tags) = storage.list_tags() {
            for saved in saved_tags {
                tag_registry.upsert_tag(saved);
            }
        }
        for directive in tag_registry.list_steering_directives() {
            let _ = storage.upsert_steering(&directive);
        }
        if let Ok(saved_steering) = storage.list_steering() {
            for saved in saved_steering {
                tag_registry.upsert_steering(saved);
            }
        }

        // Default unrestricted tool permissions (`Allow` for all tools) + load overrides from SQLite
        let mut tool_permissions = default_tool_permissions();
        if let Ok(saved_perms) = storage.list_tool_permissions() {
            for (k, v) in saved_perms {
                tool_permissions.insert(k, v);
            }
        }

        // Load non-secret persisted endpoint configuration (`{ base_url, model_id, subagent_model_id }`).
        // Default is unconfigured (`""`) with zero network calls on startup. API keys are NEVER persisted in SQLite.
        let persisted_ep: PersistedEndpointConfig = storage
            .get_kv_document("settings", "endpoint_config")
            .ok()
            .flatten()
            .and_then(|raw| serde_json::from_str::<PersistedEndpointConfig>(&raw).ok())
            .unwrap_or_default();

        let initial_base_url = {
            let trimmed = persisted_ep.base_url.trim();
            if trimmed.is_empty() {
                DEFAULT_BASE_URL.to_string()
            } else if let Ok(parsed) = crate::transport::normalize_and_parse_endpoint(trimmed) {
                parsed.normalized_base_url().to_string()
            } else {
                DEFAULT_BASE_URL.to_string()
            }
        };
        let initial_model_id = persisted_ep.model_id.trim().to_string();
        let initial_subagent_model_id = persisted_ep.subagent_model_id.trim().to_string();

        let critic_enabled = storage
            .get_kv_document("settings", "critic_enabled")
            .ok()
            .flatten()
            .map(|raw| matches!(raw.trim().to_ascii_lowercase().as_str(), "true" | "1" | "yes"))
            .unwrap_or(false);

        let http_transport = Arc::new(HttpTransport::new(initial_base_url, locator.clone()));
        let transport: Arc<dyn EngineTransport> = http_transport.clone();

        let message_terminal = MessageTerminalState {
            is_open: false,
            working_directory: primary_root.to_string_lossy().to_string(),
            reason: None,
            session_id: 0,
            last_output: String::new(),
        };

        let mut online_research_config = crate::online_fetch::OnlineResearchConfig::with_defaults();
        online_research_config.workspace_root = Some(primary_root.clone());
        online_research_config.fallback_data_dir = Some(locator.data_dir.clone());

        let subagent_protocol = storage
            .get_kv_document("settings", "subagent_protocol")
            .ok()
            .flatten()
            .map(|s| crate::subagent_coding::SubagentProtocol::from_wire(&s))
            .unwrap_or(crate::subagent_coding::SubagentProtocol::Auto);

        let (delegation_undo_log, next_delegation_undo_seq, undone_delegation_ids) = storage
            .get_kv_document("delegation_undo", "state_v1")
            .ok()
            .flatten()
            .and_then(|raw| serde_json::from_str::<PersistedDelegationUndoState>(&raw).ok())
            .map(|st| {
                let mut deque: VecDeque<crate::subagent_coding::DelegationUndoRecord> =
                    st.records.into_iter().collect();
                while deque.len() > crate::subagent_coding::MAX_DELEGATION_UNDO_RECORDS {
                    deque.pop_front();
                }
                let max_seen = deque
                    .iter()
                    .map(|r| r.undo_id.as_str())
                    .chain(st.undone_ids.iter().map(String::as_str))
                    .filter_map(|id| id.strip_prefix("del-"))
                    .filter_map(|n| n.parse::<u64>().ok())
                    .max()
                    .unwrap_or(0);
                let safe_next = st.next_seq.max(1).max(max_seen.saturating_add(1));
                (deque, safe_next, st.undone_ids)
            })
            .unwrap_or_else(|| (VecDeque::new(), 1, Vec::new()));

        let local_subagent_slots = [
            LocalSubagentSlotState::new_idle(1),
            LocalSubagentSlotState::new_idle(2),
            LocalSubagentSlotState::new_idle(3),
            LocalSubagentSlotState::new_idle(4),
        ];

        let mut session = Self {
            locator,
            storage,
            tag_registry,
            turn_pin_cache: TurnPinCache::new(),
            http_transport,
            transport,
            uses_custom_transport: false,
            model_id: initial_model_id,
            subagent_model_id: initial_subagent_model_id,
            local_subagent_slots,
            engine_status: parking_lot::Mutex::new(EngineStatusCache::default()),
            epoch_cache_key_suffix: false,
            tool_permissions,
            pending_confirmations: Vec::new(),
            selected_project_id: None,
            selected_thread_id: None,
            mcp_client: None,
            active_mcp_req_id: None,
            last_turn_preprocessed: None,
            snapshot_store: crate::tools::WorkspaceSnapshotStore::new(),
            diagnostics_bus: crate::tools::DiagnosticsRingBuffer::new(),
            loop_state: LoopState::Idle,
            loop_cycle_count: 0,
            critic_bounce_count: 0,
            max_critic_bounces: default_max_critic_bounces(),
            last_critic_verdict: None,
            last_git_snapshot_sha: None,
            stage_write_mode: true,
            timer_tick_seq: 0,
            workspace_root: primary_root,
            message_terminal,
            active_steering_ids: Vec::new(),
            critic_enabled,
            critic_review_seq: 0,
            proposal_cache: VecDeque::new(),
            next_proposal_seq: 0,
            last_prefix_snapshot: None,
            online_research_config,
            web_tainted_threads: HashSet::new(),
            workspace_read_threads: HashSet::new(),
            last_fetch_citations: HashMap::new(),
            extra_readable_roots: Vec::new(),
            online_fetcher_override: None,
            confirmed_tool_call_ids: HashSet::new(),
            mutation_journal: VecDeque::new(),
            last_mutation_by_path: HashMap::new(),
            subagent_protocol,
            delegation_undo_log,
            next_delegation_undo_seq,
            undone_delegation_ids,
            delegation_stage_write_hold: false,
            active_batch_committed_paths: HashSet::new(),
            last_delegation_touched_paths: Vec::new(),
        };

        session.ensure_seeded_workspace()?;
        let _ = session.build_full_state_snapshot_json();
        Ok(session)
    }

    /// Creates an isolated in-memory `SpellCoreSession` (`":memory:"`) for unit and integration tests.
    pub fn new_in_memory() -> Result<Self, String> {
        Self::new(Some(":memory:"))
    }

    /// Creates a session that talks to an injected [`EngineTransport`] test double instead of HTTP.
    pub fn new_with_transport(
        db_path: Option<&str>,
        transport: Arc<dyn EngineTransport>,
    ) -> Result<Self, String> {
        let mut session = Self::new(db_path)?;
        session.transport = transport;
        session.uses_custom_transport = true;
        if session.model_id.is_empty() {
            session.model_id = "test-model".to_string();
        }
        Ok(session)
    }

    /// The transport used for model/completion calls.
    pub fn active_transport(&self) -> &dyn EngineTransport {
        self.transport.as_ref()
    }

    /// Returns `true` when an HTTP(S)/Unix endpoint base URL is configured.
    pub fn is_endpoint_configured(&self) -> bool {
        !self.http_transport.base_url().trim().is_empty()
    }

    /// Returns `subagent_model_id` if non-empty, otherwise falls back to `model_id`.
    pub fn effective_subagent_model_id(&self) -> String {
        let sub = self.subagent_model_id.trim();
        if !sub.is_empty() {
            sub.to_string()
        } else {
            self.model_id.trim().to_string()
        }
    }

    fn persist_endpoint_config(&self) {
        let cfg = PersistedEndpointConfig {
            base_url: self.http_transport.base_url(),
            model_id: self.model_id.clone(),
            subagent_model_id: self.subagent_model_id.clone(),
        };
        if let Ok(raw) = serde_json::to_string(&cfg) {
            let _ = self
                .storage
                .put_kv_document("settings", "endpoint_config", &raw);
        }
    }

    /// Configures the OpenAI-compatible endpoint URL (allowing `""` to unconfigure),
    /// optional API key (in memory only — never persisted in SQLite), optional `model_id`,
    /// and optional `subagent_model_id`.
    pub fn configure_endpoint(
        &mut self,
        base_url: Option<&str>,
        api_key: Option<Option<&str>>,
        model_id: Option<&str>,
        subagent_model_id: Option<&str>,
    ) -> Result<(), String> {
        let mut endpoint_changed = false;
        if let Some(raw_url) = base_url {
            let trimmed = raw_url.trim();
            if trimmed.is_empty() {
                self.http_transport.set_base_url("");
                endpoint_changed = true;
            } else {
                let parsed = crate::transport::normalize_and_parse_endpoint(trimmed)?;
                self.http_transport
                    .set_base_url(parsed.normalized_base_url());
                endpoint_changed = true;
            }
        }
        if let Some(key_opt) = api_key {
            self.http_transport.set_api_key_override(
                key_opt
                    .map(str::trim)
                    .filter(|k| !k.is_empty())
                    .map(str::to_string),
            );
            endpoint_changed = true;
        }
        if let Some(m) = model_id {
            self.model_id = m.trim().to_string();
        }
        if let Some(sm) = subagent_model_id {
            self.subagent_model_id = sm.trim().to_string();
        }

        if endpoint_changed {
            *self.engine_status.lock() = EngineStatusCache::default();
            self.last_prefix_snapshot = None;
        }
        self.persist_endpoint_config();
        Ok(())
    }

    /// Legacy compatibility wrapper around [`Self::configure_endpoint`].
    pub fn configure_engine(&mut self, base_url: &str, api_key: Option<&str>) -> Result<(), String> {
        self.configure_endpoint(Some(base_url), Some(api_key), None, None)
    }

    /// Enables or disables optional LLM-backed Gatekeeper Critic review, persisting in SQLite.
    pub fn set_critic_enabled(&mut self, enabled: bool) -> Result<(), String> {
        self.critic_enabled = enabled;
        let val = if enabled { "true" } else { "false" };
        self.storage
            .put_kv_document("settings", "critic_enabled", val)
    }

    /// Probes `GET /models` against either explicit `base_url_override`/`api_key_override`
    /// or the currently configured transport, caching discovered models, latency, and online status.
    pub fn fetch_models(
        &self,
        base_url_override: Option<&str>,
        api_key_override: Option<&str>,
    ) -> Result<(Vec<ModelDescriptor>, u64), String> {
        let start = Instant::now();
        let probe_res = if let Some(raw_url) = base_url_override
            .map(str::trim)
            .filter(|s| !s.is_empty())
        {
            let parsed = crate::transport::normalize_and_parse_endpoint(raw_url)?;
            let temp = HttpTransport::new(parsed.normalized_base_url(), self.locator.clone());
            if let Some(k) = api_key_override {
                temp.set_api_key_override(
                    Some(k.trim().to_string()).filter(|s| !s.is_empty()),
                );
            } else {
                temp.set_api_key_override(self.http_transport.api_key_override());
            }
            temp.list_models()
        } else {
            if !self.uses_custom_transport && !self.is_endpoint_configured() {
                return Err(
                    "Configure an API endpoint URL in Settings → Engine before fetching models."
                        .to_string(),
                );
            }
            self.active_transport().list_models()
        };
        let latency_ms = start.elapsed().as_millis() as u64;

        let mut cache = self.engine_status.lock();
        cache.refreshed_at_unix_s = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs_f64())
            .unwrap_or(0.0);

        match probe_res {
            Ok(models) => {
                cache.available_models = models.clone();
                cache.endpoint_latency_ms = Some(latency_ms);
                cache.last_error = None;
                let mut tel = cache.telemetry.clone().unwrap_or_default();
                tel.engine_online = true;
                tel.api_schema_version = "openai.v1".to_string();
                if tel.engine_version.is_empty() {
                    tel.engine_version = if !self.model_id.is_empty() {
                        self.model_id.clone()
                    } else {
                        models
                            .first()
                            .map(|m| m.id.clone())
                            .unwrap_or_else(|| "openai-compatible".to_string())
                    };
                }
                tel.decode_tok_s = cache.decode_tok_s;
                tel.decode_tok_s_ewma = cache.decode_tok_s_ewma;
                cache.telemetry = Some(tel);
                Ok((models, latency_ms))
            }
            Err(e) => {
                let err_str = e.to_string();
                cache.telemetry = None;
                cache.endpoint_latency_ms = None;
                cache.last_error = Some(err_str.clone());
                Err(err_str)
            }
        }
    }

    /// Probes the configured OpenAI-compatible endpoint via `GET /models` and refreshes local SQLite timers.
    /// When no endpoint is configured (`base_url == ""`), records offline with `last_error = None`
    /// and performs ZERO network I/O.
    pub fn refresh_engine_status(&self) {
        let local_timers = self.storage.list_timers(None, false).unwrap_or_default();
        if !self.uses_custom_transport && !self.is_endpoint_configured() {
            let mut cache = self.engine_status.lock();
            cache.telemetry = None;
            cache.endpoint_latency_ms = None;
            cache.last_error = None;
            cache.timers = local_timers;
            cache.refreshed_at_unix_s = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs_f64())
                .unwrap_or(0.0);
            return;
        }

        let start = Instant::now();
        let models_res = self.active_transport().list_models();
        let latency_ms = start.elapsed().as_millis() as u64;

        let mut cache = self.engine_status.lock();
        cache.timers = local_timers;
        match models_res {
            Ok(models) => {
                cache.available_models = models.clone();
                cache.endpoint_latency_ms = Some(latency_ms);
                cache.last_error = None;
                let mut tel = cache.telemetry.clone().unwrap_or_default();
                tel.engine_online = true;
                tel.api_schema_version = "openai.v1".to_string();
                if tel.engine_version.is_empty() {
                    tel.engine_version = if !self.model_id.is_empty() {
                        self.model_id.clone()
                    } else {
                        models
                            .first()
                            .map(|m| m.id.clone())
                            .unwrap_or_else(|| "openai-compatible".to_string())
                    };
                }
                tel.decode_tok_s = cache.decode_tok_s;
                tel.decode_tok_s_ewma = cache.decode_tok_s_ewma;
                cache.telemetry = Some(tel);
            }
            Err(e) => {
                cache.telemetry = None;
                cache.endpoint_latency_ms = None;
                cache.last_error = Some(e.to_string());
            }
        }
        cache.refreshed_at_unix_s = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs_f64())
            .unwrap_or(0.0);
    }

    fn record_main_turn_telemetry(&self, summary: &ChatTurnSummary, turn_latency_ms: u64) {
        let mut cache = self.engine_status.lock();
        let measured_tps = if summary.context_advice.decode_tok_s > 0.0 {
            summary.context_advice.decode_tok_s
        } else {
            summary
                .usage
                .generation_tokens_per_second
                .unwrap_or(0.0)
        };
        if measured_tps.is_finite() && measured_tps > 0.0 {
            cache.decode_tok_s = measured_tps;
            cache.decode_tok_s_ewma = if cache.decode_tok_s_ewma <= 0.0 {
                measured_tps
            } else {
                0.3 * measured_tps + 0.7 * cache.decode_tok_s_ewma
            };
        }
        cache.total_prompt_tokens = cache
            .total_prompt_tokens
            .saturating_add(summary.usage.prompt_tokens);
        cache.total_completion_tokens = cache
            .total_completion_tokens
            .saturating_add(summary.usage.completion_tokens);
        let cached = summary
            .usage
            .prompt_tokens_details
            .as_ref()
            .map(|d| d.cached_tokens)
            .unwrap_or(0);
        cache.total_cached_tokens = cache.total_cached_tokens.saturating_add(cached);
        cache.endpoint_latency_ms = Some(turn_latency_ms);
        cache.last_error = None;

        let mut tel = cache.telemetry.clone().unwrap_or_default();
        tel.engine_online = true;
        tel.api_schema_version = "openai.v1".to_string();
        if !summary.model.trim().is_empty() {
            tel.engine_version = summary.model.clone();
        } else if !self.model_id.is_empty() {
            tel.engine_version = self.model_id.clone();
        }
        tel.decode_tok_s = cache.decode_tok_s;
        tel.decode_tok_s_ewma = cache.decode_tok_s_ewma;
        cache.telemetry = Some(tel);
        cache.refreshed_at_unix_s = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs_f64())
            .unwrap_or(0.0);
    }

    pub(crate) fn record_live_subagent_completion(&self, resp: &SubagentCompletionResponse) {
        let mut cache = self.engine_status.lock();
        cache.saw_live_subagent_completion = true;
        if resp.decode_tok_s.is_finite() && resp.decode_tok_s > 0.0 {
            cache.subagent_decode_tok_s = resp.decode_tok_s;
        }
        cache.total_prompt_tokens = cache
            .total_prompt_tokens
            .saturating_add(resp.prompt_tokens as u64);
        cache.total_completion_tokens = cache
            .total_completion_tokens
            .saturating_add(resp.completion_tokens as u64);
        cache.total_cached_tokens = cache
            .total_cached_tokens
            .saturating_add(resp.cached_tokens as u64);
        if resp.prefix_cache_reused || resp.cached_tokens > 0 {
            cache.prefix_cache_reuses = cache.prefix_cache_reuses.saturating_add(1);
        }
    }

    /// Restores the selection from storage. Nothing is seeded: projects are folders the user
    /// adds, and each project gets its first conversation when it is added.
    fn ensure_seeded_workspace(&mut self) -> Result<(), String> {
        // One-time migration: remove only known legacy simulator-seeded project and thread IDs
        // from pre-production builds. Never inspect or match user message content.
        if self
            .storage
            .get_kv_document("migrations", "purge_legacy_simulator_seeds_v1")?
            .is_none()
        {
            const LEGACY_PROJECT_IDS: &[&str] = &[
                "proj-beatrix-ide",
            ];
            const LEGACY_THREAD_IDS: &[&str] = &[
                "thread-main-01",
                "thread-1a1a4017e7f-0002",
            ];
            for pid in LEGACY_PROJECT_IDS {
                let _ = self.storage.delete_project(pid);
            }
            for tid in LEGACY_THREAD_IDS {
                let _ = self.storage.delete_thread(tid);
            }
            for t in self.storage.list_threads(None)? {
                if LEGACY_PROJECT_IDS.contains(&t.project_id.as_str()) {
                    let _ = self.storage.delete_thread(&t.id);
                }
            }
            self.storage
                .put_kv_document("migrations", "purge_legacy_simulator_seeds_v1", "1")?;
        }
        let projects = self.storage.list_projects()?;
        self.selected_project_id = projects.first().map(|p| p.id.clone());
        if let Some(first) = projects.first() {
            self.workspace_root = std::path::PathBuf::from(&first.root_path);
        }
        let threads = self
            .storage
            .list_threads(self.selected_project_id.as_deref())?;
        self.selected_thread_id = threads.first().map(|t| t.id.clone());
        Ok(())
    }

}

mod critic;
mod snapshot_ffi;
mod subagents;
mod tool_dispatch;
mod turn_runner;
mod workspace;
#[cfg(test)]
mod tests;

use critic::default_max_critic_bounces;
pub use critic::{
    create_git_loop_snapshot, load_loops_manifest, rollback_to_git_loop_snapshot,
    save_loops_manifest, CriticVerdict, LoopOrchestratorConfig, LoopState, ProposalEditHunk,
    SubagentCodingProposal,
};
#[allow(unused_imports)]
pub(crate) use critic::{
    repair_lenient_json_text, sanitize_prefill_context_body, strip_think_blocks,
};
#[allow(unused_imports)]
pub(crate) use turn_runner::{
    truncate_prefix_before_untrusted_web, wire_message_contains_untrusted_web,
};
