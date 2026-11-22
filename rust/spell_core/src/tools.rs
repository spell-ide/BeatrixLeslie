//! Headless Agent Tools, Workspace Path Resolution, Turn-Pinning & SHA-256 Deduplication,
//! OpenAI Tool Schema Builder, and Clean-Room System Prompt Builder for BeatrixLeslie IDE (`spell_core`).
//!
//! Key capabilities:
//! - **Constants**: `DEFAULT_MAX_AUTO_EXPAND_LINES` (1,500), `MAX_SINGLE_FILE_CHARS` (48,000),
//!   `MAX_TOTAL_TOOL_CHARS` (96,000).
//! - **Permissions & Profiles**: `AgentProfile` (`Write`, `Ask`) and `ToolPermissionPolicy`
//!   (`Allow`, `Confirm`, `Deny`).
//! - **Workspace Path Resolution (`resolve_workspace_path`)**: Protected globs & workspace confinement,
//!   `:line:col` and `#L123-L456` suffix stripping, relative paths, and worktree-prefixed paths.
//! - **Turn-Pinning & Real SHA-256 Deduplication (`TurnPinCache`)**: Byte-identical historical
//!   turn pinning, 16-hex-char `sha2::Sha256` duplicate-file reference tag deduplication,
//!   and middle-out head/tail preservation.
//! - **Headless Tool Suite (NO `spawn_agent`; `skill` replaced by `tag`)**:
//!   `read_file`, `edit_file`, `write_file`, `list_directory`, `find_path`, `grep`,
//!   `create_directory`, `copy_path`, `move_path`, `delete_path`, `terminal`, `fetch`,
//!   `schedule_wakeup`, and `tag`.

use std::collections::{HashMap, VecDeque};
use std::fs;
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use once_cell::sync::Lazy;
use regex::{Regex, RegexBuilder};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use walkdir::WalkDir;

use crate::tags::{format_tags_rubric_block, ConversationTag, TagRegistry};

/// Tag emitted when an unchanged file is re-read within a turn.
pub const DUPLICATE_FILE_REF_TAG: &str = "duplicate_file_ref";

// ============================================================================
// Constants
// ============================================================================

/// Default maximum lines automatically expanded when `read_file` is called without explicit
/// `start_line` / `end_line` bounds.
pub const DEFAULT_MAX_AUTO_EXPAND_LINES: usize = 1_500;

/// Maximum characters returned for a single file or tool output before middle-out head/tail
/// truncation is applied (`48,000` chars).
pub const MAX_SINGLE_FILE_CHARS: usize = 48_000;

/// Maximum cumulative tool output characters across a turn/context window (`96,000` chars).
pub const MAX_TOTAL_TOOL_CHARS: usize = 96_000;

/// Maximum line length for unrestricted reads: `usize::MAX` (zero line truncation).
pub const UNBOUNDED_READ_MAX_LINE_LEN: usize = usize::MAX;

/// Banner emitted when a local agent wakeup timer fires.
pub const SCHEDULE_WAKEUP_BANNER: &str = "[SPELL AGENT TIMER ALARM FIRED]";

/// Canonical list of all headless tool names (no unrestricted `spawn_agent`; `skill` replaced by `tag`;
/// includes `prefill_context_subagent`, `delegate_coding_subagent`, and `apply_subagent_proposal`).
pub const HEADLESS_TOOL_NAMES: [&str; 17] = [
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

// ============================================================================
// Permissions & Profiles (`AgentProfile`, `ToolPermissionPolicy`)
// ============================================================================

/// Execution profile controlling which tools the agent may invoke.
#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum AgentProfile {
    /// Full read/write/terminal access (subject to `is_read_only` project constraints).
    #[default]
    Write,
    /// Read-only / inquiry profile: mutating tools and `terminal` are blocked unless overridden.
    Ask,
}

/// Policy governing whether a specific tool call is permitted, requires confirmation, or is denied.
#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ToolPermissionPolicy {
    #[default]
    Allow,
    Confirm,
    Deny,
}

/// Folder-level capability access tier.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum FolderAccessTier {
    ReadOnly,
    StageWrite,
    #[default]
    DirectWrite,
}

impl FolderAccessTier {
    /// Parses a [`FolderAccessTier`] from a string identifier.
    pub fn from_str_name(s: &str) -> Self {
        match s.trim().to_ascii_lowercase().as_str() {
            "read_only" | "readonly" | "ask" => Self::ReadOnly,
            "stage_write" | "stage" | "staged" => Self::StageWrite,
            _ => Self::DirectWrite,
        }
    }

    /// Canonical snake_case string name for this tier.
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::ReadOnly => "read_only",
            Self::StageWrite => "stage_write",
            Self::DirectWrite => "direct_write",
        }
    }
}

/// Environment variable names scrubbed from spawned child processes.
pub const SCRUBBED_SENSITIVE_ENV_VARS: [&str; 5] = [
    "SPELL_API_KEY",
    "OPENAI_API_KEY",
    "ANTHROPIC_API_KEY",
    "AWS_SECRET_ACCESS_KEY",
    "SSH_AUTH_SOCK",
];

/// Returns `true` if `tool_name` mutates the filesystem or executes shell commands.
pub fn is_mutating_tool(tool_name: &str) -> bool {
    matches!(
        tool_name,
        "edit_file"
            | "write_file"
            | "apply_subagent_proposal"
            | "create_directory"
            | "delete_path"
            | "move_path"
            | "copy_path"
            | "terminal"
    )
}

impl AgentProfile {
    /// Parses an [`AgentProfile`] from a string (e.g. `"write"`, `"ask"`).
    pub fn from_str_name(s: &str) -> Self {
        match s.trim().to_lowercase().as_str() {
            "ask" | "read_only" | "readonly" => Self::Ask,
            _ => Self::Write,
        }
    }

    /// Canonical lowercase name for the profile.
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Write => "write",
            Self::Ask => "ask",
        }
    }

    /// Returns the default [`ToolPermissionPolicy`] for `tool_name` under this profile.
    pub fn permission_for_tool(&self, tool_name: &str) -> ToolPermissionPolicy {
        if tool_name == "spawn_agent" || tool_name == "skill" {
            return ToolPermissionPolicy::Deny;
        }
        match self {
            Self::Write => {
                if tool_name == "apply_subagent_proposal" {
                    ToolPermissionPolicy::Confirm
                } else if HEADLESS_TOOL_NAMES.contains(&tool_name) {
                    ToolPermissionPolicy::Allow
                } else {
                    ToolPermissionPolicy::Deny
                }
            }
            Self::Ask => {
                if is_mutating_tool(tool_name) || tool_name == "delegate_coding_subagent" {
                    ToolPermissionPolicy::Deny
                } else if matches!(
                    tool_name,
                    "read_file"
                        | "list_directory"
                        | "find_path"
                        | "grep"
                        | "fetch"
                        | "schedule_wakeup"
                        | "tag"
                        | "prefill_context_subagent"
                ) {
                    ToolPermissionPolicy::Allow
                } else {
                    ToolPermissionPolicy::Deny
                }
            }
        }
    }

    /// Returns `true` if `tool_name` is available (not denied) in this profile's tool schema.
    pub fn allows_tool(&self, tool_name: &str) -> bool {
        self.permission_for_tool(tool_name) != ToolPermissionPolicy::Deny
    }
}

// ============================================================================
// Errors (`ToolError`)
// ============================================================================

/// Errors returned by headless agent tools.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum ToolError {
    SecurityViolation(String),
    NotFound(String),
    AmbiguousEdit(String),
    IoError(String),
    PermissionDenied(String),
    InvalidArguments(String),
}

impl std::fmt::Display for ToolError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ToolError::SecurityViolation(msg) => write!(f, "SECURITY_BLOCK: {msg}"),
            ToolError::NotFound(msg) => write!(f, "NOT_FOUND: {msg}"),
            ToolError::AmbiguousEdit(msg) => write!(f, "AMBIGUOUS_EDIT: {msg}"),
            ToolError::IoError(msg) => write!(f, "IO_ERROR: {msg}"),
            ToolError::PermissionDenied(msg) => write!(f, "PERMISSION_DENIED: {msg}"),
            ToolError::InvalidArguments(msg) => write!(f, "INVALID_ARGUMENTS: {msg}"),
        }
    }
}

impl std::error::Error for ToolError {}

// ============================================================================
// Tool error taxonomy & retry classification
// ============================================================================

/// Classifies a tool result into a stable error category, or `None` when the call did not fail.
///
/// Categories: `security`, `permission`, `not_found`, `invalid_arguments`, `ambiguous_edit`,
/// `timeout`, `io`, `parse`, `other`.
pub fn classify_tool_error(output: &str, status: &str) -> Option<&'static str> {
    if !matches!(status, "failed" | "denied") {
        return None;
    }
    let head = output.trim_start();
    let upper_head: String = head.chars().take(64).collect::<String>().to_ascii_uppercase();
    let lower = head.to_ascii_lowercase();
    let kind = if upper_head.starts_with("SECURITY_BLOCK") {
        "security"
    } else if upper_head.starts_with("PERMISSION_DENIED")
        || upper_head.starts_with("READ_ONLY_PROJECT")
        || upper_head.starts_with("USER_DENIED")
        || status == "denied"
    {
        "permission"
    } else if upper_head.starts_with("NOT_FOUND") {
        "not_found"
    } else if upper_head.starts_with("INVALID_ARGUMENTS")
        || upper_head.starts_with("INVALID_EDIT")
        || lower.starts_with("invalid regex")
    {
        "invalid_arguments"
    } else if upper_head.starts_with("AMBIGUOUS_EDIT") {
        "ambiguous_edit"
    } else if lower.contains("timed out") || lower.contains("timeout") {
        "timeout"
    } else if upper_head.starts_with("IO_ERROR")
        || lower.contains("resource temporarily unavailable")
        || lower.contains("interrupted system call")
    {
        "io"
    } else if lower.contains("parse") || lower.contains("invalid json") || lower.contains("utf-8") {
        "parse"
    } else {
        "other"
    };
    Some(kind)
}

/// Returns `true` for error categories that may succeed on an automatic retry.
pub fn is_retryable_error_kind(kind: &str) -> bool {
    matches!(kind, "io" | "timeout")
}

/// Read-only tools that are safe to retry automatically after a transient failure.
pub fn is_auto_retry_safe_tool(tool_name: &str) -> bool {
    matches!(
        tool_name,
        "read_file" | "grep" | "find_path" | "list_directory" | "outline_symbols" | "get_symbol_body"
    )
}

/// Returns the smallest outline symbol (function, method, class, ...) whose line span fully
/// contains `start_line..=end_line`, as `(symbol_start_line, symbol_end_line)` (1-indexed).
pub fn enclosing_symbol_range(
    content: &str,
    start_line: usize,
    end_line: Option<usize>,
) -> Option<(usize, usize)> {
    let start = start_line.max(1);
    let end = end_line.unwrap_or(start).max(start);
    extract_outline_symbols(content)
        .into_iter()
        .filter(|e| e.start_line <= start && e.end_line >= end && e.end_line >= e.start_line)
        .min_by_key(|e| e.end_line - e.start_line)
        .map(|e| (e.start_line, e.end_line))
}

// ============================================================================
// Workspace Path Resolution (`resolve_workspace_path`) & Guardrails
// ============================================================================

fn check_single_path_for_protected_globs(path_str: &str) -> Result<(), ToolError> {
    let (cleaned, _, _) = strip_path_line_col_suffix(path_str);
    let trimmed = cleaned.trim();
    if trimmed.is_empty() {
        return Ok(());
    }

    let lower = trimmed.to_ascii_lowercase();

    let segments: Vec<&str> = lower
        .split(['/', '\\'])
        .filter(|s| !s.is_empty() && *s != ".")
        .collect();

    for (idx, &seg) in segments.iter().enumerate() {
        // 1. `.git/config` or `.git/hooks` (`.git/hooks/*`)
        if seg == ".git" {
            if let Some(&next_seg) = segments.get(idx + 1) {
                if next_seg == "config" || next_seg == "hooks" {
                    return Err(ToolError::SecurityViolation(format!(
                        "Access to protected git path '{}' is forbidden",
                        trimmed
                    )));
                }
            }
        }

        // 2. `.env` or `.env.*` (e.g., `.env.local`, `.env.production`)
        if seg == ".env" || seg.starts_with(".env.") {
            return Err(ToolError::SecurityViolation(format!(
                "Access to protected environment file '{}' is forbidden",
                trimmed
            )));
        }

        // 3. `/.ssh/` or ends with `/.ssh`, `/.aws/` or ends with `/.aws`
        if seg == ".ssh" || seg == ".aws" {
            return Err(ToolError::SecurityViolation(format!(
                "Access to protected credential directory '{}' is forbidden",
                trimmed
            )));
        }
    }

    Ok(())
}

/// Enforces the Protected Globs Matrix across both `raw_input` and `canonical_path`:
/// - `.git/config` or any path inside `.git/hooks` (`.git/hooks/*`)
/// - `.env` or `.env.*` (e.g., `.env.local`, `.env.production`)
/// - `/.ssh/` or ending with `/.ssh`, `/.aws/` or ending with `/.aws`
pub fn enforce_protected_globs(canonical_path: &Path, raw_input: &str) -> Result<(), ToolError> {
    check_single_path_for_protected_globs(raw_input)?;
    check_single_path_for_protected_globs(&canonical_path.to_string_lossy())?;
    if let Ok(meta) = fs::symlink_metadata(canonical_path) {
        if meta.file_type().is_symlink() {
            if let Ok(link_target) = fs::read_link(canonical_path) {
                check_single_path_for_protected_globs(&link_target.to_string_lossy())?;
            }
        }
    }
    Ok(())
}

/// Validates that `path` does not violate protected globs security guardrails.
pub fn enforce_path_guardrail(path: &Path) -> Result<(), ToolError> {
    let raw = path.to_string_lossy();
    enforce_protected_globs(path, &raw)?;
    if path.exists() {
        if let Ok(canon) = fs::canonicalize(path) {
            check_single_path_for_protected_globs(&canon.to_string_lossy())?;
        }
    }
    Ok(())
}

/// Rejects any mutating filesystem operation targeting `.spell/research_cache` staging files
/// (including deleting/moving the parent directory when `research_cache` exists inside it).
pub fn enforce_research_cache_write_guard(path: &Path) -> Result<(), ToolError> {
    if crate::online_fetch::is_research_cache_path(path) {
        return Err(ToolError::SecurityViolation(
            "Cannot modify read-only research_cache staging files".to_string(),
        ));
    }
    if path
        .file_name()
        .and_then(|s| s.to_str())
        .map(|n| n.eq_ignore_ascii_case(".spell"))
        .unwrap_or(false)
    {
        let has_cache_child = path.join("research_cache").exists()
            || fs::read_dir(path)
                .ok()
                .map(|entries| {
                    entries.flatten().any(|e| {
                        e.file_name()
                            .to_str()
                            .map(|n| n.eq_ignore_ascii_case("research_cache"))
                            .unwrap_or(false)
                    })
                })
                .unwrap_or(false);
        if has_cache_child {
            return Err(ToolError::SecurityViolation(
                "Cannot modify read-only research_cache staging files".to_string(),
            ));
        }
    }
    Ok(())
}

/// Returns `true` if `rel_path` targets an auto-loaded workspace instruction file or build/IDE hook
/// (`AGENTS.md`, `CLAUDE.md`, `.rules`, `.cursorrules`, `build.rs`, `.cargo/config.toml`,
/// `.git/hooks/*`, `.vscode/tasks.json`, `.zed/tasks.json`), matched case-insensitively for macOS APFS.
pub fn is_web_taint_sensitive_workspace_file(rel_path: &str) -> bool {
    let (cleaned, _, _) = strip_path_line_col_suffix(rel_path);
    let normalized = cleaned.trim().replace('\\', "/");
    let segments: Vec<String> = normalized
        .split('/')
        .filter(|s| !s.is_empty() && *s != ".")
        .map(|s| s.to_ascii_lowercase())
        .collect();
    if segments.is_empty() {
        return false;
    }

    if let Some(last) = segments.last().map(String::as_str) {
        if matches!(
            last,
            "agents.md" | "claude.md" | ".rules" | ".cursorrules" | "build.rs"
        ) {
            return true;
        }
    }

    for (idx, seg) in segments.iter().enumerate() {
        match seg.as_str() {
            ".cargo" => {
                if matches!(
                    segments.get(idx + 1).map(String::as_str),
                    Some("config.toml" | "config")
                ) {
                    return true;
                }
            }
            ".git" => {
                if segments.get(idx + 1).map(String::as_str) == Some("hooks") {
                    return true;
                }
            }
            ".vscode" => {
                if matches!(
                    segments.get(idx + 1).map(String::as_str),
                    Some("tasks.json" | "launch.json")
                ) {
                    return true;
                }
            }
            ".zed" => {
                if segments.get(idx + 1).map(String::as_str) == Some("tasks.json") {
                    return true;
                }
            }
            _ => {}
        }
    }

    false
}

/// Canonicalizes a path or its deepest existing ancestor on disk, validating that the non-existent
/// tail contains no `..`, root, or prefix components.
fn canonicalize_ancestor_and_join_tail(path: &Path) -> Result<PathBuf, ToolError> {
    let mut tail_reversed: Vec<std::ffi::OsString> = Vec::new();
    let mut current = path.to_path_buf();

    loop {
        if current.exists() {
            let canonical_ancestor = fs::canonicalize(&current).map_err(|e| {
                ToolError::IoError(format!(
                    "Failed to canonicalize existing ancestor '{}': {e}",
                    current.display()
                ))
            })?;
            let mut result = canonical_ancestor;
            for comp in tail_reversed.into_iter().rev() {
                result.push(comp);
            }
            return Ok(result);
        }

        // Check if `current` is itself a broken/dangling symlink via `symlink_metadata` (`lstat`)
        if let Ok(meta) = fs::symlink_metadata(&current) {
            if meta.file_type().is_symlink() {
                let link_target = fs::read_link(&current).map_err(|e| {
                    ToolError::IoError(format!(
                        "Failed to read symlink '{}': {e}",
                        current.display()
                    ))
                })?;
                check_single_path_for_protected_globs(&link_target.to_string_lossy())?;
                let resolved_link = if link_target.is_absolute() {
                    link_target
                } else {
                    current
                        .parent()
                        .unwrap_or_else(|| Path::new("/"))
                        .join(link_target)
                };
                let mut canonical_link = canonicalize_ancestor_and_join_tail(&resolved_link)?;
                for comp in tail_reversed.into_iter().rev() {
                    canonical_link.push(comp);
                }
                return Ok(canonical_link);
            }
        }

        // Inspect the last component before popping
        let mut comps = current.components();
        let last_comp = comps.next_back();
        match last_comp {
            Some(Component::Normal(name)) => {
                tail_reversed.push(name.to_os_string());
                current = comps.as_path().to_path_buf();
                if current.as_os_str().is_empty() {
                    current = PathBuf::from(".");
                }
            }
            Some(Component::CurDir) => {
                current = comps.as_path().to_path_buf();
                if current.as_os_str().is_empty() {
                    current = PathBuf::from(".");
                }
            }
            Some(Component::ParentDir) | Some(Component::RootDir) | Some(Component::Prefix(_)) => {
                return Err(ToolError::SecurityViolation(
                    "Path traversal ('..') in non-existent tail is forbidden".to_string(),
                ));
            }
            None => {
                return Err(ToolError::SecurityViolation(format!(
                    "Unable to find existing ancestor for path '{}'",
                    path.display()
                )));
            }
        }
    }
}

/// Ancestor-based canonical path resolver across multiple `allowed_roots`:
/// 1. Rejects raw inputs starting with `~` (`~/...` or `~`) unless they canonicalize inside one of `allowed_roots`.
/// 2. Constructs the full candidate path (stripping leading `<worktree_name>/` prefix when relative, then joining onto `primary_root`).
/// 3. Checks existing prefix components for symlinks pointing outside `allowed_roots`, and walks up `full_candidate` until the deepest existing ancestor on disk is found.
/// 4. Calls `std::fs::canonicalize` on that deepest existing ancestor AND on `primary_root` / `allowed_roots` (resolving macOS `/var` -> `/private/var` and `/tmp` -> `/private/tmp` firmlinks).
/// 5. Rejects any `Component::ParentDir` (`..`), `Component::RootDir`, or `Component::Prefix` in the non-existent tail.
/// 6. Reconstructs `canonical_target = canonical_ancestor.join(tail)` and verifies `canonical_target.starts_with(&canonical_allowed_root)` for at least one root in `allowed_roots`.
pub fn canonicalize_target_path_multi(
    allowed_roots: &[PathBuf],
    primary_root: &Path,
    candidate: &Path,
) -> Result<PathBuf, ToolError> {
    let raw_candidate_str = candidate.to_string_lossy();
    let (cleaned_str, _, _) = strip_path_line_col_suffix(&raw_candidate_str);
    let cleaned = cleaned_str.trim();

    check_single_path_for_protected_globs(cleaned)?;

    // Build canonicalized allowed roots list (always including `primary_root`)
    let mut canonical_roots: Vec<PathBuf> = Vec::with_capacity(allowed_roots.len() + 1);
    let canonical_primary = canonicalize_ancestor_and_join_tail(primary_root)?;
    canonical_roots.push(canonical_primary.clone());
    for root in allowed_roots {
        if let Ok(canon_r) = canonicalize_ancestor_and_join_tail(root) {
            if !canonical_roots.contains(&canon_r) {
                canonical_roots.push(canon_r);
            }
        }
    }

    if cleaned.is_empty() || cleaned == "." || cleaned == "./" {
        enforce_protected_globs(&canonical_primary, cleaned)?;
        return Ok(canonical_primary);
    }

    // 1. Handle raw inputs starting with `~`
    let full_candidate: PathBuf = if cleaned == "~" || cleaned.starts_with("~/") {
        let home = std::env::var("HOME")
            .ok()
            .filter(|s| !s.trim().is_empty())
            .ok_or_else(|| {
                ToolError::SecurityViolation(
                    "Home directory ('~') expansion is not permitted outside workspace".to_string(),
                )
            })?;
        let home_path = PathBuf::from(home);
        let rel = cleaned.strip_prefix("~/").unwrap_or("");
        if rel.is_empty() {
            home_path
        } else {
            home_path.join(rel)
        }
    } else if cleaned.starts_with('~') {
        return Err(ToolError::SecurityViolation(format!(
            "Tilde user expansion '{}' is forbidden",
            cleaned
        )));
    } else {
        let cand_path = Path::new(cleaned);
        if cand_path.is_absolute() {
            cand_path.to_path_buf()
        } else {
            // 2. Relative path: strip leading `./` and optional `<worktree_name>/` prefix
            let mut normalized_rel = cleaned;
            while let Some(stripped) = normalized_rel.strip_prefix("./") {
                normalized_rel = stripped;
            }
            let rel_path = Path::new(normalized_rel);

            let mut joined_opt: Option<PathBuf> = None;
            let mut comps = rel_path.components();
            if let Some(Component::Normal(first)) = comps.next() {
                let remainder: PathBuf = comps.collect();
                if let Some(worktree_name) = primary_root.file_name() {
                    if first == worktree_name {
                        joined_opt = Some(if remainder.as_os_str().is_empty() {
                            primary_root.to_path_buf()
                        } else {
                            primary_root.join(&remainder)
                        });
                    }
                }
                if joined_opt.is_none() {
                    for extra_root in allowed_roots {
                        if extra_root.file_name() == Some(first) {
                            joined_opt = Some(if remainder.as_os_str().is_empty() {
                                extra_root.to_path_buf()
                            } else {
                                extra_root.join(&remainder)
                            });
                            break;
                        }
                    }
                }
            }
            joined_opt.unwrap_or_else(|| primary_root.join(rel_path))
        }
    };

    // 3. Before walking ancestors, inspect any symlink component at or below the workspace root
    // (so symlinks pointing outside `allowed_roots` or to protected paths are caught even if dangling
    // or followed by non-existent tail segments).
    let mut prefix_acc = PathBuf::new();
    let mut comps_iter = full_candidate.components().peekable();
    while let Some(comp) = comps_iter.next() {
        prefix_acc.push(comp.as_os_str());
        // Skip system firmlinks/symlinks above `primary_root` (such as macOS `/var` -> `private/var`)
        let is_ancestor_of_root = primary_root.starts_with(&prefix_acc)
            || allowed_roots.iter().any(|r| r.starts_with(&prefix_acc));
        if is_ancestor_of_root && comps_iter.peek().is_some() {
            continue;
        }
        if let Ok(meta) = fs::symlink_metadata(&prefix_acc) {
            if meta.file_type().is_symlink() {
                let link_target = fs::read_link(&prefix_acc).map_err(|e| {
                    ToolError::IoError(format!(
                        "Failed to read symlink '{}': {e}",
                        prefix_acc.display()
                    ))
                })?;
                check_single_path_for_protected_globs(&link_target.to_string_lossy())?;
                let resolved_link = if link_target.is_absolute() {
                    link_target
                } else {
                    prefix_acc
                        .parent()
                        .unwrap_or_else(|| Path::new("/"))
                        .join(link_target)
                };
                let canon_link = canonicalize_ancestor_and_join_tail(&resolved_link)?;
                check_single_path_for_protected_globs(&canon_link.to_string_lossy())?;
                if !canonical_roots
                    .iter()
                    .any(|root| canon_link.starts_with(root))
                {
                    return Err(ToolError::SecurityViolation(format!(
                        "Symlink '{}' points outside allowed workspace roots ('{}')",
                        prefix_acc.display(),
                        canon_link.display()
                    )));
                }
            }
        }
    }

    // 4, 5 & 6. Walk up `full_candidate` to deepest existing ancestor, canonicalize, validate non-existent tail,
    // and verify `canonical_target.starts_with(&canonical_allowed_root)`.
    let canonical_target = canonicalize_ancestor_and_join_tail(&full_candidate)?;
    enforce_protected_globs(&canonical_target, cleaned)?;

    if !canonical_roots
        .iter()
        .any(|root| canonical_target.starts_with(root))
    {
        return Err(ToolError::SecurityViolation(format!(
            "Target path '{}' (canonical '{}') escapes allowed workspace roots",
            candidate.display(),
            canonical_target.display()
        )));
    }

    Ok(canonical_target)
}

/// Ancestor-based canonical path resolver confining `candidate` strictly within `workspace_root`.
pub fn canonicalize_target_path(
    workspace_root: &Path,
    candidate: &Path,
) -> Result<PathBuf, ToolError> {
    canonicalize_target_path_multi(&[workspace_root.to_path_buf()], workspace_root, candidate)
}

/// Verifies whether `target` may be written under `active_tier` and `registered_roots`:
/// 1. Finds the longest matching canonical root in `registered_roots` and denies if its tier or `active_tier` is `FolderAccessTier::ReadOnly`.
/// 2. Returns the effective write tier (`StageWrite` or `DirectWrite`).
pub fn verify_write_access_for_path(
    target: &Path,
    active_tier: FolderAccessTier,
    registered_roots: &[(PathBuf, FolderAccessTier)],
) -> Result<FolderAccessTier, ToolError> {
    if active_tier == FolderAccessTier::ReadOnly {
        return Err(ToolError::PermissionDenied(format!(
            "Write access denied for '{}': active folder access tier is ReadOnly",
            target.display()
        )));
    }

    if !registered_roots.is_empty() {
        let canonical_target = canonicalize_ancestor_and_join_tail(target)?;
        let mut best_match: Option<(usize, FolderAccessTier)> = None;

        for (root_path, root_tier) in registered_roots {
            if let Ok(canonical_root) = canonicalize_ancestor_and_join_tail(root_path) {
                if canonical_target.starts_with(&canonical_root) {
                    let depth = canonical_root.components().count();
                    if best_match.map_or(true, |(best_depth, _)| depth > best_depth) {
                        best_match = Some((depth, *root_tier));
                    }
                }
            }
        }

        match best_match {
            Some((_, matched_tier)) => {
                if matched_tier == FolderAccessTier::ReadOnly {
                    return Err(ToolError::PermissionDenied(format!(
                        "Write access denied for '{}': target root is Read-Only",
                        target.display()
                    )));
                }
                if matched_tier == FolderAccessTier::StageWrite
                    || active_tier == FolderAccessTier::StageWrite
                {
                    return Ok(FolderAccessTier::StageWrite);
                }
                return Ok(FolderAccessTier::DirectWrite);
            }
            None => {
                return Err(ToolError::PermissionDenied(format!(
                    "Write access denied for '{}': path is outside registered workspace roots",
                    target.display()
                )));
            }
        }
    }

    Ok(active_tier)
}

/// Strips trailing `:line:col`, `:line`, or `#L123-L456` / `#L123` location suffixes from a path string,
/// optionally returning the parsed 1-indexed `(start_line, end_line)` range if present.
pub fn strip_path_line_col_suffix(raw_path: &str) -> (&str, Option<usize>, Option<usize>) {
    let trimmed = raw_path.trim();

    // 1. Check `#L123-L456` or `#L123` anchor suffix
    if let Some(hash_idx) = trimmed.rfind("#L") {
        let base = &trimmed[..hash_idx];
        let range_part = &trimmed[hash_idx + 2..];
        if !base.is_empty() {
            if let Some((start_s, end_s)) = range_part.split_once('-') {
                let end_clean = end_s.strip_prefix('L').unwrap_or(end_s);
                if let (Ok(s), Ok(e)) = (start_s.parse::<usize>(), end_clean.parse::<usize>()) {
                    return (base, Some(s), Some(e));
                }
            } else if let Ok(s) = range_part.parse::<usize>() {
                return (base, Some(s), Some(s));
            }
        }
    }

    // 2. Check trailing `:line:col` or `:line` suffix after the final `/` or `\`
    let last_slash = trimmed.rfind(['/', '\\']).unwrap_or(0);
    let filename_slice = &trimmed[last_slash..];
    if let Some(first_colon_rel) = filename_slice.find(':') {
        let colon_idx = last_slash + first_colon_rel;
        let base = &trimmed[..colon_idx];
        let suffix = &trimmed[colon_idx + 1..];
        if !base.is_empty() && !suffix.is_empty() {
            let parts: Vec<&str> = suffix.split(':').collect();
            if parts.len() <= 2 && parts.iter().all(|p| !p.is_empty() && p.chars().all(|c| c.is_ascii_digit())) {
                let line = parts[0].parse::<usize>().ok();
                return (base, line, line);
            }
        }
    }

    (trimmed, None, None)
}

/// Resolves `raw_path` across `primary_root` and optional `extra_roots` (such as a registered
/// read-only reference root), enforcing `enforce_protected_globs` and `canonicalize_target_path_multi`.
pub fn resolve_workspace_path_with_roots(
    primary_root: &Path,
    extra_roots: &[PathBuf],
    raw_path: &str,
) -> Result<PathBuf, ToolError> {
    enforce_path_guardrail(primary_root)?;
    let (cleaned, _, _) = strip_path_line_col_suffix(raw_path);
    let cleaned = cleaned.trim();

    check_single_path_for_protected_globs(raw_path)?;
    check_single_path_for_protected_globs(cleaned)?;

    let mut all_roots = Vec::with_capacity(extra_roots.len() + 1);
    all_roots.push(primary_root.to_path_buf());
    for r in extra_roots {
        enforce_path_guardrail(r)?;
        if !all_roots.contains(r) {
            all_roots.push(r.clone());
        }
    }

    let canonical_target =
        canonicalize_target_path_multi(&all_roots, primary_root, Path::new(cleaned))?;
    enforce_protected_globs(&canonical_target, raw_path)?;

    // Preserve the caller's root prefix representation (e.g., `/var/folders/...` on macOS when
    // `primary_root` was passed via `std::env::temp_dir()`) while keeping the canonicalized
    // relative suffix inside the matched root.
    for root in &all_roots {
        if let Ok(canon_root) = canonicalize_ancestor_and_join_tail(root) {
            if let Ok(rel_within_root) = canonical_target.strip_prefix(&canon_root) {
                return if rel_within_root.as_os_str().is_empty() {
                    Ok(root.clone())
                } else {
                    Ok(root.join(rel_within_root))
                };
            }
        }
    }

    Ok(canonical_target)
}

/// Resolves `raw_path` strictly confined within `project_root`:
/// - Enforces `enforce_protected_globs` (`.git/config`, `.git/hooks/*`, `.env*`, `.ssh`, `.aws`).
/// - Strips trailing `:line:col` or `#L123-L456`.
/// - Resolves relative and `<worktree_name>/...` paths via `canonicalize_target_path(project_root, ...)`,
///   rejecting any symlink, `..` traversal, `~` path, or absolute path that escapes `project_root`.
pub fn resolve_workspace_path(project_root: &Path, raw_path: &str) -> Result<PathBuf, ToolError> {
    resolve_workspace_path_with_roots(project_root, &[], raw_path)
}

/// Canonical workspace root ACL path validation helper.
pub fn validate_path(project_root: &Path, raw_path: &str) -> Result<PathBuf, ToolError> {
    resolve_workspace_path(project_root, raw_path)
}

#[inline]
fn debug_app_sandbox_env_override() -> bool {
    #[cfg(any(test, debug_assertions))]
    {
        std::env::var("SPELL_APP_SANDBOX").ok().as_deref() == Some("1")
    }
    #[cfg(not(any(test, debug_assertions)))]
    {
        false
    }
}

/// Returns `true` when running inside an OS App Sandbox (`APP_SANDBOX_CONTAINER_ID` is set),
/// or in debug/test builds when `SPELL_APP_SANDBOX=1` is explicitly enabled.
pub fn is_app_sandbox_active() -> bool {
    std::env::var_os("APP_SANDBOX_CONTAINER_ID").is_some() || debug_app_sandbox_env_override()
}

// ============================================================================
// Middle-Out Truncation & Real SHA-256 Content Hashing
// ============================================================================

/// Computes the full 64-character lowercase hex `sha2::Sha256` digest of `content`.
pub fn compute_sha256_full_hex(content: &str) -> String {
    let mut hasher = Sha256::new();
    hasher.update(content.as_bytes());
    let digest = hasher.finalize();
    let mut hex = String::with_capacity(64);
    for b in digest {
        hex.push_str(&format!("{b:02x}"));
    }
    hex
}

/// Computes the first 16 lowercase hex characters of the real `sha2::Sha256` digest of `content`
/// for duplicate-file reference deduplication.
pub fn compute_sha256_short_hex(content: &str) -> String {
    let full = compute_sha256_full_hex(content);
    full[..16].to_string()
}

/// Computes the real `sha2::Sha256` hex digest for content deduplication and file stats.
pub fn compute_content_hash_hex(content: &str) -> String {
    compute_sha256_short_hex(content)
}

/// Preserves the head and tail of `content` when its character count exceeds `max_chars`,
/// replacing the middle section with a compact truncation marker so total length `<= max_chars`.
pub fn truncate_middle_out(content: &str, max_chars: usize) -> String {
    let total_chars = content.chars().count();
    if total_chars <= max_chars || max_chars == 0 {
        return content.to_string();
    }

    let marker_template_reserve = 96usize;
    if max_chars <= marker_template_reserve {
        return content.chars().take(max_chars).collect();
    }

    let budget = max_chars - marker_template_reserve;
    let head_chars = (budget * 3) / 5; // 60% head
    let tail_chars = budget - head_chars; // 40% tail
    let omitted_chars = total_chars.saturating_sub(head_chars + tail_chars);

    let head: String = content.chars().take(head_chars).collect();
    let tail: String = content
        .chars()
        .skip(total_chars.saturating_sub(tail_chars))
        .collect();

    format!(
        "{head}\n... [middle-out truncated {omitted_chars} chars to fit {max_chars} char budget] ...\n{tail}"
    )
}

// ============================================================================
// Turn-Pinning & SHA-256 Duplicate File Deduplication (`TurnPinCache`)
// ============================================================================

/// Entry tracked inside [`TurnPinCache`] for duplicate file deduplication.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SeenFileEntry {
    pub path: String,
    pub sha256: String,
    pub total_lines: usize,
    pub first_tool_call_id: String,
}

/// Prefix-Safe Turn-Pinning & SHA-256 Duplicate File Deduplication Cache.
///
/// Guarantees:
/// 1. **Turn-Pinning**: Once a `tool_call_id` produces output on Turn $T$, that exact string is
///    pinned in `pinned_outputs_by_call_id`. Re-serializing historical turns returns the
///    100% byte-identical output from Turn $T$.
/// 2. **SHA-256 Duplicate File Deduplication**: Computes real `sha2::Sha256` (first 16 hex chars)
///    of file contents and, if the same unchanged file is read again in the same thread, returns:
///    duplicate-file reference tag (`path`, `lines="1-N"`, `sha256`)
/// 3. **Character Budgets**: Enforces `MAX_SINGLE_FILE_CHARS` (`48,000`) with middle-out head/tail
///    preservation and `MAX_TOTAL_TOOL_CHARS` (`96,000`).
#[derive(Debug, Clone)]
pub struct TurnPinCache {
    pinned_outputs_by_call_id: HashMap<String, String>,
    seen_files_by_path: HashMap<String, SeenFileEntry>,
    total_tool_chars: usize,
    max_single_file_chars: usize,
    max_total_tool_chars: usize,
}

impl Default for TurnPinCache {
    fn default() -> Self {
        Self::new()
    }
}

impl TurnPinCache {
    pub fn new() -> Self {
        Self {
            pinned_outputs_by_call_id: HashMap::new(),
            seen_files_by_path: HashMap::new(),
            total_tool_chars: 0,
            max_single_file_chars: MAX_SINGLE_FILE_CHARS,
            max_total_tool_chars: MAX_TOTAL_TOOL_CHARS,
        }
    }

    /// Creates a [`TurnPinCache`] with custom per-file and total character budgets.
    pub fn with_budgets(max_single_file_chars: usize, max_total_tool_chars: usize) -> Self {
        Self {
            pinned_outputs_by_call_id: HashMap::new(),
            seen_files_by_path: HashMap::new(),
            total_tool_chars: 0,
            max_single_file_chars,
            max_total_tool_chars,
        }
    }

    /// Returns the cumulative characters pinned across tool outputs in the current epoch.
    pub fn total_tool_chars(&self) -> usize {
        self.total_tool_chars
    }

    /// Drops a pinned output so a retried call with the same `tool_call_id` re-executes.
    pub fn forget_tool_output(&mut self, tool_call_id: &str) {
        self.pinned_outputs_by_call_id.remove(tool_call_id);
    }

    /// Applies `MAX_SINGLE_FILE_CHARS` and `MAX_TOTAL_TOOL_CHARS` budgets using middle-out truncation
    /// and pins the resulting string for `tool_call_id`.
    pub fn pin_tool_output(&mut self, tool_call_id: &str, raw_output: String) -> String {
        if let Some(existing) = self.pinned_outputs_by_call_id.get(tool_call_id) {
            return existing.clone();
        }

        let single_bounded = truncate_middle_out(&raw_output, self.max_single_file_chars);
        let remaining_total = self
            .max_total_tool_chars
            .saturating_sub(self.total_tool_chars);

        let final_output = if remaining_total == 0 {
            truncate_middle_out(&single_bounded, 256)
        } else if single_bounded.chars().count() > remaining_total {
            truncate_middle_out(&single_bounded, remaining_total)
        } else {
            single_bounded
        };

        self.total_tool_chars = self
            .total_tool_chars
            .saturating_add(final_output.chars().count());
        self.pinned_outputs_by_call_id
            .insert(tool_call_id.to_string(), final_output.clone());
        final_output
    }

    /// Reads a file through the Turn-Pinning & SHA-256 Duplicate File Deduplication layer.
    pub fn read_file_pinned(
        &mut self,
        tool_call_id: &str,
        path: &Path,
        start_line: Option<usize>,
        end_line: Option<usize>,
    ) -> Result<String, ToolError> {
        // 1. If this tool_call_id was already evaluated in an earlier turn, return its exact pinned bytes.
        if let Some(pinned) = self.pinned_outputs_by_call_id.get(tool_call_id) {
            return Ok(pinned.clone());
        }

        // 2. Perform unrestricted read.
        let content = read_file_unrestricted(path, start_line, end_line)?;
        let canonical_key = path.to_string_lossy().to_string();
        let sha256_short = compute_sha256_short_hex(&content);
        let line_count = content.lines().count().max(1);

        // 3. Check if this exact file + range SHA-256 was already injected earlier in the thread.
        let output = if let Some(seen) = self.seen_files_by_path.get(&canonical_key) {
            if seen.sha256 == sha256_short {
                format!(
                    "<{DUPLICATE_FILE_REF_TAG} path=\"{}\" lines=\"1-{}\" sha256=\"{}\"/>",
                    canonical_key, seen.total_lines, seen.sha256
                )
            } else {
                self.seen_files_by_path.insert(
                    canonical_key.clone(),
                    SeenFileEntry {
                        path: canonical_key,
                        sha256: sha256_short,
                        total_lines: line_count,
                        first_tool_call_id: tool_call_id.to_string(),
                    },
                );
                content
            }
        } else {
            self.seen_files_by_path.insert(
                canonical_key.clone(),
                SeenFileEntry {
                    path: canonical_key,
                    sha256: sha256_short,
                    total_lines: line_count,
                    first_tool_call_id: tool_call_id.to_string(),
                },
            );
            content
        };

        // 4. Enforce character budgets (`MAX_SINGLE_FILE_CHARS` and `MAX_TOTAL_TOOL_CHARS`) and pin.
        Ok(self.pin_tool_output(tool_call_id, output))
    }

    /// Invalidates the seen-file entry for `path` when the file is modified or deleted.
    pub fn invalidate_path(&mut self, path: &Path) {
        let key = path.to_string_lossy().to_string();
        self.seen_files_by_path.remove(&key);
    }

    /// Returns the pinned output for a historical `tool_call_id` if present.
    pub fn get_pinned_output(&self, tool_call_id: &str) -> Option<&str> {
        self.pinned_outputs_by_call_id
            .get(tool_call_id)
            .map(|s| s.as_str())
    }

    /// Clears duplicate-file tracking and resets the per-epoch character counter across a
    /// compaction boundary while retaining pinned historical `tool_call_id` outputs.
    pub fn clear_seen_files_for_new_epoch(&mut self) {
        self.seen_files_by_path.clear();
        self.total_tool_chars = 0;
    }
}

// ============================================================================
// Tool 1: `read_file` (`read_file_unrestricted` + `read_file_auto_expand`)
// ============================================================================

/// Reads a file with zero line truncation (`max_line_len = usize::MAX`) and NO 16 KB
/// symbol-outline replacement.
///
/// - Enforces protected-glob path guardrails.
/// - If `path` is a directory, falls back to a formatted `[DIR]` / `[FILE]` listing instead
///   of failing with `EISDIR`.
/// - Supports optional 1-indexed inclusive `[start_line, end_line]` range (`0` means unbounded).
pub fn read_file_unrestricted(
    path: &Path,
    start_line: Option<usize>,
    end_line: Option<usize>,
) -> Result<String, ToolError> {
    enforce_path_guardrail(path)?;

    if !path.exists() {
        return Err(ToolError::NotFound(format!(
            "Path does not exist: {}",
            path.display()
        )));
    }

    if path.is_dir() {
        return list_directory(path);
    }

    let content = fs::read_to_string(path)
        .map_err(|e| ToolError::IoError(format!("Failed to read {}: {e}", path.display())))?;

    let start = start_line.unwrap_or(1).max(1);
    let end = end_line.unwrap_or(usize::MAX);

    if start == 1 && end == usize::MAX {
        return Ok(content);
    }

    let lines: Vec<&str> = content.lines().collect();
    if lines.is_empty() {
        return Ok(String::new());
    }

    let start_idx = (start - 1).min(lines.len());
    let end_idx = end.min(lines.len());
    if start_idx >= end_idx {
        return Ok(String::new());
    }

    let mut selected = String::new();
    for (i, line) in lines[start_idx..end_idx].iter().enumerate() {
        if i > 0 {
            selected.push('\n');
        }
        let slice_len = line.len().min(UNBOUNDED_READ_MAX_LINE_LEN);
        selected.push_str(&line[..slice_len]);
    }
    Ok(selected)
}

/// Reads a workspace file with automatic windowing and SHA-256 deduplication:
/// - Resolves `raw_path` relative to `project_root` (including worktree-prefixed paths and `:line:col` / `#L` stripping).
/// - Defaults to `1..=min(total_lines, DEFAULT_MAX_AUTO_EXPAND_LINES)` (1,500 lines) when `start_line` and `end_line`
///   are both omitted, or reads the requested range when specified.
/// - Falls back to `[DIR]` / `[FILE]` directory listing if the resolved path is a directory.
/// - Deduplicates unchanged repeat reads via `TurnPinCache` (duplicate-file reference tag).
pub fn execute_read_file_tool(
    project_root: &Path,
    raw_path: &str,
    start_line: Option<usize>,
    end_line: Option<usize>,
    pin_cache: &mut TurnPinCache,
    tool_call_id: &str,
) -> Result<String, ToolError> {
    if let Some(pinned) = pin_cache.get_pinned_output(tool_call_id) {
        return Ok(pinned.to_string());
    }

    let (_, suffix_start, suffix_end) = strip_path_line_col_suffix(raw_path);
    let resolved = resolve_workspace_path(project_root, raw_path)?;

    if !resolved.exists() {
        return Err(ToolError::NotFound(format!(
            "Path does not exist: {}",
            resolved.display()
        )));
    }

    if resolved.is_dir() {
        let listing = list_directory(&resolved)?;
        return Ok(pin_cache.pin_tool_output(tool_call_id, listing));
    }

    let effective_start = start_line.or(suffix_start);
    let effective_end = end_line.or(suffix_end);

    let full_content = fs::read_to_string(&resolved)
        .map_err(|e| ToolError::IoError(format!("Failed to read {}: {e}", resolved.display())))?;
    let total_lines = full_content.lines().count();

    let (req_start, req_end) = match (effective_start, effective_end) {
        (None, None) => (1usize, total_lines.min(DEFAULT_MAX_AUTO_EXPAND_LINES).max(1)),
        (Some(s), None) => (s.max(1), total_lines.max(1)),
        (None, Some(e)) => (1usize, e.max(1)),
        (Some(s), Some(e)) => (s.max(1), e.max(s.max(1))),
    };

    let selected = if req_start == 1 && req_end >= total_lines {
        full_content
    } else {
        read_file_unrestricted(&resolved, Some(req_start), Some(req_end))?
    };

    let canonical_key = resolved.to_string_lossy().to_string();
    let sha256_short = compute_sha256_short_hex(&selected);
    let shown_lines = selected.lines().count().max(1);

    let output = if let Some(seen) = pin_cache.seen_files_by_path.get(&canonical_key) {
        if seen.sha256 == sha256_short {
            format!(
                "<{DUPLICATE_FILE_REF_TAG} path=\"{}\" lines=\"1-{}\" sha256=\"{}\"/>",
                canonical_key, seen.total_lines, seen.sha256
            )
        } else {
            pin_cache.seen_files_by_path.insert(
                canonical_key.clone(),
                SeenFileEntry {
                    path: canonical_key,
                    sha256: sha256_short,
                    total_lines: shown_lines,
                    first_tool_call_id: tool_call_id.to_string(),
                },
            );
            selected
        }
    } else {
        pin_cache.seen_files_by_path.insert(
            canonical_key.clone(),
            SeenFileEntry {
                path: canonical_key,
                sha256: sha256_short,
                total_lines: shown_lines,
                first_tool_call_id: tool_call_id.to_string(),
            },
        );
        selected
    };

    Ok(pin_cache.pin_tool_output(tool_call_id, output))
}

// ============================================================================
// Tool 2: `edit_file` (`FileEditOperation`, `EditFileToolInput`, `edit_file_surgical`)
// ============================================================================

/// Single search-and-replace operation for `edit_file`.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct FileEditOperation {
    pub old_text: String,
    pub new_text: String,
}

/// Input structure for `edit_file` supporting both `edits` array and single `old_text`/`new_text` fallback.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct EditFileToolInput {
    pub path: String,
    #[serde(default)]
    pub display_description: Option<String>,
    #[serde(default)]
    pub edits: Vec<FileEditOperation>,
    #[serde(default)]
    pub old_text: Option<String>,
    #[serde(default)]
    pub new_text: Option<String>,
}

/// Surgically replaces an exact `old_text` block with `new_text` inside `path`.
/// Rejects ambiguous edits if `old_text` occurs 0 times or more than once.
pub fn edit_file_surgical(path: &Path, old_text: &str, new_text: &str) -> Result<String, ToolError> {
    enforce_path_guardrail(path)?;
    enforce_research_cache_write_guard(path)?;
    let content = fs::read_to_string(path)
        .map_err(|e| ToolError::IoError(format!("Failed to read {}: {e}", path.display())))?;

    let matches: Vec<_> = content.match_indices(old_text).collect();
    if matches.is_empty() {
        return Err(ToolError::NotFound(format!(
            "Target text not found in {}",
            path.display()
        )));
    }
    if matches.len() > 1 {
        return Err(ToolError::AmbiguousEdit(format!(
            "Target text matched {} times in {}; surgical edit requires a unique target block",
            matches.len(),
            path.display()
        )));
    }

    let updated = content.replacen(old_text, new_text, 1);
    fs::write(path, &updated)
        .map_err(|e| ToolError::IoError(format!("Failed to write {}: {e}", path.display())))?;

    Ok(format!(
        "Surgically updated {} ({} bytes -> {} bytes)",
        path.display(),
        content.len(),
        updated.len()
    ))
}

/// Executes `edit_file` using `EditFileToolInput` (supporting `edits: Vec<FileEditOperation>`
/// as well as single `old_text`/`new_text` fallback), blocking if `is_read_only` is true.
pub fn execute_edit_file_tool(
    project_root: &Path,
    input: &EditFileToolInput,
    is_read_only: bool,
) -> Result<String, ToolError> {
    if is_read_only {
        return Err(ToolError::PermissionDenied(
            "Cannot execute edit_file: project is mounted read-only".to_string(),
        ));
    }

    let resolved = resolve_workspace_path(project_root, &input.path)?;
    enforce_research_cache_write_guard(&resolved)?;
    if !resolved.exists() {
        return Err(ToolError::NotFound(format!(
            "Cannot edit non-existent file: {}",
            resolved.display()
        )));
    }

    let mut operations = input.edits.clone();
    if operations.is_empty() {
        if let (Some(old_t), Some(new_t)) = (&input.old_text, &input.new_text) {
            operations.push(FileEditOperation {
                old_text: old_t.clone(),
                new_text: new_t.clone(),
            });
        }
    }

    if operations.is_empty() {
        return Err(ToolError::InvalidArguments(
            "edit_file requires either non-empty `edits` array or `old_text` + `new_text`".to_string(),
        ));
    }

    let original = fs::read_to_string(&resolved)
        .map_err(|e| ToolError::IoError(format!("Failed to read {}: {e}", resolved.display())))?;
    let mut current = original.clone();

    for (idx, op) in operations.iter().enumerate() {
        if op.old_text.is_empty() {
            return Err(ToolError::InvalidArguments(format!(
                "Edit operation #{} has empty `old_text`",
                idx + 1
            )));
        }
        let count = current.matches(&op.old_text).count();
        if count == 0 {
            return Err(ToolError::NotFound(format!(
                "Edit operation #{}: target text not found in {}",
                idx + 1,
                resolved.display()
            )));
        }
        if count > 1 {
            return Err(ToolError::AmbiguousEdit(format!(
                "Edit operation #{}: target text matched {} times in {}; provide more surrounding context",
                idx + 1,
                count,
                resolved.display()
            )));
        }
        current = current.replacen(&op.old_text, &op.new_text, 1);
    }

    fs::write(&resolved, &current)
        .map_err(|e| ToolError::IoError(format!("Failed to write {}: {e}", resolved.display())))?;

    let desc = input
        .display_description
        .as_deref()
        .map(|d| format!(" ({d})"))
        .unwrap_or_default();

    Ok(format!(
        "Surgically updated {}{} [{} edit(s), {} bytes -> {} bytes]",
        resolved.display(),
        desc,
        operations.len(),
        original.len(),
        current.len()
    ))
}

// ============================================================================
// Tool 3-10: Filesystem & Search Tools (`write_file`, `list_directory`,
// `find_path`, `grep`, `create_directory`, `copy_path`, `move_path`, `delete_path`)
// ============================================================================

/// Writes or appends `content` to `path`, creating parent directories automatically.
pub fn write_file(path: &Path, content: &str, append: bool) -> Result<String, ToolError> {
    enforce_path_guardrail(path)?;
    enforce_research_cache_write_guard(path)?;
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|e| {
                ToolError::IoError(format!(
                    "Failed to create parent dir {}: {e}",
                    parent.display()
                ))
            })?;
        }
    }

    if append {
        let mut file = fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(path)
            .map_err(|e| ToolError::IoError(format!("Failed to open {}: {e}", path.display())))?;
        file.write_all(content.as_bytes())
            .map_err(|e| ToolError::IoError(format!("Failed to append {}: {e}", path.display())))?;
    } else {
        fs::write(path, content)
            .map_err(|e| ToolError::IoError(format!("Failed to write {}: {e}", path.display())))?;
    }

    Ok(format!(
        "Wrote {} bytes to {} (append={})",
        content.len(),
        path.display(),
        append
    ))
}

/// Creates a directory and all missing parent directories.
pub fn create_directory(path: &Path) -> Result<String, ToolError> {
    enforce_path_guardrail(path)?;
    enforce_research_cache_write_guard(path)?;
    fs::create_dir_all(path)
        .map_err(|e| ToolError::IoError(format!("Failed to create dir {}: {e}", path.display())))?;
    Ok(format!("Created directory {}", path.display()))
}

/// Deletes a file or directory tree.
pub fn delete_path(path: &Path) -> Result<String, ToolError> {
    enforce_path_guardrail(path)?;
    enforce_research_cache_write_guard(path)?;
    if !path.exists() {
        return Err(ToolError::NotFound(format!(
            "Cannot delete non-existent path: {}",
            path.display()
        )));
    }
    if path.is_dir() {
        fs::remove_dir_all(path).map_err(|e| {
            ToolError::IoError(format!("Failed to remove dir {}: {e}", path.display()))
        })?;
    } else {
        fs::remove_file(path).map_err(|e| {
            ToolError::IoError(format!("Failed to remove file {}: {e}", path.display()))
        })?;
    }
    Ok(format!("Deleted {}", path.display()))
}

/// Moves/renames `src` to `dst`, creating destination parent directories if needed.
pub fn move_path(src: &Path, dst: &Path) -> Result<String, ToolError> {
    enforce_path_guardrail(src)?;
    enforce_path_guardrail(dst)?;
    enforce_research_cache_write_guard(src)?;
    enforce_research_cache_write_guard(dst)?;
    if !src.exists() {
        return Err(ToolError::NotFound(format!(
            "Source path does not exist: {}",
            src.display()
        )));
    }
    if let Some(parent) = dst.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|e| {
                ToolError::IoError(format!(
                    "Failed to create parent dir {}: {e}",
                    parent.display()
                ))
            })?;
        }
    }
    fs::rename(src, dst).map_err(|e| {
        ToolError::IoError(format!(
            "Failed to move {} to {}: {e}",
            src.display(),
            dst.display()
        ))
    })?;
    Ok(format!("Moved {} -> {}", src.display(), dst.display()))
}

/// Copies a file or directory from `src` to `dst`.
pub fn copy_path(src: &Path, dst: &Path) -> Result<String, ToolError> {
    enforce_path_guardrail(src)?;
    enforce_path_guardrail(dst)?;
    enforce_research_cache_write_guard(dst)?;
    if !src.exists() {
        return Err(ToolError::NotFound(format!(
            "Source path does not exist: {}",
            src.display()
        )));
    }
    if src.is_dir() {
        copy_dir_recursive(src, dst)?;
        return Ok(format!(
            "Copied directory {} -> {}",
            src.display(),
            dst.display()
        ));
    }
    if let Some(parent) = dst.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent).map_err(|e| {
                ToolError::IoError(format!(
                    "Failed to create parent dir {}: {e}",
                    parent.display()
                ))
            })?;
        }
    }
    let bytes = fs::copy(src, dst).map_err(|e| {
        ToolError::IoError(format!(
            "Failed to copy {} to {}: {e}",
            src.display(),
            dst.display()
        ))
    })?;
    Ok(format!(
        "Copied {} -> {} ({} bytes)",
        src.display(),
        dst.display(),
        bytes
    ))
}

fn copy_dir_recursive(src: &Path, dst: &Path) -> Result<(), ToolError> {
    enforce_path_guardrail(src)?;
    enforce_path_guardrail(dst)?;
    fs::create_dir_all(dst)
        .map_err(|e| ToolError::IoError(format!("Failed to create dir {}: {e}", dst.display())))?;
    let entries = fs::read_dir(src)
        .map_err(|e| ToolError::IoError(format!("Failed to read dir {}: {e}", src.display())))?;
    for entry in entries.flatten() {
        let p = entry.path();
        let target = dst.join(entry.file_name());
        if p.is_dir() {
            copy_dir_recursive(&p, &target)?;
        } else {
            fs::copy(&p, &target).map_err(|e| {
                ToolError::IoError(format!(
                    "Failed to copy {} to {}: {e}",
                    p.display(),
                    target.display()
                ))
            })?;
        }
    }
    Ok(())
}

/// Lists immediate entries in `dir` sorted alphabetically with `[DIR]` and `[FILE]` markers.
pub fn list_directory(dir: &Path) -> Result<String, ToolError> {
    enforce_path_guardrail(dir)?;
    let entries = fs::read_dir(dir)
        .map_err(|e| ToolError::IoError(format!("Failed to read dir {}: {e}", dir.display())))?;

    let mut rows = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        let name = entry.file_name().to_string_lossy().to_string();
        if path.is_dir() {
            rows.push(format!("[DIR]  {name}/"));
        } else {
            let size = entry.metadata().map(|m| m.len()).unwrap_or(0);
            rows.push(format!("[FILE] {name} ({size} bytes)"));
        }
    }
    rows.sort();
    Ok(rows.join("\n"))
}

fn should_skip_walk_entry(path: &Path) -> bool {
    if let Some(name) = path.file_name().and_then(|s| s.to_str()) {
        matches!(name, ".git" | "target" | ".build" | "node_modules")
    } else {
        false
    }
}

/// Converts a glob pattern (e.g. `*.rs`, `src/**/*.md`) into a case-insensitive regular expression,
/// or returns `None` if `pattern` does not contain glob wildcards (`*`, `?`, `[`).
fn glob_to_regex(pattern: &str) -> Option<regex::Regex> {
    if !pattern.contains('*') && !pattern.contains('?') && !pattern.contains('[') {
        return None;
    }
    let mut re = String::from("^");
    let chars: Vec<char> = pattern.chars().collect();
    let mut i = 0;
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

/// Recursively finds files/directories under `root` matching `needle` (supports both glob patterns
/// such as `*.rs` / `**/*.toml` and case-insensitive substring matching via `walkdir::WalkDir`).
pub fn find_path(root: &Path, needle: &str) -> Result<Vec<PathBuf>, ToolError> {
    enforce_path_guardrail(root)?;
    if !root.exists() {
        return Err(ToolError::NotFound(format!(
            "Search root does not exist: {}",
            root.display()
        )));
    }

    let needle_trimmed = needle.trim();
    let needle_lower = needle_trimmed.to_lowercase();
    let glob_re = glob_to_regex(needle_trimmed);
    let mut matches = Vec::new();

    let walker = WalkDir::new(root)
        .follow_links(false)
        .into_iter()
        .filter_entry(|e| e.depth() == 0 || !should_skip_walk_entry(e.path()));

    for entry in walker.flatten() {
        if entry.depth() == 0 {
            continue;
        }
        let p = entry.path();
        let file_name = entry.file_name().to_string_lossy();
        let rel_str = p
            .strip_prefix(root)
            .unwrap_or(p)
            .to_string_lossy();

        let is_match = if let Some(ref re) = glob_re {
            re.is_match(&file_name) || re.is_match(&rel_str)
        } else {
            file_name.to_lowercase().contains(&needle_lower)
                || rel_str.to_lowercase().contains(&needle_lower)
        };

        if is_match {
            matches.push(p.to_path_buf());
        }
    }

    matches.sort();
    Ok(matches)
}

/// Single match returned by [`grep_workspace`] and [`grep_workspace_regex`].
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct GrepMatch {
    pub path: PathBuf,
    pub line_number: usize,
    pub line_text: String,
}

/// Recursively searches text files under `root` for `query` using `regex::Regex` and `walkdir::WalkDir`.
pub fn grep_workspace(root: &Path, query: &str) -> Result<Vec<GrepMatch>, ToolError> {
    grep_workspace_regex(root, query, true, 500)
}

/// Recursively searches text files under `root` for a regex `pattern` using `regex::Regex` and `walkdir::WalkDir`.
pub fn grep_workspace_regex(
    root: &Path,
    pattern: &str,
    case_sensitive: bool,
    max_results: usize,
) -> Result<Vec<GrepMatch>, ToolError> {
    enforce_path_guardrail(root)?;
    if !root.exists() {
        return Err(ToolError::NotFound(format!(
            "Grep root does not exist: {}",
            root.display()
        )));
    }
    if pattern.is_empty() {
        return Ok(Vec::new());
    }

    let compiled_re = RegexBuilder::new(pattern)
        .case_insensitive(!case_sensitive)
        .build()
        .or_else(|_| {
            RegexBuilder::new(&regex::escape(pattern))
                .case_insensitive(!case_sensitive)
                .build()
        })
        .map_err(|e| ToolError::InvalidArguments(format!("Invalid grep regex '{pattern}': {e}")))?;

    let limit = if max_results == 0 { 500 } else { max_results };
    let mut results = Vec::new();

    if root.is_file() {
        if let Ok(content) = fs::read_to_string(root) {
            for (idx, line) in content.lines().enumerate() {
                if compiled_re.is_match(line) {
                    results.push(GrepMatch {
                        path: root.to_path_buf(),
                        line_number: idx + 1,
                        line_text: line.to_string(),
                    });
                    if results.len() >= limit {
                        break;
                    }
                }
            }
        }
        return Ok(results);
    }

    let walker = WalkDir::new(root)
        .follow_links(false)
        .into_iter()
        .filter_entry(|e| e.depth() == 0 || !should_skip_walk_entry(e.path()));

    let mut files = Vec::new();
    for entry in walker.flatten() {
        if entry.file_type().is_file() {
            files.push(entry.path().to_path_buf());
        }
    }
    files.sort();

    'file_loop: for file_path in files {
        if let Ok(content) = fs::read_to_string(&file_path) {
            for (idx, line) in content.lines().enumerate() {
                if compiled_re.is_match(line) {
                    results.push(GrepMatch {
                        path: file_path.clone(),
                        line_number: idx + 1,
                        line_text: line.to_string(),
                    });
                    if results.len() >= limit {
                        break 'file_loop;
                    }
                }
            }
        }
    }

    Ok(results)
}

// ============================================================================
// Tool 11: `terminal` (`validate_allow_scoped_command`,
// Process-Group Timeout & `/usr/sbin/taskpolicy -b` E-core QoS wrapper)
// ============================================================================

/// Validates `command` against an `Allow-Scoped` regular expression:
/// 1. Rejects shell chaining, subshell, and redirection metacharacters (`;`, `&&`, `||`, `|`, `>`, `<`, `` ` ``, `$(`, `\n`, `\r`).
/// 2. Compiles `allowed_regex` via `regex::Regex::new` and verifies `re.is_match(command.trim())`,
///    returning `Err(ToolError::PermissionDenied(...))` if it does not match.
pub fn validate_allow_scoped_command(command: &str, allowed_regex: &str) -> Result<(), ToolError> {
    let trimmed = command.trim();
    if trimmed.is_empty() {
        return Err(ToolError::PermissionDenied(
            "Empty terminal command is not permitted under allow-scoped policy".to_string(),
        ));
    }

    for forbidden in [";", "&&", "||", "|", ">", "<", "`", "$(", "\n", "\r"] {
        if command.contains(forbidden) {
            return Err(ToolError::PermissionDenied(format!(
                "Terminal command contains forbidden shell metacharacter '{}'",
                forbidden.escape_default()
            )));
        }
    }

    enforce_command_protected_globs(trimmed)?;

    let re = regex::Regex::new(allowed_regex).map_err(|e| {
        ToolError::InvalidArguments(format!("Invalid allow-scoped regex '{allowed_regex}': {e}"))
    })?;

    if !re.is_match(trimmed) {
        return Err(ToolError::PermissionDenied(format!(
            "Command '{}' does not match allow-scoped pattern '{}'",
            trimmed, allowed_regex
        )));
    }

    Ok(())
}

fn enforce_command_protected_globs(command: &str) -> Result<(), ToolError> {
    let lower = command.to_ascii_lowercase();
    let normalized = lower.replace('\\', "/");
    for protected in [
        ".ssh",
        ".aws",
        ".git/hooks",
        ".git/config",
    ] {
        if normalized.contains(protected) {
            return Err(ToolError::SecurityViolation(format!(
                "Terminal command references protected path pattern '{protected}'"
            )));
        }
    }
    Ok(())
}

/// Executes a shell command (`sh -c` on Unix, `cmd /C` on Windows) inside `project_root`
/// (or optional relative `cd` subdirectory), enforcing:
/// - `enforce_protected_globs` on `project_root` and `command` (`.ssh`, `.aws`, `.git/hooks`, `.git/config`)
/// - Sensitive environment variable scrubbing (`SPELL_API_KEY`, `OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `AWS_SECRET_ACCESS_KEY`, `SSH_AUTH_SOCK`)
/// - macOS `/usr/sbin/taskpolicy -b` E-core QoS when `use_efficiency_cores` is `true`
/// - On Unix: process-group leader isolation (`process_group(0)`) and whole-process-group `SIGKILL` (`libc::kill(-pgid, SIGKILL)`) on timeout.
/// - On Windows: `cmd /C` execution with timeout termination via `taskkill /F /T` and `child.kill()`.
pub fn run_terminal_command_in_dir(
    project_root: &Path,
    command: &str,
    cd: Option<&str>,
    timeout_ms: Option<u64>,
    use_efficiency_cores: bool,
) -> Result<String, ToolError> {
    enforce_path_guardrail(project_root)?;
    enforce_command_protected_globs(command)?;

    let work_dir = if let Some(sub) = cd {
        resolve_workspace_path(project_root, sub)?
    } else {
        project_root.to_path_buf()
    };

    if !work_dir.exists() {
        return Err(ToolError::NotFound(format!(
            "Terminal working directory does not exist: {}",
            work_dir.display()
        )));
    }

    let canonical_work_dir = fs::canonicalize(&work_dir).unwrap_or_else(|_| work_dir.clone());
    enforce_protected_globs(&canonical_work_dir, &work_dir.to_string_lossy())?;

    let effective_timeout_ms = match timeout_ms {
        Some(0) | None => 30_000,
        Some(ms) => ms.max(25),
    };
    let timeout = Duration::from_millis(effective_timeout_ms);

    #[cfg(unix)]
    let mut cmd = {
        use std::os::unix::process::CommandExt;

        #[cfg(target_os = "macos")]
        let mut c = {
            if use_efficiency_cores && Path::new("/usr/sbin/taskpolicy").exists() {
                let mut c = Command::new("/usr/sbin/taskpolicy");
                c.args(["-b", "/bin/sh", "-c", command]);
                c
            } else {
                let mut c = Command::new("/bin/sh");
                c.args(["-c", command]);
                c
            }
        };

        #[cfg(not(target_os = "macos"))]
        let mut c = {
            let _ = use_efficiency_cores;
            let mut c = Command::new("/bin/sh");
            c.args(["-c", command]);
            c
        };

        c.process_group(0);
        c
    };

    #[cfg(windows)]
    let mut cmd = {
        use std::os::windows::process::CommandExt;
        let _ = use_efficiency_cores;
        let mut c = Command::new("cmd");
        c.arg("/D")
            .arg("/S")
            .arg("/C")
            .raw_arg(format!("\"{command}\""));
        c
    };

    #[cfg(not(any(unix, windows)))]
    let mut cmd = {
        let _ = use_efficiency_cores;
        let mut c = Command::new("/bin/sh");
        c.args(["-c", command]);
        c
    };

    for env_key in SCRUBBED_SENSITIVE_ENV_VARS {
        cmd.env_remove(env_key);
    }

    // Strip Windows extended-length verbatim prefix (`\\?\`) so `cmd.exe` accepts `current_dir`
    // without warning about UNC working directories.
    #[cfg(windows)]
    let spawn_work_dir = {
        let raw = canonical_work_dir.to_string_lossy();
        if let Some(stripped) = raw.strip_prefix(r"\\?\UNC\") {
            PathBuf::from(format!(r"\\{stripped}"))
        } else if let Some(stripped) = raw.strip_prefix(r"\\?\") {
            PathBuf::from(stripped)
        } else {
            canonical_work_dir.clone()
        }
    };
    #[cfg(not(windows))]
    let spawn_work_dir = canonical_work_dir;

    cmd.current_dir(&spawn_work_dir)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());

    let mut child = cmd
        .spawn()
        .map_err(|e| ToolError::IoError(format!("Failed to spawn terminal command: {e}")))?;

    let stdout_pipe = child.stdout.take();
    let stderr_pipe = child.stderr.take();

    let stdout_reader = std::thread::spawn(move || -> String {
        let mut buf = Vec::new();
        if let Some(mut out) = stdout_pipe {
            let _ = out.read_to_end(&mut buf);
        }
        String::from_utf8_lossy(&buf).into_owned()
    });

    let stderr_reader = std::thread::spawn(move || -> String {
        let mut buf = Vec::new();
        if let Some(mut err) = stderr_pipe {
            let _ = err.read_to_end(&mut buf);
        }
        String::from_utf8_lossy(&buf).into_owned()
    });

    let start = Instant::now();
    let mut timed_out = false;

    let status = loop {
        match child.try_wait() {
            Ok(Some(st)) => break st,
            Ok(None) => {
                if start.elapsed() >= timeout {
                    #[cfg(unix)]
                    {
                        let pgid = child.id() as i32;
                        if pgid > 0 {
                            // SAFETY: `child` was spawned with `process_group(0)`, so `-pgid` signals
                            // only this child's process group (`pgid > 0`) on timeout.
                            unsafe {
                                libc::kill(-pgid, libc::SIGKILL);
                            }
                        }
                        let _ = child.kill();
                    }
                    #[cfg(windows)]
                    {
                        let pid = child.id().to_string();
                        let _ = Command::new("taskkill")
                            .args(["/F", "/T", "/PID", &pid])
                            .stdin(Stdio::null())
                            .stdout(Stdio::null())
                            .stderr(Stdio::null())
                            .status();
                        let _ = child.kill();
                    }
                    #[cfg(not(any(unix, windows)))]
                    {
                        let _ = child.kill();
                    }
                    timed_out = true;
                    break child.wait().map_err(|e| {
                        ToolError::IoError(format!("Failed waiting on killed process: {e}"))
                    })?;
                }
                std::thread::sleep(Duration::from_millis(5));
            }
            Err(e) => {
                return Err(ToolError::IoError(format!(
                    "Error polling terminal command status: {e}"
                )))
            }
        }
    };

    let stdout_str = stdout_reader.join().unwrap_or_default();
    let stderr_str = stderr_reader.join().unwrap_or_default();
    let code = status.code().unwrap_or(-1);

    let mut combined = String::new();
    if timed_out {
        combined.push_str(&format!(
            "[TERMINAL TIMEOUT after {} ms]\n",
            timeout.as_millis()
        ));
    }
    combined.push_str(&format!("exit_code: {code}\n"));
    if !stdout_str.is_empty() {
        combined.push_str("stdout:\n");
        combined.push_str(&stdout_str);
        if !stdout_str.ends_with('\n') {
            combined.push('\n');
        }
    }
    if !stderr_str.is_empty() {
        combined.push_str("stderr:\n");
        combined.push_str(&stderr_str);
        if !stderr_str.ends_with('\n') {
            combined.push('\n');
        }
    }

    Ok(truncate_middle_out(&combined, MAX_SINGLE_FILE_CHARS))
}

/// Executes a shell command (`sh -c`) inside `project_root` under the Seatbelt sandbox wrapper (network allowed),
/// environment credential scrubbing, process-group timeout termination, and optional Apple Silicon
/// E-core QoS.
pub fn run_terminal_command(
    project_root: &Path,
    command: &str,
    timeout_ms: u64,
    use_efficiency_cores: bool,
) -> Result<String, ToolError> {
    run_terminal_command_in_dir(
        project_root,
        command,
        None,
        Some(timeout_ms),
        use_efficiency_cores,
    )
}

// ============================================================================
// Tool 12: `fetch` (ArXiv `abs`/`pdf` -> `html` URL Normalization)
// ============================================================================

/// Rewrites `https://arxiv.org/abs/<id>`, `http://arxiv.org/abs/<id>`,
/// `https://arxiv.org/pdf/<id>(.pdf)`, or bare `arxiv.org/abs/<id>` / `arxiv.org/pdf/<id>(.pdf)`
/// to `https://arxiv.org/html/<id>` for clean HTML extraction in the Research Tool.
pub fn normalize_arxiv_url(url: &str) -> String {
    let trimmed = url.trim();
    for prefix in [
        "https://arxiv.org/abs/",
        "http://arxiv.org/abs/",
        "https://www.arxiv.org/abs/",
        "http://www.arxiv.org/abs/",
        "arxiv.org/abs/",
        "https://arxiv.org/pdf/",
        "http://arxiv.org/pdf/",
        "https://www.arxiv.org/pdf/",
        "http://www.arxiv.org/pdf/",
        "arxiv.org/pdf/",
    ] {
        if let Some(rest) = trimmed.strip_prefix(prefix) {
            let clean_id = rest.strip_suffix(".pdf").unwrap_or(rest);
            return format!("https://arxiv.org/html/{clean_id}");
        }
    }
    trimmed.to_string()
}

/// Fetches a URL with automatic arXiv `abs`/`pdf` -> `html` normalization.
pub fn execute_fetch_tool(url: &str) -> Result<String, ToolError> {
    if url.trim().is_empty() {
        return Err(ToolError::InvalidArguments(
            "fetch requires a non-empty `url`".to_string(),
        ));
    }
    let normalized = normalize_arxiv_url(url);
    Ok(format!(
        "<!-- Fetched Research Content (normalized_url: {normalized}) -->\n\
         <meta name=\"canonical-url\" content=\"{normalized}\" />\n\
         <article>\n\
         <h1>Research Document: {normalized}</h1>\n\
         <p>Canonical URL resolved to <code>{normalized}</code>.</p>\n\
         </article>"
    ))
}

/// Executes gated online research via [`crate::online_fetch::execute_online_fetch`], failing closed
/// with zero DNS/TCP sockets when `cfg.mode == OnlineResearchMode::Offline`.
pub fn execute_fetch_tool_with_online_config(
    url: &str,
    cfg: &crate::online_fetch::OnlineResearchConfig,
) -> Result<crate::online_fetch::OnlineFetchOutcome, ToolError> {
    if url.trim().is_empty() {
        return Err(ToolError::InvalidArguments(
            "fetch requires a non-empty `url`".to_string(),
        ));
    }
    crate::online_fetch::execute_online_fetch(url, cfg).map_err(|err| match err {
        crate::online_fetch::OnlineFetchError::OfflineAirGapped(msg) => {
            ToolError::PermissionDenied(format!(
                "Online research is disabled (Air-Gapped mode): {msg}"
            ))
        }
        crate::online_fetch::OnlineFetchError::SecurityBlocked(msg) => {
            ToolError::SecurityViolation(msg)
        }
        crate::online_fetch::OnlineFetchError::DomainApprovalRequired { domain, url } => {
            ToolError::PermissionDenied(format!(
                "Domain approval required for '{domain}' ({url})"
            ))
        }
        crate::online_fetch::OnlineFetchError::TaintConfirmationRequired {
            domain,
            url,
            reason,
        } => ToolError::PermissionDenied(format!(
            "Session taint confirmation required for '{domain}' ({url}): {reason}"
        )),
        crate::online_fetch::OnlineFetchError::SizeOrBinaryBlocked { url, bytes, reason } => {
            ToolError::PermissionDenied(format!(
                "Blocked large or binary download ({bytes} bytes) from '{url}': {reason}"
            ))
        }
        crate::online_fetch::OnlineFetchError::ChallengeOrRateLimited {
            domain,
            status,
            detail,
        } => ToolError::PermissionDenied(format!(
            "Stopped at upstream challenge or rate limit on '{domain}' (HTTP {status}): {detail}"
        )),
        crate::online_fetch::OnlineFetchError::TransportError(msg) => ToolError::IoError(msg),
        crate::online_fetch::OnlineFetchError::ExtractionError(msg) => {
            ToolError::InvalidArguments(msg)
        }
    })
}

// ============================================================================
// Tool 13: Autonomous Agent Timer (`schedule_wakeup`)
// ============================================================================

/// Structured watch target for `schedule_wakeup` (`{"kind": "file_tail", "path": "..."}`).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct WakeupWatchTarget {
    pub kind: String,
    pub path: String,
}

impl WakeupWatchTarget {
    pub fn file_tail(path: impl Into<String>) -> Self {
        Self {
            kind: "file_tail".to_string(),
            path: path.into(),
        }
    }
}

fn default_wakeup_duration_secs() -> u64 {
    5
}

fn default_wakeup_label() -> String {
    "wakeup-timer".to_string()
}

/// Input schema for `schedule_wakeup`.
///
/// Accepts:
/// - `duration_s` or `duration_secs`
/// - `note` or `alarm_note`
/// - `label`
/// - `watch_target` (`{"kind": "file_tail", "path": p}`) or `watch_path` (`p`, automatically
///   wrapped into `{"kind": "file_tail", "path": p}`)
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ScheduleWakeupInput {
    #[serde(alias = "duration_s", default = "default_wakeup_duration_secs")]
    pub duration_secs: u64,
    #[serde(default = "default_wakeup_label")]
    pub label: String,
    #[serde(alias = "note", default)]
    pub alarm_note: String,
    #[serde(default)]
    pub watch_path: Option<String>,
    #[serde(default)]
    pub thread_id: Option<String>,
}

impl ScheduleWakeupInput {
    /// Parses a [`ScheduleWakeupInput`] from arbitrary JSON arguments, supporting `duration_s` / `duration_secs`,
    /// `note` / `alarm_note`, `label`, and `watch_target` / `watch_path`.
    pub fn from_json_value(args: &Value) -> Result<Self, ToolError> {
        let duration_secs = args
            .get("duration_s")
            .or_else(|| args.get("duration_secs"))
            .and_then(|v| v.as_u64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
            .unwrap_or(5);

        let note = args
            .get("note")
            .or_else(|| args.get("alarm_note"))
            .and_then(|v| v.as_str())
            .unwrap_or("Scheduled agent check-in")
            .to_string();

        let label = args
            .get("label")
            .and_then(|v| v.as_str())
            .filter(|s| !s.trim().is_empty())
            .unwrap_or("wakeup-timer")
            .to_string();

        let watch_path = if let Some(wt) = args.get("watch_target") {
            if let Some(p_str) = wt.as_str() {
                if p_str.trim().is_empty() {
                    None
                } else {
                    Some(p_str.to_string())
                }
            } else if let Some(obj) = wt.as_object() {
                obj.get("path")
                    .and_then(|v| v.as_str())
                    .filter(|s| !s.trim().is_empty())
                    .map(|s| s.to_string())
            } else {
                None
            }
        } else {
            args.get("watch_path")
                .and_then(|v| v.as_str())
                .filter(|s| !s.trim().is_empty())
                .map(|s| s.to_string())
        };

        if let Some(ref wp) = watch_path {
            enforce_path_guardrail(Path::new(wp))?;
        }

        let thread_id = args
            .get("thread_id")
            .and_then(|v| v.as_str())
            .map(|s| s.to_string());

        Ok(Self {
            duration_secs,
            label,
            alarm_note: note,
            watch_path,
            thread_id,
        })
    }

    /// Returns the normalized [`WakeupWatchTarget`] (`{"kind": "file_tail", "path": p}`) if a watch path is set.
    pub fn watch_target(&self) -> Option<WakeupWatchTarget> {
        self.watch_path
            .as_ref()
            .filter(|p| !p.trim().is_empty())
            .map(|p| WakeupWatchTarget::file_tail(p.clone()))
    }

    /// Serializes this wakeup input into the admin/daemon JSON payload including both canonical
    /// (`duration_s`, `note`, `watch_target`) and legacy (`duration_secs`, `alarm_note`, `watch_path`) fields.
    pub fn to_admin_json(&self) -> String {
        let watch = self.watch_path.as_deref().unwrap_or("");
        let tid = self.thread_id.as_deref().unwrap_or("default-thread");
        let watch_target_val = if watch.is_empty() {
            Value::Null
        } else {
            json!({
                "kind": "file_tail",
                "path": watch,
            })
        };
        json!({
            "duration_s": self.duration_secs,
            "duration_secs": self.duration_secs,
            "label": self.label,
            "note": self.alarm_note,
            "alarm_note": self.alarm_note,
            "watch_path": watch,
            "watch_target": watch_target_val,
            "thread_id": tid,
        })
        .to_string()
    }
}

/// Formats the wakeup message injected into the agent thread when a wakeup timer expires.
pub fn format_schedule_wakeup_banner(input: &ScheduleWakeupInput) -> String {
    let mut msg = format!(
        "{} Timer '{}' ({}s) expired.\nAlarm Note: {}",
        SCHEDULE_WAKEUP_BANNER, input.label, input.duration_secs, input.alarm_note
    );

    if let Some(ref wp) = input.watch_path {
        if !wp.is_empty() {
            let p = Path::new(wp);
            if enforce_path_guardrail(p).is_err() {
                msg.push_str(&format!(
                    "\nWatched Path '{}': [SECURITY_BLOCK: protected path]",
                    wp
                ));
            } else if let Ok(content) = fs::read_to_string(p) {
                let all_lines: Vec<&str> = content.lines().collect();
                let tail_start = all_lines.len().saturating_sub(20);
                let tail = all_lines[tail_start..].join("\n");
                msg.push_str(&format!("\nWatched Path '{}' Tail:\n{}", wp, tail));
            } else {
                msg.push_str(&format!("\nWatched Path '{}': [unreadable or missing]", wp));
            }
        }
    }
    msg
}

/// Executes a wakeup timer with millisecond-precision testability and optional sleep override.
pub fn run_schedule_wakeup_blocking(
    input: &ScheduleWakeupInput,
    override_sleep: Option<Duration>,
) -> String {
    let wait_dur =
        override_sleep.unwrap_or_else(|| Duration::from_secs(input.duration_secs.min(5)));
    if !wait_dur.is_zero() {
        std::thread::sleep(wait_dur);
    }
    format_schedule_wakeup_banner(input)
}

/// Thread-safe registry of active/fired wakeup timers for both native and MCP timer inspection.
#[derive(Debug, Clone)]
pub struct TimerRecord {
    pub timer_id: String,
    pub input: ScheduleWakeupInput,
    pub created_at_ms: u128,
    pub expires_at_ms: u128,
    pub cancelled: bool,
}

#[derive(Debug, Default, Clone)]
pub struct WakeupTimerRegistry {
    timers: Arc<Mutex<HashMap<String, TimerRecord>>>,
}

impl WakeupTimerRegistry {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn register_timer(
        &self,
        timer_id: impl Into<String>,
        input: ScheduleWakeupInput,
    ) -> TimerRecord {
        let now_ms = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis();
        let expires_at_ms = now_ms + (input.duration_secs as u128) * 1000;
        let id = timer_id.into();
        let rec = TimerRecord {
            timer_id: id.clone(),
            input,
            created_at_ms: now_ms,
            expires_at_ms,
            cancelled: false,
        };
        if let Ok(mut guard) = self.timers.lock() {
            guard.insert(id, rec.clone());
        }
        rec
    }

    pub fn cancel_timer(&self, timer_id: &str) -> bool {
        if let Ok(mut guard) = self.timers.lock() {
            if let Some(rec) = guard.get_mut(timer_id) {
                rec.cancelled = true;
                return true;
            }
        }
        false
    }

    pub fn list_timers(&self) -> Vec<TimerRecord> {
        if let Ok(guard) = self.timers.lock() {
            let mut list: Vec<TimerRecord> = guard.values().cloned().collect();
            list.sort_by(|a, b| a.timer_id.cmp(&b.timer_id));
            return list;
        }
        Vec::new()
    }
}

// ============================================================================
// Tool 14: `tag` (Conversation Rubric Tag Management Tool)
// ============================================================================

/// Executes the `tag` tool (`action`: `"list" | "get" | "apply" | "remove" | "create"`).
pub fn execute_tag_tool(
    registry: &TagRegistry,
    args: &Value,
) -> Result<String, ToolError> {
    let action = args
        .get("action")
        .and_then(|v| v.as_str())
        .unwrap_or("list")
        .trim()
        .to_lowercase();

    let name = args
        .get("name")
        .or_else(|| args.get("id"))
        .and_then(|v| v.as_str())
        .unwrap_or("")
        .trim();

    match action.as_str() {
        "list" => {
            let tags = registry.list_tags();
            let active = registry.active_tag_names();
            let mut lines = Vec::with_capacity(tags.len() + 1);
            lines.push(format!("Available Conversation Rubric Tags ({}):", tags.len()));
            for t in tags {
                let active_marker = if active.contains(&t.id) { " [ACTIVE]" } else { "" };
                let origin = if t.is_builtin { "builtin" } else { "custom" };
                lines.push(format!(
                    "- {} (category: {}, {}){}: {}",
                    t.id, t.category, origin, active_marker, t.description
                ));
            }
            Ok(lines.join("\n"))
        }
        "get" => {
            if name.is_empty() {
                return Err(ToolError::InvalidArguments(
                    "tag action 'get' requires `name`".to_string(),
                ));
            }
            let tag = registry.get_tag(name).ok_or_else(|| {
                ToolError::NotFound(format!("Conversation tag '{}' not found", name))
            })?;
            Ok(format_tags_rubric_block(&[tag]))
        }
        "apply" | "activate" | "enable" => {
            if name.is_empty() {
                return Err(ToolError::InvalidArguments(
                    "tag action 'apply' requires `name`".to_string(),
                ));
            }
            let tag = registry.apply_tag(name).ok_or_else(|| {
                ToolError::NotFound(format!("Conversation tag '{}' not found", name))
            })?;
            let block = registry.format_active_tags_rubric_block(&registry.active_tag_names());
            Ok(format!(
                "Applied conversation rubric tag '{}' (category: {}).\n{}",
                tag.id, tag.category, block
            ))
        }
        "remove" | "delete" | "deactivate" => {
            if name.is_empty() {
                return Err(ToolError::InvalidArguments(
                    "tag action 'remove' requires `name`".to_string(),
                ));
            }
            let was_active = registry.deactivate_tag(name);
            if let Some(tag) = registry.get_tag(name) {
                if !tag.is_builtin {
                    registry.remove_tag(name);
                    return Ok(format!("Removed custom conversation tag '{}'", tag.id));
                }
                return Ok(format!(
                    "Deactivated built-in conversation tag '{}' (active_removed={})",
                    tag.id, was_active
                ));
            }
            if was_active {
                Ok(format!("Deactivated conversation tag '{name}'"))
            } else {
                Err(ToolError::NotFound(format!(
                    "Conversation tag '{name}' not found"
                )))
            }
        }
        "create" | "upsert" => {
            if name.is_empty() {
                return Err(ToolError::InvalidArguments(
                    "tag action 'create' requires `name`".to_string(),
                ));
            }
            let category = args
                .get("category")
                .and_then(|v| v.as_str())
                .unwrap_or("general")
                .to_string();
            let description = args
                .get("description")
                .and_then(|v| v.as_str())
                .unwrap_or("Custom conversation rubric tag")
                .to_string();
            let rubric_prompt = args
                .get("rubric_prompt")
                .and_then(|v| v.as_str())
                .unwrap_or(&description)
                .to_string();

            let tag = ConversationTag::new(
                crate::tags::normalize_tag_key(name),
                name.to_string(),
                category,
                description,
                rubric_prompt,
                false,
            );
            let tag_id = tag.id.clone();
            registry.upsert_tag(tag);
            Ok(format!("Created conversation rubric tag '{tag_id}'"))
        }
        other => Err(ToolError::InvalidArguments(format!(
            "Unsupported tag action '{other}'; expected 'list' | 'get' | 'apply' | 'remove' | 'create'"
        ))),
    }
}

// ============================================================================
// Unified Headless Tool Dispatcher (`HeadlessToolContext` & `execute_headless_tool`)
// ============================================================================

/// Stateful execution context for the 14 headless tools in a workspace session.
#[derive(Debug, Clone)]
pub struct HeadlessToolContext {
    pub project_root: PathBuf,
    pub project_name: String,
    pub is_read_only: bool,
    pub profile: AgentProfile,
    pub permission_overrides: HashMap<String, ToolPermissionPolicy>,
    pub pin_cache: TurnPinCache,
    pub tag_registry: TagRegistry,
    pub timer_registry: WakeupTimerRegistry,
    pub macos_background_qos: bool,
}

impl HeadlessToolContext {
    pub fn new(project_root: impl Into<PathBuf>) -> Self {
        let root = project_root.into();
        let name = root
            .file_name()
            .and_then(|s| s.to_str())
            .unwrap_or("workspace")
            .to_string();
        let tag_registry = TagRegistry::with_project_root(&root);
        Self {
            project_root: root,
            project_name: name,
            is_read_only: false,
            profile: AgentProfile::Write,
            permission_overrides: HashMap::new(),
            pin_cache: TurnPinCache::new(),
            tag_registry,
            timer_registry: WakeupTimerRegistry::new(),
            macos_background_qos: true,
        }
    }

    pub fn with_profile(mut self, profile: AgentProfile) -> Self {
        self.profile = profile;
        self
    }

    pub fn with_read_only(mut self, is_read_only: bool) -> Self {
        self.is_read_only = is_read_only;
        self
    }

    /// Evaluates effective permission policy for `tool_name`, checking explicit overrides first.
    pub fn effective_permission(&self, tool_name: &str) -> ToolPermissionPolicy {
        if tool_name == "spawn_agent" || tool_name == "skill" {
            return ToolPermissionPolicy::Deny;
        }
        if let Some(&policy) = self.permission_overrides.get(tool_name) {
            return policy;
        }
        self.profile.permission_for_tool(tool_name)
    }

    /// Executes a headless tool call by `tool_name` with JSON `args`, pinning output by `tool_call_id`.
    pub fn execute_tool(
        &mut self,
        tool_call_id: &str,
        tool_name: &str,
        args: &Value,
    ) -> Result<String, ToolError> {
        if let Some(pinned) = self.pin_cache.get_pinned_output(tool_call_id) {
            return Ok(pinned.to_string());
        }

        let policy = self.effective_permission(tool_name);
        if policy == ToolPermissionPolicy::Deny {
            return Err(ToolError::PermissionDenied(format!(
                "Tool '{}' is blocked under {:?} profile",
                tool_name, self.profile
            )));
        }

        if self.is_read_only && is_mutating_tool(tool_name) {
            return Err(ToolError::PermissionDenied(format!(
                "Tool '{}' is blocked because project '{}' is read-only",
                tool_name, self.project_name
            )));
        }

        match tool_name {
            "read_file" => {
                let raw_path = args
                    .get("path")
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| ToolError::InvalidArguments("read_file requires `path`".into()))?;
                let start_line = args
                    .get("start_line")
                    .and_then(|v| v.as_u64())
                    .map(|v| v as usize);
                let end_line = args
                    .get("end_line")
                    .and_then(|v| v.as_u64())
                    .map(|v| v as usize);
                execute_read_file_tool(
                    &self.project_root,
                    raw_path,
                    start_line,
                    end_line,
                    &mut self.pin_cache,
                    tool_call_id,
                )
            }
            "edit_file" => {
                let input: EditFileToolInput = serde_json::from_value(args.clone())
                    .map_err(|e| ToolError::InvalidArguments(format!("Invalid edit_file args: {e}")))?;
                let resolved = resolve_workspace_path(&self.project_root, &input.path)?;
                let res = execute_edit_file_tool(&self.project_root, &input, self.is_read_only)?;
                self.pin_cache.invalidate_path(&resolved);
                Ok(self.pin_cache.pin_tool_output(tool_call_id, res))
            }
            "write_file" => {
                let raw_path = args
                    .get("path")
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| ToolError::InvalidArguments("write_file requires `path`".into()))?;
                let content = args
                    .get("content")
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| ToolError::InvalidArguments("write_file requires `content`".into()))?;
                let append = args.get("append").and_then(|v| v.as_bool()).unwrap_or(false);
                let resolved = resolve_workspace_path(&self.project_root, raw_path)?;
                let res = write_file(&resolved, content, append)?;
                self.pin_cache.invalidate_path(&resolved);
                Ok(self.pin_cache.pin_tool_output(tool_call_id, res))
            }
            "list_directory" => {
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or(".");
                let resolved = resolve_workspace_path(&self.project_root, raw_path)?;
                let res = list_directory(&resolved)?;
                Ok(self.pin_cache.pin_tool_output(tool_call_id, res))
            }
            "find_path" => {
                let pattern = args
                    .get("glob")
                    .or_else(|| args.get("pattern"))
                    .or_else(|| args.get("query"))
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| {
                        ToolError::InvalidArguments("find_path requires `glob` or `pattern`".into())
                    })?;
                let raw_root = args.get("path").and_then(|v| v.as_str()).unwrap_or(".");
                let resolved_root = resolve_workspace_path(&self.project_root, raw_root)?;
                let found = find_path(&resolved_root, pattern)?;
                let formatted = found
                    .iter()
                    .map(|p| p.display().to_string())
                    .collect::<Vec<_>>()
                    .join("\n");
                Ok(self.pin_cache.pin_tool_output(tool_call_id, formatted))
            }
            "grep" => {
                let pattern = args
                    .get("regex")
                    .or_else(|| args.get("pattern"))
                    .or_else(|| args.get("query"))
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| {
                        ToolError::InvalidArguments("grep requires `regex` or `pattern`".into())
                    })?;
                let raw_root = args.get("path").and_then(|v| v.as_str()).unwrap_or(".");
                let case_sensitive = args
                    .get("case_sensitive")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(true);
                let max_results = args
                    .get("max_results")
                    .and_then(|v| v.as_u64())
                    .map(|v| v as usize)
                    .unwrap_or(100);
                let resolved_root = resolve_workspace_path(&self.project_root, raw_root)?;
                let matches =
                    grep_workspace_regex(&resolved_root, pattern, case_sensitive, max_results)?;
                let formatted = matches
                    .iter()
                    .map(|m| format!("{}:{}:{}", m.path.display(), m.line_number, m.line_text))
                    .collect::<Vec<_>>()
                    .join("\n");
                Ok(self.pin_cache.pin_tool_output(tool_call_id, formatted))
            }
            "create_directory" => {
                let raw_path = args
                    .get("path")
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| {
                        ToolError::InvalidArguments("create_directory requires `path`".into())
                    })?;
                let resolved = resolve_workspace_path(&self.project_root, raw_path)?;
                let res = create_directory(&resolved)?;
                Ok(self.pin_cache.pin_tool_output(tool_call_id, res))
            }
            "copy_path" => {
                let src_raw = args
                    .get("source_path")
                    .or_else(|| args.get("src"))
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| {
                        ToolError::InvalidArguments("copy_path requires `source_path`".into())
                    })?;
                let dst_raw = args
                    .get("destination_path")
                    .or_else(|| args.get("dst"))
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| {
                        ToolError::InvalidArguments("copy_path requires `destination_path`".into())
                    })?;
                let src = resolve_workspace_path(&self.project_root, src_raw)?;
                let dst = resolve_workspace_path(&self.project_root, dst_raw)?;
                let res = copy_path(&src, &dst)?;
                self.pin_cache.invalidate_path(&dst);
                Ok(self.pin_cache.pin_tool_output(tool_call_id, res))
            }
            "move_path" => {
                let src_raw = args
                    .get("source_path")
                    .or_else(|| args.get("src"))
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| {
                        ToolError::InvalidArguments("move_path requires `source_path`".into())
                    })?;
                let dst_raw = args
                    .get("destination_path")
                    .or_else(|| args.get("dst"))
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| {
                        ToolError::InvalidArguments("move_path requires `destination_path`".into())
                    })?;
                let src = resolve_workspace_path(&self.project_root, src_raw)?;
                let dst = resolve_workspace_path(&self.project_root, dst_raw)?;
                let res = move_path(&src, &dst)?;
                self.pin_cache.invalidate_path(&src);
                self.pin_cache.invalidate_path(&dst);
                Ok(self.pin_cache.pin_tool_output(tool_call_id, res))
            }
            "delete_path" => {
                let raw_path = args
                    .get("path")
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| {
                        ToolError::InvalidArguments("delete_path requires `path`".into())
                    })?;
                let resolved = resolve_workspace_path(&self.project_root, raw_path)?;
                let res = delete_path(&resolved)?;
                self.pin_cache.invalidate_path(&resolved);
                Ok(self.pin_cache.pin_tool_output(tool_call_id, res))
            }
            "terminal" => {
                let command = args
                    .get("command")
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| {
                        ToolError::InvalidArguments("terminal requires `command`".into())
                    })?;
                let cd = args.get("cd").and_then(|v| v.as_str());
                let timeout_ms = args.get("timeout_ms").and_then(|v| v.as_u64());
                let res = run_terminal_command_in_dir(
                    &self.project_root,
                    command,
                    cd,
                    timeout_ms,
                    self.macos_background_qos,
                )?;
                Ok(self.pin_cache.pin_tool_output(tool_call_id, res))
            }
            "fetch" => {
                let url = args
                    .get("url")
                    .and_then(|v| v.as_str())
                    .ok_or_else(|| ToolError::InvalidArguments("fetch requires `url`".into()))?;
                let res = execute_fetch_tool(url)?;
                Ok(self.pin_cache.pin_tool_output(tool_call_id, res))
            }
            "schedule_wakeup" => {
                let mut wakeup = ScheduleWakeupInput::from_json_value(args)?;
                if let Some(ref wp) = wakeup.watch_path {
                    let resolved = resolve_workspace_path(&self.project_root, wp)?;
                    wakeup.watch_path = Some(resolved.to_string_lossy().to_string());
                }
                self.timer_registry
                    .register_timer(format!("tmr-{tool_call_id}"), wakeup.clone());
                let banner = format_schedule_wakeup_banner(&wakeup);
                let out = format!("{}\n{}", banner, wakeup.to_admin_json());
                Ok(self.pin_cache.pin_tool_output(tool_call_id, out))
            }
            "tag" => {
                let res = execute_tag_tool(&self.tag_registry, args)?;
                Ok(self.pin_cache.pin_tool_output(tool_call_id, res))
            }
            other => Err(ToolError::InvalidArguments(format!(
                "Unknown or unsupported headless tool: '{other}'"
            ))),
        }
    }
}

/// Alias for [`execute_edit_file_tool`].
pub fn edit_file(
    project_root: &Path,
    input: &EditFileToolInput,
    is_read_only: bool,
) -> Result<String, ToolError> {
    execute_edit_file_tool(project_root, input, is_read_only)
}

/// Alias for [`grep_workspace_regex`].
pub fn grep(
    root: &Path,
    pattern: &str,
    case_sensitive: bool,
    max_results: usize,
) -> Result<Vec<GrepMatch>, ToolError> {
    grep_workspace_regex(root, pattern, case_sensitive, max_results)
}

/// Alias for [`run_terminal_command_in_dir`] with macOS E-core background QoS enabled by default.
pub fn terminal(
    project_root: &Path,
    command: &str,
    cd: Option<&str>,
    timeout_ms: Option<u64>,
) -> Result<String, ToolError> {
    run_terminal_command_in_dir(project_root, command, cd, timeout_ms, true)
}

/// Alias for [`execute_fetch_tool`].
pub fn fetch(url: &str) -> Result<String, ToolError> {
    execute_fetch_tool(url)
}

/// Alias for [`ScheduleWakeupInput::from_json_value`].
pub fn schedule_wakeup(args: &Value) -> Result<ScheduleWakeupInput, ToolError> {
    ScheduleWakeupInput::from_json_value(args)
}

/// Alias for [`execute_tag_tool`].
pub fn tag(registry: &TagRegistry, args: &Value) -> Result<String, ToolError> {
    execute_tag_tool(registry, args)
}

/// Stateless convenience dispatcher for executing any of the 14 headless tools.
#[allow(clippy::too_many_arguments)]
pub fn execute_headless_tool(
    tool_call_id: &str,
    tool_name: &str,
    args: &Value,
    project_root: &Path,
    is_read_only: bool,
    profile: AgentProfile,
    pin_cache: &mut TurnPinCache,
    tag_registry: &TagRegistry,
) -> Result<String, ToolError> {
    let mut ctx = HeadlessToolContext {
        project_root: project_root.to_path_buf(),
        project_name: project_root
            .file_name()
            .and_then(|s| s.to_str())
            .unwrap_or("workspace")
            .to_string(),
        is_read_only,
        profile,
        permission_overrides: HashMap::new(),
        pin_cache: pin_cache.clone(),
        tag_registry: tag_registry.clone(),
        timer_registry: WakeupTimerRegistry::new(),
        macos_background_qos: true,
    };
    let result = ctx.execute_tool(tool_call_id, tool_name, args);
    *pin_cache = ctx.pin_cache;
    result
}

// ============================================================================
// OpenAI Tool Schema Generator & Clean-Room System Prompt Builder
// ============================================================================

/// Builds the OpenAI-compatible function tool schema array for the given [`AgentProfile`].
///
/// - Never includes `spawn_agent` or `skill`.
/// - In [`AgentProfile::Write`], emits all 14 headless tools.
/// - In [`AgentProfile::Ask`], emits only non-mutating read/search/timer/`tag` tools.
pub fn build_openai_tools_schema(profile: AgentProfile) -> Vec<Value> {
    let all_tools = vec![
        (
            "read_file",
            json!({
                "type": "function",
                "function": {
                    "name": "read_file",
                    "description": "Reads text content from a file in the workspace without line truncation. Defaults to lines 1..=1500 when start_line and end_line are omitted, or falls back to a directory listing when path is a directory.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "path": {
                                "type": "string",
                                "description": "Workspace-relative or absolute path to the file."
                            },
                            "start_line": {
                                "type": "integer",
                                "description": "Optional 1-indexed starting line number (inclusive)."
                            },
                            "end_line": {
                                "type": "integer",
                                "description": "Optional 1-indexed ending line number (inclusive)."
                            },
                            "expand_to_symbol": {
                                "type": "boolean",
                                "description": "Optional. When true and start_line is given, widen the range to the whole enclosing function/method/class."
                            }
                        },
                        "required": ["path"]
                    }
                }
            }),
        ),
        (
            "edit_file",
            json!({
                "type": "function",
                "function": {
                    "name": "edit_file",
                    "description": "Applies surgical search-and-replace edits to an existing file in the workspace, or applies a cached subagent coding proposal by proposal_id without re-generating hunks.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "path": {
                                "type": "string",
                                "description": "Path to the file to modify (optional when proposal_id is supplied)."
                            },
                            "proposal_id": {
                                "type": "string",
                                "description": "Optional cached proposal_id (e.g. 'prop-1') emitted by delegate_coding_subagent to apply directly without repeating old_text/new_text."
                            },
                            "hunk_indices": {
                                "type": "array",
                                "items": { "type": "integer", "minimum": 0 },
                                "description": "Optional 0-based hunk indices to apply when using proposal_id (defaults to all hunks in the proposal)."
                            },
                            "display_description": {
                                "type": "string",
                                "description": "Brief summary of the edit operation."
                            },
                            "edits": {
                                "type": "array",
                                "description": "Ordered list of exact old_text -> new_text replacements.",
                                "items": {
                                    "type": "object",
                                    "properties": {
                                        "old_text": { "type": "string" },
                                        "new_text": { "type": "string" }
                                    },
                                    "required": ["old_text", "new_text"]
                                }
                            },
                            "old_text": {
                                "type": "string",
                                "description": "Single-edit fallback: exact text block to replace."
                            },
                            "new_text": {
                                "type": "string",
                                "description": "Single-edit fallback: replacement text block."
                            }
                        },
                        "required": []
                    }
                }
            }),
        ),
        (
            "write_file",
            json!({
                "type": "function",
                "function": {
                    "name": "write_file",
                    "description": "Creates or overwrites a file with the specified content, creating parent directories if needed.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "path": { "type": "string", "description": "Destination file path." },
                            "content": { "type": "string", "description": "Full text content to write." },
                            "append": { "type": "boolean", "description": "If true, appends to the file instead of overwriting." }
                        },
                        "required": ["path", "content"]
                    }
                }
            }),
        ),
        (
            "list_directory",
            json!({
                "type": "function",
                "function": {
                    "name": "list_directory",
                    "description": "Lists immediate files and subdirectories inside a workspace directory.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "path": { "type": "string", "description": "Directory path to inspect (defaults to workspace root)." }
                        },
                        "required": ["path"]
                    }
                }
            }),
        ),
        (
            "find_path",
            json!({
                "type": "function",
                "function": {
                    "name": "find_path",
                    "description": "Searches for file or directory paths matching a glob or substring pattern under the workspace.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "glob": { "type": "string", "description": "Glob or filename pattern to match (e.g. '*.rs' or 'Cargo.toml')." },
                            "pattern": { "type": "string", "description": "Alias for glob pattern." },
                            "path": { "type": "string", "description": "Optional base directory for the search." }
                        }
                    }
                }
            }),
        ),
        (
            "grep",
            json!({
                "type": "function",
                "function": {
                    "name": "grep",
                    "description": "Searches text files in the workspace using a regular expression.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "regex": { "type": "string", "description": "Regular expression pattern to search for." },
                            "pattern": { "type": "string", "description": "Alias for regex pattern." },
                            "path": { "type": "string", "description": "Optional subdirectory or file path to restrict the search." },
                            "case_sensitive": { "type": "boolean", "description": "Whether matching is case-sensitive (default true)." },
                            "max_results": { "type": "integer", "description": "Maximum number of matches to return (default 100)." }
                        }
                    }
                }
            }),
        ),
        (
            "create_directory",
            json!({
                "type": "function",
                "function": {
                    "name": "create_directory",
                    "description": "Creates a directory and any necessary parent directories.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "path": { "type": "string", "description": "Directory path to create." }
                        },
                        "required": ["path"]
                    }
                }
            }),
        ),
        (
            "copy_path",
            json!({
                "type": "function",
                "function": {
                    "name": "copy_path",
                    "description": "Copies a file or directory tree from source_path to destination_path.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "source_path": { "type": "string", "description": "Existing path to copy from." },
                            "destination_path": { "type": "string", "description": "Target path to copy to." }
                        },
                        "required": ["source_path", "destination_path"]
                    }
                }
            }),
        ),
        (
            "move_path",
            json!({
                "type": "function",
                "function": {
                    "name": "move_path",
                    "description": "Moves or renames a file or directory from source_path to destination_path.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "source_path": { "type": "string", "description": "Existing path to move." },
                            "destination_path": { "type": "string", "description": "Destination path." }
                        },
                        "required": ["source_path", "destination_path"]
                    }
                }
            }),
        ),
        (
            "delete_path",
            json!({
                "type": "function",
                "function": {
                    "name": "delete_path",
                    "description": "Deletes a file or directory tree inside the workspace.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "path": { "type": "string", "description": "Path to delete." }
                        },
                        "required": ["path"]
                    }
                }
            }),
        ),
        (
            "terminal",
            json!({
                "type": "function",
                "function": {
                    "name": "terminal",
                    "description": "Executes a non-interactive shell command (sh -c) in the workspace. On macOS, runs with background QoS (/usr/sbin/taskpolicy -b) to avoid contesting Apple Silicon P-cores/AMX.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "command": { "type": "string", "description": "Shell command line to execute." },
                            "cd": { "type": "string", "description": "Optional working directory relative to the project root." },
                            "timeout_ms": { "type": "integer", "description": "Optional timeout in milliseconds (default 30000)." }
                        },
                        "required": ["command"]
                    }
                }
            }),
        ),
        (
            "fetch",
            json!({
                "type": "function",
                "function": {
                    "name": "fetch",
                    "description": "Fetches a web or research document URL, automatically normalizing arXiv abs/pdf links to https://arxiv.org/html/<id>.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "url": { "type": "string", "description": "URL to fetch." }
                        },
                        "required": ["url"]
                    }
                }
            }),
        ),
        (
            "schedule_wakeup",
            json!({
                "type": "function",
                "function": {
                    "name": "schedule_wakeup",
                    "description": "Schedules an autonomous wakeup timer that resumes the agent after duration_s seconds and optionally tails a watched file.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "duration_s": { "type": "integer", "description": "Wakeup delay in seconds (also accepts duration_secs)." },
                            "duration_secs": { "type": "integer", "description": "Alias for duration_s." },
                            "note": { "type": "string", "description": "Reminder note injected when the timer fires (also accepts alarm_note)." },
                            "alarm_note": { "type": "string", "description": "Alias for note." },
                            "label": { "type": "string", "description": "Short identifier for the timer." },
                            "watch_target": {
                                "type": "object",
                                "description": "Optional watched target object {\"kind\": \"file_tail\", \"path\": \"...\"}.",
                                "properties": {
                                    "kind": { "type": "string" },
                                    "path": { "type": "string" }
                                }
                            },
                            "watch_path": { "type": "string", "description": "Optional file path string automatically wrapped into watch_target." }
                        }
                    }
                }
            }),
        ),
        (
            "tag",
            json!({
                "type": "function",
                "function": {
                    "name": "tag",
                    "description": "Lists, inspects, applies, removes, or creates general-purpose conversation rubric tags (research, writing, analysis, engineering, governance).",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "action": {
                                "type": "string",
                                "enum": ["list", "get", "apply", "remove", "create"],
                                "description": "Tag operation to perform."
                            },
                            "name": { "type": "string", "description": "Tag identifier or name." },
                            "category": { "type": "string", "description": "Domain category when creating a tag." },
                            "description": { "type": "string", "description": "Short summary when creating a tag." },
                            "rubric_prompt": { "type": "string", "description": "Rubric instructions when creating a tag." }
                        },
                        "required": ["action"]
                    }
                }
            }),
        ),
        (
            "prefill_context_subagent",
            json!({
                "type": "function",
                "function": {
                    "name": "prefill_context_subagent",
                    "description": "Reads 1..=32 workspace files or directories directly in Rust (8K-128K tokens) without inserting raw dumps into the main conversation history, runs a dedicated read-only context prefill subagent, and returns a compact <subagent_context_packet> answering query.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "paths": {
                                "type": "array",
                                "items": { "type": "string" },
                                "minItems": 1,
                                "maxItems": 32,
                                "description": "1..=32 workspace-relative files or directories to prefill into the read-only subagent."
                            },
                            "query": {
                                "type": "string",
                                "description": "Focused question, symbol lookup, or architecture trace to extract."
                            },
                            "max_summary_tokens": {
                                "type": "integer",
                                "minimum": 256,
                                "maximum": 4096,
                                "description": "Optional summary budget in tokens (default 2048, clamped 256..=4096)."
                            }
                        },
                        "required": ["paths", "query"]
                    }
                }
            }),
        ),
        (
            "delegate_coding_subagent",
            json!({
                "type": "function",
                "function": {
                    "name": "delegate_coding_subagent",
                    "description": "Dispatches an autonomous multi-step coding subagent inside WorkspaceSandbox: explores files, applies surgical edits in an isolated overlay, runs Gatekeeper Critic verification per write, and auto-commits transactionally on finish (returning a committed summary + undo_id, or escalating to the main orchestrator only on critic bounce exhaustion, cross-thread conflict, web-taint sensitive files, or step budget; supports mode: 'propose' for single-shot non-mutating proposals).",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "target_files": {
                                "type": "array",
                                "items": { "type": "string" },
                                "maxItems": 32,
                                "description": "Optional workspace-relative seed paths — may include existing files, new target file paths to create, or existing directories (including empty directories). Defaults to [] when the subagent discovers target files via find_path or grep."
                            },
                            "instruction": {
                                "type": "string",
                                "description": "Focused coding, refactoring, or test-writing specification."
                            },
                            "context_notes": {
                                "type": "string",
                                "description": "Optional architectural constraints or invariants from the main agent."
                            },
                            "mode": {
                                "type": "string",
                                "enum": ["auto", "propose"],
                                "description": "Execution mode: 'auto' (default) runs multi-step coding + Gatekeeper Critic + transactional auto-commit; 'propose' returns a single-shot cached proposal without writing to disk."
                            },
                            "protocol": {
                                "type": "string",
                                "enum": ["auto", "native", "json"],
                                "description": "Optional per-call override for subagent action protocol."
                            },
                            "max_steps": {
                                "type": "integer",
                                "description": "Maximum subagent loop steps (1..=24, default 12)."
                            }
                        },
                        "required": ["instruction"]
                    }
                }
            }),
        ),
        (
            "apply_subagent_proposal",
            json!({
                "type": "function",
                "function": {
                    "name": "apply_subagent_proposal",
                    "description": "Applies a cached coding proposal (proposal_id, e.g. 'prop-1') synthesized by delegate_coding_subagent without re-emitting large old_text/new_text blocks.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "proposal_id": {
                                "type": "string",
                                "description": "Cached proposal identifier returned on <subagent_coding_proposal proposal_id=\"...\">."
                            },
                            "path": {
                                "type": "string",
                                "description": "Optional path assertion matching the proposal target file."
                            },
                            "hunk_indices": {
                                "type": "array",
                                "items": { "type": "integer", "minimum": 0 },
                                "description": "Optional 0-based subset of hunk indices to apply (defaults to all hunks)."
                            },
                            "display_description": {
                                "type": "string",
                                "description": "Optional brief description for confirmation/UI cards."
                            }
                        },
                        "required": ["proposal_id"]
                    }
                }
            }),
        ),
    ];

    all_tools
        .into_iter()
        .filter(|(name, _)| profile.allows_tool(name))
        .map(|(_, schema)| schema)
        .collect()
}

/// Structured output from [`collect_numbered_subagent_context_detailed`] containing the
/// line-numbered workspace file bundle alongside any non-existent workspace paths that were
/// skipped while valid sibling paths succeeded.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NumberedSubagentContext {
    pub files_read: usize,
    pub approx_tokens: usize,
    pub bundle: String,
    pub missing_paths: Vec<String>,
}

/// Reads and line-numbers `1..=32` workspace-relative files (and optionally directories) inside
/// `WorkspaceSandbox` (`resolve_workspace_path`) for subagent prefill (`8K..=128K` tokens)
/// without inserting raw file dumps into main conversation history.
///
/// Returns `(files_read_count, estimated_prefill_tokens, formatted_numbered_bundle)`.
pub fn collect_numbered_subagent_context(
    project_root: &Path,
    raw_paths: &[String],
    snapshot_store: Option<&mut WorkspaceSnapshotStore>,
    max_total_chars: usize,
    allow_directories: bool,
) -> Result<(usize, usize, String), ToolError> {
    collect_numbered_subagent_context_with_roots(
        project_root,
        &[],
        raw_paths,
        snapshot_store,
        max_total_chars,
        allow_directories,
    )
}

/// Variant of [`collect_numbered_subagent_context`] accepting optional `extra_roots` (such as
/// staged `.spell/research_cache` fallback directories on read-only workspaces).
///
/// Tolerates partial missing paths when at least one valid readable file resolves (appending a
/// `<missing_paths>` block at the end of the returned bundle) while keeping security, sandbox,
/// traversal, and protected-glob checks strictly fail-fast.
pub fn collect_numbered_subagent_context_with_roots(
    project_root: &Path,
    extra_roots: &[PathBuf],
    raw_paths: &[String],
    snapshot_store: Option<&mut WorkspaceSnapshotStore>,
    max_total_chars: usize,
    allow_directories: bool,
) -> Result<(usize, usize, String), ToolError> {
    let ctx = collect_numbered_subagent_context_detailed(
        project_root,
        extra_roots,
        raw_paths,
        snapshot_store,
        max_total_chars,
        allow_directories,
    )?;
    Ok((ctx.files_read, ctx.approx_tokens, ctx.bundle))
}

/// Detailed variant of [`collect_numbered_subagent_context_with_roots`] returning
/// [`NumberedSubagentContext`] so callers can inspect `missing_paths` alongside the formatted
/// bundle (which also includes a trailing `<missing_paths>` block when non-empty).
pub fn collect_numbered_subagent_context_detailed(
    project_root: &Path,
    extra_roots: &[PathBuf],
    raw_paths: &[String],
    mut snapshot_store: Option<&mut WorkspaceSnapshotStore>,
    max_total_chars: usize,
    allow_directories: bool,
) -> Result<NumberedSubagentContext, ToolError> {
    if raw_paths.is_empty() || raw_paths.len() > 32 {
        return Err(ToolError::InvalidArguments(format!(
            "Expected between 1 and 32 workspace-relative paths, got {}",
            raw_paths.len()
        )));
    }

    let mut resolved_files: Vec<PathBuf> = Vec::new();
    let mut missing_paths: Vec<String> = Vec::new();
    for raw in raw_paths {
        let trimmed = raw.trim();
        if trimmed.is_empty() {
            return Err(ToolError::InvalidArguments(
                "Subagent path entry must not be empty".to_string(),
            ));
        }
        // Keep path resolution STRICT so any security, sandbox, traversal, or protected-glob
        // violation (`..`, external paths, `.env`, `.ssh`, `.git/config`, etc.)
        // immediately hard-fails even when valid sibling paths are present.
        let resolved = resolve_workspace_path_with_roots(project_root, extra_roots, trimmed)?;
        if resolved.is_dir() {
            if !allow_directories {
                return Err(ToolError::InvalidArguments(format!(
                    "'{}' is a directory; target_files requires regular file paths",
                    trimmed
                )));
            }
            for entry in WalkDir::new(&resolved)
                .follow_links(false)
                .max_depth(8)
                .into_iter()
                .filter_entry(|e| {
                    let name = e.file_name().to_string_lossy();
                    !matches!(
                        name.as_ref(),
                        ".git" | "target" | ".build" | "node_modules" | ".DS_Store"
                    )
                })
                .flatten()
            {
                if entry.path_is_symlink() || !entry.file_type().is_file() {
                    continue;
                }
                let candidate = entry.path().to_path_buf();
                if enforce_path_guardrail(&candidate).is_ok() && !resolved_files.contains(&candidate)
                {
                    resolved_files.push(candidate);
                }
                if resolved_files.len() >= 64 {
                    break;
                }
            }
        } else if resolved.is_file()
            || snapshot_store
                .as_deref()
                .map(|s| s.has_staged(&resolved))
                .unwrap_or(false)
        {
            if !resolved_files.contains(&resolved) {
                resolved_files.push(resolved);
            }
        } else if !missing_paths.iter().any(|p| p == trimmed) {
            missing_paths.push(trimmed.to_string());
        }
    }

    if resolved_files.is_empty() {
        if missing_paths.len() == 1 {
            return Err(ToolError::NotFound(format!(
                "Workspace path does not exist: '{}'",
                missing_paths[0]
            )));
        }
        if !missing_paths.is_empty() {
            let formatted = missing_paths
                .iter()
                .map(|p| format!("'{p}'"))
                .collect::<Vec<_>>()
                .join(", ");
            return Err(ToolError::NotFound(format!(
                "Workspace paths do not exist: {formatted}"
            )));
        }
        return Err(ToolError::NotFound(
            "No readable UTF-8 workspace files matched the requested paths".to_string(),
        ));
    }

    let budget_chars = max_total_chars.clamp(4_096, 512_000);
    let mut bundle = String::with_capacity(budget_chars.min(65_536));
    let mut files_read = 0usize;

    for file_path in &resolved_files {
        if bundle.len() >= budget_chars {
            bundle.push_str("\n<!-- subagent_context_budget_cap_reached -->\n");
            break;
        }
        let raw_content = if let Some(ref mut store) = snapshot_store {
            match store.get_or_load_snapshot(file_path) {
                Ok(snap) => snap.content,
                Err(_) => match fs::read_to_string(file_path) {
                    Ok(t) => t,
                    Err(_) => continue,
                },
            }
        } else {
            match fs::read_to_string(file_path) {
                Ok(t) => t,
                Err(_) => continue,
            }
        };

        let rel_display = file_path
            .strip_prefix(project_root)
            .unwrap_or(file_path)
            .to_string_lossy()
            .replace('\\', "/");
        let lines: Vec<&str> = raw_content.lines().collect();
        let total_lines = lines.len();
        let remaining = budget_chars.saturating_sub(bundle.len());
        let mut file_block = format!(
            "<file path=\"{rel_display}\" total_lines=\"{total_lines}\">\n"
        );
        for (idx, line) in lines.iter().enumerate() {
            let numbered = format!("{}: {}\n", idx + 1, line);
            if file_block.len() + numbered.len() + 16 > remaining {
                file_block.push_str(&format!(
                    "... [truncated at line {} of {total_lines} within 128K subagent prefill budget]\n",
                    idx + 1
                ));
                break;
            }
            file_block.push_str(&numbered);
        }
        file_block.push_str("</file>\n");
        bundle.push_str(&file_block);
        files_read += 1;
    }

    if files_read == 0 {
        return Err(ToolError::IoError(
            "Selected paths contained no readable UTF-8 text files".to_string(),
        ));
    }

    if !missing_paths.is_empty() {
        bundle.push_str(
            "<missing_paths>\nThe following requested workspace paths do not exist and were skipped:\n",
        );
        for mp in &missing_paths {
            bundle.push_str(&format!("- {mp}\n"));
        }
        bundle.push_str("</missing_paths>\n");
    }

    let approx_tokens = (bundle.len() / 4).max(1);
    Ok(NumberedSubagentContext {
        files_read,
        approx_tokens,
        bundle,
        missing_paths,
    })
}

/// Builds the clean-room system prompt for BeatrixLeslie IDE.
///
/// - Injects workspace context (`project_name`, `project_root`, `is_read_only`, `profile`).
/// - Loads project instruction files from `<project_root>/AGENTS.md`, `CLAUDE.md`, or `.rules` if present.
/// - Appends `<conversation_rubric_tags>...</conversation_rubric_tags>` when `active_tags` is non-empty.
pub fn build_clean_system_prompt(
    project_name: &str,
    project_root: &Path,
    is_read_only: bool,
    profile: AgentProfile,
    active_tags: &[ConversationTag],
) -> String {
    let mut prompt = String::with_capacity(4096);

    prompt.push_str("# BeatrixLeslie IDE — Autonomous Workspace Agent\n\n");
    prompt.push_str(
        "You are an autonomous coding and research assistant integrated into BeatrixLeslie IDE. \
         Keep responses direct, accurate, and grounded in verified workspace state.\n\n",
    );

    prompt.push_str("## Workspace Environment\n");
    prompt.push_str(&format!("- **Project Name**: `{project_name}`\n"));
    prompt.push_str(&format!(
        "- **Project Root**: `{}`\n",
        project_root.display()
    ));
    prompt.push_str(&format!("- **Agent Profile**: `{}`\n", profile.as_str()));
    prompt.push_str(&format!("- **Read-Only Mode**: `{is_read_only}`\n"));
    prompt.push_str(&format!("- **Operating System**: `{}`\n\n", std::env::consts::OS));

    prompt.push_str("## Tool & Context Discipline\n");
    prompt.push_str(
        "1. Use `read_file`, `grep`, `find_path`, and `list_directory` to inspect files before making assumptions.\n\
         2. When a previously read file has not changed, the runtime emits a duplicate-file reference tag to preserve context budget; rely on the earlier file contents in the conversation history.\n\
         3. Use `tag` (`list`, `get`, `apply`, `remove`, `create`) to activate or customize domain rubrics for research, writing, analysis, or engineering.\n\
         4. Use `schedule_wakeup` when monitoring asynchronous jobs or log files.\n\
         5. Only invoke tools explicitly listed in the active tool schema, and answer directly without calling tools when no workspace or external lookup is needed.\n",
    );
    if profile == AgentProfile::Ask || is_read_only {
        prompt.push_str(
            "6. Mutating filesystem tools and shell execution are disabled in the current mode; focus on analysis, answers, and read-only inspection.\n\
             7. Offload multi-file or large-file (`>= 8K` tokens / `>= 3` files) exploration to `prefill_context_subagent` so the main conversation preserves clean context.\n",
        );
    } else {
        prompt.push_str(
            "6. Use `edit_file` with exact unique context blocks for surgical modifications, and `terminal` for verification builds or tests.\n\
             7. Subagent Orchestration: Offload multi-file or large-file (`>= 8K` tokens / `>= 3` files) exploration to `prefill_context_subagent`, and offload bounded coding, refactoring, and multi-file edits to `delegate_coding_subagent`, which autonomously explores, edits, runs Gatekeeper Critic verification per write, and auto-commits changes directly (returning a committed summary + `undo_id`, or escalating only when blocked; pass `mode: \"propose\"` only when a non-mutating proposal is needed).\n",
        );
    }

    // Load `<project_root>/AGENTS.md`, `CLAUDE.md`, or `.rules` if present on disk.
    let mut rules_sections = Vec::new();
    if enforce_path_guardrail(project_root).is_ok() {
        for rule_file in ["AGENTS.md", "CLAUDE.md", ".rules"] {
            let candidate = project_root.join(rule_file);
            if candidate.is_file() && enforce_path_guardrail(&candidate).is_ok() {
                if let Ok(text) = fs::read_to_string(&candidate) {
                    let trimmed = text.trim();
                    if !trimmed.is_empty() {
                        rules_sections.push(format!(
                            "<rule_file name=\"{rule_file}\">\n{}\n</rule_file>",
                            truncate_middle_out(trimmed, MAX_SINGLE_FILE_CHARS)
                        ));
                    }
                }
            }
        }
    }

    if !rules_sections.is_empty() {
        prompt.push_str("\n<project_rules>\n");
        prompt.push_str(&rules_sections.join("\n"));
        prompt.push_str("\n</project_rules>\n");
    }

    if !active_tags.is_empty() {
        prompt.push('\n');
        prompt.push_str(&format_tags_rubric_block(active_tags));
        prompt.push('\n');
    }

    prompt
}

// ============================================================================
// $O(\log N)$ `WorkspaceSnapshotStore` & `Stage-Write` Shadow Overlay
// ============================================================================

/// Pre-indexed file snapshot with precomputed line-start byte offsets (`line_offsets`)
/// and full 64-character SHA-256 hex digest.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FileSnapshot {
    pub path: PathBuf,
    pub content: String,
    pub line_offsets: Vec<usize>,
    pub total_lines: usize,
    pub sha256_hex: String,
}

impl FileSnapshot {
    /// Constructs a [`FileSnapshot`] from `path` and `content`, precomputing the byte offset
    /// of every 1-indexed line start (`line_offsets`) and the SHA-256 hex digest (`sha256_hex`).
    pub fn from_content(path: PathBuf, content: String) -> Self {
        let sha256_hex = compute_sha256_full_hex(&content);
        let bytes = content.as_bytes();
        let mut line_offsets = Vec::new();
        if !bytes.is_empty() {
            line_offsets.push(0);
            for (idx, &b) in bytes.iter().enumerate() {
                if b == b'\n' && idx + 1 < bytes.len() {
                    line_offsets.push(idx + 1);
                }
            }
        }
        let total_lines = line_offsets.len();
        Self {
            path,
            content,
            line_offsets,
            total_lines,
            sha256_hex,
        }
    }

    /// Slices 1-indexed inclusive `[start_line, end_line]` in $O(1)$ time via `self.line_offsets`
    /// without re-splitting lines, returning `(actual_start_line, actual_end_line, slice)`.
    pub fn slice_lines(&self, start_line: usize, end_line: usize) -> (usize, usize, &str) {
        if self.total_lines == 0 || self.content.is_empty() {
            return (0, 0, "");
        }
        let actual_start = start_line.max(1).min(self.total_lines);
        let actual_end = end_line.max(actual_start).min(self.total_lines);
        let start_byte = self.line_offsets[actual_start - 1];
        let end_byte = if actual_end < self.total_lines {
            self.line_offsets[actual_end]
        } else {
            self.content.len()
        };
        let raw_slice = &self.content[start_byte..end_byte];
        let trimmed = raw_slice
            .strip_suffix("\r\n")
            .or_else(|| raw_slice.strip_suffix('\n'))
            .unwrap_or(raw_slice);
        (actual_start, actual_end, trimmed)
    }

    /// Resolves a byte offset in `self.content` to its 1-indexed line number in $O(\log N)$ time.
    pub fn line_for_byte_offset(&self, byte_offset: usize) -> usize {
        if self.line_offsets.is_empty() {
            return 1;
        }
        let clamped = byte_offset.min(self.content.len());
        self.line_offsets
            .partition_point(|&off| off <= clamped)
            .max(1)
    }
}

/// Structured line entry inside a unified diff hunk.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct DiffHunkLine {
    /// `"header" | "context" | "add" | "remove"`
    pub kind: String,
    pub old_line: Option<usize>,
    pub new_line: Option<usize>,
    pub text: String,
}

/// Staged file mutation tracked inside [`WorkspaceSnapshotStore`]'s shadow overlay.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct StagedFileMutation {
    pub path: PathBuf,
    pub relative_path: String,
    pub original_content: Option<String>,
    pub staged_content: String,
    pub unified_diff: String,
    pub added_lines: usize,
    pub removed_lines: usize,
    /// `"staged" | "verified" | "committed" | "rolled_back"`
    pub status: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum DiffEditOp<'a> {
    Context(&'a str),
    Remove(&'a str),
    Add(&'a str),
}

/// Generates a clean unified diff (`--- a/<path_label>\n+++ b/<path_label>\n@@ ... @@\n-... \n+...`)
/// with 3 lines of surrounding context along with `(added_lines, removed_lines)`.
pub fn generate_unified_diff(
    path_label: &str,
    old_text: &str,
    new_text: &str,
) -> (String, usize, usize) {
    generate_unified_diff_with_context(path_label, old_text, new_text, 3)
}

/// Generates a clean unified diff (`--- a/<path_label>\n+++ b/<path_label>\n@@ ... @@\n-... \n+...`)
/// with `context_lines` surrounding context lines along with `(added_lines, removed_lines)`.
pub fn generate_unified_diff_with_context(
    path_label: &str,
    old_text: &str,
    new_text: &str,
    context_lines: usize,
) -> (String, usize, usize) {
    let clean_label = path_label.trim().trim_start_matches("./");
    let label = if clean_label.is_empty() {
        "file"
    } else {
        clean_label
    };

    let old_lines: Vec<&str> = old_text.lines().collect();
    let new_lines: Vec<&str> = new_text.lines().collect();

    if old_lines == new_lines {
        let diff = format!("--- a/{label}\n+++ b/{label}\n");
        return (diff, 0, 0);
    }

    let mut prefix_len = 0usize;
    let min_len = old_lines.len().min(new_lines.len());
    while prefix_len < min_len && old_lines[prefix_len] == new_lines[prefix_len] {
        prefix_len += 1;
    }

    let mut suffix_len = 0usize;
    while suffix_len + prefix_len < min_len
        && old_lines[old_lines.len() - 1 - suffix_len]
            == new_lines[new_lines.len() - 1 - suffix_len]
    {
        suffix_len += 1;
    }

    let old_mid = &old_lines[prefix_len..old_lines.len() - suffix_len];
    let new_mid = &new_lines[prefix_len..new_lines.len() - suffix_len];

    let mut mid_ops: Vec<DiffEditOp<'_>> = Vec::new();
    let mut added_lines = 0usize;
    let mut removed_lines = 0usize;

    let m = old_mid.len();
    let n = new_mid.len();
    if m > 0 && n > 0 && m.saturating_mul(n) <= 65_536 {
        let width = n + 1;
        let mut dp = vec![0u32; (m + 1) * width];
        for i in (0..m).rev() {
            for j in (0..n).rev() {
                dp[i * width + j] = if old_mid[i] == new_mid[j] {
                    dp[(i + 1) * width + (j + 1)] + 1
                } else {
                    dp[(i + 1) * width + j].max(dp[i * width + (j + 1)])
                };
            }
        }
        let mut i = 0usize;
        let mut j = 0usize;
        while i < m && j < n {
            if old_mid[i] == new_mid[j] {
                mid_ops.push(DiffEditOp::Context(old_mid[i]));
                i += 1;
                j += 1;
            } else if dp[(i + 1) * width + j] >= dp[i * width + (j + 1)] {
                mid_ops.push(DiffEditOp::Remove(old_mid[i]));
                removed_lines += 1;
                i += 1;
            } else {
                mid_ops.push(DiffEditOp::Add(new_mid[j]));
                added_lines += 1;
                j += 1;
            }
        }
        while i < m {
            mid_ops.push(DiffEditOp::Remove(old_mid[i]));
            removed_lines += 1;
            i += 1;
        }
        while j < n {
            mid_ops.push(DiffEditOp::Add(new_mid[j]));
            added_lines += 1;
            j += 1;
        }
    } else {
        for &line in old_mid {
            mid_ops.push(DiffEditOp::Remove(line));
            removed_lines += 1;
        }
        for &line in new_mid {
            mid_ops.push(DiffEditOp::Add(line));
            added_lines += 1;
        }
    }

    let ctx_before_start = prefix_len.saturating_sub(context_lines);
    let ctx_before = &old_lines[ctx_before_start..prefix_len];
    let suffix_start_old = old_lines.len() - suffix_len;
    let ctx_after_end = (suffix_start_old + context_lines).min(old_lines.len());
    let ctx_after = &old_lines[suffix_start_old..ctx_after_end];

    let old_hunk_count = ctx_before.len() + m + ctx_after.len();
    let new_hunk_count = ctx_before.len() + n + ctx_after.len();
    let old_hunk_start = if old_lines.is_empty() {
        0
    } else {
        ctx_before_start + 1
    };
    let new_hunk_start = if new_lines.is_empty() {
        0
    } else {
        ctx_before_start + 1
    };

    let mut diff = String::new();
    diff.push_str(&format!("--- a/{label}\n+++ b/{label}\n"));
    diff.push_str(&format!(
        "@@ -{old_hunk_start},{old_hunk_count} +{new_hunk_start},{new_hunk_count} @@\n"
    ));
    for &line in ctx_before {
        diff.push(' ');
        diff.push_str(line);
        diff.push('\n');
    }
    for op in mid_ops {
        match op {
            DiffEditOp::Context(line) => {
                diff.push(' ');
                diff.push_str(line);
                diff.push('\n');
            }
            DiffEditOp::Remove(line) => {
                diff.push('-');
                diff.push_str(line);
                diff.push('\n');
            }
            DiffEditOp::Add(line) => {
                diff.push('+');
                diff.push_str(line);
                diff.push('\n');
            }
        }
    }
    for &line in ctx_after {
        diff.push(' ');
        diff.push_str(line);
        diff.push('\n');
    }

    (diff, added_lines, removed_lines)
}

/// Parses a unified diff string into structured [`DiffHunkLine`] entries.
pub fn parse_unified_diff_hunk_lines(unified_diff: &str) -> Vec<DiffHunkLine> {
    let mut out = Vec::new();
    let mut old_ln = 1usize;
    let mut new_ln = 1usize;

    for line in unified_diff.lines() {
        if line.starts_with("--- ") || line.starts_with("+++ ") || line.starts_with("@@ ") {
            out.push(DiffHunkLine {
                kind: "header".to_string(),
                old_line: None,
                new_line: None,
                text: line.to_string(),
            });
        } else if let Some(rest) = line.strip_prefix('-') {
            out.push(DiffHunkLine {
                kind: "remove".to_string(),
                old_line: Some(old_ln),
                new_line: None,
                text: rest.to_string(),
            });
            old_ln += 1;
        } else if let Some(rest) = line.strip_prefix('+') {
            out.push(DiffHunkLine {
                kind: "add".to_string(),
                old_line: None,
                new_line: Some(new_ln),
                text: rest.to_string(),
            });
            new_ln += 1;
        } else {
            let rest = line.strip_prefix(' ').unwrap_or(line);
            out.push(DiffHunkLine {
                kind: "context".to_string(),
                old_line: Some(old_ln),
                new_line: Some(new_ln),
                text: rest.to_string(),
            });
            old_ln += 1;
            new_ln += 1;
        }
    }
    out
}

fn snapshot_paths_match(a: &Path, b: &Path) -> bool {
    if a == b {
        return true;
    }
    match (
        canonicalize_ancestor_and_join_tail(a),
        canonicalize_ancestor_and_join_tail(b),
    ) {
        (Ok(ca), Ok(cb)) => ca == cb,
        _ => false,
    }
}

fn is_macos_root_system_firmlink(path: &Path) -> bool {
    #[cfg(target_os = "macos")]
    {
        if path == Path::new("/var") {
            return fs::read_link(path).ok().as_deref() == Some(Path::new("private/var"));
        }
        if path == Path::new("/tmp") {
            return fs::read_link(path).ok().as_deref() == Some(Path::new("private/tmp"));
        }
        if path == Path::new("/etc") {
            return fs::read_link(path).ok().as_deref() == Some(Path::new("private/etc"));
        }
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = path;
    }
    false
}

fn verify_staged_target_no_symlinks(target: &Path) -> Result<(), ToolError> {
    if let Ok(meta) = fs::symlink_metadata(target) {
        if meta.file_type().is_symlink() {
            return Err(ToolError::SecurityViolation(format!(
                "Refusing to commit staged write to symlink target '{}'",
                target.display()
            )));
        }
    }

    let mut cur = PathBuf::new();
    for comp in target.components() {
        match comp {
            Component::Prefix(p) => cur.push(p.as_os_str()),
            Component::RootDir => cur.push(comp.as_os_str()),
            Component::CurDir => continue,
            Component::ParentDir => {
                return Err(ToolError::SecurityViolation(format!(
                    "Refusing to commit staged write with parent traversal '{}'",
                    target.display()
                )));
            }
            Component::Normal(seg) => {
                cur.push(seg);
                if cur != target && is_macos_root_system_firmlink(&cur) {
                    continue;
                }
                match fs::symlink_metadata(&cur) {
                    Ok(meta) => {
                        if meta.file_type().is_symlink() {
                            return Err(ToolError::SecurityViolation(format!(
                                "Refusing to commit staged write through symlink component '{}'",
                                cur.display()
                            )));
                        }
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                    Err(e) => {
                        return Err(ToolError::IoError(format!(
                            "Failed inspecting path metadata for {}: {e}",
                            cur.display()
                        )));
                    }
                }
            }
        }
    }
    Ok(())
}

fn write_staged_file_nofollow(target: &Path, content: &str) -> Result<(), ToolError> {
    let mut opts = fs::OpenOptions::new();
    opts.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.custom_flags(libc::O_NOFOLLOW);
    }
    #[cfg(not(unix))]
    {
        if let Ok(meta) = fs::symlink_metadata(target) {
            if meta.file_type().is_symlink() {
                return Err(ToolError::SecurityViolation(format!(
                    "Refusing to commit staged write to symlink target '{}'",
                    target.display()
                )));
            }
        }
    }
    let mut file = opts.open(target).map_err(|e| {
        #[cfg(unix)]
        if e.raw_os_error() == Some(libc::ELOOP) {
            return ToolError::SecurityViolation(format!(
                "Refusing to commit staged write to symlink target '{}' (O_NOFOLLOW)",
                target.display()
            ));
        }
        ToolError::IoError(format!(
            "Failed to commit staged write to {}: {e}",
            target.display()
        ))
    })?;
    file.write_all(content.as_bytes()).map_err(|e| {
        ToolError::IoError(format!(
            "Failed to commit staged write to {}: {e}",
            target.display()
        ))
    })?;
    file.flush().map_err(|e| {
        ToolError::IoError(format!(
            "Failed to flush staged write to {}: {e}",
            target.display()
        ))
    })
}

/// Workspace snapshot store and `Stage-Write` shadow overlay.
#[derive(Clone, Debug, Default)]
pub struct WorkspaceSnapshotStore {
    pub snapshots: HashMap<PathBuf, FileSnapshot>,
    pub staged_overlay: HashMap<PathBuf, StagedFileMutation>,
}

impl WorkspaceSnapshotStore {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn has_staged(&self, target: &Path) -> bool {
        self.find_overlay_key(target)
            .and_then(|k| self.staged_overlay.get(&k))
            .map(|st| st.status == "staged" || st.status == "verified")
            .unwrap_or(false)
    }

    fn find_overlay_key(&self, target: &Path) -> Option<PathBuf> {
        if self.staged_overlay.contains_key(target) {
            return Some(target.to_path_buf());
        }
        self.staged_overlay
            .keys()
            .find(|k| snapshot_paths_match(k, target))
            .cloned()
    }

    fn remove_matching_snapshots(&mut self, target: &Path) {
        let keys: Vec<PathBuf> = self
            .snapshots
            .keys()
            .filter(|k| snapshot_paths_match(k, target))
            .cloned()
            .collect();
        for k in keys {
            self.snapshots.remove(&k);
        }
    }

    /// Loads or returns a [`FileSnapshot`] for `canonical_path`.
    ///
    /// **CRITICAL OVERLAY CONSISTENCY RULE**: Checks `self.staged_overlay` first
    /// (when `status == "staged" || status == "verified"`), returning a [`FileSnapshot`]
    /// built from `staged_content` before falling back to reading `canonical_path` from disk!
    pub fn get_or_load_snapshot(&mut self, canonical_path: &Path) -> Result<FileSnapshot, ToolError> {
        enforce_path_guardrail(canonical_path)?;

        // 1. Check `staged_overlay` first for uncommitted staged/verified mutations.
        if let Some(overlay_key) = self.find_overlay_key(canonical_path) {
            if let Some(staged) = self.staged_overlay.get(&overlay_key) {
                if staged.status == "staged" || staged.status == "verified" {
                    let snap = FileSnapshot::from_content(
                        canonical_path.to_path_buf(),
                        staged.staged_content.clone(),
                    );
                    self.snapshots
                        .insert(canonical_path.to_path_buf(), snap.clone());
                    return Ok(snap);
                }
            }
        }

        // 2. Fallback to reading `canonical_path` from disk.
        if !canonical_path.exists() {
            return Err(ToolError::NotFound(format!(
                "Snapshot target does not exist on disk: {}",
                canonical_path.display()
            )));
        }

        let disk_content = fs::read_to_string(canonical_path).map_err(|e| {
            ToolError::IoError(format!(
                "Failed to read {} into FileSnapshot: {e}",
                canonical_path.display()
            ))
        })?;

        if let Some(existing) = self.snapshots.get(canonical_path) {
            if existing.content == disk_content {
                return Ok(existing.clone());
            }
        }

        let snap = FileSnapshot::from_content(canonical_path.to_path_buf(), disk_content);
        self.snapshots
            .insert(canonical_path.to_path_buf(), snap.clone());
        Ok(snap)
    }

    /// Stages a write mutation in `self.staged_overlay` without modifying the on-disk file.
    pub fn stage_write(
        &mut self,
        canonical_path: &Path,
        relative_path: &str,
        new_content: String,
    ) -> Result<StagedFileMutation, ToolError> {
        enforce_path_guardrail(canonical_path)?;
        enforce_research_cache_write_guard(canonical_path)?;

        let existing_key = self
            .find_overlay_key(canonical_path)
            .unwrap_or_else(|| canonical_path.to_path_buf());

        let original_content = if let Some(prev) = self.staged_overlay.get(&existing_key) {
            if prev.status == "staged" || prev.status == "verified" {
                prev.original_content.clone()
            } else if canonical_path.exists() {
                Some(fs::read_to_string(canonical_path).map_err(|e| {
                    ToolError::IoError(format!("Failed to read {}: {e}", canonical_path.display()))
                })?)
            } else {
                None
            }
        } else if canonical_path.exists() {
            Some(fs::read_to_string(canonical_path).map_err(|e| {
                ToolError::IoError(format!("Failed to read {}: {e}", canonical_path.display()))
            })?)
        } else {
            None
        };

        let rel_label = if relative_path.trim().is_empty() {
            canonical_path
                .file_name()
                .and_then(|s| s.to_str())
                .unwrap_or("file")
                .to_string()
        } else {
            relative_path.trim().to_string()
        };

        let (unified_diff, added_lines, removed_lines) = generate_unified_diff(
            &rel_label,
            original_content.as_deref().unwrap_or(""),
            &new_content,
        );

        let mutation = StagedFileMutation {
            path: canonical_path.to_path_buf(),
            relative_path: rel_label,
            original_content,
            staged_content: new_content.clone(),
            unified_diff,
            added_lines,
            removed_lines,
            status: "staged".to_string(),
        };

        self.staged_overlay
            .insert(existing_key, mutation.clone());
        let snap = FileSnapshot::from_content(canonical_path.to_path_buf(), new_content);
        self.snapshots.insert(canonical_path.to_path_buf(), snap);

        Ok(mutation)
    }

    /// Transitions a staged mutation's `status` from `"staged"` to `"verified"` (`agent.diff.verified`).
    pub fn mark_staged_verified(&mut self, canonical_path: &Path) -> Option<StagedFileMutation> {
        let key = self.find_overlay_key(canonical_path)?;
        let entry = self.staged_overlay.get_mut(&key)?;
        if entry.status == "staged" || entry.status == "verified" {
            entry.status = "verified".to_string();
            return Some(entry.clone());
        }
        None
    }

    /// Commits matching `"staged"` / `"verified"` overlay entries to disk, sets `status = "committed"`,
    /// and updates `self.snapshots`.
    pub fn commit_staged(
        &mut self,
        path_filter: Option<&Path>,
    ) -> Result<Vec<StagedFileMutation>, ToolError> {
        let mut matching_keys: Vec<PathBuf> = self
            .staged_overlay
            .iter()
            .filter(|(k, v)| {
                (v.status == "staged" || v.status == "verified")
                    && path_filter.map_or(true, |f| {
                        snapshot_paths_match(k, f) || snapshot_paths_match(&v.path, f)
                    })
            })
            .map(|(k, _)| k.clone())
            .collect();
        matching_keys.sort();

        let mut committed = Vec::with_capacity(matching_keys.len());
        for key in matching_keys {
            if let Some(entry) = self.staged_overlay.get_mut(&key) {
                enforce_path_guardrail(&entry.path)?;
                enforce_research_cache_write_guard(&entry.path)?;
                verify_staged_target_no_symlinks(&entry.path)?;
                if let Some(parent) = entry.path.parent() {
                    if !parent.as_os_str().is_empty() {
                        fs::create_dir_all(parent).map_err(|e| {
                            ToolError::IoError(format!(
                                "Failed to create parent dir {}: {e}",
                                parent.display()
                            ))
                        })?;
                    }
                }
                verify_staged_target_no_symlinks(&entry.path)?;
                write_staged_file_nofollow(&entry.path, &entry.staged_content)?;
                entry.status = "committed".to_string();
                let snap =
                    FileSnapshot::from_content(entry.path.clone(), entry.staged_content.clone());
                self.snapshots.insert(entry.path.clone(), snap);
                committed.push(entry.clone());
            }
        }
        Ok(committed)
    }

    /// Rolls back matching entries from `self.staged_overlay`, restoring `original_content` on disk
    /// if the mutation was already written to disk, setting `status = "rolled_back"`, and
    /// invalidating `self.snapshots`.
    pub fn rollback_staged(
        &mut self,
        path_filter: Option<&Path>,
    ) -> Result<Vec<StagedFileMutation>, ToolError> {
        let mut matching_keys: Vec<PathBuf> = self
            .staged_overlay
            .iter()
            .filter(|(k, v)| {
                path_filter.map_or(true, |f| {
                    snapshot_paths_match(k, f) || snapshot_paths_match(&v.path, f)
                })
            })
            .map(|(k, _)| k.clone())
            .collect();
        matching_keys.sort();

        let mut rolled_back = Vec::with_capacity(matching_keys.len());
        for key in matching_keys {
            if let Some(mut entry) = self.staged_overlay.remove(&key) {
                if let Some(ref orig) = entry.original_content {
                    let disk_differs = entry.status == "committed"
                        || fs::read_to_string(&entry.path).ok().as_ref() != Some(orig);
                    if disk_differs {
                        if let Some(parent) = entry.path.parent() {
                            if !parent.as_os_str().is_empty() {
                                let _ = fs::create_dir_all(parent);
                            }
                        }
                        fs::write(&entry.path, orig).map_err(|e| {
                            ToolError::IoError(format!(
                                "Failed to restore original content to {}: {e}",
                                entry.path.display()
                            ))
                        })?;
                    }
                } else if entry.status == "committed" && entry.path.exists() {
                    let _ = fs::remove_file(&entry.path);
                }

                entry.status = "rolled_back".to_string();
                self.remove_matching_snapshots(&entry.path);
                rolled_back.push(entry);
            }
        }
        Ok(rolled_back)
    }

    /// Lists all staged mutations in deterministic order.
    pub fn list_staged_mutations(&self) -> Vec<StagedFileMutation> {
        let mut list: Vec<StagedFileMutation> = self.staged_overlay.values().cloned().collect();
        list.sort_by(|a, b| {
            a.relative_path
                .cmp(&b.relative_path)
                .then_with(|| a.path.cmp(&b.path))
        });
        list
    }

    /// Returns `true` if `target` currently has an uncommitted `"staged"` or `"verified"` mutation in `self.staged_overlay`.
    pub fn has_staged_mutation(&self, target: &Path) -> bool {
        if let Some(key) = self.find_overlay_key(target) {
            if let Some(entry) = self.staged_overlay.get(&key) {
                return entry.status == "staged" || entry.status == "verified";
            }
        }
        false
    }

    /// Reads the full text of `target` through `self.get_or_load_snapshot` (returning any uncommitted `Stage-Write` overlay first).
    pub fn read_file_text(&mut self, target: &Path) -> Result<String, ToolError> {
        let snap = self.get_or_load_snapshot(target)?;
        Ok(snap.content)
    }

    /// Reads an optional 1-indexed inclusive `[start_line, end_line]` range from `target` through `self.get_or_load_snapshot`
    /// (returning any uncommitted `Stage-Write` overlay first).
    pub fn read_file_lines(
        &mut self,
        target: &Path,
        start_line: Option<usize>,
        end_line: Option<usize>,
    ) -> Result<String, ToolError> {
        let snap = self.get_or_load_snapshot(target)?;
        let start = start_line.unwrap_or(1).max(1);
        let end = end_line.unwrap_or(snap.total_lines.max(1));
        if start_line.is_none() && end_line.is_none() {
            return Ok(snap.content);
        }
        let (_, _, slice) = snap.slice_lines(start, end);
        Ok(slice.to_string())
    }
}

// ============================================================================
// Structural AST / Symbol Outline Queries (`outline_symbols` & `get_symbol_body`)
// ============================================================================

/// Structural symbol outline entry extracted from Rust, Swift, or Python source files.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct SymbolOutlineEntry {
    pub name: String,
    pub kind: String,
    pub signature: String,
    pub start_line: usize,
    pub end_line: usize,
}

fn is_ident_char(ch: char) -> bool {
    ch.is_ascii_alphanumeric() || ch == '_'
}

fn extract_leading_ident(s: &str) -> Option<&str> {
    let trimmed = s.trim_start();
    let end = trimmed
        .char_indices()
        .find(|&(_, ch)| !is_ident_char(ch))
        .map(|(idx, _)| idx)
        .unwrap_or(trimmed.len());
    if end == 0 {
        None
    } else {
        Some(&trimmed[..end])
    }
}

fn strip_leading_generic_params(s: &str) -> &str {
    let trimmed = s.trim_start();
    if !trimmed.starts_with('<') {
        return trimmed;
    }
    let mut depth = 0usize;
    for (idx, ch) in trimmed.char_indices() {
        if ch == '<' {
            depth += 1;
        } else if ch == '>' {
            depth = depth.saturating_sub(1);
            if depth == 0 {
                return trimmed[idx + 1..].trim_start();
            }
        }
    }
    trimmed
}

fn extract_impl_target_name(after_impl: &str) -> Option<String> {
    let without_generics = strip_leading_generic_params(after_impl);
    let before_brace = without_generics
        .split('{')
        .next()
        .unwrap_or(without_generics)
        .split(" where ")
        .next()
        .unwrap_or(without_generics)
        .trim();
    if before_brace.is_empty() {
        return None;
    }
    let target_expr = if let Some((_, rhs)) = before_brace.rsplit_once(" for ") {
        rhs.trim()
    } else {
        before_brace
    };
    let cleaned = target_expr
        .trim_start_matches('&')
        .trim_start_matches("mut ")
        .trim();
    let base_before_generics = cleaned.split('<').next().unwrap_or(cleaned).trim();
    let last_seg = base_before_generics
        .rsplit("::")
        .next()
        .unwrap_or(base_before_generics)
        .trim();
    extract_leading_ident(last_seg).map(str::to_string)
}

struct ParsedDeclHeader {
    name: String,
    kind: String,
    is_python: bool,
}

fn parse_decl_header_at(slice: &str) -> Option<ParsedDeclHeader> {
    let mut cur = slice.trim_start();
    if cur.is_empty()
        || cur.starts_with("//")
        || cur.starts_with("/*")
        || cur.starts_with('*')
        || cur.starts_with('#')
    {
        return None;
    }

    // 1. Check Python `async def <name>` or `def <name>`
    if let Some(rest) = cur.strip_prefix("async def ") {
        if let Some(name) = extract_leading_ident(rest) {
            return Some(ParsedDeclHeader {
                name: name.to_string(),
                kind: "def".to_string(),
                is_python: true,
            });
        }
    }
    if let Some(rest) = cur.strip_prefix("def ") {
        if let Some(name) = extract_leading_ident(rest) {
            return Some(ParsedDeclHeader {
                name: name.to_string(),
                kind: "def".to_string(),
                is_python: true,
            });
        }
    }

    // 2. Check Python `class <name>...:` (colon present and no `{` on line)
    if let Some(rest) = cur.strip_prefix("class ") {
        let line_no_comment = cur.split('#').next().unwrap_or(cur).trim_end();
        if line_no_comment.ends_with(':') && !line_no_comment.contains('{') {
            if let Some(name) = extract_leading_ident(rest) {
                return Some(ParsedDeclHeader {
                    name: name.to_string(),
                    kind: "class".to_string(),
                    is_python: true,
                });
            }
        }
    }

    // 3. Strip leading Rust / Swift attributes and visibility / qualifier modifiers
    loop {
        cur = cur.trim_start();
        if cur.starts_with('@') {
            // Swift attribute like `@MainActor` or `@objc(...)`
            if let Some(after_attr) = cur.strip_prefix('@') {
                if let Some(ident) = extract_leading_ident(after_attr) {
                    let mut rem = &after_attr[ident.len()..];
                    if rem.starts_with('(') {
                        if let Some(close_idx) = rem.find(')') {
                            rem = &rem[close_idx + 1..];
                        }
                    }
                    cur = rem;
                    continue;
                }
            }
        }
        if cur.starts_with("pub(") {
            if let Some(close_idx) = cur.find(')') {
                cur = &cur[close_idx + 1..];
                continue;
            }
        }
        let mut stripped_mod = false;
        for modifier in [
            "pub",
            "public",
            "private",
            "internal",
            "fileprivate",
            "open",
            "final",
            "static",
            "async",
            "const",
            "unsafe",
            "override",
            "mutating",
            "nonmutating",
            "indirect",
        ] {
            if let Some(rem) = cur.strip_prefix(modifier) {
                if rem.starts_with(char::is_whitespace) {
                    cur = rem.trim_start();
                    stripped_mod = true;
                    break;
                }
            }
        }
        if stripped_mod {
            continue;
        }
        if let Some(rem) = cur.strip_prefix("extern") {
            if rem.starts_with(char::is_whitespace) {
                let mut r = rem.trim_start();
                if r.starts_with('"') {
                    if let Some(end_q) = r[1..].find('"') {
                        r = r[end_q + 2..].trim_start();
                    }
                }
                cur = r;
                continue;
            }
        }
        break;
    }

    // 4. Match Rust / Swift declaration keyword
    for (kw, kind_str) in [
        ("fn ", "fn"),
        ("func ", "func"),
        ("struct ", "struct"),
        ("enum ", "enum"),
        ("trait ", "trait"),
        ("protocol ", "protocol"),
        ("class ", "class"),
        ("extension ", "extension"),
    ] {
        if let Some(rest) = cur.strip_prefix(kw) {
            if let Some(name) = extract_leading_ident(rest) {
                return Some(ParsedDeclHeader {
                    name: name.to_string(),
                    kind: kind_str.to_string(),
                    is_python: false,
                });
            }
        }
    }

    if let Some(rest) = cur.strip_prefix("impl") {
        if rest.starts_with(char::is_whitespace) || rest.starts_with('<') {
            if let Some(name) = extract_impl_target_name(rest) {
                return Some(ParsedDeclHeader {
                    name,
                    kind: "impl".to_string(),
                    is_python: false,
                });
            }
        }
    }

    None
}

/// Scans forward from `decl_start_byte` in `content` to locate the declaration's opening `{`
/// and matching closing `}` (or `;` for bodyless declarations), returning `(open_brace_byte, end_byte)`.
fn scan_brace_declaration_span(content: &str, decl_start_byte: usize) -> (Option<usize>, usize) {
    let bytes = content.as_bytes();
    let mut i = decl_start_byte;
    let mut in_line_comment = false;
    let mut block_comment_depth = 0usize;
    let mut in_double_quote = false;
    let mut paren_depth = 0usize;
    let mut brace_depth = 0usize;
    let mut found_open_brace: Option<usize> = None;

    while i < bytes.len() {
        let b = bytes[i];
        if in_line_comment {
            if b == b'\n' {
                in_line_comment = false;
            }
            i += 1;
            continue;
        }
        if block_comment_depth > 0 {
            if b == b'/' && bytes.get(i + 1) == Some(&b'*') {
                block_comment_depth += 1;
                i += 2;
                continue;
            }
            if b == b'*' && bytes.get(i + 1) == Some(&b'/') {
                block_comment_depth -= 1;
                i += 2;
                continue;
            }
            i += 1;
            continue;
        }
        if in_double_quote {
            if b == b'\\' {
                i += 2;
                continue;
            }
            if b == b'"' {
                in_double_quote = false;
            }
            i += 1;
            continue;
        }

        if b == b'/' && bytes.get(i + 1) == Some(&b'/') {
            in_line_comment = true;
            i += 2;
            continue;
        }
        if b == b'/' && bytes.get(i + 1) == Some(&b'*') {
            block_comment_depth = 1;
            i += 2;
            continue;
        }

        // Rust raw string literal (`r"..."`, `r#"..."#`, `br"..."`, `br#"..."#`)
        let prev_is_ident =
            i > 0 && (bytes[i - 1].is_ascii_alphanumeric() || bytes[i - 1] == b'_');
        if !prev_is_ident {
            let raw_r_idx = if b == b'r' {
                Some(i)
            } else if (b == b'b' || b == b'c') && bytes.get(i + 1) == Some(&b'r') {
                Some(i + 1)
            } else {
                None
            };
            if let Some(r_idx) = raw_r_idx {
                let mut hash_count = 0usize;
                while bytes.get(r_idx + 1 + hash_count) == Some(&b'#') {
                    hash_count += 1;
                }
                if bytes.get(r_idx + 1 + hash_count) == Some(&b'"') {
                    let mut j = r_idx + 2 + hash_count;
                    while j < bytes.len() {
                        if bytes[j] == b'"' {
                            let mut matched_hashes = 0usize;
                            while matched_hashes < hash_count
                                && bytes.get(j + 1 + matched_hashes) == Some(&b'#')
                            {
                                matched_hashes += 1;
                            }
                            if matched_hashes == hash_count {
                                j = j + 1 + hash_count;
                                break;
                            }
                        }
                        j += 1;
                    }
                    i = j;
                    continue;
                }
            }
        }

        // Swift / Python multiline triple-quoted string (`"""..."""`)
        if bytes[i..].starts_with(b"\"\"\"") {
            i += 3;
            while i < bytes.len() {
                if bytes[i] == b'\\' {
                    i += 2;
                    continue;
                }
                if bytes[i..].starts_with(b"\"\"\"") {
                    i += 3;
                    break;
                }
                i += 1;
            }
            continue;
        }

        if b == b'"' {
            in_double_quote = true;
            i += 1;
            continue;
        }
        if b == b'\'' {
            if bytes.get(i + 1) == Some(&b'\\') {
                // Escaped character literal (e.g. `'\{'`, `'\\'`, `'\n'`, `'\x7f'`)
                let mut j = i + 3;
                let mut closed = false;
                while j < bytes.len() && j <= i + 12 && bytes[j] != b'\n' {
                    if bytes[j] == b'\'' {
                        i = j + 1;
                        closed = true;
                        break;
                    }
                    j += 1;
                }
                if closed {
                    continue;
                }
            } else if bytes.get(i + 2) == Some(&b'\'') && bytes.get(i + 1) != Some(&b'\'') {
                // Single ASCII character literal (e.g. `'{'`, `'}'`, `'a'`)
                i += 3;
                continue;
            } else {
                // Rust lifetime (e.g. `'a`, `'static`) — advance past `'ident` without entering char mode
                let mut j = i + 1;
                while j < bytes.len()
                    && (bytes[j].is_ascii_alphanumeric() || bytes[j] == b'_')
                {
                    j += 1;
                }
                if j > i + 1 {
                    i = j;
                    continue;
                }
            }
        }

        if b == b'(' || b == b'[' {
            paren_depth += 1;
        } else if b == b')' || b == b']' {
            paren_depth = paren_depth.saturating_sub(1);
        } else if b == b'{' {
            if found_open_brace.is_none() {
                found_open_brace = Some(i);
            }
            brace_depth += 1;
        } else if b == b'}' {
            if brace_depth > 0 {
                brace_depth -= 1;
                if brace_depth == 0 && found_open_brace.is_some() {
                    return (found_open_brace, i);
                }
            }
        } else if b == b';' && found_open_brace.is_none() && paren_depth == 0 {
            return (None, i);
        }

        i += 1;
    }

    (found_open_brace, bytes.len().saturating_sub(1))
}

fn python_indent_width(line: &str) -> usize {
    let mut width = 0usize;
    for ch in line.chars() {
        if ch == ' ' {
            width += 1;
        } else if ch == '\t' {
            width += 4;
        } else {
            break;
        }
    }
    width
}

fn update_python_triple_quote_state(
    line: &str,
    mut in_triple: Option<&'static str>,
) -> Option<&'static str> {
    let bytes = line.as_bytes();
    let mut i = 0usize;
    while i < bytes.len() {
        if bytes[i] == b'\\' {
            i += 2;
            continue;
        }
        if let Some(delim) = in_triple {
            if bytes[i..].starts_with(delim.as_bytes()) {
                in_triple = None;
                i += 3;
                continue;
            }
            i += 1;
        } else {
            if bytes[i] == b'#' {
                break;
            }
            if bytes[i..].starts_with(b"\"\"\"") {
                in_triple = Some("\"\"\"");
                i += 3;
                continue;
            }
            if bytes[i..].starts_with(b"'''") {
                in_triple = Some("'''");
                i += 3;
                continue;
            }
            if bytes[i] == b'"' || bytes[i] == b'\'' {
                let q = bytes[i];
                i += 1;
                while i < bytes.len() {
                    if bytes[i] == b'\\' {
                        i += 2;
                        continue;
                    }
                    if bytes[i] == q {
                        i += 1;
                        break;
                    }
                    i += 1;
                }
                continue;
            }
            i += 1;
        }
    }
    in_triple
}

fn compute_python_block_end_line(lines: &[&str], start_line_idx: usize) -> usize {
    let base_indent = python_indent_width(lines[start_line_idx]);
    let mut last_body_line = start_line_idx + 1;
    let mut in_triple = update_python_triple_quote_state(lines[start_line_idx], None);

    for (idx, &line) in lines.iter().enumerate().skip(start_line_idx + 1) {
        if in_triple.is_some() {
            in_triple = update_python_triple_quote_state(line, in_triple);
            last_body_line = idx + 1;
            continue;
        }
        let trimmed = line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        let indent = python_indent_width(line);
        if indent <= base_indent {
            break;
        }
        last_body_line = idx + 1;
        in_triple = update_python_triple_quote_state(line, in_triple);
    }
    last_body_line
}

/// Extracts structural symbol outline entries (`[start_line, end_line]`) for Rust, Swift,
/// and Python source code.
pub fn extract_outline_symbols(content: &str) -> Vec<SymbolOutlineEntry> {
    if content.trim().is_empty() {
        return Vec::new();
    }

    let snapshot = FileSnapshot::from_content(PathBuf::new(), content.to_string());
    let lines: Vec<&str> = content.lines().collect();
    let mut entries: Vec<SymbolOutlineEntry> = Vec::new();

    for (line_idx, &raw_line) in lines.iter().enumerate() {
        let start_line = line_idx + 1;
        let line_start_byte = snapshot
            .line_offsets
            .get(line_idx)
            .copied()
            .unwrap_or(0);
        let line_end_byte = line_start_byte + raw_line.len();

        let mut col_offset = 0usize;
        while col_offset < raw_line.len() {
            let sub = &raw_line[col_offset..];
            let leading_ws = sub.len() - sub.trim_start().len();
            col_offset += leading_ws;
            if col_offset >= raw_line.len() {
                break;
            }

            let candidate_slice = &raw_line[col_offset..];
            let Some(parsed) = parse_decl_header_at(candidate_slice) else {
                break;
            };

            if parsed.is_python {
                let end_line = compute_python_block_end_line(&lines, line_idx);
                let sig = candidate_slice
                    .split('#')
                    .next()
                    .unwrap_or(candidate_slice)
                    .trim()
                    .to_string();
                entries.push(SymbolOutlineEntry {
                    name: parsed.name,
                    kind: parsed.kind,
                    signature: sig,
                    start_line,
                    end_line,
                });
                break;
            } else {
                let decl_abs_byte = line_start_byte + col_offset;
                let (open_brace_opt, end_byte) =
                    scan_brace_declaration_span(content, decl_abs_byte);
                let end_line = snapshot
                    .line_for_byte_offset(end_byte)
                    .max(start_line);

                let sig_end_byte = open_brace_opt.unwrap_or(end_byte).min(content.len());
                let raw_sig = if sig_end_byte > decl_abs_byte
                    && content.is_char_boundary(decl_abs_byte)
                    && content.is_char_boundary(sig_end_byte)
                {
                    &content[decl_abs_byte..sig_end_byte]
                } else {
                    candidate_slice
                };
                let normalized_sig = raw_sig
                    .split_whitespace()
                    .collect::<Vec<_>>()
                    .join(" ");

                entries.push(SymbolOutlineEntry {
                    name: parsed.name,
                    kind: parsed.kind.clone(),
                    signature: normalized_sig,
                    start_line,
                    end_line,
                });

                // Support nested single-line declarations (e.g. `impl LoopScheduler { pub fn tick() {} }`)
                if let Some(open_b) = open_brace_opt {
                    if open_b < line_end_byte {
                        let next_abs = if end_byte < line_end_byte
                            && parsed.kind != "impl"
                            && parsed.kind != "trait"
                            && parsed.kind != "protocol"
                            && parsed.kind != "class"
                            && parsed.kind != "struct"
                            && parsed.kind != "extension"
                        {
                            end_byte + 1
                        } else {
                            open_b + 1
                        };
                        col_offset = next_abs.saturating_sub(line_start_byte);
                        while col_offset < raw_line.len() {
                            let ch = raw_line[col_offset..].chars().next().unwrap_or(' ');
                            if ch.is_whitespace() || ch == '}' || ch == ';' {
                                col_offset += ch.len_utf8();
                            } else {
                                break;
                            }
                        }
                        continue;
                    }
                }
                break;
            }
        }
    }

    entries
}

/// Alias for [`extract_outline_symbols`].
pub fn outline_symbols(content: &str) -> Vec<SymbolOutlineEntry> {
    extract_outline_symbols(content)
}

/// Looks up a symbol by `symbol_name` (matching `entry.name == symbol_name`, `Container::member`,
/// or `Container.member`) and returns `(SymbolOutlineEntry, formatted_1_indexed_lines)`.
pub fn get_symbol_body(
    content: &str,
    symbol_name: &str,
) -> Option<(SymbolOutlineEntry, String)> {
    let trimmed_query = symbol_name.trim();
    if trimmed_query.is_empty() {
        return None;
    }

    let entries = extract_outline_symbols(content);
    if entries.is_empty() {
        return None;
    }

    let qualifier_and_member = trimmed_query
        .rsplit_once("::")
        .or_else(|| trimmed_query.rsplit_once('.'));

    let matched_entry = if let Some((container_name, member_name)) = qualifier_and_member {
        // First try to find a member enclosed within a matching container (`impl`, `class`, `struct`, `trait`, `protocol`, `extension`)
        let container_spans: Vec<(usize, usize)> = entries
            .iter()
            .filter(|e| {
                e.name == container_name
                    && matches!(
                        e.kind.as_str(),
                        "impl" | "class" | "struct" | "trait" | "protocol" | "extension"
                    )
            })
            .map(|e| (e.start_line, e.end_line))
            .collect();

        entries
            .iter()
            .find(|e| {
                e.name == member_name
                    && e.kind != "impl"
                    && container_spans
                        .iter()
                        .any(|&(cs, ce)| e.start_line >= cs && e.end_line <= ce)
            })
            .or_else(|| {
                let dcolon_suffix = format!("::{}", member_name);
                let dot_suffix = format!(".{}", member_name);
                entries.iter().find(|e| {
                    e.name == member_name
                        && (trimmed_query.ends_with(&dcolon_suffix)
                            || trimmed_query.ends_with(&dot_suffix))
                })
            })
            .cloned()
    } else {
        entries
            .iter()
            .find(|e| e.name == trimmed_query)
            .or_else(|| {
                let dcolon_suffix = format!("::{}", trimmed_query);
                let dot_suffix = format!(".{}", trimmed_query);
                entries.iter().find(|e| {
                    trimmed_query.ends_with(&format!("::{}", e.name))
                        || trimmed_query.ends_with(&format!(".{}", e.name))
                        || e.name.ends_with(&dcolon_suffix)
                        || e.name.ends_with(&dot_suffix)
                })
            })
            .cloned()
    }?;

    let lines: Vec<&str> = content.lines().collect();
    if lines.is_empty() {
        return Some((matched_entry, String::new()));
    }
    let start_idx = matched_entry.start_line.saturating_sub(1).min(lines.len());
    let end_idx = matched_entry.end_line.min(lines.len());
    let mut formatted = Vec::with_capacity(end_idx.saturating_sub(start_idx));
    for (offset, &line_str) in lines[start_idx..end_idx].iter().enumerate() {
        let line_no = start_idx + offset + 1;
        formatted.push(format!("{line_no}: {line_str}"));
    }

    Some((matched_entry, formatted.join("\n")))
}

/// Extracts symbol outlines for `canonical_path` through [`WorkspaceSnapshotStore::get_or_load_snapshot`]
/// so `Stage-Write` shadow overlay mutations are automatically reflected.
pub fn outline_symbols_in_snapshot(
    store: &mut WorkspaceSnapshotStore,
    canonical_path: &Path,
) -> Result<Vec<SymbolOutlineEntry>, ToolError> {
    let snap = store.get_or_load_snapshot(canonical_path)?;
    Ok(extract_outline_symbols(&snap.content))
}

/// Retrieves a symbol's outline metadata and 1-indexed body through [`WorkspaceSnapshotStore::get_or_load_snapshot`]
/// so `Stage-Write` shadow overlay mutations are automatically reflected.
pub fn get_symbol_body_in_snapshot(
    store: &mut WorkspaceSnapshotStore,
    canonical_path: &Path,
    symbol_name: &str,
) -> Result<(SymbolOutlineEntry, String), ToolError> {
    let snap = store.get_or_load_snapshot(canonical_path)?;
    get_symbol_body(&snap.content, symbol_name).ok_or_else(|| {
        ToolError::NotFound(format!(
            "Symbol '{}' not found in {}",
            symbol_name,
            canonical_path.display()
        ))
    })
}

// ============================================================================
// Structured Diagnostics Bus (`StructuredDiagnostic` & `DiagnosticsRingBuffer`)
// ============================================================================

/// Default maximum number of diagnostics retained in [`DiagnosticsRingBuffer`].
pub const DEFAULT_DIAGNOSTICS_RING_CAPACITY: usize = 256;

/// Normalized compiler diagnostic entry parsed from `cargo check --message-format=json`,
/// `rustc`, or `swiftc` output.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct StructuredDiagnostic {
    pub file: String,
    pub line: usize,
    pub column: usize,
    /// `"error" | "warning" | "note"`
    pub severity: String,
    pub code: Option<String>,
    pub message: String,
}

/// Bounded ring buffer of [`StructuredDiagnostic`] entries.
#[derive(Clone, Debug)]
pub struct DiagnosticsRingBuffer {
    entries: VecDeque<StructuredDiagnostic>,
    capacity: usize,
}

impl Default for DiagnosticsRingBuffer {
    fn default() -> Self {
        Self::new()
    }
}

impl DiagnosticsRingBuffer {
    /// Creates a [`DiagnosticsRingBuffer`] with the default capacity (`256`).
    pub fn new() -> Self {
        Self::with_capacity(DEFAULT_DIAGNOSTICS_RING_CAPACITY)
    }

    /// Creates a [`DiagnosticsRingBuffer`] with a custom `capacity`.
    pub fn with_capacity(capacity: usize) -> Self {
        let cap = capacity.max(1);
        Self {
            entries: VecDeque::with_capacity(cap),
            capacity: cap,
        }
    }

    /// Returns the ring buffer capacity.
    pub fn capacity(&self) -> usize {
        self.capacity
    }

    /// Returns the number of diagnostics currently stored.
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Returns `true` if no diagnostics are stored.
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// Pushes a [`StructuredDiagnostic`], evicting the oldest entry if at capacity.
    pub fn push(&mut self, diagnostic: StructuredDiagnostic) {
        if self.entries.len() >= self.capacity {
            self.entries.pop_front();
        }
        self.entries.push_back(diagnostic);
    }

    /// Clears all stored diagnostics.
    pub fn clear(&mut self) {
        self.entries.clear();
    }

    /// Returns all stored diagnostics in insertion order.
    pub fn all(&self) -> Vec<StructuredDiagnostic> {
        self.entries.iter().cloned().collect()
    }

    /// Returns all stored diagnostics with `severity == "error"`.
    pub fn errors(&self) -> Vec<StructuredDiagnostic> {
        self.entries
            .iter()
            .filter(|d| d.severity == "error")
            .cloned()
            .collect()
    }

    fn normalize_severity(raw: &str) -> Option<String> {
        let lower = raw.trim().to_ascii_lowercase();
        if lower.starts_with("error") {
            Some("error".to_string())
        } else if lower.starts_with("warn") {
            Some("warning".to_string())
        } else if lower == "note" || lower == "help" {
            Some("note".to_string())
        } else {
            None
        }
    }

    /// Ingests `cargo check --message-format=json` lines where `"reason": "compiler-message"`,
    /// and falls back to parsing standard `rustc` `error[E...]: ... \n --> file:line:col` blocks.
    pub fn ingest_cargo_json_output(&mut self, output: &str) -> usize {
        static RUSTC_HDR_RE: Lazy<Regex> = Lazy::new(|| {
            Regex::new(r"^\s*(?P<sev>error|warning|note)(?:\[(?P<code>[^\]]+)\])?:\s*(?P<msg>.+)$")
                .expect("valid RUSTC_HDR_RE pattern")
        });
        static RUSTC_SPAN_RE: Lazy<Regex> = Lazy::new(|| {
            Regex::new(r"^\s*-->\s*(?P<file>[^:\n]+):(?P<line>\d+):(?P<col>\d+)")
                .expect("valid RUSTC_SPAN_RE pattern")
        });

        let mut added = 0usize;
        let lines: Vec<&str> = output.lines().collect();
        let mut idx = 0usize;

        while idx < lines.len() {
            let trimmed = lines[idx].trim();
            if trimmed.starts_with('{') {
                if let Ok(val) = serde_json::from_str::<Value>(trimmed) {
                    if val.get("reason").and_then(|v| v.as_str()) == Some("compiler-message") {
                        let msg_obj = val.get("message").unwrap_or(&val);
                        let raw_level = msg_obj
                            .get("level")
                            .and_then(|v| v.as_str())
                            .unwrap_or("error");
                        if let Some(severity) = Self::normalize_severity(raw_level) {
                            let message_text = msg_obj
                                .get("message")
                                .and_then(|v| v.as_str())
                                .unwrap_or("")
                                .trim()
                                .to_string();
                            let code = msg_obj.get("code").and_then(|c| {
                                c.get("code")
                                    .and_then(|v| v.as_str())
                                    .or_else(|| c.as_str())
                                    .map(|s| s.to_string())
                            });
                            if let Some(spans) = msg_obj.get("spans").and_then(|v| v.as_array()) {
                                let primary_span = spans
                                    .iter()
                                    .find(|s| {
                                        s.get("is_primary").and_then(|v| v.as_bool()) == Some(true)
                                    })
                                    .or_else(|| spans.first());
                                if let Some(span) = primary_span {
                                    let file = span
                                        .get("file_name")
                                        .and_then(|v| v.as_str())
                                        .unwrap_or("<unknown>")
                                        .to_string();
                                    let line = span
                                        .get("line_start")
                                        .and_then(|v| v.as_u64())
                                        .unwrap_or(1) as usize;
                                    let column = span
                                        .get("column_start")
                                        .and_then(|v| v.as_u64())
                                        .unwrap_or(1) as usize;
                                    self.push(StructuredDiagnostic {
                                        file,
                                        line,
                                        column,
                                        severity,
                                        code,
                                        message: message_text,
                                    });
                                    added += 1;
                                }
                            }
                        }
                    }
                }
                idx += 1;
                continue;
            }

            if let Some(hdr_caps) = RUSTC_HDR_RE.captures(lines[idx]) {
                let lookahead_end = (idx + 4).min(lines.len());
                let mut matched_span = None;
                for next_i in (idx + 1)..lookahead_end {
                    if let Some(span_caps) = RUSTC_SPAN_RE.captures(lines[next_i]) {
                        matched_span = Some((next_i, span_caps));
                        break;
                    }
                }
                if let Some((span_idx, span_caps)) = matched_span {
                    let sev_raw = hdr_caps.name("sev").map(|m| m.as_str()).unwrap_or("error");
                    let severity =
                        Self::normalize_severity(sev_raw).unwrap_or_else(|| "error".to_string());
                    let code = hdr_caps.name("code").map(|m| m.as_str().to_string());
                    let message = hdr_caps
                        .name("msg")
                        .map(|m| m.as_str().trim().to_string())
                        .unwrap_or_default();
                    let file = span_caps
                        .name("file")
                        .map(|m| m.as_str().trim().to_string())
                        .unwrap_or_default();
                    let line = span_caps
                        .name("line")
                        .and_then(|m| m.as_str().parse::<usize>().ok())
                        .unwrap_or(1);
                    let column = span_caps
                        .name("col")
                        .and_then(|m| m.as_str().parse::<usize>().ok())
                        .unwrap_or(1);
                    self.push(StructuredDiagnostic {
                        file,
                        line,
                        column,
                        severity,
                        code,
                        message,
                    });
                    added += 1;
                    idx = span_idx + 1;
                    continue;
                }
            }

            idx += 1;
        }

        added
    }

    /// Ingests `swiftc` diagnostic lines (`file.swift:line:col: error: message` and `warning:` / `note:`).
    pub fn ingest_swiftc_output(&mut self, output: &str) -> usize {
        static SWIFTC_DIAG_RE: Lazy<Regex> = Lazy::new(|| {
            Regex::new(
                r"^\s*(?P<file>[^\n:]+):(?P<line>\d+):(?P<col>\d+):\s*(?P<sev>error|warning|note)(?:\[(?P<code>[^\]]+)\])?:\s*(?P<msg>.+)$",
            )
            .expect("valid SWIFTC_DIAG_RE pattern")
        });

        let mut added = 0usize;

        for line in output.lines() {
            let trimmed = line.trim();
            if trimmed.starts_with('{') || trimmed.starts_with("-->") {
                continue;
            }
            if let Some(caps) = SWIFTC_DIAG_RE.captures(trimmed) {
                let file = caps
                    .name("file")
                    .map(|m| m.as_str().trim().to_string())
                    .unwrap_or_default();
                let line_no = caps
                    .name("line")
                    .and_then(|m| m.as_str().parse::<usize>().ok())
                    .unwrap_or(1);
                let col_no = caps
                    .name("col")
                    .and_then(|m| m.as_str().parse::<usize>().ok())
                    .unwrap_or(1);
                let sev_raw = caps.name("sev").map(|m| m.as_str()).unwrap_or("error");
                let severity =
                    Self::normalize_severity(sev_raw).unwrap_or_else(|| "error".to_string());
                let code = caps.name("code").map(|m| m.as_str().to_string());
                let message = caps
                    .name("msg")
                    .map(|m| m.as_str().trim().to_string())
                    .unwrap_or_default();

                self.push(StructuredDiagnostic {
                    file,
                    line: line_no,
                    column: col_no,
                    severity,
                    code,
                    message,
                });
                added += 1;
            }
        }

        added
    }

    /// Runs both `ingest_cargo_json_output` and `ingest_swiftc_output` on `output`.
    pub fn ingest_compiler_output(&mut self, output: &str) -> usize {
        let c_count = self.ingest_cargo_json_output(output);
        let s_count = self.ingest_swiftc_output(output);
        c_count + s_count
    }
}

// ============================================================================
// Unit Tests
// ============================================================================

#[cfg(test)]
pub(crate) static SANDBOX_ENV_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_resolve_workspace_path_and_worktree_prefix() {
        let root = std::env::temp_dir().join("BeatrixLeslie_IDE");

        // 1. Worktree-prefixed path matches project_root.file_name()
        let p1 = resolve_workspace_path(&root, "BeatrixLeslie_IDE/README.md").unwrap();
        assert_eq!(p1, root.join("README.md"));

        // 2. Strips :line:col and #L123-L456 suffixes
        let p2 = resolve_workspace_path(&root, "src/lib.rs:42:10").unwrap();
        assert_eq!(p2, root.join("src/lib.rs"));

        let p3 = resolve_workspace_path(&root, "BeatrixLeslie_IDE/src/tools.rs#L10-L50").unwrap();
        assert_eq!(p3, root.join("src/tools.rs"));

        // 3. Blocks out-of-workspace path traversal
        let err = resolve_workspace_path(&root, "../outside_secret.bin").unwrap_err();
        assert!(matches!(err, ToolError::SecurityViolation(_)));
    }

    #[test]
    fn test_profiles_and_openai_tools_schema() {
        let write_schema = build_openai_tools_schema(AgentProfile::Write);
        assert_eq!(write_schema.len(), 17);

        let ask_schema = build_openai_tools_schema(AgentProfile::Ask);
        assert_eq!(ask_schema.len(), 8);

        for item in &write_schema {
            let name = item["function"]["name"].as_str().unwrap();
            assert_ne!(name, "spawn_agent");
            assert_ne!(name, "skill");
        }
        assert!(write_schema
            .iter()
            .any(|s| s["function"]["name"] == "tag"));
        assert!(write_schema
            .iter()
            .any(|s| s["function"]["name"] == "prefill_context_subagent"));
        assert!(write_schema
            .iter()
            .any(|s| s["function"]["name"] == "delegate_coding_subagent"));
        assert!(write_schema
            .iter()
            .any(|s| s["function"]["name"] == "apply_subagent_proposal"));
        assert_eq!(
            AgentProfile::Write.permission_for_tool("apply_subagent_proposal"),
            ToolPermissionPolicy::Confirm
        );
        assert!(ask_schema.iter().any(|s| s["function"]["name"] == "tag"));
        assert!(ask_schema
            .iter()
            .any(|s| s["function"]["name"] == "prefill_context_subagent"));
        assert!(!ask_schema
            .iter()
            .any(|s| s["function"]["name"] == "delegate_coding_subagent"));
        assert!(!ask_schema
            .iter()
            .any(|s| s["function"]["name"] == "apply_subagent_proposal"));
        assert!(!ask_schema
            .iter()
            .any(|s| s["function"]["name"] == "edit_file"));
        assert!(!ask_schema
            .iter()
            .any(|s| s["function"]["name"] == "terminal"));
    }

    #[test]
    fn test_schedule_wakeup_aliases_and_watch_target_wrapping() {
        let args = json!({
            "duration_s": 12,
            "note": "Check test runner",
            "label": "ci-watch",
            "watch_path": "logs/build.log"
        });
        let input = ScheduleWakeupInput::from_json_value(&args).unwrap();
        assert_eq!(input.duration_secs, 12);
        assert_eq!(input.alarm_note, "Check test runner");
        assert_eq!(
            input.watch_target(),
            Some(WakeupWatchTarget {
                kind: "file_tail".to_string(),
                path: "logs/build.log".to_string()
            })
        );
        let admin_json = input.to_admin_json();
        assert!(admin_json.contains("\"kind\":\"file_tail\""));
        assert!(admin_json.contains("\"duration_s\":12"));
    }

    #[test]
    fn test_clean_system_prompt_loads_agents_md_and_tags() {
        let tmp_dir =
            std::env::temp_dir().join(format!("spell_sysprompt_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&tmp_dir);
        fs::create_dir_all(&tmp_dir).unwrap();
        fs::write(tmp_dir.join("AGENTS.md"), "Always run unit tests.").unwrap();

        let reg = TagRegistry::new();
        let tag = reg.get_tag("systems-architecture").unwrap();

        let prompt = build_clean_system_prompt(
            "TestProj",
            &tmp_dir,
            false,
            AgentProfile::Write,
            &[tag],
        );
        assert!(prompt.contains("# BeatrixLeslie IDE — Autonomous Workspace Agent"));
        assert!(prompt.contains("integrated into BeatrixLeslie IDE."));
        assert!(prompt.contains("Only invoke tools explicitly listed in the active tool schema"));
        assert!(prompt.contains("TestProj"));
        assert!(prompt.contains("<project_rules>"));
        assert!(prompt.contains("Always run unit tests."));
        assert!(prompt.contains("<conversation_rubric_tags>"));
        assert!(prompt.contains("systems-architecture"));

        let _ = fs::remove_dir_all(&tmp_dir);
    }

    #[test]
    fn test_canonical_acl_symlinks_and_protected_globs() {
        let base_dir =
            std::env::temp_dir().join(format!("spell_acl_symlinks_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&base_dir);
        let ws = base_dir.join("ws");
        fs::create_dir_all(ws.join("subdir")).unwrap();

        let outside_file = base_dir.join("outside.txt");
        fs::write(&outside_file, "secret outside content\n").unwrap();

        #[cfg(unix)]
        let (symlink_outside, symlink_ssh) = {
            use std::os::unix::fs::symlink;
            let s_out = ws.join("link_outside.txt");
            let s_ssh = ws.join("link_ssh");
            symlink(&outside_file, &s_out).unwrap();
            symlink(Path::new("/etc"), &s_ssh).unwrap();
            (s_out, s_ssh)
        };

        // 1. Valid non-existent file inside workspace (`ws/subdir/new_file.rs` and `ws/new_dir/new_file.rs`) succeeds
        let ok1 = resolve_workspace_path(&ws, "subdir/new_file.rs").unwrap();
        assert_eq!(ok1, ws.join("subdir/new_file.rs"));
        let ok2 = resolve_workspace_path(&ws, "nonexistent_dir/new_file.rs").unwrap();
        assert_eq!(ok2, ws.join("nonexistent_dir/new_file.rs"));

        // 2. Traversal `../outside.txt` or `nonexistent_dir/../../outside.txt` -> SecurityViolation
        assert!(matches!(
            resolve_workspace_path(&ws, "../outside.txt"),
            Err(ToolError::SecurityViolation(_))
        ));
        assert!(matches!(
            resolve_workspace_path(&ws, "nonexistent_dir/../../outside.txt"),
            Err(ToolError::SecurityViolation(_))
        ));

        // 3. Outside absolute path `/etc/hosts` and tilde path `~/.ssh/id_rsa` -> SecurityViolation
        assert!(matches!(
            resolve_workspace_path(&ws, "/etc/hosts"),
            Err(ToolError::SecurityViolation(_))
        ));
        assert!(matches!(
            resolve_workspace_path(&ws, "~/.ssh/id_rsa"),
            Err(ToolError::SecurityViolation(_))
        ));

        // 4. Symlinks pointing to `outside.txt` and `/etc` -> SecurityViolation
        #[cfg(unix)]
        {
            assert!(matches!(
                resolve_workspace_path(&ws, "link_outside.txt"),
                Err(ToolError::SecurityViolation(_))
            ));
            assert!(matches!(
                resolve_workspace_path(&ws, &symlink_outside.to_string_lossy()),
                Err(ToolError::SecurityViolation(_))
            ));
            assert!(matches!(
                resolve_workspace_path(&ws, "link_ssh"),
                Err(ToolError::SecurityViolation(_))
            ));
            assert!(matches!(
                resolve_workspace_path(&ws, &symlink_ssh.to_string_lossy()),
                Err(ToolError::SecurityViolation(_))
            ));
        }

        // 5. Protected globs matrix: `.git/config`, `.git/hooks/pre-commit`, `.env`, `.env.local`, `.ssh`, `.aws`
        for protected_rel in [
            ".git/config",
            ".git/hooks/pre-commit",
            ".env",
            ".env.local",
            ".ssh/id_ed25519",
            ".aws/credentials",
        ] {
            assert!(
                matches!(
                    resolve_workspace_path(&ws, protected_rel),
                    Err(ToolError::SecurityViolation(_))
                ),
                "Expected SecurityViolation for protected path '{protected_rel}'"
            );
        }

        let _ = fs::remove_dir_all(&base_dir);
    }

    #[test]
    fn test_folder_access_tiers_and_read_only_root() {
        let base_dir =
            std::env::temp_dir().join(format!("spell_access_tiers_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&base_dir);
        let ws = base_dir.join("ws");
        let ro_root = base_dir.join("ro_sibling");
        fs::create_dir_all(&ws).unwrap();
        fs::create_dir_all(&ro_root).unwrap();

        let registered = vec![
            (ws.clone(), FolderAccessTier::DirectWrite),
            (ro_root.clone(), FolderAccessTier::ReadOnly),
        ];

        assert!(matches!(
            verify_write_access_for_path(
                &ro_root.join("notes.md"),
                FolderAccessTier::DirectWrite,
                &registered
            ),
            Err(ToolError::PermissionDenied(_))
        ));
        assert_eq!(
            verify_write_access_for_path(
                &ws.join("src/lib.rs"),
                FolderAccessTier::StageWrite,
                &registered
            )
            .unwrap(),
            FolderAccessTier::StageWrite
        );
        assert_eq!(
            verify_write_access_for_path(
                &ws.join("src/lib.rs"),
                FolderAccessTier::DirectWrite,
                &registered
            )
            .unwrap(),
            FolderAccessTier::DirectWrite
        );

        let _ = fs::remove_dir_all(&base_dir);
    }

    #[test]
    fn test_terminal_allow_scoped_timeout_and_env_scrub() {
        let _env_guard = SANDBOX_ENV_TEST_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let allowed_re = r"^cargo (check|test)(\s+[A-Za-z0-9_\-\.]+)*$";

        // 1. Allow-scoped regex validation & shell metacharacter rejection
        assert!(validate_allow_scoped_command("cargo test --offline", allowed_re).is_ok());
        assert!(validate_allow_scoped_command("cargo check --lib", allowed_re).is_ok());
        assert!(validate_allow_scoped_command("cargo test; rm -rf /", allowed_re).is_err());
        assert!(validate_allow_scoped_command("cargo test && echo pwned", allowed_re).is_err());
        assert!(validate_allow_scoped_command("python3 -c 'print(1)'", allowed_re).is_err());

        let ws = std::env::temp_dir().join(format!("spell_terminal_scrub_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&ws);
        fs::create_dir_all(&ws).unwrap();

        // 2. Process-group timeout kills hanging process group (`sh -c "sleep 10"` on Unix,
        //    `cmd /C "ping -n 10 127.0.0.1 >nul"` on Windows) within ~250ms
        #[cfg(not(windows))]
        let sleep_cmd = "sleep 10";
        #[cfg(windows)]
        let sleep_cmd = "ping -n 10 127.0.0.1 >nul";
        let t0 = Instant::now();
        let timeout_out = run_terminal_command(&ws, sleep_cmd, 250, false).unwrap();
        let elapsed = t0.elapsed();
        assert!(
            timeout_out.contains("[TERMINAL TIMEOUT after 250 ms]"),
            "Expected timeout banner, got: {timeout_out}"
        );
        assert!(
            elapsed < Duration::from_millis(2_000),
            "Process group kill should not wait for sleep 10; elapsed={elapsed:?}"
        );

        // 3. Sensitive environment variable scrubbing (SPELL_API_KEY)
        std::env::set_var("SPELL_API_KEY", "super-secret-spell-token-999");
        #[cfg(not(windows))]
        let env_cmd = "printf '%s' \"${SPELL_API_KEY:-SCRUBBED}\"";
        #[cfg(windows)]
        let env_cmd = "if defined SPELL_API_KEY (echo %SPELL_API_KEY%) else (echo SCRUBBED)";
        let env_out = run_terminal_command(&ws, env_cmd, 5_000, false).unwrap();
        assert!(
            env_out.contains("SCRUBBED") && !env_out.contains("super-secret-spell-token-999"),
            "Expected SPELL_API_KEY to be scrubbed, got: {env_out}"
        );

        // 4. Local commands (`echo hello`) and loopback TCP connections succeed cleanly.
        let local_out = run_terminal_command(&ws, "echo hello", 5_000, false).unwrap();
        assert!(local_out.contains("exit_code: 0"));
        assert!(local_out.contains("hello"));

        if cfg!(target_os = "macos") && Path::new("/usr/bin/python3").exists() {
            let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
            let port = listener.local_addr().unwrap().port();
            let net_out = run_terminal_command(
                &ws,
                &format!("python3 -c \"import socket; s=socket.socket(); s.settimeout(2.0); s.connect(('127.0.0.1', {port}))\""),
                5_000,
                false,
            )
            .unwrap();
            assert!(
                net_out.contains("exit_code: 0"),
                "Expected terminal to allow IP socket connections, got: {net_out}"
            );
        }

        let _ = fs::remove_dir_all(&ws);
    }

    #[test]
    fn test_stage_write_shadow_overlay_commit_and_rollback() {
        let ws = std::env::temp_dir()
            .join(format!("spell_stage_write_overlay_{}", std::process::id()));
        let _ = fs::remove_dir_all(&ws);
        let src_dir = ws.join("src");
        fs::create_dir_all(&src_dir).unwrap();

        let lib_rs = src_dir.join("lib.rs");
        let original_text = "pub fn compute() -> i32 {\n    41\n}\n";
        let staged_text = "pub fn compute() -> i32 {\n    42\n}\n";
        fs::write(&lib_rs, original_text).unwrap();
        let original_sha = compute_sha256_full_hex(original_text);

        let mut store = WorkspaceSnapshotStore::new();
        let staged_mut = store
            .stage_write(&lib_rs, "src/lib.rs", staged_text.to_string())
            .unwrap();
        assert_eq!(staged_mut.status, "staged");
        assert_eq!(staged_mut.added_lines, 1);
        assert_eq!(staged_mut.removed_lines, 1);

        // 1. On-disk `src/lib.rs` still contains `41` (untouched!).
        let on_disk_before_commit = fs::read_to_string(&lib_rs).unwrap();
        assert_eq!(on_disk_before_commit, original_text);
        assert!(on_disk_before_commit.contains("41"));
        assert!(!on_disk_before_commit.contains("42"));

        // 2. `get_or_load_snapshot` and `get_symbol_body_in_snapshot(..., "compute")` return the staged version with `42`.
        let snap = store.get_or_load_snapshot(&lib_rs).unwrap();
        assert!(snap.content.contains("42"));
        assert_eq!(snap.slice_lines(2, 2), (2, 2, "    42"));

        let (sym_entry, sym_body) =
            get_symbol_body_in_snapshot(&mut store, &lib_rs, "compute").unwrap();
        assert_eq!(sym_entry.name, "compute");
        assert_eq!(sym_entry.kind, "fn");
        assert_eq!((sym_entry.start_line, sym_entry.end_line), (1, 3));
        assert!(sym_body.contains("2:     42"));

        // 3. `unified_diff` contains `"-    41"` and `"+    42"`.
        assert!(staged_mut.unified_diff.contains("--- a/src/lib.rs"));
        assert!(staged_mut.unified_diff.contains("+++ b/src/lib.rs"));
        assert!(staged_mut.unified_diff.contains("-    41"));
        assert!(staged_mut.unified_diff.contains("+    42"));

        // 4. `mark_staged_verified` transitions status to `"verified"`.
        let verified_mut = store.mark_staged_verified(&lib_rs).unwrap();
        assert_eq!(verified_mut.status, "verified");

        // 5. `commit_staged` writes `42` to disk; subsequent `rollback_staged` restores `41` on disk with matching SHA-256.
        let committed = store.commit_staged(Some(&lib_rs)).unwrap();
        assert_eq!(committed.len(), 1);
        assert_eq!(committed[0].status, "committed");
        let on_disk_after_commit = fs::read_to_string(&lib_rs).unwrap();
        assert_eq!(on_disk_after_commit, staged_text);

        let rolled_back = store.rollback_staged(Some(&lib_rs)).unwrap();
        assert_eq!(rolled_back.len(), 1);
        assert_eq!(rolled_back[0].status, "rolled_back");
        let on_disk_after_rollback = fs::read_to_string(&lib_rs).unwrap();
        assert_eq!(on_disk_after_rollback, original_text);
        let restored_snap = store.get_or_load_snapshot(&lib_rs).unwrap();
        assert_eq!(restored_snap.sha256_hex, original_sha);

        let _ = fs::remove_dir_all(&ws);
    }

    #[test]
    fn test_primitives_snapshots_symbols_and_diagnostics() {
        // 1. Verify `FileSnapshot::slice_lines` O(1) line slicing and SHA-256
        let sample = "line_alpha\nline_beta\nline_gamma\nline_delta\n";
        let snap = FileSnapshot::from_content(PathBuf::from("sample.txt"), sample.to_string());
        assert_eq!(snap.total_lines, 4);
        assert_eq!(snap.sha256_hex, compute_sha256_full_hex(sample));
        assert_eq!(snap.slice_lines(2, 3), (2, 3, "line_beta\nline_gamma"));
        assert_eq!(snap.slice_lines(4, 4), (4, 4, "line_delta"));

        // 2. Verify `extract_outline_symbols` and `get_symbol_body` on Rust (`impl LoopScheduler { pub fn tick() {} }`), Swift, and Python
        let rust_single = "impl LoopScheduler { pub fn tick() {} }";
        let rust_syms = extract_outline_symbols(rust_single);
        assert!(rust_syms
            .iter()
            .any(|s| s.name == "LoopScheduler" && s.kind == "impl"));
        assert!(rust_syms
            .iter()
            .any(|s| s.name == "tick" && s.kind == "fn" && s.start_line == 1 && s.end_line == 1));
        let (tick_entry, tick_body) = get_symbol_body(rust_single, "LoopScheduler::tick").unwrap();
        assert_eq!(tick_entry.name, "tick");
        assert!(tick_body.contains("pub fn tick()"));

        let swift_src = "\
public struct WorkspacePane {
    public func refreshView() {
        let _ = 1
    }
}
";
        let swift_syms = extract_outline_symbols(swift_src);
        assert!(swift_syms
            .iter()
            .any(|s| s.name == "WorkspacePane" && s.kind == "struct" && s.start_line == 1 && s.end_line == 5));
        assert!(swift_syms
            .iter()
            .any(|s| s.name == "refreshView" && s.kind == "func" && s.start_line == 2 && s.end_line == 4));
        let (_, swift_body) = get_symbol_body(swift_src, "WorkspacePane.refreshView").unwrap();
        assert!(swift_body.contains("2:     public func refreshView() {"));

        let py_src = "\
class CacheMonitor:
    async def check_epoch(self, epoch: int) -> bool:
        return epoch >= 0

def parse_metric(raw: str) -> int:
    return len(raw)
";
        let py_syms = extract_outline_symbols(py_src);
        assert!(py_syms
            .iter()
            .any(|s| s.name == "CacheMonitor" && s.kind == "class" && s.start_line == 1 && s.end_line == 3));
        assert!(py_syms
            .iter()
            .any(|s| s.name == "check_epoch" && s.kind == "def" && s.start_line == 2 && s.end_line == 3));
        assert!(py_syms
            .iter()
            .any(|s| s.name == "parse_metric" && s.kind == "def" && s.start_line == 5 && s.end_line == 6));

        // 3. Verify `DiagnosticsRingBuffer` parses cargo JSON and swiftc lines
        let mut ring = DiagnosticsRingBuffer::new();
        let cargo_json_line = r#"{"reason":"compiler-message","message":{"level":"error","code":{"code":"E0425"},"message":"cannot find value `missing_var` in this scope","spans":[{"file_name":"src/tools.rs","line_start":88,"column_start":13,"is_primary":true}]}}"#;
        let swiftc_line = "Sources/App/MainView.swift:14:9: error: cannot find 'foo' in scope";
        let swiftc_warn = "Sources/App/MainView.swift:20:5: warning: immutable value 'x' was never used";

        let c_count = ring.ingest_cargo_json_output(cargo_json_line);
        assert_eq!(c_count, 1);
        let s_count = ring.ingest_swiftc_output(&format!("{swiftc_line}\n{swiftc_warn}\n"));
        assert_eq!(s_count, 2);

        let all_diags = ring.all();
        assert_eq!(all_diags.len(), 3);
        let errs = ring.errors();
        assert_eq!(errs.len(), 2);
        assert_eq!(errs[0].file, "src/tools.rs");
        assert_eq!(errs[0].line, 88);
        assert_eq!(errs[0].column, 13);
        assert_eq!(errs[0].code.as_deref(), Some("E0425"));
        assert!(errs[0].message.contains("missing_var"));

        assert_eq!(errs[1].file, "Sources/App/MainView.swift");
        assert_eq!(errs[1].line, 14);
        assert_eq!(errs[1].column, 9);
        assert_eq!(errs[1].severity, "error");
        assert_eq!(errs[1].message, "cannot find 'foo' in scope");
    }

    #[test]
    fn test_lexical_symbol_outliner_raw_strings_lifetimes_and_docstrings() {
        let rust_src = "\
impl<'a> LexerScanner<'a> {
    pub fn scan_token<'b>(&'a self, input: &'b str) -> &'b str {
        let raw_pat = r#\"unbalanced { { { brace\"#;
        let open_brace_char = '{';
        let escaped_brace_char = '\\{';
        let _ = (raw_pat, open_brace_char, escaped_brace_char);
        input
    }
}
";
        let rust_syms = extract_outline_symbols(rust_src);
        assert!(rust_syms
            .iter()
            .any(|s| s.name == "LexerScanner" && s.kind == "impl" && s.start_line == 1 && s.end_line == 9));
        assert!(rust_syms
            .iter()
            .any(|s| s.name == "scan_token" && s.kind == "fn" && s.start_line == 2 && s.end_line == 8));

        let (fn_entry, fn_body) = get_symbol_body(rust_src, "LexerScanner::scan_token").unwrap();
        assert_eq!((fn_entry.start_line, fn_entry.end_line), (2, 8));
        assert!(fn_body.contains("r#\"unbalanced { { { brace\"#"));
        assert!(fn_body.contains("7:         input"));
        assert!(fn_body.ends_with("8:     }"));

        let py_src = "\
def evaluate():
    \"\"\"Multi-line docstring header
Column-0 unindented docstring line that must not terminate def
Another column-0 line inside triple quotes
\"\"\"
    return True

def next_fn():
    return False
";
        let py_syms = extract_outline_symbols(py_src);
        assert!(py_syms
            .iter()
            .any(|s| s.name == "evaluate" && s.kind == "def" && s.start_line == 1 && s.end_line == 6));
        let (py_entry, py_body) = get_symbol_body(py_src, "evaluate").unwrap();
        assert_eq!((py_entry.start_line, py_entry.end_line), (1, 6));
        assert!(py_body.contains("6:     return True"));
    }

    #[test]
    fn test_app_sandbox_compatibility_and_in_process_guards() {
        let _env_guard = SANDBOX_ENV_TEST_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let ws = std::env::temp_dir()
            .join(format!("spell_app_sandbox_guards_{}", std::process::id()));
        let _ = fs::remove_dir_all(&ws);
        fs::create_dir_all(&ws).unwrap();

        std::env::set_var("SPELL_APP_SANDBOX", "1");
        std::env::set_var("SPELL_API_KEY", "sandbox-secret-token-777");
        assert!(is_app_sandbox_active());

        // 1. Direct shell execution succeeds under SPELL_APP_SANDBOX=1 and SPELL_API_KEY is scrubbed.
        #[cfg(not(windows))]
        let check_cmd = "printf 'OK:%s' \"${SPELL_API_KEY:-SCRUBBED}\"";
        #[cfg(windows)]
        let check_cmd = "if defined SPELL_API_KEY (echo OK:%SPELL_API_KEY%) else (echo OK:SCRUBBED)";
        let out = run_terminal_command(&ws, check_cmd, 5_000, false).unwrap();

        // 2. All in-process security controls remain strictly enforced under SPELL_APP_SANDBOX=1
        assert!(validate_path(&ws, "../escape.txt").is_err());
        assert!(run_terminal_command(&ws, "cat ~/.ssh/id_rsa", 5_000, false).is_err());
        assert!(run_terminal_command(&ws, "cat .git/config", 5_000, false).is_err());

        std::env::remove_var("SPELL_APP_SANDBOX");

        assert!(out.contains("exit_code: 0"), "Expected exit_code: 0, got: {out}");
        assert!(
            out.contains("OK:SCRUBBED") && !out.contains("sandbox-secret-token-777"),
            "Expected direct execution with scrubbed env under SPELL_APP_SANDBOX=1, got: {out}"
        );

        let _ = fs::remove_dir_all(&ws);
    }

    #[test]
    fn test_adversarial_exit_71_never_retries_or_runs_twice() {
        let _env_guard = SANDBOX_ENV_TEST_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        std::env::remove_var("SPELL_APP_SANDBOX");

        let ws = std::env::temp_dir().join(format!(
            "spell_adv_exit71_noretry_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&ws);
        fs::create_dir_all(&ws).unwrap();

        #[cfg(not(windows))]
        let cmd = "echo \"ran\" >> side_effect.log; echo \"operation failed\" >&2; exit 71";
        #[cfg(windows)]
        let cmd = "(echo ran>>side_effect.log) & (echo operation failed 1>&2) & exit /b 71";
        let out = run_terminal_command(&ws, cmd, 5_000, false).unwrap();

        assert!(
            out.contains("exit_code: 71"),
            "Expected exit_code: 71 from command, got: {out}"
        );

        let side_effect = fs::read_to_string(ws.join("side_effect.log")).unwrap();
        assert_eq!(
            side_effect.replace("\r\n", "\n"),
            "ran\n",
            "Command must execute exactly once and never retry; side_effect.log={side_effect:?}"
        );
        assert_eq!(side_effect.lines().count(), 1);

        let _ = fs::remove_dir_all(&ws);
    }

    #[test]
    fn test_adversarial_network_with_exit_71_runs_once() {
        let _env_guard = SANDBOX_ENV_TEST_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        std::env::remove_var("SPELL_APP_SANDBOX");

        if !Path::new("/usr/bin/python3").exists() {
            return;
        }

        let ws = std::env::temp_dir().join(format!(
            "spell_adv_net_exit71_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&ws);
        fs::create_dir_all(&ws).unwrap();

        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let port = listener.local_addr().unwrap().port();

        let cmd = format!(
            "echo attempt >> attempts.log; \
             python3 -c \"import socket, sys\n\
             try:\n\
             \x20   s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)\n\
             \x20   s.settimeout(0.5)\n\
             \x20   s.connect(('127.0.0.1', {port}))\n\
             \x20   s.close()\n\
             except Exception:\n\
             \x20   pass\n\
             sys.stderr.write('operation failed\\n')\n\
             sys.exit(71)\""
        );

        let out = run_terminal_command(&ws, &cmd, 5_000, false).unwrap();
        assert!(
            out.contains("exit_code: 71"),
            "Expected exit_code: 71, got: {out}"
        );

        let attempts = fs::read_to_string(ws.join("attempts.log")).unwrap();
        assert_eq!(
            attempts.lines().count(),
            1,
            "Adversarial network+exit71 command must run only once"
        );
        let _ = listener.accept();

        let _ = fs::remove_dir_all(&ws);
    }

    #[test]
    fn test_large_1mb_stdout_and_stderr_no_pipe_deadlock() {
        let _env_guard = SANDBOX_ENV_TEST_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        std::env::remove_var("SPELL_APP_SANDBOX");

        let ws = std::env::temp_dir().join(format!(
            "spell_large_pipe_test_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&ws);
        fs::create_dir_all(&ws).unwrap();

        // Write 1,048,576 bytes (1 MB) to stdout and 262,144 bytes (256 KB) to stderr
        #[cfg(not(windows))]
        let py = "python3";
        #[cfg(windows)]
        let py = "python";
        let cmd = format!(
            "{py} -c \"import sys; sys.stdout.write('A' * 1048576); sys.stdout.flush(); sys.stderr.write('B' * 262144); sys.stderr.flush()\""
        );
        let start = Instant::now();
        let out = run_terminal_command(&ws, &cmd, 10_000, false).unwrap();
        let elapsed = start.elapsed();

        assert!(
            !out.contains("[TERMINAL TIMEOUT"),
            "1 MB stdout + 256 KB stderr must not deadlock or time out; got: {}",
            &out[..out.len().min(200)]
        );
        assert!(
            out.contains("exit_code: 0"),
            "Expected exit_code: 0, got: {}",
            &out[..out.len().min(200)]
        );
        assert!(
            elapsed < Duration::from_millis(8_000),
            "Expected large pipe drain to finish in < 8000ms, took {elapsed:?}"
        );

        let _ = fs::remove_dir_all(&ws);
    }

    #[test]
    fn test_frozen_fetch_tool_schema_and_system_prompt_invariance() {
        let write_tools = build_openai_tools_schema(AgentProfile::Write);
        assert_eq!(write_tools.len(), 17);
        assert_eq!(write_tools[11]["function"]["name"], "fetch");
        let expected_fetch_schema = json!({
            "type": "function",
            "function": {
                "name": "fetch",
                "description": "Fetches a web or research document URL, automatically normalizing arXiv abs/pdf links to https://arxiv.org/html/<id>.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "url": { "type": "string", "description": "URL to fetch." }
                    },
                    "required": ["url"]
                }
            }
        });
        assert_eq!(write_tools[11], expected_fetch_schema);

        let ask_tools = build_openai_tools_schema(AgentProfile::Ask);
        assert_eq!(ask_tools.len(), 8);
        assert_eq!(ask_tools[4]["function"]["name"], "fetch");
        assert_eq!(ask_tools[4], expected_fetch_schema);

        // Legacy offline execute_fetch_tool remains backward-compatible
        let legacy = execute_fetch_tool("https://arxiv.org/abs/2501.12345").unwrap();
        assert!(legacy.contains("https://arxiv.org/html/2501.12345"));

        // Online fetch helper fails closed in default Offline (Air-Gapped) mode
        let cfg = crate::online_fetch::OnlineResearchConfig::with_defaults();
        let offline_err =
            execute_fetch_tool_with_online_config("https://arxiv.org/abs/2501.12345", &cfg)
                .unwrap_err();
        assert!(offline_err.to_string().contains("Air-Gapped"));
    }

    #[test]
    fn test_research_cache_read_allowed_and_write_blocked() {
        let ws = std::env::temp_dir().join(format!(
            "spell_research_cache_guard_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&ws);
        let cache_dir = ws.join(".spell/research_cache/papers/arxiv_2501_12345");
        fs::create_dir_all(&cache_dir).unwrap();
        let cached_doc = cache_dir.join("document.md");
        fs::write(&cached_doc, "# Theorem 1\nNEEDLE_EQUATION_42\n").unwrap();

        // 1. Read tools succeed seamlessly on <workspace>/.spell/research_cache/...
        let mut pin_cache = TurnPinCache::new();
        let read_out = execute_read_file_tool(
            &ws,
            ".spell/research_cache/papers/arxiv_2501_12345/document.md",
            None,
            None,
            &mut pin_cache,
            "call_rc_1",
        )
        .unwrap();
        assert!(read_out.contains("NEEDLE_EQUATION_42"));

        let grep_hits = grep_workspace(&ws, "NEEDLE_EQUATION_42").unwrap();
        assert_eq!(grep_hits.len(), 1);

        let found = find_path(&ws, "document.md").unwrap();
        assert_eq!(found.len(), 1);

        let dir_list = list_directory(&cache_dir).unwrap();
        assert!(dir_list.contains("document.md"));

        let (sub_count, _, sub_bundle) = collect_numbered_subagent_context(
            &ws,
            &[".spell/research_cache/papers/arxiv_2501_12345/document.md".to_string()],
            None,
            64_000,
            true,
        )
        .unwrap();
        assert_eq!(sub_count, 1);
        assert!(sub_bundle.contains("NEEDLE_EQUATION_42"));

        // Copying FROM research_cache into normal workspace is allowed
        let copied_ws = ws.join("notes_copy.md");
        copy_path(&cached_doc, &copied_ws).unwrap();
        assert!(copied_ws.exists());

        // 2. Write / edit / delete / move / copy(dst) / stage_write are strictly blocked
        let expected_err = ToolError::SecurityViolation(
            "Cannot modify read-only research_cache staging files".to_string(),
        );
        assert_eq!(write_file(&cached_doc, "tamper", false), Err(expected_err.clone()));
        assert_eq!(
            edit_file_surgical(&cached_doc, "Theorem 1", "Tampered"),
            Err(expected_err.clone())
        );
        assert_eq!(
            execute_edit_file_tool(
                &ws,
                &EditFileToolInput {
                    path: ".spell/research_cache/papers/arxiv_2501_12345/document.md".to_string(),
                    display_description: None,
                    edits: vec![FileEditOperation {
                        old_text: "Theorem 1".to_string(),
                        new_text: "Tampered".to_string(),
                    }],
                    old_text: None,
                    new_text: None,
                },
                false,
            ),
            Err(expected_err.clone())
        );
        assert_eq!(delete_path(&cached_doc), Err(expected_err.clone()));
        assert_eq!(move_path(&cached_doc, &ws.join("moved.md")), Err(expected_err.clone()));
        assert_eq!(move_path(&copied_ws, &cached_doc), Err(expected_err.clone()));
        assert_eq!(copy_path(&copied_ws, &cached_doc), Err(expected_err.clone()));

        let mut store = WorkspaceSnapshotStore::new();
        assert_eq!(
            store.stage_write(
                &cached_doc,
                ".spell/research_cache/papers/arxiv_2501_12345/document.md",
                "tamper".to_string(),
            ),
            Err(expected_err)
        );

        let _ = fs::remove_dir_all(&ws);
    }

    #[test]
    fn test_is_web_taint_sensitive_workspace_file_apfs_case_insensitive() {
        for sensitive in [
            "AGENTS.md",
            "agents.md",
            "./Agents.MD",
            "packages/sub/AGENTS.md",
            "CLAUDE.md",
            "claude.md",
            ".rules",
            ".RULES",
            ".cursorrules",
            ".CursorRules",
            "build.rs",
            "rust/spell_core/BUILD.RS",
            ".cargo/config.toml",
            ".CARGO/CONFIG.TOML",
            ".cargo/config",
            ".git/hooks/pre-commit",
            ".GIT/HOOKS/post-merge",
            ".vscode/tasks.json",
            ".VSCODE/TASKS.JSON",
            ".zed/tasks.json",
            ".ZED/TASKS.JSON",
        ] {
            assert!(
                is_web_taint_sensitive_workspace_file(sensitive),
                "Expected '{sensitive}' to be classified as web-taint sensitive"
            );
        }

        for benign in [
            "src/lib.rs",
            "src/agent_loop.rs",
            "Cargo.toml",
            "README.md",
            "docs/architecture.md",
            ".spell/research_cache/doc.md",
        ] {
            assert!(
                !is_web_taint_sensitive_workspace_file(benign),
                "Expected '{benign}' NOT to be classified as web-taint sensitive"
            );
        }
    }

    #[test]
    fn test_delegate_coding_subagent_autonomous_schema_and_system_prompt() {
        assert_eq!(HEADLESS_TOOL_NAMES.len(), 17);
        let write_tools = build_openai_tools_schema(AgentProfile::Write);
        assert_eq!(write_tools.len(), 17);
        assert_eq!(write_tools[11]["function"]["name"], "fetch");

        let ask_tools = build_openai_tools_schema(AgentProfile::Ask);
        assert_eq!(ask_tools.len(), 8);
        assert_eq!(ask_tools[4]["function"]["name"], "fetch");

        let delegate_schema = write_tools
            .iter()
            .find(|t| t["function"]["name"] == "delegate_coding_subagent")
            .expect("delegate_coding_subagent present in Write schema");
        let params = &delegate_schema["function"]["parameters"];
        assert_eq!(params["required"], json!(["instruction"]));
        assert_eq!(params["properties"]["mode"]["enum"], json!(["auto", "propose"]));
        assert_eq!(
            params["properties"]["protocol"]["enum"],
            json!(["auto", "native", "json"])
        );
        assert_eq!(params["properties"]["max_steps"]["type"], "integer");
        assert_eq!(params["properties"]["target_files"]["type"], "array");
        assert!(params["properties"]["target_files"].get("minItems").is_none());

        let tmp = std::env::temp_dir().join(format!(
            "spell_delegate_sysprompt_test_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&tmp);
        fs::create_dir_all(&tmp).unwrap();
        let sys = build_clean_system_prompt("Proj", &tmp, false, AgentProfile::Write, &[]);
        assert!(sys.contains("auto-commits changes directly"));
        assert!(sys.contains("undo_id"));
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn test_enforce_research_cache_write_guard_mixed_case_apfs() {
        let ws = std::env::temp_dir().join(format!(
            "spell_research_cache_mixed_case_test_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&ws);
        fs::create_dir_all(&ws).unwrap();

        let expected_err = ToolError::SecurityViolation(
            "Cannot modify read-only research_cache staging files".to_string(),
        );

        // Caught even before `.Spell/Research_Cache` exists on disk!
        for mixed in [
            PathBuf::from(".Spell/Research_Cache/foo.md"),
            PathBuf::from("./.SPELL/research_cache/papers/doc.md"),
            ws.join(".Spell/Research_Cache/foo.md"),
            ws.join(".spell/RESEARCH_CACHE/nested/bar.md"),
        ] {
            assert!(
                crate::online_fetch::is_research_cache_path(&mixed),
                "Expected is_research_cache_path == true for '{}'",
                mixed.display()
            );
            assert_eq!(
                enforce_research_cache_write_guard(&mixed),
                Err(expected_err.clone())
            );
        }

        let abs_mixed = ws.join(".Spell/Research_Cache/foo.md");
        assert_eq!(
            write_file(&abs_mixed, "tamper", true),
            Err(expected_err.clone())
        );
        let mut store = WorkspaceSnapshotStore::new();
        assert_eq!(
            store.stage_write(&abs_mixed, ".Spell/Research_Cache/foo.md", "tamper".into()),
            Err(expected_err)
        );
        assert!(!abs_mixed.exists());

        let _ = fs::remove_dir_all(&ws);
    }

    #[cfg(unix)]
    #[test]
    fn test_commit_staged_rejects_leaf_and_parent_symlinks_nofollow() {
        use std::os::unix::fs::symlink;

        let ws = std::env::temp_dir().join(format!(
            "spell_commit_staged_symlink_test_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&ws);
        fs::create_dir_all(ws.join("src")).unwrap();

        let cargo_toml = ws.join("Cargo.toml");
        fs::write(&cargo_toml, "[package]\nname = \"sacred\"\n").unwrap();
        let target_rs = ws.join("src/foo.rs");
        fs::write(&target_rs, "pub fn orig() {}\n").unwrap();

        // 1. Stage legitimate write, then swap `src/foo.rs` for intra-workspace symlink `-> ../Cargo.toml`
        let mut store = WorkspaceSnapshotStore::new();
        store
            .stage_write(&target_rs, "src/foo.rs", "CLOBBERED\n".to_string())
            .unwrap();
        fs::remove_file(&target_rs).unwrap();
        symlink("../Cargo.toml", &target_rs).unwrap();

        let leaf_err = store.commit_staged(None).unwrap_err();
        assert!(
            matches!(leaf_err, ToolError::SecurityViolation(_)),
            "Expected SecurityViolation on leaf symlink, got {leaf_err:?}"
        );
        assert_eq!(
            fs::read_to_string(&cargo_toml).unwrap(),
            "[package]\nname = \"sacred\"\n",
            "Cargo.toml must NOT be clobbered through symlink!"
        );

        // 2. Stage write under subdir `src/sub/bar.rs`, then swap `src/sub` for a directory symlink
        let real_outside_dir = ws.join("protected_dir");
        fs::create_dir_all(&real_outside_dir).unwrap();
        let bar_target = ws.join("src/sub/bar.rs");
        let mut store2 = WorkspaceSnapshotStore::new();
        store2
            .stage_write(&bar_target, "src/sub/bar.rs", "CLOBBERED_SUB\n".to_string())
            .unwrap();
        symlink(&real_outside_dir, ws.join("src/sub")).unwrap();

        let parent_err = store2.commit_staged(None).unwrap_err();
        assert!(
            matches!(parent_err, ToolError::SecurityViolation(_)),
            "Expected SecurityViolation on parent symlink, got {parent_err:?}"
        );
        assert!(!real_outside_dir.join("bar.rs").exists());

        let _ = fs::remove_dir_all(&ws);
    }

    #[test]
    fn test_collect_numbered_subagent_context_partial_missing_all_missing_and_security_hard_fail() {
        let ws = std::env::temp_dir().join(format!(
            "spell_subagent_ctx_partial_missing_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&ws);
        fs::create_dir_all(ws.join("empty_dir")).unwrap();
        fs::write(ws.join("existing.md"), "# Existing Doc\nAlpha line\n").unwrap();
        fs::write(ws.join("README.md"), "# Readme\nBeta line\n").unwrap();

        // (a) Partial missing path (`[existing.md, README.md, missing.md]`) succeeds,
        // includes existing files + `<missing_paths>` note, and populates `missing_paths`.
        let partial_paths = vec![
            "existing.md".to_string(),
            "README.md".to_string(),
            "missing.md".to_string(),
        ];
        let detailed = collect_numbered_subagent_context_detailed(
            &ws,
            &[],
            &partial_paths,
            None,
            64_000,
            true,
        )
        .expect("partial missing paths must succeed when valid sibling files exist");
        assert_eq!(detailed.files_read, 2);
        assert_eq!(detailed.missing_paths, vec!["missing.md".to_string()]);
        assert!(detailed.bundle.contains("<file path=\"existing.md\""));
        assert!(detailed.bundle.contains("Alpha line"));
        assert!(detailed.bundle.contains("<file path=\"README.md\""));
        assert!(detailed.bundle.contains("Beta line"));
        assert!(detailed.bundle.contains("<missing_paths>"));
        assert!(detailed.bundle.contains("- missing.md"));
        assert!(detailed.bundle.contains("</missing_paths>"));

        let (tuple_count, tuple_tokens, tuple_bundle) =
            collect_numbered_subagent_context_with_roots(&ws, &[], &partial_paths, None, 64_000, true)
                .expect("tuple wrapper must also tolerate partial missing paths");
        assert_eq!(tuple_count, detailed.files_read);
        assert_eq!(tuple_tokens, detailed.approx_tokens);
        assert_eq!(tuple_bundle, detailed.bundle);

        // (b) All missing paths (`[missing1.md, missing2.md]` and single `[missing1.md]`)
        // as well as empty directory still return `Err(ToolError::NotFound(_))`.
        let all_missing_err = collect_numbered_subagent_context_with_roots(
            &ws,
            &[],
            &["missing1.md".to_string(), "missing2.md".to_string()],
            None,
            64_000,
            true,
        )
        .unwrap_err();
        assert!(
            matches!(all_missing_err, ToolError::NotFound(_)),
            "expected NotFound for all missing paths, got {all_missing_err:?}"
        );
        let single_missing_err = collect_numbered_subagent_context_with_roots(
            &ws,
            &[],
            &["missing_only.md".to_string()],
            None,
            64_000,
            true,
        )
        .unwrap_err();
        assert!(
            matches!(single_missing_err, ToolError::NotFound(ref msg) if msg.contains("Workspace path does not exist: 'missing_only.md'")),
            "expected exact single-missing NotFound message, got {single_missing_err:?}"
        );
        let empty_dir_err = collect_numbered_subagent_context_with_roots(
            &ws,
            &[],
            &["empty_dir".to_string()],
            None,
            64_000,
            true,
        )
        .unwrap_err();
        assert!(
            matches!(empty_dir_err, ToolError::NotFound(_)),
            "expected NotFound for empty directory, got {empty_dir_err:?}"
        );

        // (c) Security/protected/traversal paths STILL hard-fail immediately even when
        // paired with a valid existing file!
        for forbidden in [
            "../outside.txt",
            ".env",
            ".git/config",
            "~/.ssh/id_rsa",
        ] {
            let err = collect_numbered_subagent_context_with_roots(
                &ws,
                &[],
                &["existing.md".to_string(), forbidden.to_string()],
                None,
                64_000,
                true,
            )
            .unwrap_err();
            assert!(
                matches!(err, ToolError::SecurityViolation(_)),
                "expected SecurityViolation for forbidden path '{forbidden}', got {err:?}"
            );
        }

        // Verify `delegate_coding_subagent` schema clarifies target_files accepts existing files,
        // new target file paths, and existing/empty directories.
        let write_tools = build_openai_tools_schema(AgentProfile::Write);
        let delegate_schema = write_tools
            .iter()
            .find(|t| t["function"]["name"] == "delegate_coding_subagent")
            .expect("delegate_coding_subagent present");
        let desc = delegate_schema["function"]["parameters"]["properties"]["target_files"]
            ["description"]
            .as_str()
            .unwrap_or("");
        assert!(desc.contains("existing files"), "{desc}");
        assert!(desc.contains("new target file paths"), "{desc}");
        assert!(desc.contains("empty directories"), "{desc}");

        let _ = fs::remove_dir_all(&ws);
    }
}
