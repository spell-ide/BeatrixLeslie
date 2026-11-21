//! Autonomous Multi-Step Coding Subagent (`delegate_coding_subagent`):
//! Protocol Router, Sticky Option A -> Option B Fallback State Machine, Option B JSON Action Parser,
//! 9-Tool Subagent Allowlist & Argument Validator, `DelegationOverlay` Shadow Filesystem & Search
//! Helpers, `DelegationUndoRecord` Transactional Restore, and `<subagent_coding_result>` Metadata.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fs;
use std::path::{Component, Path, PathBuf};

use regex::RegexBuilder;
use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};
use walkdir::WalkDir;

use crate::contract::{WireToolCall, WireToolFunction};
use crate::tools::{
    canonicalize_target_path, enforce_path_guardrail, enforce_protected_globs,
    enforce_research_cache_write_guard, FileSnapshot, StagedFileMutation, ToolError, TurnPinCache,
    WorkspaceSnapshotStore,
};

// ============================================================================
// Constants & Step Budget Helpers
// ============================================================================

/// Default step budget for an autonomous `delegate_coding_subagent` invocation.
pub const DEFAULT_DELEGATION_MAX_STEPS: usize = 12;

/// Hard upper bound on `max_steps` for a single `delegate_coding_subagent` invocation.
pub const MAX_DELEGATION_MAX_STEPS: usize = 24;

/// Maximum retained per-session `DelegationUndoRecord` entries for one-click Undo.
pub const MAX_DELEGATION_UNDO_RECORDS: usize = 64;

/// HTML comment prefix carrying machine-readable [`DelegationResultMeta`] JSON for UI cards.
pub const DELEGATION_META_COMMENT_PREFIX: &str = "<!-- spell_delegation_meta: ";

/// Exact 9-tool allowlist exposed inside `delegate_coding_subagent`
/// (shared identically across Option A native tool calls and Option B JSON actions).
pub const SUBAGENT_ALLOWED_TOOLS: [&str; 9] = [
    "read_file",
    "grep",
    "find_path",
    "outline_symbols",
    "get_symbol_body",
    "edit_file",
    "write_file",
    "create_directory",
    "finish",
];

/// Known parent/orchestrator tools excluded from the subagent subset (`PERMISSION_DENIED` in-loop
/// without counting as a malformed syntax step).
pub const SUBAGENT_DISALLOWED_PARENT_TOOLS: [&str; 11] = [
    "terminal",
    "delete_path",
    "move_path",
    "copy_path",
    "fetch",
    "schedule_wakeup",
    "tag",
    "prefill_context_subagent",
    "delegate_coding_subagent",
    "apply_subagent_proposal",
    "list_directory",
];

/// Clamps an optional caller-supplied `max_steps` parameter to `1..=MAX_DELEGATION_MAX_STEPS`,
/// defaulting to [`DEFAULT_DELEGATION_MAX_STEPS`] (`12`).
pub fn clamp_delegation_max_steps(raw: Option<u64>) -> usize {
    raw.map(|n| (n as usize).clamp(1, MAX_DELEGATION_MAX_STEPS))
        .unwrap_or(DEFAULT_DELEGATION_MAX_STEPS)
}

// ============================================================================
// Protocol & Mode Enums (`SubagentProtocol`, `ActiveStepProtocol`, `DelegationMode`)
// ============================================================================

/// User/session setting or per-call override controlling which action protocol
/// `delegate_coding_subagent` uses to drive the coding subagent.
#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum SubagentProtocol {
    /// Starts with Option A (`Native` streaming tool calls) and falls back stickily to
    /// Option B (`Json`) if content parses as a JSON action or after 2 consecutive or cumulative
    /// malformed native steps.
    #[default]
    Auto,
    /// Starts with Option A (`Native` streaming tool calls), with sticky safety fallback on
    /// JSON-in-content or 2 consecutive or cumulative malformed native steps (§2).
    Native,
    /// Runs Option B (`Json` single-action JSON objects) from Step 0.
    Json,
}

impl SubagentProtocol {
    /// Parses a wire/settings string (`"auto"`, `"native"`, `"json"`) case-insensitively,
    /// defaulting unknown or empty values to [`SubagentProtocol::Auto`].
    pub fn from_wire(raw: &str) -> Self {
        match raw.trim().to_ascii_lowercase().as_str() {
            "native" | "option_a" | "tool_calls" | "tools" => Self::Native,
            "json" | "option_b" | "json_action" => Self::Json,
            _ => Self::Auto,
        }
    }

    /// Canonical wire string (`"auto"`, `"native"`, `"json"`).
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Auto => "auto",
            Self::Native => "native",
            Self::Json => "json",
        }
    }
}

/// Concrete step execution protocol active at a given step inside a delegation loop.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ActiveStepProtocol {
    /// Option A: OpenAI-compatible streaming native `tool_calls`.
    Native,
    /// Option B: Single JSON object (`{"action": "<tool>", ...}`) in assistant text content.
    Json,
}

impl ActiveStepProtocol {
    /// Resolves the initial step protocol from the session `setting` and optional per-call
    /// `arg_override` (`args["protocol"]`).
    pub fn initial(setting: SubagentProtocol, arg_override: Option<&str>) -> Self {
        let effective = match arg_override.map(str::trim).filter(|s| !s.is_empty()) {
            Some(raw) => match raw.to_ascii_lowercase().as_str() {
                "native" | "option_a" | "tool_calls" | "tools" => SubagentProtocol::Native,
                "json" | "option_b" | "json_action" => SubagentProtocol::Json,
                "auto" => SubagentProtocol::Auto,
                _ => setting,
            },
            None => setting,
        };
        match effective {
            SubagentProtocol::Auto | SubagentProtocol::Native => Self::Native,
            SubagentProtocol::Json => Self::Json,
        }
    }

    /// Canonical wire string (`"native"`, `"json"`).
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Native => "native",
            Self::Json => "json",
        }
    }
}

/// Execution mode for `delegate_coding_subagent`.
#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum DelegationMode {
    /// Multi-step autonomous coding loop + per-write Gatekeeper Critic + transactional auto-commit.
    #[default]
    Auto,
    /// Legacy single-shot non-mutating proposal synthesis (`<subagent_coding_proposal proposal_id="prop-N">`).
    Propose,
}

impl DelegationMode {
    /// Resolves [`DelegationMode`] from tool `args` and `instruction`:
    /// - Returns [`DelegationMode::Propose`] when `args["mode"]` is `"propose"` / `"proposal"`,
    ///   **OR** when `args["mode"]` is omitted/empty and `instruction` explicitly asks for a
    ///   single-shot JSON proposal hunk containing both `"\"edits\""` and `"\"old_text\""`
    ///   (preserving 100% compatibility with `live_engine_contract.rs` Test #8).
    /// - Otherwise returns [`DelegationMode::Auto`] (also accepting `"auto"` / `"execute"` / `"autonomous"`).
    pub fn from_args_and_instruction(args: &Value, instruction: &str) -> Self {
        if let Some(raw_mode) = args
            .get("mode")
            .and_then(|v| v.as_str())
            .map(str::trim)
            .filter(|s| !s.is_empty())
        {
            return match raw_mode.to_ascii_lowercase().as_str() {
                "propose" | "proposal" => Self::Propose,
                "auto" | "execute" | "autonomous" => Self::Auto,
                _ => Self::Auto,
            };
        }

        if instruction.contains("\"edits\"") && instruction.contains("\"old_text\"") {
            return Self::Propose;
        }

        Self::Auto
    }

    /// Canonical wire string (`"auto"`, `"propose"`).
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Auto => "auto",
            Self::Propose => "propose",
        }
    }
}

// ============================================================================
// Sticky Fallback State Machine (`ProtocolFallbackTracker`)
// ============================================================================

/// Number of consecutive or cumulative malformed Option A native steps that trigger a sticky
/// fallback to Option B (`Json`).
pub const MALFORMED_STEP_FALLBACK_THRESHOLD: usize = 2;

/// Per-delegation sticky protocol router implementing §2 of `subagent_autonomous_coding_design.md`.
///
/// Starts in `ActiveStepProtocol::initial(setting, arg_override)` and transitions stickily from
/// `Native` (Option A) to `Json` (Option B) when either:
/// 1. A native step produces no `tool_calls`, and its text content parses as a valid Option B JSON action.
/// 2. Two (`>= 2`) consecutive or cumulative native steps are malformed (bad JSON args, unknown tool,
///    missing required field, or leaked/unclosed `<tool_call>` syntax).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProtocolFallbackTracker {
    pub active: ActiveStepProtocol,
    pub consecutive_malformed: usize,
    pub total_malformed: usize,
    pub fallback_triggered: bool,
}

impl ProtocolFallbackTracker {
    /// Initializes a tracker for a new `delegate_coding_subagent` invocation.
    pub fn new(setting: SubagentProtocol, arg_override: Option<&str>) -> Self {
        Self {
            active: ActiveStepProtocol::initial(setting, arg_override),
            consecutive_malformed: 0,
            total_malformed: 0,
            fallback_triggered: false,
        }
    }

    /// Initializes a tracker directly from a resolved [`ActiveStepProtocol`].
    pub fn from_active(active: ActiveStepProtocol) -> Self {
        Self {
            active,
            consecutive_malformed: 0,
            total_malformed: 0,
            fallback_triggered: false,
        }
    }

    /// Records a valid Option A native tool-call step, resetting `consecutive_malformed` to `0`
    /// while preserving `total_malformed` so interleaved malformed steps still trigger fallback.
    pub fn on_valid_native_step(&mut self) {
        self.consecutive_malformed = 0;
    }

    /// Triggers an immediate sticky switch from Option A (`Native`) to Option B (`Json`) when a
    /// native step returned no `tool_calls` and its text content parsed as a valid JSON action.
    pub fn on_json_action_in_native_content(&mut self) {
        self.active = ActiveStepProtocol::Json;
        self.fallback_triggered = true;
        self.consecutive_malformed = 0;
    }

    /// Records a malformed Option A step and switches stickily to Option B (`Json`) once
    /// `consecutive_malformed >= MALFORMED_STEP_FALLBACK_THRESHOLD || total_malformed >= MALFORMED_STEP_FALLBACK_THRESHOLD`.
    /// Returns `true` (`switched_now`) on the exact step that triggered the transition.
    pub fn on_malformed_native_step(&mut self) -> bool {
        self.consecutive_malformed = self.consecutive_malformed.saturating_add(1);
        self.total_malformed = self.total_malformed.saturating_add(1);
        if self.active == ActiveStepProtocol::Native
            && (self.consecutive_malformed >= MALFORMED_STEP_FALLBACK_THRESHOLD
                || self.total_malformed >= MALFORMED_STEP_FALLBACK_THRESHOLD)
        {
            self.active = ActiveStepProtocol::Json;
            self.fallback_triggered = true;
            return true;
        }
        false
    }
}

// ============================================================================
// Subagent Tool Allowlist, Schema & Step Validation
// ============================================================================

/// Validation failure classes for a subagent tool call.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SubagentStepValidationError {
    /// Known parent tool outside the 9-tool subagent subset; returns an in-loop
    /// `PERMISSION_DENIED` tool error message to the subagent without counting as a
    /// malformed syntax step.
    DisallowedTool(String),
    /// Unknown/empty tool name, invalid/non-object JSON arguments, or missing/empty required fields.
    MalformedStep(String),
}

impl SubagentStepValidationError {
    /// Returns `true` when this error counts toward `ProtocolFallbackTracker::on_malformed_native_step`.
    pub fn is_malformed_step(&self) -> bool {
        matches!(self, Self::MalformedStep(_))
    }

    /// Formats the error as a deterministic tool output string suitable for feeding back to the subagent.
    pub fn to_tool_error_message(&self) -> String {
        match self {
            Self::DisallowedTool(msg) => format!("PERMISSION_DENIED: {msg}"),
            Self::MalformedStep(msg) => format!("INVALID_ARGUMENTS: {msg}"),
        }
    }
}

impl std::fmt::Display for SubagentStepValidationError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::DisallowedTool(msg) => write!(f, "PERMISSION_DENIED: {msg}"),
            Self::MalformedStep(msg) => write!(f, "MALFORMED_STEP: {msg}"),
        }
    }
}

impl std::error::Error for SubagentStepValidationError {}

/// Returns `true` if `name` is one of the 9 tools permitted inside `delegate_coding_subagent`.
pub fn is_subagent_allowed_tool(name: &str) -> bool {
    SUBAGENT_ALLOWED_TOOLS.contains(&name.trim())
}

/// Returns `true` if `name` is a known parent tool explicitly denied to the coding subagent.
pub fn is_disallowed_parent_tool(name: &str) -> bool {
    SUBAGENT_DISALLOWED_PARENT_TOOLS.contains(&name.trim())
}

fn require_non_empty_trimmed_string(
    map: &Map<String, Value>,
    tool_name: &str,
    field: &str,
) -> Result<String, SubagentStepValidationError> {
    match map.get(field) {
        Some(Value::String(s)) if !s.trim().is_empty() => Ok(s.trim().to_string()),
        Some(Value::String(_)) => Err(SubagentStepValidationError::MalformedStep(format!(
            "Tool '{tool_name}' requires non-empty string field '{field}'."
        ))),
        Some(_) => Err(SubagentStepValidationError::MalformedStep(format!(
            "Tool '{tool_name}' field '{field}' must be a string."
        ))),
        None => Err(SubagentStepValidationError::MalformedStep(format!(
            "Tool '{tool_name}' missing required string field '{field}'."
        ))),
    }
}

/// Validates a subagent [`WireToolCall`] against [`SUBAGENT_ALLOWED_TOOLS`] and normalizes its
/// JSON object arguments, distinguishing [`SubagentStepValidationError::DisallowedTool`] from
/// [`SubagentStepValidationError::MalformedStep`].
pub fn validate_subagent_tool_call(
    tc: &WireToolCall,
) -> Result<Value, SubagentStepValidationError> {
    let name = tc.function.name.trim();
    if name.is_empty() {
        return Err(SubagentStepValidationError::MalformedStep(
            "Tool call is missing function name.".to_string(),
        ));
    }

    if is_disallowed_parent_tool(name) {
        return Err(SubagentStepValidationError::DisallowedTool(format!(
            "Tool '{name}' is disabled inside delegate_coding_subagent (no shell, destructive path ops, network fetch, or nested delegation). Allowed tools: {}.",
            SUBAGENT_ALLOWED_TOOLS.join(", ")
        )));
    }

    if !is_subagent_allowed_tool(name) {
        return Err(SubagentStepValidationError::MalformedStep(format!(
            "Unknown subagent tool '{name}'. Allowed tools: {}.",
            SUBAGENT_ALLOWED_TOOLS.join(", ")
        )));
    }

    let parsed: Value = serde_json::from_str(&tc.function.arguments).map_err(|e| {
        SubagentStepValidationError::MalformedStep(format!(
            "Tool '{name}' arguments failed JSON parsing: {e}"
        ))
    })?;

    let Value::Object(mut map) = parsed else {
        return Err(SubagentStepValidationError::MalformedStep(format!(
            "Tool '{name}' arguments must be a JSON object."
        )));
    };

    match name {
        "read_file" => {
            let path = require_non_empty_trimmed_string(&map, name, "path")?;
            map.insert("path".to_string(), Value::String(path));
        }
        "grep" => {
            let raw_pattern = if let Some(val) = map.get("pattern") {
                Some(("pattern", val))
            } else if let Some(val) = map.get("query") {
                Some(("query", val))
            } else {
                map.get("regex").map(|val| ("regex", val))
            };
            let pattern = match raw_pattern {
                Some((_, Value::String(s))) if !s.trim().is_empty() => s.clone(),
                Some((key, Value::String(_))) => {
                    return Err(SubagentStepValidationError::MalformedStep(format!(
                        "Tool 'grep' requires non-empty string '{key}'."
                    )))
                }
                Some((key, _)) => {
                    return Err(SubagentStepValidationError::MalformedStep(format!(
                        "Tool 'grep' field '{key}' must be a string."
                    )))
                }
                None => {
                    return Err(SubagentStepValidationError::MalformedStep(
                        "Tool 'grep' missing required non-empty string field 'pattern' (or alias 'query')."
                            .to_string(),
                    ))
                }
            };
            map.insert("pattern".to_string(), Value::String(pattern));
        }
        "find_path" => {
            let raw_pat = if let Some(val) = map.get("pattern") {
                Some(("pattern", val))
            } else if let Some(val) = map.get("needle") {
                Some(("needle", val))
            } else {
                map.get("glob").map(|val| ("glob", val))
            };
            let normalized = match raw_pat {
                Some((_, Value::String(s))) => s.clone(),
                Some((key, _)) => {
                    return Err(SubagentStepValidationError::MalformedStep(format!(
                        "Tool 'find_path' field '{key}' must be a string."
                    )))
                }
                None => {
                    return Err(SubagentStepValidationError::MalformedStep(
                        "Tool 'find_path' requires string field 'pattern' (or 'needle' / 'glob'; use \"\" to list all files)."
                            .to_string(),
                    ))
                }
            };
            map.insert("pattern".to_string(), Value::String(normalized));
        }
        "outline_symbols" => {
            let path = require_non_empty_trimmed_string(&map, name, "path")?;
            map.insert("path".to_string(), Value::String(path));
        }
        "get_symbol_body" => {
            let path = require_non_empty_trimmed_string(&map, name, "path")?;
            let sym_opt = map
                .get("symbol")
                .or_else(|| map.get("symbol_name"))
                .or_else(|| map.get("name"));
            let symbol = match sym_opt {
                Some(Value::String(s)) if !s.trim().is_empty() => s.trim().to_string(),
                Some(Value::String(_)) => {
                    return Err(SubagentStepValidationError::MalformedStep(
                        "Tool 'get_symbol_body' requires a non-empty 'symbol' string.".to_string(),
                    ))
                }
                Some(_) => {
                    return Err(SubagentStepValidationError::MalformedStep(
                        "Tool 'get_symbol_body' field 'symbol' must be a string.".to_string(),
                    ))
                }
                None => {
                    return Err(SubagentStepValidationError::MalformedStep(
                        "Tool 'get_symbol_body' missing required string field 'symbol' (or 'symbol_name' / 'name')."
                            .to_string(),
                    ))
                }
            };
            map.insert("path".to_string(), Value::String(path));
            map.insert("symbol".to_string(), Value::String(symbol));
        }
        "edit_file" => {
            let path = require_non_empty_trimmed_string(&map, name, "path")?;
            let has_valid_edits = match map.get("edits") {
                Some(Value::Array(arr)) if !arr.is_empty() => {
                    for (idx, item) in arr.iter().enumerate() {
                        let Some(obj) = item.as_object() else {
                            return Err(SubagentStepValidationError::MalformedStep(format!(
                                "Tool 'edit_file' edits[{idx}] must be an object with 'old_text' and 'new_text'."
                            )));
                        };
                        match (obj.get("old_text"), obj.get("new_text")) {
                            (Some(Value::String(old_t)), Some(Value::String(_new_t)))
                                if !old_t.is_empty() => {}
                            (Some(Value::String(_)), Some(Value::String(_))) => {
                                return Err(SubagentStepValidationError::MalformedStep(format!(
                                    "Tool 'edit_file' edits[{idx}] 'old_text' must be non-empty."
                                )))
                            }
                            _ => {
                                return Err(SubagentStepValidationError::MalformedStep(format!(
                                    "Tool 'edit_file' edits[{idx}] requires string 'old_text' and string 'new_text'."
                                )))
                            }
                        }
                    }
                    true
                }
                Some(Value::Array(_)) | None | Some(Value::Null) => false,
                Some(_) => {
                    return Err(SubagentStepValidationError::MalformedStep(
                        "Tool 'edit_file' field 'edits' must be an array.".to_string(),
                    ))
                }
            };

            if !has_valid_edits {
                match (map.get("old_text"), map.get("new_text")) {
                    (Some(Value::String(old_t)), Some(Value::String(_new_t)))
                        if !old_t.is_empty() => {}
                    (Some(Value::String(_)), Some(Value::String(_))) => {
                        return Err(SubagentStepValidationError::MalformedStep(
                            "Tool 'edit_file' field 'old_text' must be non-empty.".to_string(),
                        ))
                    }
                    _ => {
                        return Err(SubagentStepValidationError::MalformedStep(
                            "Tool 'edit_file' requires either a non-empty 'edits' array or string 'old_text' + string 'new_text'."
                                .to_string(),
                        ))
                    }
                }
            }
            map.insert("path".to_string(), Value::String(path));
        }
        "write_file" => {
            let path = require_non_empty_trimmed_string(&map, name, "path")?;
            match map.get("content") {
                Some(Value::String(_)) => {}
                Some(_) => {
                    return Err(SubagentStepValidationError::MalformedStep(
                        "Tool 'write_file' field 'content' must be a string.".to_string(),
                    ))
                }
                None => {
                    return Err(SubagentStepValidationError::MalformedStep(
                        "Tool 'write_file' missing required string field 'content'.".to_string(),
                    ))
                }
            }
            map.insert("path".to_string(), Value::String(path));
        }
        "create_directory" => {
            let path = require_non_empty_trimmed_string(&map, name, "path")?;
            map.insert("path".to_string(), Value::String(path));
        }
        "finish" => {
            let summary = match map.get("summary") {
                Some(Value::String(s)) if !s.trim().is_empty() => s.trim().to_string(),
                Some(Value::String(_)) | Some(Value::Null) | None => {
                    "Completed requested changes.".to_string()
                }
                Some(_) => {
                    return Err(SubagentStepValidationError::MalformedStep(
                        "Tool 'finish' optional field 'summary' must be a string.".to_string(),
                    ))
                }
            };
            if let Some(vn) = map.get("verification_notes") {
                if !vn.is_null() && !vn.is_string() {
                    return Err(SubagentStepValidationError::MalformedStep(
                        "Tool 'finish' optional field 'verification_notes' must be a string."
                            .to_string(),
                    ));
                }
            }
            map.insert("summary".to_string(), Value::String(summary));
        }
        _ => unreachable!("guarded by SUBAGENT_ALLOWED_TOOLS check"),
    }

    Ok(Value::Object(map))
}

/// Builds the 9-tool OpenAI function schema array for Option A native tool-calling steps on
/// coding subagent.
pub fn build_subagent_openai_tools_schema() -> Vec<Value> {
    vec![
        json!({
            "type": "function",
            "function": {
                "name": "read_file",
                "description": "Reads overlay-aware text content from a workspace file (defaults to lines 1..=1500 when start_line/end_line are omitted).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": { "type": "string", "description": "Workspace-relative file path." },
                        "start_line": { "type": "integer", "description": "Optional 1-indexed start line (inclusive)." },
                        "end_line": { "type": "integer", "description": "Optional 1-indexed end line (inclusive)." },
                        "expand_to_symbol": { "type": "boolean", "description": "Optional. Expand start_line..=end_line to enclosing symbol." }
                    },
                    "required": ["path"]
                }
            }
        }),
        json!({
            "type": "function",
            "function": {
                "name": "grep",
                "description": "Searches workspace files (including staged overlay edits) using a regular expression.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "pattern": { "type": "string", "description": "Regular expression pattern to search for." },
                        "query": { "type": "string", "description": "Alias for pattern." },
                        "path": { "type": "string", "description": "Optional directory or file path filter." },
                        "max_results": { "type": "integer", "description": "Maximum matches to return (default 100)." }
                    },
                    "required": ["pattern"]
                }
            }
        }),
        json!({
            "type": "function",
            "function": {
                "name": "find_path",
                "description": "Finds workspace file or directory paths (including staged overlay files) matching a glob or substring pattern.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "pattern": { "type": "string", "description": "Glob or filename substring pattern (use \"\" to list all files)." },
                        "glob": { "type": "string", "description": "Alias for pattern." },
                        "needle": { "type": "string", "description": "Alias for pattern." },
                        "path": { "type": "string", "description": "Optional base directory for search." }
                    },
                    "required": ["pattern"]
                }
            }
        }),
        json!({
            "type": "function",
            "function": {
                "name": "outline_symbols",
                "description": "Extracts structural function, method, struct, class, and impl declarations with 1-indexed line spans from a workspace file.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": { "type": "string", "description": "Workspace-relative source file path." }
                    },
                    "required": ["path"]
                }
            }
        }),
        json!({
            "type": "function",
            "function": {
                "name": "get_symbol_body",
                "description": "Returns the numbered source lines of a named function, method, struct, class, or impl in a workspace file.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": { "type": "string", "description": "Workspace-relative source file path." },
                        "symbol": { "type": "string", "description": "Symbol name (e.g. 'lookup_limit' or 'Type::method')." }
                    },
                    "required": ["path", "symbol"]
                }
            }
        }),
        json!({
            "type": "function",
            "function": {
                "name": "edit_file",
                "description": "Stages surgical old_text -> new_text replacements on an existing file and runs Gatekeeper Critic verification.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": { "type": "string", "description": "Workspace-relative file path to modify." },
                        "old_text": { "type": "string", "description": "Unique exact text block to replace (never include line-number gutters)." },
                        "new_text": { "type": "string", "description": "Replacement text block." },
                        "edits": {
                            "type": "array",
                            "description": "Optional ordered list of old_text -> new_text replacements.",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "old_text": { "type": "string" },
                                    "new_text": { "type": "string" }
                                },
                                "required": ["old_text", "new_text"]
                            }
                        },
                        "display_description": { "type": "string", "description": "Optional brief description of the edit." }
                    },
                    "required": ["path"]
                }
            }
        }),
        json!({
            "type": "function",
            "function": {
                "name": "write_file",
                "description": "Stages creation or full replacement of a workspace file and runs Gatekeeper Critic verification.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": { "type": "string", "description": "Destination workspace-relative file path." },
                        "content": { "type": "string", "description": "Complete UTF-8 text content to write." }
                    },
                    "required": ["path", "content"]
                }
            }
        }),
        json!({
            "type": "function",
            "function": {
                "name": "create_directory",
                "description": "Stages creation of a directory (and missing parent directories) inside the workspace overlay.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": { "type": "string", "description": "Workspace-relative directory path to create." }
                    },
                    "required": ["path"]
                }
            }
        }),
        json!({
            "type": "function",
            "function": {
                "name": "finish",
                "description": "Completes the coding delegation and transactionally commits all Gatekeeper Critic PASS-verified staged files to disk.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "summary": { "type": "string", "description": "Concise summary of all completed changes." },
                        "verification_notes": { "type": "string", "description": "Optional notes on edge cases or invariants verified." }
                    }
                }
            }
        }),
    ]
}

// ============================================================================
// Option B JSON Action Parser (`parse_option_b_json_action` & `contains_malformed_tool_attempt`)
// ============================================================================

fn find_ascii_xml_tag_ci(lower: &str, tag_prefix: &str) -> Option<usize> {
    let bytes = lower.as_bytes();
    let mut offset = 0usize;
    while offset <= lower.len() {
        let Some(rel) = lower[offset..].find(tag_prefix) else {
            break;
        };
        let idx = offset + rel;
        let after = idx + tag_prefix.len();
        if after == bytes.len()
            || matches!(bytes[after], b'>' | b' ' | b'\t' | b'\r' | b'\n' | b'/')
        {
            return Some(idx);
        }
        offset = after;
    }
    None
}

/// Strips `<think>...</think>` blocks (as well as orphan `</think>` prefixes and unclosed trailing
/// `<think>` blocks) using ASCII-lowercased byte indices verified on UTF-8 char boundaries.
pub fn strip_subagent_think_blocks(raw: &str) -> String {
    let mut out = raw.to_string();
    loop {
        let lower = out.to_ascii_lowercase();
        if let Some(open_idx) = find_ascii_xml_tag_ci(&lower, "<think") {
            let Some(rel_gt) = lower[open_idx..].find('>') else {
                if out.is_char_boundary(open_idx) {
                    out.truncate(open_idx);
                }
                break;
            };
            let body_start = open_idx + rel_gt + 1;
            if let Some(rel_close) = lower[body_start..].find("</think>") {
                let close_end = body_start + rel_close + "</think>".len();
                if out.is_char_boundary(open_idx) && out.is_char_boundary(close_end) {
                    out.replace_range(open_idx..close_end, "");
                    continue;
                }
            } else if out.is_char_boundary(open_idx) {
                out.truncate(open_idx);
                break;
            }
        }
        if let Some(bare_close) = lower.find("</think>") {
            let close_end = bare_close + "</think>".len();
            if out.is_char_boundary(close_end) {
                out.replace_range(0..close_end, "");
                continue;
            }
        }
        break;
    }
    out
}

fn extract_fenced_blocks(text: &str) -> Vec<String> {
    let mut blocks = Vec::new();
    let mut search_from = 0usize;
    while search_from < text.len() {
        let Some(fence_rel) = text[search_from..].find("```") else {
            break;
        };
        let after_ticks = search_from + fence_rel + 3;
        let Some(nl_rel) = text[after_ticks..].find('\n') else {
            // Unclosed single-line or truncated fence: capture remainder so broken JSON is still inspectable.
            let rem = text[after_ticks..].trim();
            if !rem.is_empty() {
                blocks.push(rem.to_string());
            }
            break;
        };
        let content_start = after_ticks + nl_rel + 1;
        if let Some(close_rel) = text[content_start..].find("```") {
            let content_end = content_start + close_rel;
            if text.is_char_boundary(content_start) && text.is_char_boundary(content_end) {
                let inner = text[content_start..content_end].trim();
                if !inner.is_empty() {
                    blocks.push(inner.to_string());
                }
            }
            search_from = content_end + 3;
        } else {
            if text.is_char_boundary(content_start) {
                let inner = text[content_start..].trim();
                if !inner.is_empty() {
                    blocks.push(inner.to_string());
                }
            }
            break;
        }
    }
    blocks
}

/// Extracts the first balanced `{...}` UTF-8 slice starting at or after `start_offset`.
fn extract_first_balanced_brace_object(text: &str, start_offset: usize) -> Option<&str> {
    if start_offset >= text.len() || !text.is_char_boundary(start_offset) {
        return None;
    }
    let slice = &text[start_offset..];
    let open_rel = slice.find('{')?;
    let obj_start = start_offset + open_rel;
    let mut depth = 0usize;
    let mut in_string = false;
    let mut escaped = false;

    for (rel_idx, ch) in text[obj_start..].char_indices() {
        if in_string {
            if escaped {
                escaped = false;
            } else if ch == '\\' {
                escaped = true;
            } else if ch == '"' {
                in_string = false;
            }
            continue;
        }
        match ch {
            '"' => in_string = true,
            '{' => depth += 1,
            '}' => {
                depth = depth.saturating_sub(1);
                if depth == 0 {
                    let obj_end = obj_start + rel_idx + ch.len_utf8();
                    return Some(&text[obj_start..obj_end]);
                }
            }
            _ => {}
        }
    }
    None
}

/// Returns `true` if `s` starts with `{` followed (after optional whitespace) by `"action"` or `"tool"`
/// as the first JSON object key.
fn starts_with_action_or_tool_json_header(s: &str) -> bool {
    let trimmed = s.trim_start();
    let Some(after_brace) = trimmed.strip_prefix('{') else {
        return false;
    };
    let after_ws = after_brace.trim_start();
    for key in ["\"action\"", "\"tool\""] {
        if let Some(after_key) = after_ws.strip_prefix(key) {
            if after_key.trim_start().starts_with(':') {
                return true;
            }
        }
    }
    false
}

fn find_action_or_tool_header_offset(text: &str) -> Option<usize> {
    for (idx, ch) in text.char_indices() {
        if ch == '{' && starts_with_action_or_tool_json_header(&text[idx..]) {
            return Some(idx);
        }
    }
    None
}

fn convert_option_b_object_to_tool_call(
    mut map: Map<String, Value>,
    step_idx: usize,
) -> Option<Result<WireToolCall, String>> {
    // CRITICAL COMPATIBILITY INVARIANT:
    // Only classify a JSON object as an Option B action if it has a top-level `"action"` or `"tool"` key!
    // Legacy proposal objects like `{"path": "src/math.rs", "edits": [...]}` MUST return `None`.
    let has_action_key = map.contains_key("action");
    let has_tool_key = map.contains_key("tool");
    if !has_action_key && !has_tool_key {
        return None;
    }

    let raw_action_val = map.remove("action").or_else(|| map.remove("tool"))?;
    let Some(action_name) = raw_action_val
        .as_str()
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(str::to_string)
    else {
        return Some(Err(
            "MALFORMED_STEP: Option B JSON object requires a non-empty string 'action' (or 'tool') key."
                .to_string(),
        ));
    };

    // Support nested `"arguments"` / `"args"` / `"parameters"` / `"params"` (either object or JSON-string)
    // as well as flat top-level parameter fields.
    let mut args_map = Map::new();
    for nested_key in ["arguments", "args", "parameters", "params"] {
        if let Some(nested_val) = map.remove(nested_key) {
            match nested_val {
                Value::Object(nested_obj) => {
                    for (k, v) in nested_obj {
                        args_map.insert(k, v);
                    }
                }
                Value::String(nested_str) => match serde_json::from_str::<Value>(&nested_str) {
                    Ok(Value::Object(nested_obj)) => {
                        for (k, v) in nested_obj {
                            args_map.insert(k, v);
                        }
                    }
                    Ok(_) => {
                        return Some(Err(format!(
                            "MALFORMED_STEP: Option B '{nested_key}' string must decode to a JSON object."
                        )));
                    }
                    Err(e) => {
                        return Some(Err(format!(
                            "MALFORMED_STEP: Option B '{nested_key}' failed JSON decoding: {e}"
                        )));
                    }
                },
                Value::Null => {}
                _ => {
                    return Some(Err(format!(
                        "MALFORMED_STEP: Option B '{nested_key}' must be a JSON object."
                    )));
                }
            }
        }
    }

    // Merge remaining flat keys (excluding envelope metadata keys `id` / `type` unless the tool itself uses them).
    for (k, v) in map {
        if matches!(k.as_str(), "id" | "type") && !args_map.is_empty() {
            continue;
        }
        if matches!(k.as_str(), "id" | "type") {
            continue;
        }
        args_map.entry(k).or_insert(v);
    }

    let raw_args_json =
        serde_json::to_string(&Value::Object(args_map)).unwrap_or_else(|_| "{}".to_string());
    let mut tc = WireToolCall {
        id: format!("sub_json_{step_idx}"),
        r#type: "function".to_string(),
        function: WireToolFunction {
            name: action_name,
            arguments: raw_args_json,
        },
        index: Some(0),
    };

    match validate_subagent_tool_call(&tc) {
        Ok(normalized) => {
            tc.function.arguments =
                serde_json::to_string(&normalized).unwrap_or(tc.function.arguments);
            Some(Ok(tc))
        }
        Err(err) => Some(Err(err.to_string())),
    }
}

/// Parses subagent text output (stripping `<think>...</think>` blocks first, then checking raw
/// trimmed JSON, fenced ```` ```json ... ``` ```` blocks, or inline `{ "action": ... }` objects)
/// into `Option<Result<WireToolCall, String>>`.
///
/// ### Critical Compatibility Invariant
/// Only classifies a JSON object as an Option B action when it contains a top-level `"action"` or
/// `"tool"` key. Legacy proposal payloads such as
/// `{"path": "src/math.rs", "edits": [{"old_text": "...", "new_text": "..."}]}` return `None`.
pub fn parse_option_b_json_action(
    content: &str,
    step_idx: usize,
) -> Option<Result<WireToolCall, String>> {
    let stripped = strip_subagent_think_blocks(content);
    let trimmed = stripped.trim();
    if trimmed.is_empty() {
        return None;
    }

    let mut candidates: Vec<String> = Vec::new();
    for fenced in extract_fenced_blocks(trimmed) {
        candidates.push(fenced);
    }
    if trimmed.starts_with('{') {
        candidates.push(trimmed.to_string());
        if let Some(balanced) = extract_first_balanced_brace_object(trimmed, 0) {
            if balanced != trimmed {
                candidates.push(balanced.to_string());
            }
        }
    } else if let Some(hdr_off) = find_action_or_tool_header_offset(trimmed) {
        if let Some(balanced) = extract_first_balanced_brace_object(trimmed, hdr_off) {
            candidates.push(balanced.to_string());
        } else {
            candidates.push(trimmed[hdr_off..].to_string());
        }
    }

    let mut saw_broken_action_json: Option<String> = None;

    for cand in &candidates {
        match serde_json::from_str::<Value>(cand) {
            Ok(Value::Object(map)) => {
                if let Some(res) = convert_option_b_object_to_tool_call(map, step_idx) {
                    return Some(res);
                }
            }
            Ok(_) => {}
            Err(parse_err) => {
                if starts_with_action_or_tool_json_header(cand)
                    && saw_broken_action_json.is_none()
                {
                    saw_broken_action_json = Some(format!(
                        "MALFORMED_STEP: Broken Option B JSON action syntax: {parse_err}"
                    ));
                }
            }
        }
    }

    saw_broken_action_json.map(Err)
}

/// Returns `true` when `<think>`-stripped content contains leaked `<tool_call`, `<function=`,
/// or broken JSON starting with `{"action"` / `{"tool"` that failed to parse.
pub fn contains_malformed_tool_attempt(content: &str) -> bool {
    let stripped = strip_subagent_think_blocks(content);
    let trimmed = stripped.trim();
    if trimmed.is_empty() {
        return false;
    }

    // If the entire trimmed body or any fenced block is already a valid JSON value without top-level
    // tool errors (e.g. a valid legacy proposal JSON whose string literals happen to mention
    // `<tool_call` or `{"action"`), do not falsely classify it as a broken attempt.
    if let Ok(Value::Object(ref map)) = serde_json::from_str::<Value>(trimmed) {
        if !map.contains_key("action") && !map.contains_key("tool") {
            return false;
        }
    }

    let lower = trimmed.to_ascii_lowercase();
    if lower.contains("<tool_call") || lower.contains("<function=") {
        return true;
    }

    for block in extract_fenced_blocks(trimmed) {
        if starts_with_action_or_tool_json_header(&block)
            && serde_json::from_str::<Value>(&block).is_err()
        {
            return true;
        }
    }

    if starts_with_action_or_tool_json_header(trimmed)
        && serde_json::from_str::<Value>(trimmed).is_err()
    {
        return true;
    }

    if let Some(hdr_off) = find_action_or_tool_header_offset(trimmed) {
        if let Some(balanced) = extract_first_balanced_brace_object(trimmed, hdr_off) {
            if serde_json::from_str::<Value>(balanced).is_err() {
                return true;
            }
        } else {
            return true;
        }
    }

    false
}

// ============================================================================
// `DelegationOverlay` & Overlay-Aware Search Helpers (`find_path` & `grep`)
// ============================================================================

/// Per-delegation transactional shadow overlay isolating uncommitted subagent writes, virtual
/// directories, `TurnPinCache` deduplication, and per-file Gatekeeper Critic bounce counters until
/// `finish` commits verified mutations to the parent workspace.
#[derive(Clone, Debug, Default)]
pub struct DelegationOverlay {
    pub store: WorkspaceSnapshotStore,
    pub pin_cache: TurnPinCache,
    pub touched_files: BTreeSet<PathBuf>,
    pub virtual_dirs: BTreeSet<PathBuf>,
    pub file_bounces: HashMap<PathBuf, u8>,
}

impl DelegationOverlay {
    /// Creates an empty [`DelegationOverlay`].
    pub fn new() -> Self {
        Self::default()
    }

    /// Forks a per-delegation overlay from `parent_store` so any already-staged parent files remain
    /// readable while isolating all subagent writes until transactional commit.
    pub fn fork_from(parent_store: &WorkspaceSnapshotStore) -> Self {
        Self {
            store: parent_store.clone(),
            pin_cache: TurnPinCache::new(),
            touched_files: BTreeSet::new(),
            virtual_dirs: BTreeSet::new(),
            file_bounces: HashMap::new(),
        }
    }

    /// Stages a file write inside `self.store`, records `canonical_path` in `self.touched_files`,
    /// and invalidates `self.pin_cache` for `canonical_path`.
    pub fn stage_file(
        &mut self,
        canonical_path: &Path,
        relative_path: &str,
        new_content: String,
    ) -> Result<StagedFileMutation, ToolError> {
        let mutation = self
            .store
            .stage_write(canonical_path, relative_path, new_content)?;
        self.touched_files.insert(canonical_path.to_path_buf());
        self.pin_cache.invalidate_path(canonical_path);
        Ok(mutation)
    }

    /// Records a staged directory creation in `self.virtual_dirs` after validating path guardrails.
    pub fn stage_virtual_dir(&mut self, canonical_dir: &Path) -> Result<(), ToolError> {
        enforce_path_guardrail(canonical_dir)?;
        enforce_research_cache_write_guard(canonical_dir)?;
        self.virtual_dirs.insert(canonical_dir.to_path_buf());
        Ok(())
    }

    /// Increments and returns the Gatekeeper Critic bounce count for `canonical_path`.
    pub fn record_critic_bounce(&mut self, canonical_path: &Path) -> u8 {
        let entry = self
            .file_bounces
            .entry(canonical_path.to_path_buf())
            .or_insert(0);
        *entry = entry.saturating_add(1);
        *entry
    }

    /// Returns the recorded Gatekeeper Critic bounce count for `canonical_path`.
    pub fn bounces_for(&self, canonical_path: &Path) -> u8 {
        self.file_bounces.get(canonical_path).copied().unwrap_or(0)
    }
}

fn normalize_path_preserving_tail(path: &Path) -> PathBuf {
    let mut tail_rev = Vec::new();
    let mut cur = path.to_path_buf();
    loop {
        if cur.exists() {
            if let Ok(canon) = fs::canonicalize(&cur) {
                let mut out = canon;
                for seg in tail_rev.into_iter().rev() {
                    out.push(seg);
                }
                return out;
            }
            break;
        }
        let mut comps = cur.components();
        match comps.next_back() {
            Some(Component::Normal(name)) => {
                tail_rev.push(name.to_os_string());
                cur = comps.as_path().to_path_buf();
                if cur.as_os_str().is_empty() {
                    break;
                }
            }
            _ => break,
        }
    }
    path.to_path_buf()
}

fn relativize_display_path(root: &Path, target: &Path) -> String {
    if let Ok(rel) = target.strip_prefix(root) {
        let s = rel.to_string_lossy().replace('\\', "/");
        if !s.is_empty() {
            return s;
        }
    }
    let norm_root = normalize_path_preserving_tail(root);
    let norm_target = normalize_path_preserving_tail(target);
    if let Ok(rel) = norm_target.strip_prefix(&norm_root) {
        let s = rel.to_string_lossy().replace('\\', "/");
        if !s.is_empty() {
            return s;
        }
    }
    target.to_string_lossy().replace('\\', "/")
}

fn is_under_search_base(norm_base: &Path, candidate: &Path) -> bool {
    let norm_cand = normalize_path_preserving_tail(candidate);
    norm_cand.starts_with(norm_base)
}

fn is_protected_or_guarded_path(path: &Path) -> bool {
    let raw = path.to_string_lossy();
    enforce_path_guardrail(path).is_err() || enforce_protected_globs(path, &raw).is_err()
}

fn should_skip_walk_dir_entry(path: &Path) -> bool {
    if is_protected_or_guarded_path(path) {
        return true;
    }
    matches!(
        path.file_name().and_then(|s| s.to_str()),
        Some(".git" | "target" | ".build" | "node_modules")
    )
}

fn compile_simple_glob_regex(pattern: &str) -> Option<regex::Regex> {
    if !pattern.contains('*') && !pattern.contains('?') && !pattern.contains('[') {
        return None;
    }
    let mut re = String::from("^");
    let chars: Vec<char> = pattern.chars().collect();
    let mut i = 0usize;
    while i < chars.len() {
        match chars[i] {
            '*' => {
                if i + 1 < chars.len() && chars[i + 1] == '*' {
                    i += 2;
                    if i < chars.len() && chars[i] == '/' {
                        i += 1;
                    }
                    re.push_str(".*");
                    continue;
                } else {
                    re.push_str("[^/]*");
                }
            }
            '?' => re.push('.'),
            '.' | '+' | '(' | ')' | '|' | '^' | '$' | '{' | '}' | '\\' => {
                re.push('\\');
                re.push(chars[i]);
            }
            c => re.push(c),
        }
        i += 1;
    }
    re.push('$');
    RegexBuilder::new(&re).case_insensitive(true).build().ok()
}

/// Overlay-aware `find_path` search under `search_base` (relative to workspace `root`), merging
/// on-disk entries with `overlay.touched_files`, `overlay.virtual_dirs`, and uncommitted staged
/// overlay entries, formatted with a `[find_path summary: ...]` footer.
pub fn find_paths_with_overlay(
    root: &Path,
    search_base: &Path,
    needle: &str,
    overlay: &DelegationOverlay,
    max_results: usize,
) -> String {
    if let Err(e) = enforce_path_guardrail(root) {
        return e.to_string();
    }
    if let Err(e) = enforce_protected_globs(root, &root.to_string_lossy()) {
        return e.to_string();
    }
    if let Err(e) = enforce_path_guardrail(search_base) {
        return e.to_string();
    }
    if let Err(e) = enforce_protected_globs(search_base, &search_base.to_string_lossy()) {
        return e.to_string();
    }
    let norm_root = normalize_path_preserving_tail(root);
    if !is_under_search_base(&norm_root, search_base) {
        return format!(
            "SECURITY_BLOCK: Search base '{}' escapes workspace root '{}'",
            search_base.display(),
            root.display()
        );
    }

    let cap = if max_results == 0 { 2_000 } else { max_results };
    let norm_base = normalize_path_preserving_tail(search_base);

    // Map normalized canonical path -> display string (deduplicates macOS `/var` vs `/private/var` seamlessly).
    let mut all_entries: BTreeMap<String, PathBuf> = BTreeMap::new();

    if search_base.exists() {
        for entry in WalkDir::new(search_base)
            .follow_links(false)
            .into_iter()
            .filter_entry(|e| e.depth() == 0 || !should_skip_walk_dir_entry(e.path()))
            .flatten()
        {
            if entry.depth() == 0 && search_base.is_dir() {
                continue;
            }
            let p = entry.path();
            if is_protected_or_guarded_path(p) {
                continue;
            }
            let disp = relativize_display_path(root, p);
            if enforce_protected_globs(p, &disp).is_err() {
                continue;
            }
            all_entries.entry(disp).or_insert_with(|| p.to_path_buf());
        }
    }

    let mut overlay_paths: Vec<PathBuf> = Vec::new();
    overlay_paths.extend(overlay.virtual_dirs.iter().cloned());
    overlay_paths.extend(overlay.touched_files.iter().cloned());
    for staged in overlay.store.staged_overlay.values() {
        if staged.status == "staged" || staged.status == "verified" {
            overlay_paths.push(staged.path.clone());
        }
    }

    for ov_path in overlay_paths {
        if is_protected_or_guarded_path(&ov_path) {
            continue;
        }
        let norm_ov = normalize_path_preserving_tail(&ov_path);
        if is_protected_or_guarded_path(&norm_ov) {
            continue;
        }
        if !is_under_search_base(&norm_base, &norm_ov) {
            continue;
        }
        // Also surface any newly implied intermediate parent directories under `norm_base`.
        let mut cur = norm_ov.as_path();
        loop {
            if cur == norm_base && norm_base != norm_ov {
                break;
            }
            if !cur.starts_with(&norm_base) {
                break;
            }
            if cur != norm_base || !search_base.is_dir() {
                let disp = relativize_display_path(root, cur);
                all_entries.entry(disp).or_insert_with(|| cur.to_path_buf());
            }
            let Some(parent) = cur.parent() else {
                break;
            };
            if parent == cur {
                break;
            }
            cur = parent;
        }
    }

    let trimmed_needle = needle.trim();
    let glob_re = compile_simple_glob_regex(trimmed_needle);
    let substr_lower = trimmed_needle.trim_matches('*').to_ascii_lowercase();

    let scanned = all_entries.len();
    let mut matched = Vec::new();
    let mut truncated = false;

    for (disp, full_path) in &all_entries {
        let file_name = full_path
            .file_name()
            .map(|f| f.to_string_lossy().into_owned())
            .unwrap_or_else(|| disp.clone());

        let is_match = if trimmed_needle.is_empty() || trimmed_needle == "*" {
            true
        } else if let Some(ref re) = glob_re {
            re.is_match(&file_name) || re.is_match(disp)
        } else {
            file_name.to_ascii_lowercase().contains(&substr_lower)
                || disp.to_ascii_lowercase().contains(&substr_lower)
        };

        if is_match {
            if matched.len() >= cap {
                truncated = true;
                continue;
            }
            matched.push(disp.clone());
        }
    }

    let footer = if truncated {
        format!(
            "[find_path summary: scanned {scanned} entries; showing first {} matches (truncated) — narrow the pattern or path]",
            matched.len()
        )
    } else {
        format!(
            "[find_path summary: scanned {scanned} entries, {} matches]",
            matched.len()
        )
    };
    matched.push(footer);
    matched.join("\n")
}

fn lookup_staged_overlay_content<'a>(
    overlay: &'a DelegationOverlay,
    norm_target: &Path,
) -> Option<&'a str> {
    if let Some(entry) = overlay.store.staged_overlay.get(norm_target) {
        if entry.status == "staged" || entry.status == "verified" {
            return Some(entry.staged_content.as_str());
        }
    }
    for (k, entry) in &overlay.store.staged_overlay {
        if (entry.status == "staged" || entry.status == "verified")
            && (normalize_path_preserving_tail(k) == norm_target
                || normalize_path_preserving_tail(&entry.path) == norm_target)
        {
            return Some(entry.staged_content.as_str());
        }
    }
    None
}

/// Overlay-aware `grep` search under `search_base` (relative to workspace `root`), substituting
/// staged overlay text for modified files and scanning newly staged files in `overlay.touched_files`
/// that do not exist on disk yet, formatted with a `[grep summary: ...]` footer.
pub fn grep_with_overlay(
    root: &Path,
    search_base: &Path,
    pattern: &str,
    max_results: usize,
    overlay: &DelegationOverlay,
) -> Result<String, String> {
    enforce_path_guardrail(root).map_err(|e| e.to_string())?;
    enforce_protected_globs(root, &root.to_string_lossy()).map_err(|e| e.to_string())?;
    enforce_path_guardrail(search_base).map_err(|e| e.to_string())?;
    enforce_protected_globs(search_base, &search_base.to_string_lossy())
        .map_err(|e| e.to_string())?;
    let norm_root = normalize_path_preserving_tail(root);
    if !is_under_search_base(&norm_root, search_base) {
        return Err(format!(
            "SECURITY_BLOCK: Search base '{}' escapes workspace root '{}'",
            search_base.display(),
            root.display()
        ));
    }

    if pattern.is_empty() {
        return Err("INVALID_ARGUMENTS: grep requires a non-empty 'pattern'.".to_string());
    }

    let (re, literal_fallback) = match regex::Regex::new(pattern) {
        Ok(r) => (r, false),
        Err(_) => (
            regex::Regex::new(&regex::escape(pattern))
                .map_err(|e| format!("Invalid regex '{pattern}': {e}"))?,
            true,
        ),
    };

    let cap = if max_results == 0 { 100 } else { max_results };
    let norm_base = normalize_path_preserving_tail(search_base);

    // Collect deduplicated candidate files mapped by relative display path -> normalized PathBuf.
    let mut candidate_files: BTreeMap<String, PathBuf> = BTreeMap::new();

    if search_base.is_file() {
        if !is_protected_or_guarded_path(search_base) && !is_protected_or_guarded_path(&norm_base) {
            let disp = relativize_display_path(root, search_base);
            if enforce_protected_globs(&norm_base, &disp).is_ok() {
                candidate_files.insert(disp, norm_base.clone());
            }
        }
    } else if search_base.is_dir() {
        for entry in WalkDir::new(search_base)
            .follow_links(false)
            .into_iter()
            .filter_entry(|e| e.depth() == 0 || !should_skip_walk_dir_entry(e.path()))
            .flatten()
        {
            if entry.file_type().is_file() {
                let p = entry.path();
                if is_protected_or_guarded_path(p) {
                    continue;
                }
                let norm_p = normalize_path_preserving_tail(p);
                if is_protected_or_guarded_path(&norm_p) {
                    continue;
                }
                let disp = relativize_display_path(root, p);
                if enforce_protected_globs(&norm_p, &disp).is_err() {
                    continue;
                }
                candidate_files.entry(disp).or_insert(norm_p);
            }
        }
    }

    // Merge staged/touched files from the overlay (covering newly staged files or virtual subdirs).
    for touched in &overlay.touched_files {
        if is_protected_or_guarded_path(touched) {
            continue;
        }
        let norm_t = normalize_path_preserving_tail(touched);
        if is_protected_or_guarded_path(&norm_t) {
            continue;
        }
        if norm_t == norm_base || is_under_search_base(&norm_base, &norm_t) {
            let disp = relativize_display_path(root, &norm_t);
            if enforce_protected_globs(&norm_t, &disp).is_ok() {
                candidate_files.entry(disp).or_insert(norm_t);
            }
        }
    }
    for staged in overlay.store.staged_overlay.values() {
        if staged.status != "staged" && staged.status != "verified" {
            continue;
        }
        if is_protected_or_guarded_path(&staged.path) {
            continue;
        }
        let norm_s = normalize_path_preserving_tail(&staged.path);
        if is_protected_or_guarded_path(&norm_s) {
            continue;
        }
        if norm_s == norm_base || is_under_search_base(&norm_base, &norm_s) {
            let disp = relativize_display_path(root, &norm_s);
            if enforce_protected_globs(&norm_s, &disp).is_ok() {
                candidate_files.entry(disp).or_insert(norm_s);
            }
        }
    }

    let mut matches = Vec::new();
    let mut files_scanned = 0usize;
    let mut unreadable = 0usize;
    let mut truncated = false;

    'file_scan: for (disp, norm_path) in &candidate_files {
        if is_protected_or_guarded_path(norm_path)
            || enforce_protected_globs(norm_path, disp).is_err()
        {
            continue;
        }
        let owned_disk;
        let text: &str = if let Some(staged_text) = lookup_staged_overlay_content(overlay, norm_path)
        {
            staged_text
        } else if let Some(snap) = overlay.store.snapshots.get(norm_path) {
            snap.content.as_str()
        } else {
            let bytes = match fs::read(norm_path) {
                Ok(b) => b,
                Err(_) => {
                    unreadable += 1;
                    continue;
                }
            };
            if bytes.contains(&0u8) {
                continue; // skip binary file
            }
            match String::from_utf8(bytes) {
                Ok(s) => {
                    owned_disk = s;
                    owned_disk.as_str()
                }
                Err(_) => {
                    unreadable += 1;
                    continue;
                }
            }
        };

        files_scanned += 1;
        for (idx, line) in text.lines().enumerate() {
            if re.is_match(line) {
                if matches.len() >= cap {
                    truncated = true;
                    break 'file_scan;
                }
                matches.push(format!("{disp}:{}:{line}", idx + 1));
            }
        }
    }

    let mut summary_parts = vec![format!("{} matches in {files_scanned} files", matches.len())];
    if literal_fallback {
        summary_parts.push("pattern was not a valid regex; searched literally".to_string());
    }
    if truncated {
        summary_parts.push(format!(
            "results truncated at {cap} matches — narrow the pattern or path"
        ));
    }
    if unreadable > 0 {
        summary_parts.push(format!(
            "{unreadable} file(s) skipped (unreadable or non-UTF-8)"
        ));
    }
    matches.push(format!("[grep summary: {}]", summary_parts.join("; ")));

    Ok(matches.join("\n"))
}

// ============================================================================
// `DelegationUndoRecord` & Transactional Undo Execution
// ============================================================================

/// Per-file prior disk state captured before an autonomous delegation commits to disk.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct DelegationFileRestore {
    pub canonical_path: PathBuf,
    pub relative_path: String,
    /// `Some(text)` when the file existed on disk prior to delegation; `None` when newly created
    /// by the delegation (removed from disk on undo).
    pub prior_disk_content: Option<String>,
}

/// Complete undo snapshot for a committed `delegate_coding_subagent` run.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct DelegationUndoRecord {
    pub undo_id: String,
    pub thread_id: String,
    pub timestamp: u64,
    pub project_root: PathBuf,
    pub git_snapshot_sha: Option<String>,
    pub files: Vec<DelegationFileRestore>,
    pub created_dirs: Vec<PathBuf>,
    pub undone: bool,
}

/// Walks every component prefix from `canon_root` down to `target` using `fs::symlink_metadata`
/// (`lstat` — ignoring `NotFound` for not-yet-existing components) and returns an error if any
/// ancestor or leaf component is a symlink.
pub fn verify_no_symlink_components_under_root(
    canon_root: &Path,
    target: &Path,
) -> Result<(), String> {
    if let Ok(root_meta) = fs::symlink_metadata(canon_root) {
        if root_meta.file_type().is_symlink() {
            return Err(format!(
                "SECURITY_VIOLATION: Symlink component detected at workspace root '{}'",
                canon_root.display()
            ));
        }
    }

    let rel = target.strip_prefix(canon_root).map_err(|_| {
        format!(
            "SECURITY_VIOLATION: Target '{}' is outside canonical workspace root '{}'",
            target.display(),
            canon_root.display()
        )
    })?;

    let mut cur = canon_root.to_path_buf();
    for comp in rel.components() {
        match comp {
            Component::CurDir => continue,
            Component::Normal(seg) => {
                cur.push(seg);
                match fs::symlink_metadata(&cur) {
                    Ok(meta) => {
                        if meta.file_type().is_symlink() {
                            return Err(format!(
                                "SECURITY_VIOLATION: Symlink component detected at '{}'",
                                cur.display()
                            ));
                        }
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                    Err(e) => {
                        return Err(format!(
                            "IO_ERROR: Failed inspecting path metadata for '{}': {e}",
                            cur.display()
                        ));
                    }
                }
            }
            Component::ParentDir | Component::RootDir | Component::Prefix(_) => {
                return Err(format!(
                    "SECURITY_VIOLATION: Disallowed traversal component in '{}'",
                    target.display()
                ));
            }
        }
    }
    Ok(())
}

fn reanchor_under_canon_root(
    project_root: &Path,
    canon_root: &Path,
    candidate: &Path,
) -> Result<PathBuf, String> {
    if candidate.starts_with(canon_root) {
        return Ok(candidate.to_path_buf());
    }
    if let Ok(rel) = candidate.strip_prefix(project_root) {
        for comp in rel.components() {
            match comp {
                Component::Normal(_) | Component::CurDir => {}
                Component::ParentDir | Component::RootDir | Component::Prefix(_) => {
                    return Err(format!(
                        "SECURITY_VIOLATION: Disallowed traversal component in '{}'",
                        candidate.display()
                    ));
                }
            }
        }
        return Ok(canon_root.join(rel));
    }
    Err(format!(
        "SECURITY_VIOLATION: Target '{}' is outside workspace root '{}'",
        candidate.display(),
        canon_root.display()
    ))
}

fn write_undo_file_nofollow(path: &Path, content: &[u8]) -> Result<(), String> {
    use std::io::Write;
    let mut opts = fs::OpenOptions::new();
    opts.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.custom_flags(libc::O_NOFOLLOW);
    }
    #[cfg(not(unix))]
    {
        if let Ok(meta) = fs::symlink_metadata(path) {
            if meta.file_type().is_symlink() {
                return Err(format!(
                    "SECURITY_VIOLATION: Symlink detected on restore of '{}'",
                    path.display()
                ));
            }
        }
    }
    let mut file = opts.open(path).map_err(|e| {
        #[cfg(unix)]
        if e.raw_os_error() == Some(libc::ELOOP) {
            return format!(
                "SECURITY_VIOLATION: Symlink detected on O_NOFOLLOW restore of '{}'",
                path.display()
            );
        }
        format!("IO_ERROR: Failed restoring '{}': {e}", path.display())
    })?;
    file.write_all(content)
        .map_err(|e| format!("IO_ERROR: Failed restoring '{}': {e}", path.display()))?;
    file.flush()
        .map_err(|e| format!("IO_ERROR: Failed flushing '{}': {e}", path.display()))
}

impl DelegationUndoRecord {
    /// Restores every file touched by this delegation to its prior disk state (or removes newly
    /// created files), prunes empty newly created directories in reverse depth order, updates
    /// `snapshot_store`, invalidates `pin_cache`, marks `self.undone = true`, and returns the
    /// restored workspace-relative file paths.
    pub fn execute_undo(
        &mut self,
        snapshot_store: &mut WorkspaceSnapshotStore,
        pin_cache: &mut TurnPinCache,
    ) -> Result<Vec<String>, String> {
        if self.undone {
            return Err(format!(
                "Delegation '{}' has already been undone.",
                self.undo_id
            ));
        }

        enforce_path_guardrail(&self.project_root).map_err(|e| e.to_string())?;
        if let Ok(root_meta) = fs::symlink_metadata(&self.project_root) {
            if root_meta.file_type().is_symlink() {
                return Err(format!(
                    "SECURITY_VIOLATION: Project root '{}' cannot be a symlink",
                    self.project_root.display()
                ));
            }
        }
        let canon_root = canonicalize_target_path(&self.project_root, &self.project_root)
            .map_err(|e| e.to_string())?;

        // 1. Pre-validate all file and directory targets before mutating disk.
        let mut validated_files: Vec<PathBuf> = Vec::with_capacity(self.files.len());
        for file in &self.files {
            enforce_path_guardrail(&file.canonical_path).map_err(|e| e.to_string())?;
            enforce_protected_globs(&file.canonical_path, &file.relative_path)
                .map_err(|e| e.to_string())?;
            enforce_research_cache_write_guard(&file.canonical_path).map_err(|e| e.to_string())?;
            if let Ok(meta) = fs::symlink_metadata(&file.canonical_path) {
                if meta.file_type().is_symlink() {
                    return Err(format!(
                        "SECURITY_VIOLATION: Symlink component detected at '{}'",
                        file.canonical_path.display()
                    ));
                }
            }
            let expected_canon =
                reanchor_under_canon_root(&self.project_root, &canon_root, &file.canonical_path)?;
            verify_no_symlink_components_under_root(&canon_root, &expected_canon)?;
            let resolved = canonicalize_target_path(&self.project_root, &file.canonical_path)
                .map_err(|e| e.to_string())?;
            if resolved != expected_canon {
                return Err(format!(
                    "SECURITY_VIOLATION: Undo target '{}' resolved to '{}' (expected '{}') via symlink",
                    file.canonical_path.display(),
                    resolved.display(),
                    expected_canon.display()
                ));
            }
            validated_files.push(expected_canon);
        }

        for dir in &self.created_dirs {
            enforce_path_guardrail(dir).map_err(|e| e.to_string())?;
            enforce_protected_globs(dir, &dir.to_string_lossy()).map_err(|e| e.to_string())?;
            enforce_research_cache_write_guard(dir).map_err(|e| e.to_string())?;
            if let Ok(meta) = fs::symlink_metadata(dir) {
                if meta.file_type().is_symlink() {
                    return Err(format!(
                        "SECURITY_VIOLATION: Symlink component detected at directory '{}'",
                        dir.display()
                    ));
                }
            }
            let expected_dir = reanchor_under_canon_root(&self.project_root, &canon_root, dir)?;
            verify_no_symlink_components_under_root(&canon_root, &expected_dir)?;
            let resolved_dir =
                canonicalize_target_path(&self.project_root, dir).map_err(|e| e.to_string())?;
            if resolved_dir != expected_dir {
                return Err(format!(
                    "SECURITY_VIOLATION: Undo directory '{}' resolved to '{}' (expected '{}') via symlink",
                    dir.display(),
                    resolved_dir.display(),
                    expected_dir.display()
                ));
            }
        }

        // 2. Restore prior file contents or delete newly created files.
        let mut restored_rel_paths = Vec::with_capacity(self.files.len());
        for (file, expected_canon) in self.files.iter().zip(validated_files.iter()) {
            verify_no_symlink_components_under_root(&canon_root, expected_canon)?;
            let norm_target = expected_canon.clone();
            match &file.prior_disk_content {
                Some(prior_content) => {
                    if let Some(parent) = expected_canon.parent() {
                        if !parent.as_os_str().is_empty() {
                            fs::create_dir_all(parent).map_err(|e| {
                                format!(
                                    "IO_ERROR: Failed creating parent directory '{}': {e}",
                                    parent.display()
                                )
                            })?;
                        }
                    }
                    verify_no_symlink_components_under_root(&canon_root, expected_canon)?;
                    write_undo_file_nofollow(expected_canon, prior_content.as_bytes())?;

                    snapshot_store.staged_overlay.retain(|k, v| {
                        normalize_path_preserving_tail(k) != norm_target
                            && normalize_path_preserving_tail(&v.path) != norm_target
                    });
                    let updated_snap = FileSnapshot::from_content(
                        file.canonical_path.clone(),
                        prior_content.clone(),
                    );
                    snapshot_store
                        .snapshots
                        .insert(file.canonical_path.clone(), updated_snap);
                }
                None => {
                    match fs::symlink_metadata(expected_canon) {
                        Ok(meta) => {
                            if meta.file_type().is_symlink() {
                                return Err(format!(
                                    "SECURITY_VIOLATION: Symlink component detected at '{}'",
                                    file.canonical_path.display()
                                ));
                            }
                            if meta.file_type().is_file() {
                                fs::remove_file(expected_canon).map_err(|e| {
                                    format!(
                                        "IO_ERROR: Failed removing newly created file '{}': {e}",
                                        file.canonical_path.display()
                                    )
                                })?;
                            }
                        }
                        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                        Err(e) => {
                            return Err(format!(
                                "IO_ERROR: Failed inspecting newly created file '{}': {e}",
                                file.canonical_path.display()
                            ));
                        }
                    }
                    snapshot_store.staged_overlay.retain(|k, v| {
                        normalize_path_preserving_tail(k) != norm_target
                            && normalize_path_preserving_tail(&v.path) != norm_target
                    });
                    snapshot_store
                        .snapshots
                        .retain(|k, _| normalize_path_preserving_tail(k) != norm_target);
                }
            }

            pin_cache.invalidate_path(&file.canonical_path);
            if expected_canon != &file.canonical_path {
                pin_cache.invalidate_path(expected_canon);
            }
            let joined_rel = self.project_root.join(&file.relative_path);
            if joined_rel != file.canonical_path {
                pin_cache.invalidate_path(&joined_rel);
            }
            restored_rel_paths.push(file.relative_path.clone());
        }

        // 3. Prune empty newly created directories in reverse depth order (deepest first).
        let mut dirs_sorted = self.created_dirs.clone();
        dirs_sorted.sort_by(|a, b| {
            b.components()
                .count()
                .cmp(&a.components().count())
                .then_with(|| b.cmp(a))
        });
        for dir in dirs_sorted {
            let Ok(expected_dir) = reanchor_under_canon_root(&self.project_root, &canon_root, &dir)
            else {
                continue;
            };
            if expected_dir == canon_root {
                continue;
            }
            if verify_no_symlink_components_under_root(&canon_root, &expected_dir).is_err() {
                continue;
            }
            if let Ok(meta) = fs::symlink_metadata(&expected_dir) {
                if meta.file_type().is_dir() && !meta.file_type().is_symlink() {
                    // `fs::remove_dir` only removes empty directories; non-empty dirs remain intact.
                    let _ = fs::remove_dir(&expected_dir);
                }
            }
        }

        self.undone = true;
        Ok(restored_rel_paths)
    }
}

// ============================================================================
// `DelegationFileMeta`, `DelegationResultMeta`, & Wire Output Formatter
// ============================================================================

/// Per-file summary metadata serialized inside `<!-- spell_delegation_meta: {...} -->`.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct DelegationFileMeta {
    pub path: String,
    pub added: usize,
    pub removed: usize,
    pub critic_passed: bool,
    pub bounces: u8,
}

/// Complete delegation outcome metadata serialized inside `<!-- spell_delegation_meta: {...} -->`.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct DelegationResultMeta {
    pub undo_id: Option<String>,
    pub protocol_used: String,
    pub fallback_triggered: bool,
    pub steps: usize,
    pub max_steps: usize,
    /// `"committed"` | `"escalated"`
    pub status: String,
    pub escalation_reason: Option<String>,
    pub summary: String,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub proposal_ids: Vec<String>,
    pub files: Vec<DelegationFileMeta>,
}

fn escape_xml_attr(raw: &str) -> String {
    let mut out = String::with_capacity(raw.len());
    for ch in raw.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '"' => out.push_str("&quot;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '\n' | '\r' => out.push(' '),
            _ => out.push(ch),
        }
    }
    out
}

/// Formats the tool result string returned to the main orchestrator (and inspected by UI cards)
/// for an autonomous `delegate_coding_subagent` run:
/// `<subagent_coding_result ...>...</subagent_coding_result>\n<!-- spell_delegation_meta: {...} -->`
pub fn format_delegation_result_output(
    model_id: &str,
    meta: &DelegationResultMeta,
    prefix_cache_reused: bool,
    tok_s: f64,
    detail_lines: &[String],
) -> String {
    let mut attrs = format!(
        "model=\"{}\" status=\"{}\" protocol=\"{}\" fallback_triggered=\"{}\" steps=\"{}\" max_steps=\"{}\" files_changed=\"{}\" prefix_cache_reused=\"{}\" tok_s=\"{:.1}\"",
        escape_xml_attr(model_id),
        escape_xml_attr(&meta.status),
        escape_xml_attr(&meta.protocol_used),
        meta.fallback_triggered,
        meta.steps,
        meta.max_steps,
        meta.files.len(),
        prefix_cache_reused,
        tok_s,
    );
    if let Some(ref uid) = meta.undo_id {
        attrs.push_str(&format!(" undo_id=\"{}\"", escape_xml_attr(uid)));
    }
    if let Some(ref reason) = meta.escalation_reason {
        attrs.push_str(&format!(
            " escalation_reason=\"{}\"",
            escape_xml_attr(reason)
        ));
    }

    let mut body_lines: Vec<String> = Vec::new();
    body_lines.push(format!("Summary: {}", meta.summary.trim()));
    if let Some(ref reason) = meta.escalation_reason {
        body_lines.push(format!("Escalation Reason: {}", reason.trim()));
    }
    if !meta.files.is_empty() {
        body_lines.push("Files:".to_string());
        for f in &meta.files {
            let verdict = if f.critic_passed { "PASS" } else { "REJECT" };
            body_lines.push(format!(
                "- `{}` (+{} / -{}, Gatekeeper Critic: {}, bounces: {})",
                f.path, f.added, f.removed, verdict, f.bounces
            ));
        }
    }
    if let Some(ref uid) = meta.undo_id {
        body_lines.push(format!("Undo ID: {uid}"));
    }
    if !meta.proposal_ids.is_empty() {
        body_lines.push(format!("Cached Proposals: {}", meta.proposal_ids.join(", ")));
    }
    for line in detail_lines {
        let trimmed = line.trim_end();
        if !trimmed.is_empty() {
            body_lines.push(trimmed.to_string());
        }
    }

    // Escape `-->` inside serialized JSON as `--\u003e` (valid RFC 8259 JSON) so user/code strings
    // containing `-->` can never prematurely close the HTML comment.
    let meta_json = serde_json::to_string(meta)
        .unwrap_or_else(|_| "{}".to_string())
        .replace("-->", "--\\u003e");

    format!(
        "<subagent_coding_result {attrs}>\n{}\n</subagent_coding_result>\n{DELEGATION_META_COMMENT_PREFIX}{meta_json} -->",
        body_lines.join("\n")
    )
}

/// Extracts and deserializes [`DelegationResultMeta`] from a formatted `<subagent_coding_result>`
/// tool output containing `<!-- spell_delegation_meta: {...} -->`.
pub fn parse_delegation_result_meta(output: &str) -> Option<DelegationResultMeta> {
    let start = output.rfind(DELEGATION_META_COMMENT_PREFIX)?;
    let json_start = start + DELEGATION_META_COMMENT_PREFIX.len();
    let tail = &output[json_start..];
    let end_rel = tail.find("-->")?;
    let json_slice = tail[..end_rel].trim();
    serde_json::from_str(json_slice).ok()
}

// ============================================================================
// Handoff Directive & Sticky Fallback Prompt Builders (Option A & Option B)
// ============================================================================

/// Builds the role directive for **Option A** (streaming native OpenAI `tool_calls`) for the
/// autonomous coding subagent.
pub fn build_option_a_handoff_directive(max_steps: usize) -> String {
    let clamped_steps = max_steps.clamp(1, MAX_DELEGATION_MAX_STEPS);
    format!(
        "You are operating as an autonomous multi-step coding subagent \
         (Option A: native tool-call protocol, budget: up to {clamped_steps} steps).\n\
         Use native tool calls (`read_file`, `grep`, `find_path`, `edit_file`, `write_file`, \
         `create_directory`) to inspect code and apply changes.\n\
         Rules:\n\
         1. Emit structured OpenAI `tool_calls` with valid JSON object arguments when inspecting or modifying files. Never emit raw `<tool_call>` or `<function=...>` XML tags in text, and do not invent unadvertised tool names.\n\
         2. Inspect unfamiliar files with `read_file`, `grep`, or `find_path` before editing. Never include line-number gutters (`12: ` or `12 | `) inside `old_text` or `new_text`.\n\
         3. Every `edit_file` or `write_file` call is staged in an isolated overlay and reviewed immediately by the Gatekeeper Critic; if rejected, read the fix hint and correct the edit in your next step.\n\
         4. Do not invoke parent-only tools (`terminal`, `delete_path`, `move_path`, `copy_path`, `fetch`, `delegate_coding_subagent`).\n\
         5. Once all requested edits have passed the Gatekeeper Critic, stop calling tools and reply directly with a concise 1–2 sentence summary of the completed changes within {clamped_steps} steps."
    )
}

/// Builds the role directive for **Option B** (single JSON action object per turn) for the
/// autonomous coding subagent.
pub fn build_option_b_handoff_directive(max_steps: usize) -> String {
    let clamped_steps = max_steps.clamp(1, MAX_DELEGATION_MAX_STEPS);
    format!(
        "You are operating as an autonomous multi-step coding subagent \
         (Option B: JSON action protocol, budget: up to {clamped_steps} steps).\n\
         Allowed actions (exact 9-tool subset): `read_file`, `grep`, `find_path`, `outline_symbols`, \
         `get_symbol_body`, `edit_file`, `write_file`, `create_directory`, `finish`.\n\
         Rules:\n\
         1. Reply on EVERY step with ONLY a single valid JSON object containing a top-level `\"action\"` string key and its arguments (no prose outside JSON, no `<tool_call>` or `<function=...>` XML tags).\n\
         2. Action schema examples:\n\
            - `{{\"action\": \"read_file\", \"path\": \"src/lib.rs\", \"start_line\": 1, \"end_line\": 120}}`\n\
            - `{{\"action\": \"grep\", \"pattern\": \"fn target_name\", \"path\": \"src\"}}`\n\
            - `{{\"action\": \"find_path\", \"pattern\": \"*.rs\", \"path\": \"src\"}}`\n\
            - `{{\"action\": \"outline_symbols\", \"path\": \"src/lib.rs\"}}`\n\
            - `{{\"action\": \"get_symbol_body\", \"path\": \"src/lib.rs\", \"symbol\": \"target_name\"}}`\n\
            - `{{\"action\": \"edit_file\", \"path\": \"src/lib.rs\", \"old_text\": \"exact old block\", \"new_text\": \"exact new block\"}}`\n\
            - `{{\"action\": \"write_file\", \"path\": \"src/new_mod.rs\", \"content\": \"...\"}}`\n\
            - `{{\"action\": \"create_directory\", \"path\": \"src/sub\"}}`\n\
            - `{{\"action\": \"finish\", \"summary\": \"Concise summary of completed edits.\", \"verification_notes\": \"...\"}}`\n\
         3. Never include line-number gutters in `old_text` or `new_text`. If Gatekeeper Critic rejects an edit, address its hint in the next JSON action.\n\
         4. Emit `{{\"action\": \"finish\", \"summary\": \"...\"}}` as soon as all requested edits pass Gatekeeper Critic verification (within {clamped_steps} steps)."
    )
}

/// Builds the corrective prompt injected when [`ProtocolFallbackTracker`] switches stickily from
/// Option A (`Native`) to Option B (`Json`) mid-delegation.
pub fn build_sticky_fallback_switch_prompt(reason: &str) -> String {
    let clean_reason = reason.trim();
    format!(
        "[PROTOCOL SWITCH -> OPTION B JSON ACTION MODE]\n\
         Native tool-call parsing failed ({clean_reason}). For all remaining steps of this delegation, \
         do NOT emit native tool calls or `<tool_call>` XML tags.\n\
         Reply with ONLY a single valid JSON object with a top-level `\"action\"` key chosen from \
         `read_file`, `grep`, `find_path`, `outline_symbols`, `get_symbol_body`, `edit_file`, \
         `write_file`, `create_directory`, or `finish`.\n\
         Example: `{{\"action\": \"edit_file\", \"path\": \"src/lib.rs\", \"old_text\": \"...\", \"new_text\": \"...\"}}` \
         or `{{\"action\": \"finish\", \"summary\": \"...\"}}`.\n\
         Re-emit your next action now as a single valid JSON object."
    )
}

// ============================================================================
// Comprehensive Unit Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn make_tc(name: &str, args: Value) -> WireToolCall {
        WireToolCall {
            id: "tc_1".to_string(),
            r#type: "function".to_string(),
            function: WireToolFunction {
                name: name.to_string(),
                arguments: args.to_string(),
            },
            index: Some(0),
        }
    }

    #[test]
    fn test_subagent_protocol_and_active_step_protocol_routing() {
        assert_eq!(SubagentProtocol::default(), SubagentProtocol::Auto);
        assert_eq!(SubagentProtocol::from_wire("auto"), SubagentProtocol::Auto);
        assert_eq!(SubagentProtocol::from_wire(""), SubagentProtocol::Auto);
        assert_eq!(SubagentProtocol::from_wire("unknown"), SubagentProtocol::Auto);
        assert_eq!(SubagentProtocol::from_wire("native"), SubagentProtocol::Native);
        assert_eq!(SubagentProtocol::from_wire("NATIVE"), SubagentProtocol::Native);
        assert_eq!(SubagentProtocol::from_wire("json"), SubagentProtocol::Json);
        assert_eq!(SubagentProtocol::from_wire("  JSON  "), SubagentProtocol::Json);

        assert_eq!(SubagentProtocol::Auto.as_str(), "auto");
        assert_eq!(SubagentProtocol::Native.as_str(), "native");
        assert_eq!(SubagentProtocol::Json.as_str(), "json");

        // Serde snake_case round-trip
        let ser = serde_json::to_string(&SubagentProtocol::Json).unwrap();
        assert_eq!(ser, "\"json\"");
        let de: SubagentProtocol = serde_json::from_str(&ser).unwrap();
        assert_eq!(de, SubagentProtocol::Json);

        // ActiveStepProtocol::initial resolution
        assert_eq!(
            ActiveStepProtocol::initial(SubagentProtocol::Auto, None),
            ActiveStepProtocol::Native
        );
        assert_eq!(
            ActiveStepProtocol::initial(SubagentProtocol::Native, None),
            ActiveStepProtocol::Native
        );
        assert_eq!(
            ActiveStepProtocol::initial(SubagentProtocol::Json, None),
            ActiveStepProtocol::Json
        );
        assert_eq!(
            ActiveStepProtocol::initial(SubagentProtocol::Auto, Some("json")),
            ActiveStepProtocol::Json
        );
        assert_eq!(
            ActiveStepProtocol::initial(SubagentProtocol::Json, Some("native")),
            ActiveStepProtocol::Native
        );
        assert_eq!(
            ActiveStepProtocol::initial(SubagentProtocol::Json, Some("auto")),
            ActiveStepProtocol::Native
        );
        assert_eq!(ActiveStepProtocol::Native.as_str(), "native");
        assert_eq!(ActiveStepProtocol::Json.as_str(), "json");
    }

    #[test]
    fn test_delegation_mode_from_args_and_instruction_including_test8_compat() {
        // Default when mode omitted and normal coding instruction -> Auto
        assert_eq!(
            DelegationMode::from_args_and_instruction(
                &json!({"instruction": "Refactor lookup_limit to default to 64"}),
                "Refactor lookup_limit to default to 64",
            ),
            DelegationMode::Auto
        );

        // Explicit mode: "propose" / "proposal" -> Propose
        assert_eq!(
            DelegationMode::from_args_and_instruction(
                &json!({"mode": "propose"}),
                "Update src/lib.rs",
            ),
            DelegationMode::Propose
        );
        assert_eq!(
            DelegationMode::from_args_and_instruction(
                &json!({"mode": "proposal"}),
                "Update src/lib.rs",
            ),
            DelegationMode::Propose
        );

        // Live Contract Test #8 compatibility: mode omitted, instruction explicitly asks for JSON with `"edits"` and `"old_text"` -> Propose
        let test8_instruction = "Emit JSON `{\"path\": \"src/quota.rs\", \"edits\": [{\"old_text\": \"    limits.get(key).copied().unwrap_or(16)\", \"new_text\": \"    limits.get(key).copied().unwrap_or(64)\"}]}`.";
        assert_eq!(
            DelegationMode::from_args_and_instruction(&json!({}), test8_instruction),
            DelegationMode::Propose
        );

        // Explicit mode: "auto" / "execute" / "autonomous" overrides even if instruction mentions `"edits"`
        assert_eq!(
            DelegationMode::from_args_and_instruction(
                &json!({"mode": "auto"}),
                test8_instruction,
            ),
            DelegationMode::Auto
        );
        assert_eq!(
            DelegationMode::from_args_and_instruction(
                &json!({"mode": "autonomous"}),
                "Apply requested changes",
            ),
            DelegationMode::Auto
        );
    }

    #[test]
    fn test_validate_subagent_tool_call_all_9_allowed_disallowed_and_malformed() {
        assert_eq!(SUBAGENT_ALLOWED_TOOLS.len(), 9);
        assert_eq!(build_subagent_openai_tools_schema().len(), 9);

        // 1. All 9 allowed tools succeed and normalize aliases
        let r_read = validate_subagent_tool_call(&make_tc(
            "read_file",
            json!({"path": " src/lib.rs ", "start_line": 1, "end_line": 50}),
        ))
        .unwrap();
        assert_eq!(r_read["path"], "src/lib.rs");

        let r_grep = validate_subagent_tool_call(&make_tc(
            "grep",
            json!({"query": "fn lookup_limit", "path": "src"}),
        ))
        .unwrap();
        assert_eq!(r_grep["pattern"], "fn lookup_limit");

        let r_find_glob = validate_subagent_tool_call(&make_tc(
            "find_path",
            json!({"glob": "*.rs"}),
        ))
        .unwrap();
        assert_eq!(r_find_glob["pattern"], "*.rs");

        let r_find_empty = validate_subagent_tool_call(&make_tc(
            "find_path",
            json!({"needle": ""}),
        ))
        .unwrap();
        assert_eq!(r_find_empty["pattern"], "");

        let r_outline = validate_subagent_tool_call(&make_tc(
            "outline_symbols",
            json!({"path": "src/tools.rs"}),
        ))
        .unwrap();
        assert_eq!(r_outline["path"], "src/tools.rs");

        let r_sym = validate_subagent_tool_call(&make_tc(
            "get_symbol_body",
            json!({"path": "src/tools.rs", "symbol_name": "TurnPinCache::new"}),
        ))
        .unwrap();
        assert_eq!(r_sym["symbol"], "TurnPinCache::new");

        let r_edit_single = validate_subagent_tool_call(&make_tc(
            "edit_file",
            json!({"path": "src/lib.rs", "old_text": "16", "new_text": "64"}),
        ))
        .unwrap();
        assert_eq!(r_edit_single["path"], "src/lib.rs");

        let r_edit_arr = validate_subagent_tool_call(&make_tc(
            "edit_file",
            json!({
                "path": "src/lib.rs",
                "edits": [{"old_text": "old_a", "new_text": ""}]
            }),
        ))
        .unwrap();
        assert_eq!(r_edit_arr["edits"].as_array().unwrap().len(), 1);

        let r_write = validate_subagent_tool_call(&make_tc(
            "write_file",
            json!({"path": "src/empty.rs", "content": ""}),
        ))
        .unwrap();
        assert_eq!(r_write["path"], "src/empty.rs");

        let r_mkdir = validate_subagent_tool_call(&make_tc(
            "create_directory",
            json!({"path": "src/new_mod"}),
        ))
        .unwrap();
        assert_eq!(r_mkdir["path"], "src/new_mod");

        let r_finish = validate_subagent_tool_call(&make_tc("finish", json!({}))).unwrap();
        assert_eq!(r_finish["summary"], "Completed requested changes.");

        // 2. DisallowedTool on parent-only tools (does NOT count as malformed step)
        for parent_tool in [
            "terminal",
            "delete_path",
            "move_path",
            "copy_path",
            "fetch",
            "schedule_wakeup",
            "tag",
            "prefill_context_subagent",
            "delegate_coding_subagent",
            "apply_subagent_proposal",
            "list_directory",
        ] {
            let err = validate_subagent_tool_call(&make_tc(parent_tool, json!({}))).unwrap_err();
            assert!(
                matches!(err, SubagentStepValidationError::DisallowedTool(_)),
                "Expected DisallowedTool for '{parent_tool}', got {err:?}"
            );
            assert!(!err.is_malformed_step());
            assert!(err.to_tool_error_message().starts_with("PERMISSION_DENIED:"));
        }

        // 3. MalformedStep on unknown tool, bad JSON, non-object JSON, or missing required fields
        let unk = validate_subagent_tool_call(&make_tc("invented_tool", json!({}))).unwrap_err();
        assert!(unk.is_malformed_step());

        let bad_json = validate_subagent_tool_call(&WireToolCall {
            id: "tc_bad".to_string(),
            r#type: "function".to_string(),
            function: WireToolFunction {
                name: "read_file".to_string(),
                arguments: "{\"path\": \"unterminated".to_string(),
            },
            index: Some(0),
        })
        .unwrap_err();
        assert!(bad_json.is_malformed_step());

        let arr_json = validate_subagent_tool_call(&WireToolCall {
            id: "tc_arr".to_string(),
            r#type: "function".to_string(),
            function: WireToolFunction {
                name: "read_file".to_string(),
                arguments: "[\"src/lib.rs\"]".to_string(),
            },
            index: Some(0),
        })
        .unwrap_err();
        assert!(arr_json.is_malformed_step());

        assert!(validate_subagent_tool_call(&make_tc("read_file", json!({"path": "  "})))
            .unwrap_err()
            .is_malformed_step());
        assert!(validate_subagent_tool_call(&make_tc("grep", json!({"pattern": ""})))
            .unwrap_err()
            .is_malformed_step());
        assert!(validate_subagent_tool_call(&make_tc("find_path", json!({})))
            .unwrap_err()
            .is_malformed_step());
        assert!(validate_subagent_tool_call(&make_tc("get_symbol_body", json!({"path": "src/lib.rs"})))
            .unwrap_err()
            .is_malformed_step());
        assert!(validate_subagent_tool_call(&make_tc("edit_file", json!({"path": "src/lib.rs", "edits": []})))
            .unwrap_err()
            .is_malformed_step());
        assert!(validate_subagent_tool_call(&make_tc("write_file", json!({"path": "src/lib.rs"})))
            .unwrap_err()
            .is_malformed_step());
    }

    #[test]
    fn test_parse_option_b_json_action_and_legacy_proposal_invariant() {
        // 1. CRITICAL INVARIANT: Legacy proposal JSON (without top-level "action"/"tool") MUST return None!
        let legacy_proposal = r#"{"path": "src/math.rs", "edits": [{"old_text": "return 1;", "new_text": "return 2;"}]}"#;
        assert!(parse_option_b_json_action(legacy_proposal, 0).is_none());
        assert!(!contains_malformed_tool_attempt(legacy_proposal));

        // Even if legacy proposal's code string mentions `{"action": ...}`, outer JSON has no top-level `action`!
        let legacy_mentioning_action = r#"{"path": "src/router.rs", "edits": [{"old_text": "// {\"action\": \"ping\"}", "new_text": "// {\"action\": \"pong\"}"}]}"#;
        assert!(parse_option_b_json_action(legacy_mentioning_action, 0).is_none());
        assert!(!contains_malformed_tool_attempt(legacy_mentioning_action));

        // 2. `<think>...</think>` + markdown ```json fence + flat params + multibyte UTF-8
        let fenced_with_think = "<think>\nLet's check 🚀 日本語 symbols & {\"edits\": []}\n</think>\n```json\n{\n  \"action\": \"edit_file\",\n  \"path\": \"src/lib.rs\",\n  \"old_text\": \"const MSG: &str = \\\"hello\\\";\",\n  \"new_text\": \"const MSG: &str = \\\"🚀 日本語 ∀x ∈ ℝ\\\";\"\n}\n```";
        let parsed = parse_option_b_json_action(fenced_with_think, 3)
            .expect("expected Some")
            .expect("expected Ok WireToolCall");
        assert_eq!(parsed.id, "sub_json_3");
        assert_eq!(parsed.function.name, "edit_file");
        let args_val: Value = serde_json::from_str(&parsed.function.arguments).unwrap();
        assert_eq!(args_val["path"], "src/lib.rs");
        assert_eq!(args_val["new_text"], "const MSG: &str = \"🚀 日本語 ∀x ∈ ℝ\";");
        assert!(!contains_malformed_tool_attempt(fenced_with_think));

        // 3. Nested `arguments` and `tool` + `args`
        let nested_args = r#"{"action": "get_symbol_body", "arguments": {"path": "src/tools.rs", "symbol_name": "resolve_workspace_path"}}"#;
        let tc_nested = parse_option_b_json_action(nested_args, 1)
            .unwrap()
            .unwrap();
        assert_eq!(tc_nested.id, "sub_json_1");
        assert_eq!(tc_nested.function.name, "get_symbol_body");
        let nested_val: Value = serde_json::from_str(&tc_nested.function.arguments).unwrap();
        assert_eq!(nested_val["symbol"], "resolve_workspace_path");

        let tool_alias = r#"{"tool": "read_file", "args": {"path": "Cargo.toml", "start_line": 1, "end_line": 20}}"#;
        let tc_alias = parse_option_b_json_action(tool_alias, 2).unwrap().unwrap();
        assert_eq!(tc_alias.function.name, "read_file");

        // 4. Disallowed tool or missing required fields in Option B -> Some(Err(...))
        let disallowed = r#"{"action": "terminal", "command": "cargo test"}"#;
        let err_disallowed = parse_option_b_json_action(disallowed, 4)
            .unwrap()
            .unwrap_err();
        assert!(err_disallowed.starts_with("PERMISSION_DENIED:"));

        let missing_field = r#"{"action": "read_file"}"#;
        let err_missing = parse_option_b_json_action(missing_field, 5)
            .unwrap()
            .unwrap_err();
        assert!(err_missing.starts_with("MALFORMED_STEP:"));

        // 5. Malformed tool attempts (`<tool_call`, `<function=`, broken `{"action"`)
        assert!(contains_malformed_tool_attempt(
            "<think>ok</think>\n<tool_call>\n{\"name\":\"read_file\"}"
        ));
        assert!(contains_malformed_tool_attempt(
            "<function=edit_file><parameter=path>src/lib.rs</parameter>"
        ));
        assert!(contains_malformed_tool_attempt(
            "{\"action\": \"edit_file\", \"path\": \"src/lib.rs\""
        ));
        assert!(parse_option_b_json_action(
            "{\"action\": \"edit_file\", \"path\": \"src/lib.rs\"",
            6
        )
        .unwrap()
        .is_err());
        // `<tool_call` inside `<think>...</think>` only -> NOT malformed!
        assert!(!contains_malformed_tool_attempt(
            "<think>Do not emit <tool_call> tags</think>Plain summary."
        ));
    }

    #[test]
    fn test_protocol_fallback_tracker_sticky_transitions() {
        // 1. JSON-in-content immediate sticky switch
        let mut tracker = ProtocolFallbackTracker::new(SubagentProtocol::Auto, None);
        assert_eq!(tracker.active, ActiveStepProtocol::Native);
        assert!(!tracker.fallback_triggered);
        assert_eq!(tracker.total_malformed, 0);

        tracker.on_json_action_in_native_content();
        assert_eq!(tracker.active, ActiveStepProtocol::Json);
        assert!(tracker.fallback_triggered);
        assert_eq!(tracker.consecutive_malformed, 0);
        assert_eq!(tracker.total_malformed, 0);

        // 2. Two consecutive malformed steps trigger sticky switch to Option B (`switched_now == true`)
        let mut tracker2 = ProtocolFallbackTracker::new(SubagentProtocol::Native, None);
        assert!(!tracker2.on_malformed_native_step());
        assert_eq!(tracker2.consecutive_malformed, 1);
        assert_eq!(tracker2.total_malformed, 1);
        assert_eq!(tracker2.active, ActiveStepProtocol::Native);
        assert!(!tracker2.fallback_triggered);

        assert!(tracker2.on_malformed_native_step());
        assert_eq!(tracker2.consecutive_malformed, 2);
        assert_eq!(tracker2.total_malformed, 2);
        assert_eq!(tracker2.active, ActiveStepProtocol::Json);
        assert!(tracker2.fallback_triggered);

        // Subsequent calls remain sticky in Json and return `switched_now == false`
        assert!(!tracker2.on_malformed_native_step());
        assert_eq!(tracker2.active, ActiveStepProtocol::Json);
        assert_eq!(tracker2.total_malformed, 3);

        // 3. A single malformed step followed by valid steps resets consecutive_malformed and stays Native
        let mut tracker3 = ProtocolFallbackTracker::new(SubagentProtocol::Auto, None);
        assert!(!tracker3.on_malformed_native_step());
        assert_eq!(tracker3.consecutive_malformed, 1);
        assert_eq!(tracker3.total_malformed, 1);
        tracker3.on_valid_native_step();
        assert_eq!(tracker3.consecutive_malformed, 0);
        assert_eq!(tracker3.total_malformed, 1);
        assert_eq!(tracker3.active, ActiveStepProtocol::Native);
        assert!(!tracker3.fallback_triggered);
    }

    #[test]
    fn test_protocol_fallback_tracker_interleaved_malformed_triggers_fallback_in_auto_mode() {
        // Interleaved `[malformed, valid_step, malformed]` (e.g., unclosed `<tool_call>` at step 2,
        // trivial `create_directory` success at step 3, unclosed `<tool_call>` at step 4) MUST
        // trigger sticky Option B JSON fallback via `total_malformed >= 2` in `auto` mode.
        let mut tracker = ProtocolFallbackTracker::new(SubagentProtocol::Auto, None);
        assert_eq!(tracker.active, ActiveStepProtocol::Native);

        // Malformed #1
        assert!(!tracker.on_malformed_native_step());
        assert_eq!(tracker.consecutive_malformed, 1);
        assert_eq!(tracker.total_malformed, 1);
        assert_eq!(tracker.active, ActiveStepProtocol::Native);
        assert!(!tracker.fallback_triggered);

        // Intervening valid step resets consecutive_malformed, preserves total_malformed
        tracker.on_valid_native_step();
        assert_eq!(tracker.consecutive_malformed, 0);
        assert_eq!(tracker.total_malformed, 1);
        assert_eq!(tracker.active, ActiveStepProtocol::Native);
        assert!(!tracker.fallback_triggered);

        // Malformed #2 -> total_malformed reaches 2 -> sticky switch to Option B (`Json`)
        assert!(tracker.on_malformed_native_step());
        assert_eq!(tracker.consecutive_malformed, 1);
        assert_eq!(tracker.total_malformed, 2);
        assert_eq!(tracker.active, ActiveStepProtocol::Json);
        assert!(tracker.fallback_triggered);

        // Already in Json -> subsequent malformed call returns switched_now == false
        assert!(!tracker.on_malformed_native_step());
        assert_eq!(tracker.active, ActiveStepProtocol::Json);
        assert_eq!(tracker.total_malformed, 3);
    }

    #[test]
    fn test_delegation_overlay_search_undo_and_result_meta_formatting() {
        let ws = std::env::temp_dir().join(format!(
            "spell_subagent_coding_overlay_test_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&ws);
        fs::create_dir_all(ws.join("src")).unwrap();

        let existing_rs = ws.join("src/existing.rs");
        let unchanged_rs = ws.join("src/unchanged.rs");
        fs::write(&existing_rs, "pub fn compute() -> i32 {\n    10\n}\n").unwrap();
        fs::write(&unchanged_rs, "pub const KEEP_TOKEN: &str = \"stable\";\n").unwrap();

        let parent_store = WorkspaceSnapshotStore::new();
        let mut overlay = DelegationOverlay::fork_from(&parent_store);

        // Stage modification to `src/existing.rs` + virtual dir `src/sub` + new staged file `src/sub/new_helper.rs`
        overlay
            .stage_file(
                &existing_rs,
                "src/existing.rs",
                "pub fn compute() -> i32 {\n    99\n}\n".to_string(),
            )
            .unwrap();
        let new_sub_dir = ws.join("src/sub");
        let new_helper_rs = new_sub_dir.join("new_helper.rs");
        overlay.stage_virtual_dir(&new_sub_dir).unwrap();
        overlay
            .stage_file(
                &new_helper_rs,
                "src/sub/new_helper.rs",
                "pub fn brand_new_overlay_fn() -> &'static str {\n    \"hello_overlay\"\n}\n"
                    .to_string(),
            )
            .unwrap();
        assert_eq!(overlay.record_critic_bounce(&existing_rs), 1);
        assert_eq!(overlay.bounces_for(&existing_rs), 1);

        // On-disk files are still untouched!
        assert!(fs::read_to_string(&existing_rs).unwrap().contains("10"));
        assert!(!new_helper_rs.exists());

        // 1. `find_paths_with_overlay` sees disk + virtual dir + newly staged file
        let find_all = find_paths_with_overlay(&ws, &ws, "", &overlay, 100);
        assert!(find_all.contains("src/existing.rs"), "{find_all}");
        assert!(find_all.contains("src/unchanged.rs"), "{find_all}");
        assert!(find_all.contains("src/sub"), "{find_all}");
        assert!(find_all.contains("src/sub/new_helper.rs"), "{find_all}");
        assert!(find_all.contains("[find_path summary: scanned"), "{find_all}");

        let find_glob = find_paths_with_overlay(&ws, &ws, "*helper.rs", &overlay, 100);
        assert!(find_glob.contains("src/sub/new_helper.rs"), "{find_glob}");
        assert!(find_glob.contains("1 matches]"), "{find_glob}");

        // 2. `grep_with_overlay` sees modified overlay value (`99`, NOT stale `10`) and new staged file
        let grep_new_val = grep_with_overlay(&ws, &ws, "99", 100, &overlay).unwrap();
        assert!(grep_new_val.contains("src/existing.rs:2:    99"), "{grep_new_val}");
        assert!(grep_new_val.contains("[grep summary: 1 matches in 3 files]"), "{grep_new_val}");

        let grep_stale_val = grep_with_overlay(&ws, &ws, "^    10$", 100, &overlay).unwrap();
        assert!(grep_stale_val.contains("[grep summary: 0 matches in 3 files]"), "{grep_stale_val}");

        let grep_staged_new_file =
            grep_with_overlay(&ws, &ws, "brand_new_overlay_fn", 100, &overlay).unwrap();
        assert!(
            grep_staged_new_file
                .contains("src/sub/new_helper.rs:1:pub fn brand_new_overlay_fn()"),
            "{grep_staged_new_file}"
        );

        // 3. Commit overlay to disk, populate parent snapshot_store & pin_cache, then run `DelegationUndoRecord::execute_undo`
        let committed = overlay.store.commit_staged(None).unwrap();
        assert_eq!(committed.len(), 2);
        assert!(new_helper_rs.exists());
        assert!(fs::read_to_string(&existing_rs).unwrap().contains("99"));

        let mut session_store = overlay.store.clone();
        let mut session_pins = TurnPinCache::new();
        let first_pinned = session_pins
            .read_file_pinned("call_after_commit", &existing_rs, None, None)
            .unwrap();
        assert!(first_pinned.contains("99"));

        let mut undo_rec = DelegationUndoRecord {
            undo_id: "undo-del-1".to_string(),
            thread_id: "thread-1".to_string(),
            timestamp: 1_700_000_000,
            project_root: ws.clone(),
            git_snapshot_sha: None,
            files: vec![
                DelegationFileRestore {
                    canonical_path: existing_rs.clone(),
                    relative_path: "src/existing.rs".to_string(),
                    prior_disk_content: Some("pub fn compute() -> i32 {\n    10\n}\n".to_string()),
                },
                DelegationFileRestore {
                    canonical_path: new_helper_rs.clone(),
                    relative_path: "src/sub/new_helper.rs".to_string(),
                    prior_disk_content: None,
                },
            ],
            created_dirs: vec![new_sub_dir.clone()],
            undone: false,
        };

        let restored = undo_rec
            .execute_undo(&mut session_store, &mut session_pins)
            .unwrap();
        assert_eq!(
            restored,
            vec![
                "src/existing.rs".to_string(),
                "src/sub/new_helper.rs".to_string()
            ]
        );
        assert!(undo_rec.undone);
        assert_eq!(
            fs::read_to_string(&existing_rs).unwrap(),
            "pub fn compute() -> i32 {\n    10\n}\n"
        );
        assert!(!new_helper_rs.exists(), "newly created file must be deleted on undo");
        assert!(!new_sub_dir.exists(), "empty created dir must be pruned on undo");
        assert!(ws.join("src").exists(), "non-empty parent dir must remain");
        assert_eq!(
            session_store.read_file_text(&existing_rs).unwrap(),
            "pub fn compute() -> i32 {\n    10\n}\n"
        );
        // Pin cache was invalidated so re-reading `existing_rs` under a new call_id returns `10` (not duplicate ref!)
        let reread_after_undo = session_pins
            .read_file_pinned("call_after_undo", &existing_rs, None, None)
            .unwrap();
        assert!(reread_after_undo.contains("10"));
        // Double undo is rejected cleanly
        assert!(undo_rec
            .execute_undo(&mut session_store, &mut session_pins)
            .is_err());

        // 4. `DelegationResultMeta` round-trip (even when summary contains `-->`)
        let meta = DelegationResultMeta {
            undo_id: Some("undo-del-1".to_string()),
            protocol_used: "native".to_string(),
            fallback_triggered: false,
            steps: 3,
            max_steps: 12,
            status: "committed".to_string(),
            escalation_reason: None,
            summary: "Updated compute() 10 --> 99 and added new_helper.rs.".to_string(),
            proposal_ids: Vec::new(),
            files: vec![
                DelegationFileMeta {
                    path: "src/existing.rs".to_string(),
                    added: 1,
                    removed: 1,
                    critic_passed: true,
                    bounces: 1,
                },
                DelegationFileMeta {
                    path: "src/sub/new_helper.rs".to_string(),
                    added: 3,
                    removed: 0,
                    critic_passed: true,
                    bounces: 0,
                },
            ],
        };
        let formatted = format_delegation_result_output(
            "test-subagent-model",
            &meta,
            true,
            184.5,
            &["Verification: all hunks passed Gatekeeper Critic.".to_string()],
        );
        assert!(formatted.starts_with("<subagent_coding_result "));
        assert!(formatted.contains("<!-- spell_delegation_meta: "));
        let parsed_meta = parse_delegation_result_meta(&formatted).expect("parse meta round-trip");
        assert_eq!(parsed_meta, meta);
        let invalid_prefix = formatted.replace(
            "<!-- spell_delegation_meta: ",
            "<!-- unknown_delegation_meta: ",
        );
        assert_eq!(parse_delegation_result_meta(&invalid_prefix), None);

        // Prompt builders sanity check
        assert!(build_option_a_handoff_directive(12).contains("Option A"));
        assert!(build_option_b_handoff_directive(12).contains("Option B"));
        assert!(build_sticky_fallback_switch_prompt("bad args").contains("OPTION B"));

        let _ = fs::remove_dir_all(&ws);
    }

    #[test]
    fn test_undo_security_guardrails_truncation_and_step_budget_clamping() {
        assert_eq!(clamp_delegation_max_steps(None), DEFAULT_DELEGATION_MAX_STEPS);
        assert_eq!(clamp_delegation_max_steps(Some(0)), 1);
        assert_eq!(clamp_delegation_max_steps(Some(99)), MAX_DELEGATION_MAX_STEPS);

        let ws = std::env::temp_dir().join(format!(
            "spell_subagent_coding_guardrail_test_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&ws);
        fs::create_dir_all(ws.join("src")).unwrap();
        for idx in 0..5 {
            fs::write(
                ws.join(format!("src/f_{idx}.rs")),
                format!("pub const VAL: usize = {idx};\n"),
            )
            .unwrap();
        }

        let overlay = DelegationOverlay::new();
        let truncated_find = find_paths_with_overlay(&ws, &ws, "*.rs", &overlay, 2);
        assert!(
            truncated_find.contains("showing first 2 matches (truncated)"),
            "{truncated_find}"
        );

        let truncated_grep =
            grep_with_overlay(&ws, &ws, "pub const VAL", 2, &overlay).unwrap();
        assert!(
            truncated_grep.contains("results truncated at 2 matches"),
            "{truncated_grep}"
        );

        // Guardrail blocks protected & out-of-workspace paths in find_paths_with_overlay & grep_with_overlay
        let protected = ws.join(".ssh");
        assert!(find_paths_with_overlay(&protected, &protected, "", &overlay, 10).contains("SECURITY_BLOCK"));
        assert!(grep_with_overlay(&protected, &protected, "x", 10, &overlay)
            .unwrap_err()
            .contains("SECURITY_BLOCK"));
        let outside = std::env::temp_dir().join("spell_outside_search_base");
        assert!(find_paths_with_overlay(&ws, &outside, "", &overlay, 10).contains("SECURITY_BLOCK"));
        assert!(grep_with_overlay(&ws, &outside, "x", 10, &overlay)
            .unwrap_err()
            .contains("SECURITY_BLOCK"));

        // Guardrail blocks outside-workspace & protected paths in DelegationUndoRecord::execute_undo
        let mut store = WorkspaceSnapshotStore::new();
        let mut pins = TurnPinCache::new();
        let mut escape_undo = DelegationUndoRecord {
            undo_id: "undo-escape".to_string(),
            thread_id: "t1".to_string(),
            timestamp: 1,
            project_root: ws.clone(),
            git_snapshot_sha: None,
            files: vec![DelegationFileRestore {
                canonical_path: PathBuf::from("/etc/hosts"),
                relative_path: "../etc/hosts".to_string(),
                prior_disk_content: Some("tamper".to_string()),
            }],
            created_dirs: Vec::new(),
            undone: false,
        };
        assert!(escape_undo.execute_undo(&mut store, &mut pins).is_err());
        assert!(!escape_undo.undone);

        let mut env_undo = DelegationUndoRecord {
            undo_id: "undo-env".to_string(),
            thread_id: "t1".to_string(),
            timestamp: 1,
            project_root: ws.clone(),
            git_snapshot_sha: None,
            files: vec![DelegationFileRestore {
                canonical_path: ws.join(".env"),
                relative_path: ".env".to_string(),
                prior_disk_content: Some("SECRET=1".to_string()),
            }],
            created_dirs: Vec::new(),
            undone: false,
        };
        assert!(env_undo.execute_undo(&mut store, &mut pins).is_err());
        assert!(!env_undo.undone);

        let _ = fs::remove_dir_all(&ws);
    }

    #[cfg(unix)]
    #[test]
    fn test_execute_undo_rejects_intra_workspace_symlink_replacement_without_clobbering() {
        use std::os::unix::fs::symlink;

        let ws = std::env::temp_dir().join(format!(
            "spell_subagent_undo_symlink_guard_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&ws);
        fs::create_dir_all(ws.join("src")).unwrap();

        let cargo_toml = ws.join("Cargo.toml");
        let sacred_cargo = "[package]\nname = \"sacred_manifest\"\nversion = \"1.0.0\"\n";
        fs::write(&cargo_toml, sacred_cargo).unwrap();

        let foo_rs = ws.join("src/foo.rs");
        fs::write(&foo_rs, "pub fn after_delegation() -> i32 { 2 }\n").unwrap();

        // Simulate post-commit intra-workspace symlink swap: `src/foo.rs -> ../Cargo.toml`
        fs::remove_file(&foo_rs).unwrap();
        symlink("../Cargo.toml", &foo_rs).unwrap();

        let mut store = WorkspaceSnapshotStore::new();
        let mut pins = TurnPinCache::new();
        let mut undo_rec = DelegationUndoRecord {
            undo_id: "undo-symlink-clobber".to_string(),
            thread_id: "thread-sec".to_string(),
            timestamp: 1_700_000_001,
            project_root: ws.clone(),
            git_snapshot_sha: None,
            files: vec![DelegationFileRestore {
                canonical_path: fs::canonicalize(&ws).unwrap().join("src/foo.rs"),
                relative_path: "src/foo.rs".to_string(),
                prior_disk_content: Some("pub fn before_delegation() -> i32 { 1 }\n".to_string()),
            }],
            created_dirs: Vec::new(),
            undone: false,
        };

        let err = undo_rec.execute_undo(&mut store, &mut pins).unwrap_err();
        assert!(
            err.contains("SECURITY_VIOLATION"),
            "Expected SECURITY_VIOLATION on intra-workspace symlink, got: {err}"
        );
        assert!(!undo_rec.undone);
        assert_eq!(
            fs::read_to_string(&cargo_toml).unwrap(),
            sacred_cargo,
            "Cargo.toml MUST NOT be clobbered by undo following src/foo.rs -> ../Cargo.toml!"
        );

        // Also verify unlink (`prior_disk_content: None`) and ancestor dir symlink rejection
        let new_rs = ws.join("src/new_created.rs");
        symlink("../Cargo.toml", &new_rs).unwrap();
        let mut unlink_undo = DelegationUndoRecord {
            undo_id: "undo-symlink-unlink".to_string(),
            thread_id: "thread-sec".to_string(),
            timestamp: 1_700_000_002,
            project_root: ws.clone(),
            git_snapshot_sha: None,
            files: vec![DelegationFileRestore {
                canonical_path: fs::canonicalize(&ws).unwrap().join("src/new_created.rs"),
                relative_path: "src/new_created.rs".to_string(),
                prior_disk_content: None,
            }],
            created_dirs: Vec::new(),
            undone: false,
        };
        let unlink_err = unlink_undo.execute_undo(&mut store, &mut pins).unwrap_err();
        assert!(
            unlink_err.contains("SECURITY_VIOLATION"),
            "Expected SECURITY_VIOLATION on symlink unlink, got: {unlink_err}"
        );
        assert!(
            fs::symlink_metadata(&new_rs)
                .unwrap()
                .file_type()
                .is_symlink(),
            "Symlink must not be silently unlinked"
        );
        assert_eq!(fs::read_to_string(&cargo_toml).unwrap(), sacred_cargo);

        let _ = fs::remove_dir_all(&ws);
    }

    #[test]
    fn test_grep_with_overlay_skips_dotenv_and_protected_secret_files() {
        let ws = std::env::temp_dir().join(format!(
            "spell_subagent_grep_protected_globs_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&ws);
        fs::create_dir_all(ws.join("src")).unwrap();
        fs::create_dir_all(ws.join(".ssh")).unwrap();
        fs::create_dir_all(ws.join(".aws")).unwrap();

        fs::write(
            ws.join("src/app.rs"),
            "pub const PUBLIC_MARKER: &str = \"SAFE_SHARED_NEEDLE\";\n",
        )
        .unwrap();
        fs::write(
            ws.join(".env"),
            "API_SECRET=ULTRA_SECRET_DOTENV_TOKEN\nSHARED=SAFE_SHARED_NEEDLE\n",
        )
        .unwrap();
        fs::write(
            ws.join(".env.production"),
            "PROD_SECRET=ULTRA_SECRET_DOTENV_TOKEN\nSHARED=SAFE_SHARED_NEEDLE\n",
        )
        .unwrap();
        fs::write(
            ws.join(".ssh/id_ed25519"),
            "PRIVATE_KEY_ULTRA_SECRET_DOTENV_TOKEN\n",
        )
        .unwrap();
        fs::write(
            ws.join(".aws/credentials"),
            "aws_secret_access_key = ULTRA_SECRET_DOTENV_TOKEN\n",
        )
        .unwrap();

        let overlay = DelegationOverlay::new();

        // 1. Secret token present ONLY in `.env`, `.env.production`, `.ssh/*`, `.aws/*` yields 0 matches in 1 scanned file
        let secret_grep =
            grep_with_overlay(&ws, &ws, "ULTRA_SECRET_DOTENV_TOKEN", 100, &overlay).unwrap();
        assert!(
            !secret_grep.contains("ULTRA_SECRET_DOTENV_TOKEN"),
            "Protected secret leaked in grep output: {secret_grep}"
        );
        assert!(
            !secret_grep.contains(".env"),
            "Protected .env path leaked in grep output: {secret_grep}"
        );
        assert!(
            secret_grep.contains("[grep summary: 0 matches in 1 files]"),
            "Expected only src/app.rs to be scanned, got: {secret_grep}"
        );

        // 2. Shared needle returns `src/app.rs` ONLY, never `.env` or `.env.production`
        let shared_grep =
            grep_with_overlay(&ws, &ws, "SAFE_SHARED_NEEDLE", 100, &overlay).unwrap();
        assert!(shared_grep.contains("src/app.rs:1:"), "{shared_grep}");
        assert!(!shared_grep.contains(".env"), "{shared_grep}");
        assert!(
            shared_grep.contains("[grep summary: 1 matches in 1 files]"),
            "{shared_grep}"
        );

        // 3. Direct grep targeting `.env` fails immediately with SECURITY_BLOCK
        let direct_env_err =
            grep_with_overlay(&ws, &ws.join(".env"), "API_SECRET", 100, &overlay).unwrap_err();
        assert!(direct_env_err.contains("SECURITY_BLOCK"), "{direct_env_err}");

        // 4. `find_paths_with_overlay` also hides `.env`, `.env.production`, `.ssh`, `.aws`
        let found = find_paths_with_overlay(&ws, &ws, "", &overlay, 100);
        assert!(found.contains("src/app.rs"), "{found}");
        assert!(!found.contains(".env"), "{found}");
        assert!(!found.contains(".ssh"), "{found}");
        assert!(!found.contains(".aws"), "{found}");

        // 5. Option A handoff directive aligns with native tool schema
        let opt_a = build_option_a_handoff_directive(12);
        assert!(opt_a.contains("read_file") && opt_a.contains("create_directory"));
        assert!(opt_a.contains("Gatekeeper Critic"));
        assert!(opt_a.contains("stop calling tools and reply directly"));
        assert!(!opt_a.contains("invoke `finish("));
        assert!(SUBAGENT_ALLOWED_TOOLS.contains(&"finish"));

        let _ = fs::remove_dir_all(&ws);
    }
}
