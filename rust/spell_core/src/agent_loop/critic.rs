use super::*;

/// Autonomous loop execution state.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum LoopState {
    #[default]
    Idle,
    Active,
    Reviewing,
    Paused,
    PausedOnCriticReject,
    Completed,
    Aborted,
}

impl LoopState {
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Idle => "idle",
            Self::Active => "active",
            Self::Reviewing => "reviewing",
            Self::Paused => "paused",
            Self::PausedOnCriticReject => "paused_on_critic_reject",
            Self::Completed => "completed",
            Self::Aborted => "aborted",
        }
    }
}

/// Gatekeeper Critic review verdict.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct CriticVerdict {
    #[serde(default)]
    pub status: String, // "PASS" | "REJECT"
    #[serde(default)]
    pub passed: bool,
    #[serde(default)]
    pub reason: String,
    #[serde(default)]
    pub violations: Vec<String>,
    #[serde(default)]
    pub fix_hint: String,
}

/// Single exact replacement hunk cached inside a [`SubagentCodingProposal`].
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct ProposalEditHunk {
    pub old_text: String,
    pub new_text: String,
}

/// Cached per-file coding proposal synthesized by `delegate_coding_subagent` (`prop-1`, `prop-2`, ...).
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct SubagentCodingProposal {
    pub proposal_id: String,
    pub path: String,
    pub edits: Vec<ProposalEditHunk>,
    pub created_at: u64,
}

/// Strips `<think>...</think>` blocks (and any leading prefix closed by `</think>`) so bounded
/// thinking traces containing `{...}` never corrupt downstream JSON / hunk extraction.
pub(crate) fn strip_think_blocks(raw: &str) -> String {
    let mut out = raw.to_string();
    loop {
        let lower = out.to_ascii_lowercase();
        if let Some(open_idx) = lower.find("<think>") {
            if let Some(rel_close) = lower[open_idx + 7..].find("</think>") {
                let close_end = open_idx + 7 + rel_close + 8;
                out.replace_range(open_idx..close_end, "");
                continue;
            }
        }
        if let Some(bare_close) = lower.find("</think>") {
            out.replace_range(0..bare_close + 8, "");
            continue;
        }
        break;
    }
    out
}

fn find_ascii_xml_tag_start(lower: &str, tag_prefix: &str) -> Option<usize> {
    let bytes = lower.as_bytes();
    let mut offset = 0usize;
    while let Some(rel) = lower[offset..].find(tag_prefix) {
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

/// Sanitizes `prefill_context_subagent` output before wrapping in `<subagent_context_packet>`:
/// 1. Strips `<think>...</think>` blocks (and any unclosed `<think>` tag cleanly).
/// 2. Unwraps/strips self-echoed `<subagent_context_packet ...>` and `</subagent_context_packet>` tags.
/// 3. Strips leaked `<tool_call>...</tool_call>` and `<function=...>...</function>` XML blocks
///    (including unclosed trailing `<tool_call` / `<function=` tags when truncated).
pub(crate) fn sanitize_prefill_context_body(raw: &str) -> String {
    let mut out = strip_think_blocks(raw);
    loop {
        let lower = out.to_ascii_lowercase();
        if let Some(idx) = find_ascii_xml_tag_start(&lower, "<think") {
            let after = idx + "<think".len();
            if let Some(rel_gt) = lower[after..].find('>') {
                out.replace_range(idx..after + rel_gt + 1, "");
            } else {
                out.replace_range(idx..out.len(), "");
            }
        } else {
            break;
        }
    }

    loop {
        let lower = out.to_ascii_lowercase();
        if let Some(idx) = find_ascii_xml_tag_start(&lower, "<subagent_context_packet") {
            let after = idx + "<subagent_context_packet".len();
            if let Some(rel_gt) = lower[after..].find('>') {
                out.replace_range(idx..after + rel_gt + 1, "");
            } else {
                out.replace_range(idx..out.len(), "");
            }
            continue;
        }
        if let Some(idx) = find_ascii_xml_tag_start(&lower, "</subagent_context_packet") {
            let after = idx + "</subagent_context_packet".len();
            if let Some(rel_gt) = lower[after..].find('>') {
                out.replace_range(idx..after + rel_gt + 1, "");
            } else {
                out.replace_range(idx..out.len(), "");
            }
            continue;
        }
        break;
    }

    loop {
        let lower = out.to_ascii_lowercase();
        if let Some(idx) = find_ascii_xml_tag_start(&lower, "<tool_call") {
            let after = idx + "<tool_call".len();
            if let Some(rel_close) = lower[after..].find("</tool_call>") {
                let close_end = after + rel_close + "</tool_call>".len();
                out.replace_range(idx..close_end, "");
            } else {
                out.replace_range(idx..out.len(), "");
            }
            continue;
        }
        if let Some(idx) = lower.find("</tool_call>") {
            out.replace_range(idx..idx + "</tool_call>".len(), "");
            continue;
        }
        if let Some(idx) = lower.find("<function=") {
            let after = idx + "<function=".len();
            if let Some(rel_close) = lower[after..].find("</function>") {
                let close_end = after + rel_close + "</function>".len();
                out.replace_range(idx..close_end, "");
            } else {
                out.replace_range(idx..out.len(), "");
            }
            continue;
        }
        if let Some(idx) = lower.find("</function>") {
            out.replace_range(idx..idx + "</function>".len(), "");
            continue;
        }
        break;
    }

    out
}

/// Repairs common lenient LLM JSON quirks (raw unescaped newlines/tabs inside string literals and
/// trailing commas before `}` or `]`) without altering valid JSON strings.
pub(crate) fn repair_lenient_json_text(raw: &str) -> String {
    let mut escaped = String::with_capacity(raw.len() + 16);
    let mut in_string = false;
    let mut prev_backslash = false;
    for ch in raw.chars() {
        if in_string {
            if prev_backslash {
                escaped.push(ch);
                prev_backslash = false;
            } else if ch == '\\' {
                escaped.push(ch);
                prev_backslash = true;
            } else if ch == '"' {
                escaped.push(ch);
                in_string = false;
            } else if ch == '\n' {
                escaped.push_str("\\n");
            } else if ch == '\r' {
                escaped.push_str("\\r");
            } else if ch == '\t' {
                escaped.push_str("\\t");
            } else {
                escaped.push(ch);
            }
        } else {
            if ch == '"' {
                in_string = true;
            }
            escaped.push(ch);
        }
    }

    // Remove trailing commas outside string literals (`..., }` or `..., ]`).
    let chars: Vec<char> = escaped.chars().collect();
    let mut cleaned = String::with_capacity(escaped.len());
    let mut str_mode = false;
    let mut bs = false;
    let mut i = 0usize;
    while i < chars.len() {
        let c = chars[i];
        if str_mode {
            if bs {
                bs = false;
            } else if c == '\\' {
                bs = true;
            } else if c == '"' {
                str_mode = false;
            }
            cleaned.push(c);
            i += 1;
            continue;
        }
        if c == '"' {
            str_mode = true;
            cleaned.push(c);
            i += 1;
            continue;
        }
        if c == ',' {
            let mut j = i + 1;
            while j < chars.len() && chars[j].is_whitespace() {
                j += 1;
            }
            if j < chars.len() && (chars[j] == '}' || chars[j] == ']') {
                i += 1;
                continue;
            }
        }
        cleaned.push(c);
        i += 1;
    }
    cleaned
}

impl CriticVerdict {
    pub fn pass(reason: impl Into<String>) -> Self {
        Self {
            status: "PASS".to_string(),
            passed: true,
            reason: reason.into(),
            violations: Vec::new(),
            fix_hint: String::new(),
        }
    }

    pub fn reject(violations: Vec<String>, fix_hint: impl Into<String>) -> Self {
        let hint = fix_hint.into();
        let reason = violations
            .first()
            .cloned()
            .unwrap_or_else(|| hint.clone());
        Self {
            status: "REJECT".to_string(),
            passed: false,
            reason,
            violations,
            fix_hint: hint,
        }
    }

    /// Parses a `CriticVerdict` JSON payload emitted by the Gatekeeper Critic subagent,
    /// stripping `<think>...</think>` blocks first and accepting either
    /// `{"passed": bool, "reason": "..."}` or `{"status": "PASS"|"REJECT", ...}`
    /// (with `"issues"` as fallback alias for `"violations"` and `"suggestion"` as fallback alias for `"fix_hint"`).
    pub fn parse_from_subagent_output(raw: &str) -> Option<Self> {
        let without_think = strip_think_blocks(raw);
        let trimmed = without_think.trim();
        let json_slice = if let (Some(start), Some(end)) = (trimmed.find('{'), trimmed.rfind('}')) {
            if start <= end {
                &trimmed[start..=end]
            } else {
                return None;
            }
        } else {
            return None;
        };
        let val: Value = serde_json::from_str(json_slice)
            .or_else(|_| serde_json::from_str(&repair_lenient_json_text(json_slice)))
            .ok()?;
        let has_passed_field = val.get("passed").and_then(|v| v.as_bool());
        let status_str = val.get("status").and_then(|v| v.as_str()).unwrap_or("");
        let passed = if let Some(p) = has_passed_field {
            p
        } else if !status_str.is_empty() {
            status_str.eq_ignore_ascii_case("PASS") || status_str.eq_ignore_ascii_case("OK")
        } else {
            return None;
        };
        let reason = val
            .get("reason")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .trim()
            .to_string();
        let extract_string_list = |v: &Value| -> Vec<String> {
            match v {
                Value::Array(arr) => arr
                    .iter()
                    .filter_map(|item| {
                        if let Some(s) = item.as_str() {
                            Some(s.trim().to_string())
                        } else if let Some(obj) = item.as_object() {
                            obj.get("message")
                                .or_else(|| obj.get("description"))
                                .or_else(|| obj.get("violation"))
                                .or_else(|| obj.get("issue"))
                                .and_then(|m| m.as_str())
                                .map(|s| s.trim().to_string())
                        } else {
                            None
                        }
                    })
                    .filter(|s| !s.is_empty())
                    .collect(),
                Value::String(single) if !single.trim().is_empty() => {
                    vec![single.trim().to_string()]
                }
                _ => Vec::new(),
            }
        };
        let mut violations: Vec<String> = val
            .get("violations")
            .map(extract_string_list)
            .filter(|v| !v.is_empty())
            .or_else(|| val.get("issues").map(extract_string_list))
            .unwrap_or_default();
        let fix_hint = val
            .get("fix_hint")
            .or_else(|| val.get("fixHint"))
            .or_else(|| val.get("suggestion"))
            .and_then(|v| v.as_str())
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| reason.clone());

        if passed {
            Some(Self::pass(if reason.is_empty() {
                "Critic passed".to_string()
            } else {
                reason
            }))
        } else {
            if violations.is_empty() {
                violations.push(if reason.is_empty() {
                    "Gatekeeper Critic rejected staged diff.".to_string()
                } else {
                    reason.clone()
                });
            }
            Some(Self {
                status: "REJECT".to_string(),
                passed: false,
                reason: if reason.is_empty() {
                    violations[0].clone()
                } else {
                    reason
                },
                violations,
                fix_hint,
            })
        }
    }
}

pub(super) fn default_max_critic_bounces() -> u8 {
    3
}

/// Dedicated Autonomous Loop Orchestrator configuration schema persisted in `.ide/loops.json`.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct LoopOrchestratorConfig {
    pub loop_id: String,
    pub name: String,
    pub instruction: String,
    #[serde(default)]
    pub review_rubric: String,
    #[serde(default)]
    pub static_gate_command: Option<String>,
    #[serde(default = "default_max_critic_bounces")]
    pub max_critic_bounces: u8,
    #[serde(default)]
    pub interval_secs: Option<u64>,
    #[serde(default)]
    pub idle_debounce_secs: Option<u64>,
    #[serde(default)]
    pub snapshot_refs: Vec<String>,
}

impl Default for LoopOrchestratorConfig {
    fn default() -> Self {
        Self {
            loop_id: "loop-default".to_string(),
            name: "Autonomous Self-Evaluation Loop".to_string(),
            instruction: String::new(),
            review_rubric: String::new(),
            static_gate_command: None,
            max_critic_bounces: default_max_critic_bounces(),
            interval_secs: None,
            idle_debounce_secs: None,
            snapshot_refs: Vec::new(),
        }
    }
}

/// Loads `.ide/loops.json` from `project_root` (supporting either a top-level array `[...]` or `{"loops": [...]}`).
pub fn load_loops_manifest(project_root: &Path) -> Vec<LoopOrchestratorConfig> {
    let manifest_path = project_root.join(".ide").join("loops.json");
    let Ok(raw) = fs::read_to_string(&manifest_path) else {
        return Vec::new();
    };
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return Vec::new();
    }
    if let Ok(list) = serde_json::from_str::<Vec<LoopOrchestratorConfig>>(trimmed) {
        return list;
    }
    if let Ok(val) = serde_json::from_str::<Value>(trimmed) {
        if let Some(arr) = val.get("loops") {
            if let Ok(list) = serde_json::from_value::<Vec<LoopOrchestratorConfig>>(arr.clone()) {
                return list;
            }
        }
    }
    Vec::new()
}

/// Saves `configs` to `<project_root>/.ide/loops.json`.
pub fn save_loops_manifest(
    project_root: &Path,
    configs: &[LoopOrchestratorConfig],
) -> Result<(), String> {
    let ide_dir = project_root.join(".ide");
    fs::create_dir_all(&ide_dir)
        .map_err(|e| format!("Failed to create {}: {e}", ide_dir.display()))?;
    let manifest_path = ide_dir.join("loops.json");
    let serialized = serde_json::to_string_pretty(configs)
        .map_err(|e| format!("Failed to serialize loops manifest: {e}"))?;
    fs::write(&manifest_path, serialized)
        .map_err(|e| format!("Failed to write {}: {e}", manifest_path.display()))
}

/// Creates a non-destructive Git loop snapshot in `project_root` using `git stash create`
/// and stores it under `refs/ide/loops/<loop_id>/step_<step_num>` via `git update-ref`.
///
/// Never alters `HEAD`, the Git index, or uncommitted human files in the working tree.
pub fn create_git_loop_snapshot(
    project_root: &Path,
    loop_id: &str,
    step_num: u64,
) -> Result<String, String> {
    let stash_out = Command::new("git")
        .args(["-c", "core.autocrlf=false"])
        .arg("-C")
        .arg(project_root)
        .arg("stash")
        .arg("create")
        .output()
        .map_err(|e| format!("Failed to execute `git stash create`: {e}"))?;

    if !stash_out.status.success() {
        let stderr = String::from_utf8_lossy(&stash_out.stderr);
        return Err(format!("`git stash create` failed: {}", stderr.trim()));
    }

    let mut sha = String::from_utf8_lossy(&stash_out.stdout).trim().to_string();
    if sha.is_empty() {
        // Working tree matches HEAD cleanly; capture current HEAD SHA
        let head_out = Command::new("git")
            .arg("-C")
            .arg(project_root)
            .arg("rev-parse")
            .arg("HEAD")
            .output()
            .map_err(|e| format!("Failed to execute `git rev-parse HEAD`: {e}"))?;
        if !head_out.status.success() {
            let stderr = String::from_utf8_lossy(&head_out.stderr);
            return Err(format!("`git rev-parse HEAD` failed: {}", stderr.trim()));
        }
        sha = String::from_utf8_lossy(&head_out.stdout).trim().to_string();
    }

    if sha.is_empty() {
        return Err("Failed to resolve non-empty commit SHA for loop snapshot".to_string());
    }

    let clean_loop_id: String = loop_id
        .trim()
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                c
            } else {
                '_'
            }
        })
        .collect();
    let ref_id = if clean_loop_id.is_empty() {
        "default"
    } else {
        clean_loop_id.as_str()
    };
    let ref_name = format!("refs/ide/loops/{ref_id}/step_{step_num}");

    let update_out = Command::new("git")
        .arg("-C")
        .arg(project_root)
        .arg("update-ref")
        .arg(&ref_name)
        .arg(&sha)
        .output()
        .map_err(|e| format!("Failed to execute `git update-ref`: {e}"))?;

    if !update_out.status.success() {
        let stderr = String::from_utf8_lossy(&update_out.stderr);
        return Err(format!(
            "`git update-ref {ref_name} {sha}` failed: {}",
            stderr.trim()
        ));
    }

    Ok(sha)
}

/// Restores tracked files in `project_root` from a non-destructive loop snapshot ref or SHA
/// via `git checkout <sha_or_ref> -- .` without destroying Git commit history.
pub fn rollback_to_git_loop_snapshot(
    project_root: &Path,
    sha_or_ref: &str,
) -> Result<String, String> {
    let target = sha_or_ref.trim();
    if target.is_empty() {
        return Err("Snapshot SHA or ref cannot be empty".to_string());
    }

    let checkout_out = Command::new("git")
        .args(["-c", "core.autocrlf=false"])
        .arg("-C")
        .arg(project_root)
        .arg("checkout")
        .arg(target)
        .arg("--")
        .arg(".")
        .output()
        .map_err(|e| format!("Failed to execute `git checkout {target} -- .`: {e}"))?;

    if !checkout_out.status.success() {
        let stderr = String::from_utf8_lossy(&checkout_out.stderr);
        return Err(format!(
            "`git checkout {target} -- .` failed: {}",
            stderr.trim()
        ));
    }

    Ok(format!("Restored working tree from snapshot '{target}'"))
}

impl SpellCoreSession {
    pub(super) fn format_coding_proposal_packet(
        resp: &SubagentCompletionResponse,
        files_read: usize,
        cached: &[SubagentCodingProposal],
    ) -> String {
        let finish_reason = resp.finish_reason.as_deref().unwrap_or("stop");
        let is_truncated = matches!(finish_reason, "length" | "max_tokens");
        let mut extra_attrs = String::new();
        let mut hint_lines = String::new();
        if let Some(primary) = cached.first() {
            let total_hunks: usize = cached.iter().map(|c| c.edits.len()).sum();
            extra_attrs.push_str(&format!(
                " proposal_id=\"{}\" path=\"{}\" hunks=\"{total_hunks}\"",
                primary.proposal_id, primary.path
            ));
            if cached.len() > 1 {
                let ids = cached
                    .iter()
                    .map(|c| c.proposal_id.as_str())
                    .collect::<Vec<_>>()
                    .join(",");
                extra_attrs.push_str(&format!(" proposal_ids=\"{ids}\""));
            }
            for prop in cached {
                hint_lines.push_str(&format!(
                    "[Cached proposal_id=\"{}\" ({} hunk(s) for `{}`): apply in ~15 tokens via edit_file({{\"proposal_id\": \"{}\"}}) or apply_subagent_proposal({{\"proposal_id\": \"{}\"}})]\n",
                    prop.proposal_id,
                    prop.edits.len(),
                    prop.path,
                    prop.proposal_id,
                    prop.proposal_id
                ));
            }
        }
        if is_truncated {
            extra_attrs.push_str(&format!(
                " truncated=\"true\" finish_reason=\"{finish_reason}\""
            ));
        }
        let trunc_warning = if is_truncated {
            "\n[WARNING: Subagent output hit max_tokens limit and was truncated; split target_files or narrow instruction before applying partial hunks.]"
        } else {
            ""
        };
        let model_label = if resp.model.trim().is_empty() {
            "subagent"
        } else {
            resp.model.trim()
        };
        format!(
            "<subagent_coding_proposal model=\"{model_label}\" files_read=\"{files_read}\" prefix_cache_reused=\"{}\" tok_s=\"{:.1}\"{extra_attrs}>\n{}{}{trunc_warning}\n</subagent_coding_proposal>",
            resp.prefix_cache_reused,
            resp.decode_tok_s,
            hint_lines,
            resp.content.trim()
        )
    }

    /// Strips a uniform numbered line gutter (`^\s*\d+\s*[:|]\s?`) from every non-empty line,
    /// returning `Some(stripped)` ONLY if every non-empty line matched the gutter pattern.
    pub(super) fn strip_uniform_numbered_gutter(block: &str) -> Option<String> {
        let mut stripped_lines = Vec::new();
        let mut saw_non_empty = false;
        for line in block.lines() {
            if line.trim().is_empty() {
                stripped_lines.push(String::new());
                continue;
            }
            let trimmed_start = line.trim_start_matches([' ', '\t']);
            let digit_len = trimmed_start
                .chars()
                .take_while(|c| c.is_ascii_digit())
                .count();
            if digit_len == 0 {
                return None;
            }
            let after_digits = &trimmed_start[digit_len..];
            let after_ws = after_digits.trim_start_matches([' ', '\t']);
            let rest = if let Some(r) = after_ws.strip_prefix('|').or_else(|| after_ws.strip_prefix(':')) {
                r.strip_prefix(' ').unwrap_or(r)
            } else {
                return None;
            };
            saw_non_empty = true;
            stripped_lines.push(rest.to_string());
        }
        if !saw_non_empty {
            return None;
        }
        let mut out = stripped_lines.join("\n");
        if block.ends_with('\n') {
            out.push('\n');
        }
        Some(out)
    }

    pub(super) fn strip_enclosing_code_fence(text: &str) -> String {
        let trimmed = text.trim_matches('\r');
        let lines: Vec<&str> = trimmed.lines().collect();
        if lines.len() >= 2
            && lines.first().map(|l| l.trim_start().starts_with("```")).unwrap_or(false)
            && lines.last().map(|l| l.trim() == "```").unwrap_or(false)
        {
            return lines[1..lines.len() - 1].join("\n");
        }
        text.trim_start_matches('\n').trim_end_matches('\n').to_string()
    }

    pub(super) fn parse_raw_proposal_candidates(body: &str) -> Vec<(Option<String>, String, String)> {
        let mut out: Vec<(Option<String>, String, String)> = Vec::new();

        // 1. Try JSON (fenced ```json ... ``` or outermost `{...}` / `[...]`) with lenient repair fallback
        let mut json_candidates: Vec<String> = Vec::new();
        let mut search_from = 0usize;
        while let Some(fence_rel) = body[search_from..].find("```") {
            let fence_start = search_from + fence_rel + 3;
            let Some(nl_rel) = body[fence_start..].find('\n') else {
                break;
            };
            let content_start = fence_start + nl_rel + 1;
            let Some(close_rel) = body[content_start..].find("```") else {
                break;
            };
            let block = body[content_start..content_start + close_rel].trim();
            if block.starts_with('{') || block.starts_with('[') {
                json_candidates.push(block.to_string());
            }
            search_from = content_start + close_rel + 3;
        }
        if let (Some(s), Some(e)) = (body.find('{'), body.rfind('}')) {
            if s < e {
                json_candidates.push(body[s..=e].to_string());
            }
        }
        if let (Some(s), Some(e)) = (body.find('['), body.rfind(']')) {
            if s < e {
                json_candidates.push(body[s..=e].to_string());
            }
        }

        let extract_from_json_val =
            |val: &Value, dst: &mut Vec<(Option<String>, String, String)>| {
                let push_edit_obj =
                    |obj: &serde_json::Map<String, Value>,
                     default_path: Option<&str>,
                     dst: &mut Vec<(Option<String>, String, String)>| {
                        let p = obj
                            .get("path")
                            .or_else(|| obj.get("file"))
                            .or_else(|| obj.get("target_file"))
                            .and_then(|v| v.as_str())
                            .or(default_path)
                            .map(|s| s.trim().to_string())
                            .filter(|s| !s.is_empty());
                        let old_t = obj
                            .get("old_text")
                            .or_else(|| obj.get("oldText"))
                            .or_else(|| obj.get("search"))
                            .and_then(|v| v.as_str());
                        let new_t = obj
                            .get("new_text")
                            .or_else(|| obj.get("newText"))
                            .or_else(|| obj.get("replace"))
                            .and_then(|v| v.as_str());
                        if let (Some(o), Some(n)) = (old_t, new_t) {
                            if !o.is_empty() {
                                dst.push((p, o.to_string(), n.to_string()));
                            }
                        }
                    };

                match val {
                    Value::Object(map) => {
                        let top_path = map
                            .get("path")
                            .or_else(|| map.get("file"))
                            .or_else(|| map.get("target_file"))
                            .and_then(|v| v.as_str());
                        if let Some(arr) = map
                            .get("edits")
                            .or_else(|| map.get("patches"))
                            .or_else(|| map.get("hunks"))
                            .and_then(|v| v.as_array())
                        {
                            for item in arr {
                                if let Some(obj) = item.as_object() {
                                    push_edit_obj(obj, top_path, dst);
                                }
                            }
                        } else if let Some(files_arr) =
                            map.get("files").and_then(|v| v.as_array())
                        {
                            for f_item in files_arr {
                                if let Some(f_obj) = f_item.as_object() {
                                    let f_path = f_obj
                                        .get("path")
                                        .or_else(|| f_obj.get("file"))
                                        .and_then(|v| v.as_str());
                                    if let Some(edits_arr) =
                                        f_obj.get("edits").and_then(|v| v.as_array())
                                    {
                                        for e_item in edits_arr {
                                            if let Some(e_obj) = e_item.as_object() {
                                                push_edit_obj(e_obj, f_path, dst);
                                            }
                                        }
                                    } else {
                                        push_edit_obj(f_obj, f_path, dst);
                                    }
                                }
                            }
                        } else {
                            push_edit_obj(map, top_path, dst);
                        }
                    }
                    Value::Array(arr) => {
                        for item in arr {
                            if let Some(obj) = item.as_object() {
                                push_edit_obj(obj, None, dst);
                            }
                        }
                    }
                    _ => {}
                }
            };

        for candidate in &json_candidates {
            if let Ok(val) = serde_json::from_str::<Value>(candidate)
                .or_else(|_| serde_json::from_str::<Value>(&repair_lenient_json_text(candidate)))
            {
                extract_from_json_val(&val, &mut out);
                if !out.is_empty() {
                    return out;
                }
            }
        }

        // 2. Try `<<<<<<< SEARCH ... ======= ... >>>>>>> REPLACE` blocks
        let lines: Vec<&str> = body.lines().collect();
        let mut i = 0usize;
        let mut active_file_hint: Option<String> = None;
        while i < lines.len() {
            let trimmed_line = lines[i].trim();
            if let Some(rest) = trimmed_line
                .strip_prefix("File:")
                .or_else(|| trimmed_line.strip_prefix("file:"))
                .or_else(|| trimmed_line.strip_prefix("path:"))
                .or_else(|| trimmed_line.strip_prefix("### "))
            {
                let clean = rest.trim().trim_matches('`').trim();
                if !clean.is_empty() && !clean.contains(' ') {
                    active_file_hint = Some(clean.to_string());
                }
            }
            if trimmed_line.starts_with("<<<<<<<") {
                let mut search_lines = Vec::new();
                i += 1;
                while i < lines.len() && !lines[i].trim().starts_with("=======") {
                    search_lines.push(lines[i]);
                    i += 1;
                }
                if i < lines.len() && lines[i].trim().starts_with("=======") {
                    i += 1;
                    let mut replace_lines = Vec::new();
                    while i < lines.len() && !lines[i].trim().starts_with(">>>>>>>") {
                        replace_lines.push(lines[i]);
                        i += 1;
                    }
                    if i < lines.len() && lines[i].trim().starts_with(">>>>>>>") {
                        let old_t = search_lines.join("\n");
                        let new_t = replace_lines.join("\n");
                        if !old_t.is_empty() {
                            out.push((active_file_hint.clone(), old_t, new_t));
                        }
                    }
                }
            }
            i += 1;
        }
        if !out.is_empty() {
            return out;
        }

        // 3. Try `old_text:` / `new_text:` blocks (plain or fenced)
        let mut idx = 0usize;
        let mut current_path: Option<String> = None;
        while idx < lines.len() {
            let t = lines[idx].trim();
            if let Some(p_rest) = t.strip_prefix("path:").or_else(|| t.strip_prefix("file:")) {
                let clean = p_rest.trim().trim_matches(['`', '"', '\'']).trim();
                if !clean.is_empty() {
                    current_path = Some(clean.to_string());
                }
            }
            let lower = t.to_ascii_lowercase();
            let is_old_header = lower == "old_text:"
                || lower == "**old_text**:"
                || lower == "`old_text`:"
                || lower.starts_with("old_text:");
            if is_old_header {
                let inline_old = t
                    .split_once(':')
                    .map(|(_, r)| r.trim())
                    .unwrap_or("");
                let mut old_buf: Vec<&str> = Vec::new();
                if !inline_old.is_empty() {
                    old_buf.push(inline_old);
                }
                idx += 1;
                while idx < lines.len() {
                    let lt = lines[idx].trim().to_ascii_lowercase();
                    if lt == "new_text:"
                        || lt == "**new_text**:"
                        || lt == "`new_text`:"
                        || lt.starts_with("new_text:")
                    {
                        break;
                    }
                    old_buf.push(lines[idx]);
                    idx += 1;
                }
                if idx < lines.len() {
                    let inline_new = lines[idx]
                        .trim()
                        .split_once(':')
                        .map(|(_, r)| r.trim())
                        .unwrap_or("");
                    let mut new_buf: Vec<&str> = Vec::new();
                    if !inline_new.is_empty() {
                        new_buf.push(inline_new);
                    }
                    idx += 1;
                    let mut in_fence = false;
                    while idx < lines.len() {
                        let raw_ln = lines[idx];
                        let trimmed_ln = raw_ln.trim();
                        let lt = trimmed_ln.to_ascii_lowercase();
                        if trimmed_ln.starts_with("```") {
                            new_buf.push(raw_ln);
                            if in_fence {
                                idx += 1;
                                break;
                            } else {
                                in_fence = true;
                                idx += 1;
                                continue;
                            }
                        }
                        if !in_fence
                            && (lt.starts_with("verification:")
                                || lt.starts_with("verification notes:")
                                || lt.starts_with("notes:")
                                || lt.starts_with("explanation:")
                                || lt == "old_text:"
                                || lt.starts_with("old_text:")
                                || lt.starts_with("path:")
                                || lt.starts_with("file:"))
                        {
                            break;
                        }
                        new_buf.push(raw_ln);
                        idx += 1;
                    }
                    let old_clean = Self::strip_enclosing_code_fence(&old_buf.join("\n"));
                    let new_clean = Self::strip_enclosing_code_fence(&new_buf.join("\n"));
                    if !old_clean.is_empty() {
                        out.push((current_path.clone(), old_clean, new_clean));
                    }
                    continue;
                }
            }
            idx += 1;
        }

        out
    }

    /// Parses `delegate_coding_subagent` output, normalizes numbered line gutters ONLY when raw
    /// `old_text` fails to match while gutter-stripped `old_text` matches uniquely (`count == 1`),
    /// caches up to 64 per-file [`SubagentCodingProposal`] entries (`prop-1`, `prop-2`, ...),
    /// and returns the newly cached proposals.
    pub(crate) fn extract_and_cache_subagent_proposals(
        &mut self,
        project_root: &Path,
        target_files: &[String],
        raw_content: &str,
    ) -> Vec<SubagentCodingProposal> {
        let stripped_body = strip_think_blocks(raw_content);
        let raw_candidates = Self::parse_raw_proposal_candidates(&stripped_body);
        if raw_candidates.is_empty() {
            return Vec::new();
        }

        let mut ordered_paths: Vec<String> = Vec::new();
        let mut grouped: HashMap<String, Vec<ProposalEditHunk>> = HashMap::new();

        for (opt_path, mut old_text, mut new_text) in raw_candidates {
            let resolved_rel_opt: Option<String> = if let Some(explicit) = opt_path {
                Some(explicit)
            } else if target_files.len() == 1 {
                Some(target_files[0].clone())
            } else {
                let mut matched_target: Option<String> = None;
                for tf in target_files {
                    if let Ok(res_p) = resolve_workspace_path(project_root, tf) {
                        if let Ok(file_text) = self
                            .snapshot_store
                            .read_file_text(&res_p)
                            .or_else(|_| {
                                fs::read_to_string(&res_p)
                                    .map_err(|e| crate::tools::ToolError::IoError(e.to_string()))
                            })
                        {
                            let raw_hit = file_text.matches(&old_text).count() == 1;
                            let gutter_hit = !raw_hit
                                && Self::strip_uniform_numbered_gutter(&old_text)
                                    .map(|g| file_text.matches(&g).count() == 1)
                                    .unwrap_or(false);
                            if raw_hit || gutter_hit {
                                matched_target = Some(tf.clone());
                                break;
                            }
                        }
                    }
                }
                matched_target.or_else(|| target_files.first().cloned())
            };

            let Some(rel_path) = resolved_rel_opt else {
                continue;
            };

            if let Ok(resolved) = resolve_workspace_path(project_root, &rel_path) {
                if let Ok(file_text) = self
                    .snapshot_store
                    .read_file_text(&resolved)
                    .or_else(|_| {
                        fs::read_to_string(&resolved)
                            .map_err(|e| crate::tools::ToolError::IoError(e.to_string()))
                    })
                {
                    if file_text.matches(&old_text).count() == 0 {
                        if let Some(stripped_old) = Self::strip_uniform_numbered_gutter(&old_text) {
                            if !stripped_old.is_empty()
                                && file_text.matches(&stripped_old).count() == 1
                            {
                                old_text = stripped_old;
                                if let Some(stripped_new) =
                                    Self::strip_uniform_numbered_gutter(&new_text)
                                {
                                    new_text = stripped_new;
                                }
                            }
                        }
                    }
                }
            }

            if !grouped.contains_key(&rel_path) {
                ordered_paths.push(rel_path.clone());
            }
            grouped
                .entry(rel_path)
                .or_default()
                .push(ProposalEditHunk { old_text, new_text });
        }

        let mut cached_out = Vec::with_capacity(ordered_paths.len());
        let created_at = now_unix_secs();
        for path in ordered_paths {
            let Some(edits) = grouped.remove(&path) else {
                continue;
            };
            if edits.is_empty() {
                continue;
            }
            self.next_proposal_seq = self.next_proposal_seq.saturating_add(1);
            let proposal = SubagentCodingProposal {
                proposal_id: format!("prop-{}", self.next_proposal_seq),
                path,
                edits,
                created_at,
            };
            while self.proposal_cache.len() >= MAX_CACHED_SUBAGENT_PROPOSALS {
                self.proposal_cache.pop_front();
            }
            self.proposal_cache.push_back(proposal.clone());
            cached_out.push(proposal);
        }
        cached_out
    }

    pub(super) fn select_proposal_hunks(
        edits: &[ProposalEditHunk],
        args: &Value,
    ) -> Result<Vec<ProposalEditHunk>, String> {
        if let Some(indices_arr) = args
            .get("hunk_indices")
            .or_else(|| args.get("hunkIndices"))
            .and_then(|v| v.as_array())
        {
            if indices_arr.is_empty() {
                return Err("INVALID_ARGUMENTS: hunk_indices cannot be empty.".to_string());
            }
            let mut selected = Vec::with_capacity(indices_arr.len());
            for val in indices_arr {
                let Some(idx_u64) = val.as_u64() else {
                    return Err("INVALID_ARGUMENTS: hunk_indices must contain non-negative integers.".to_string());
                };
                let idx = idx_u64 as usize;
                let Some(hunk) = edits.get(idx) else {
                    return Err(format!(
                        "INVALID_ARGUMENTS: hunk index {idx} out of bounds (proposal has {} hunk(s)).",
                        edits.len()
                    ));
                };
                selected.push(hunk.clone());
            }
            Ok(selected)
        } else {
            Ok(edits.to_vec())
        }
    }

    pub(super) fn apply_cached_proposal_edits(
        &mut self,
        thread_id: &str,
        project_root: &Path,
        proposal_id: &str,
        explicit_path: Option<&str>,
        args: &Value,
    ) -> (String, String, bool) {
        let Some(prop) = self
            .proposal_cache
            .iter()
            .rev()
            .find(|p| p.proposal_id == proposal_id)
            .cloned()
        else {
            return (
                format!(
                    "PROPOSAL_NOT_FOUND: Unknown or evicted proposal_id '{proposal_id}'. Re-run delegate_coding_subagent or pass explicit path + edits."
                ),
                "failed".to_string(),
                false,
            );
        };

        let target_raw_path = explicit_path.unwrap_or(&prop.path);
        let resolved = match resolve_workspace_path(project_root, target_raw_path) {
            Ok(r) => r,
            Err(e) => return (e.to_string(), "failed".to_string(), false),
        };
        if let Some(exp) = explicit_path {
            if let Ok(prop_resolved) = resolve_workspace_path(project_root, &prop.path) {
                if resolved != prop_resolved {
                    return (
                        format!(
                            "PATH_MISMATCH: proposal_id '{proposal_id}' targets '{}', but path '{}' was provided.",
                            prop.path, exp
                        ),
                        "failed".to_string(),
                        false,
                    );
                }
            }
        }
        if let Err(e) = enforce_research_cache_write_guard(&resolved) {
            return (e.to_string(), "denied".to_string(), false);
        }

        let selected_hunks = match Self::select_proposal_hunks(&prop.edits, args) {
            Ok(h) => h,
            Err(e) => return (e, "failed".to_string(), false),
        };

        let mut current = match self.snapshot_store.read_file_text(&resolved).or_else(|_| {
            fs::read_to_string(&resolved)
                .map_err(|e| crate::tools::ToolError::IoError(e.to_string()))
        }) {
            Ok(c) => c,
            Err(e) => {
                return (
                    format!("IO_ERROR: Failed to read {}: {e}", resolved.display()),
                    "failed".to_string(),
                    false,
                )
            }
        };

        let mut applied = 0usize;
        for hunk in &selected_hunks {
            if hunk.old_text.is_empty() {
                return (
                    format!("INVALID_EDIT: empty old_text in proposal '{proposal_id}'"),
                    "failed".to_string(),
                    false,
                );
            }
            let count = current.matches(&hunk.old_text).count();
            if count == 0 {
                return (
                    format!(
                        "NOT_FOUND: proposal '{proposal_id}' hunk old_text not found in {}",
                        resolved.display()
                    ),
                    "failed".to_string(),
                    false,
                );
            }
            if count > 1 {
                return (
                    format!(
                        "AMBIGUOUS_EDIT: proposal '{proposal_id}' hunk matched {count} times in {}",
                        resolved.display()
                    ),
                    "failed".to_string(),
                    false,
                );
            }
            current = current.replacen(&hunk.old_text, &hunk.new_text, 1);
            applied += 1;
        }

        if self.stage_write_mode || self.web_tainted_threads.contains(thread_id) {
            let op_label = format!(
                "Staged proposal {proposal_id} ({applied} surgical edit(s)) to {}",
                resolved.display()
            );
            match self.stage_mutation_with_live_critic(
                thread_id,
                &resolved,
                &prop.path,
                current,
                &op_label,
            ) {
                Ok(msg) => (msg, "completed".to_string(), false),
                Err(err_msg) => (err_msg, "failed".to_string(), false),
            }
        } else {
            if let Err(e) = fs::write(&resolved, &current) {
                return (
                    format!("IO_ERROR: Failed to write {}: {e}", resolved.display()),
                    "failed".to_string(),
                    false,
                );
            }
            self.turn_pin_cache.invalidate_path(&resolved);
            (
                format!(
                    "Applied proposal {proposal_id} ({applied} surgical edit(s)) to {}",
                    resolved.display()
                ),
                "completed".to_string(),
                false,
            )
        }
    }

    pub(super) fn resolve_active_critic_rubric(&self, thread_id: &str) -> String {
        let mut parts: Vec<String> = Vec::new();
        if let Some(th) = self.storage.get_thread(thread_id).ok().flatten() {
            for tag_name in &th.active_tags {
                if let Some(tag) = self.tag_registry.get_tag(tag_name) {
                    if !tag.rubric_prompt.trim().is_empty() {
                        parts.push(format!("[@tag:{}] {}", tag.name, tag.rubric_prompt.trim()));
                    }
                }
            }
        }
        for sid in &self.active_steering_ids {
            if let Some(dir) = self.tag_registry.get_steering(sid) {
                if !dir.system_append.trim().is_empty() {
                    parts.push(format!("[#steer:{}] {}", dir.trigger, dir.system_append.trim()));
                }
            }
        }
        if parts.is_empty() {
            "Verify syntax integrity, type and memory safety, bounds/error handling, security guardrails, and absence of unintended regressions.".to_string()
        } else {
            parts.join("\n")
        }
    }

    pub(super) fn paths_equivalent(a: &Path, b: &Path) -> bool {
        if a == b {
            return true;
        }
        match (a.canonicalize(), b.canonicalize()) {
            (Ok(ca), Ok(cb)) => ca == cb,
            _ => false,
        }
    }

    pub(super) fn find_active_staged_clone(
        &self,
        target: &Path,
    ) -> Option<(std::path::PathBuf, crate::tools::StagedFileMutation)> {
        if let Some(st) = self.snapshot_store.staged_overlay.get(target) {
            if st.status == "staged" || st.status == "verified" {
                return Some((target.to_path_buf(), st.clone()));
            }
        }
        for (k, st) in &self.snapshot_store.staged_overlay {
            if (st.status == "staged" || st.status == "verified")
                && (Self::paths_equivalent(k, target) || Self::paths_equivalent(&st.path, target))
            {
                return Some((k.clone(), st.clone()));
            }
        }
        None
    }

    pub(super) fn restore_prev_staged_overlay(
        &mut self,
        key: std::path::PathBuf,
        prev: crate::tools::StagedFileMutation,
    ) {
        let snap = crate::tools::FileSnapshot::from_content(
            prev.path.clone(),
            prev.staged_content.clone(),
        );
        self.snapshot_store.snapshots.insert(prev.path.clone(), snap);
        self.snapshot_store.staged_overlay.insert(key, prev);
    }

    /// Stages a live file mutation (`edit_file`, `write_file`, `apply_subagent_proposal`) and runs
    /// the Gatekeeper Critic when `self.critic_enabled` or
    /// `self.web_tainted_threads.contains(thread_id)` is true.
    pub fn stage_mutation_with_live_critic(
        &mut self,
        thread_id: &str,
        resolved: &Path,
        raw_path: &str,
        updated_content: String,
        op_label: &str,
    ) -> Result<String, String> {
        let critic_active =
            self.critic_enabled || self.web_tainted_threads.contains(thread_id);
        if !critic_active {
            self.snapshot_store
                .stage_write(resolved, raw_path, updated_content)
                .map_err(|e| e.to_string())?;
            self.turn_pin_cache.invalidate_path(resolved);
            return Ok(format!("{op_label} (pending Stage-Write commit)"));
        }

        let prev_staged = self.find_active_staged_clone(resolved);
        self.critic_review_seq = self.critic_review_seq.saturating_add(1);
        let review_seq = self.critic_review_seq;
        let rubric = self.resolve_active_critic_rubric(thread_id);
        let resolved_str = resolved.to_string_lossy();
        let file_errors: Vec<crate::tools::StructuredDiagnostic> = self
            .diagnostics_bus
            .all()
            .into_iter()
            .filter(|d| {
                d.severity.eq_ignore_ascii_case("error")
                    && (d.file == raw_path
                        || resolved_str.ends_with(&d.file)
                        || d.file == resolved_str)
            })
            .collect();

        let verdict = match self.evaluate_diff_with_gatekeeper_critic(
            thread_id,
            resolved,
            raw_path,
            updated_content,
            &rubric,
            &file_errors,
            review_seq,
        ) {
            Ok(v) => v,
            Err(err) => {
                if let Some((prev_key, prev_entry)) = prev_staged {
                    self.restore_prev_staged_overlay(prev_key, prev_entry);
                }
                self.turn_pin_cache.invalidate_path(resolved);
                return Err(err);
            }
        };

        self.turn_pin_cache.invalidate_path(resolved);
        if verdict.passed {
            Ok(format!(
                "{op_label} [Gatekeeper Critic: PASS — {}]",
                verdict.reason
            ))
        } else {
            if let Some((prev_key, prev_entry)) = prev_staged {
                self.restore_prev_staged_overlay(prev_key, prev_entry);
            }
            Err(format!(
                "[Gatekeeper Critic: REJECT (bounce {}/{}) — staged edit reverted]\nViolations: {}\nFix Hint: {}",
                self.critic_bounce_count,
                self.max_critic_bounces,
                verdict.violations.join("; "),
                verdict.fix_hint
            ))
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub fn evaluate_diff_with_gatekeeper_critic(
        &mut self,
        thread_id: &str,
        canonical_path: &Path,
        relative_path: &str,
        new_content: String,
        rubric: &str,
        static_errors: &[crate::tools::StructuredDiagnostic],
        critic_seq: u64,
    ) -> Result<CriticVerdict, String> {
        let prior_verified_baseline = self
            .find_active_staged_clone(canonical_path)
            .and_then(|(_, prev)| {
                if prev.status == "verified" && prev.staged_content != new_content {
                    Some(prev.staged_content)
                } else {
                    None
                }
            });

        let staged = self
            .snapshot_store
            .stage_write(canonical_path, relative_path, new_content.clone())
            .map_err(|e| e.to_string())?;

        let baseline_text = prior_verified_baseline
            .as_deref()
            .or(staged.original_content.as_deref())
            .unwrap_or("");
        let (critic_diff, _, _) = crate::tools::generate_unified_diff_with_context(
            &staged.relative_path,
            baseline_text,
            &new_content,
            12,
        );

        self.loop_state = LoopState::Reviewing;

        let critic_cache_key = crate::contract::resolve_slot_prompt_cache_key(
            thread_id,
            0,
            false,
            crate::contract::KvCacheSlot::CriticReviewer { seq: critic_seq },
        );

        let mut violations: Vec<String> = Vec::new();
        let mut hints: Vec<String> = Vec::new();

        // 1. Static compiler / gate errors
        for diag in static_errors {
            violations.push(format!(
                "{}:{}:{}: [{}] {}",
                diag.file, diag.line, diag.column, diag.severity, diag.message
            ));
        }
        if !static_errors.is_empty() {
            hints.push(
                "Resolve all static compiler/gate errors before re-staging the diff.".to_string(),
            );
        }

        // 2. Check if rubric itself is a JSON CriticVerdict payload
        let trimmed_rubric = rubric.trim();
        if trimmed_rubric.starts_with('{') {
            if let Ok(parsed_verdict) = serde_json::from_str::<CriticVerdict>(trimmed_rubric) {
                if parsed_verdict.status.eq_ignore_ascii_case("REJECT") {
                    if parsed_verdict.violations.is_empty() {
                        violations.push("Critic model rejected staged diff.".to_string());
                    } else {
                        violations.extend(parsed_verdict.violations);
                    }
                    if !parsed_verdict.fix_hint.is_empty() {
                        hints.push(parsed_verdict.fix_hint);
                    }
                }
            }
        }

        // 3. Semantic rubric inspection on added diff lines and candidate content
        let added_diff_lines: String = critic_diff
            .lines()
            .filter(|l| l.starts_with('+') && !l.starts_with("+++ "))
            .map(|l| &l[1..])
            .collect::<Vec<_>>()
            .join("\n");
        let code_under_review = if added_diff_lines.trim().is_empty() {
            new_content.as_str()
        } else {
            added_diff_lines.as_str()
        };

        let rubric_lower = trimmed_rubric.to_ascii_lowercase();
        if rubric_lower.contains("unwrap")
            && (code_under_review.contains(".unwrap()") || new_content.contains(".unwrap()"))
        {
            violations.push(format!(
                "Rubric violation in `{}`: forbidden `.unwrap()` detected under rubric \"{}\"",
                staged.relative_path, trimmed_rubric
            ));
            hints.push(
                "Replace `.unwrap()` with explicit error handling (e.g. `.ok_or(...)?` or `match`)."
                    .to_string(),
            );
        }
        if (rubric_lower.contains("todo!") || rubric_lower.contains("unimplemented!"))
            && (code_under_review.contains("todo!(")
                || code_under_review.contains("unimplemented!("))
        {
            violations.push(format!(
                "Rubric violation in `{}`: forbidden placeholder macro (`todo!`/`unimplemented!`)",
                staged.relative_path
            ));
            hints.push(
                "Provide a complete implementation instead of `todo!()` or `unimplemented!()`."
                    .to_string(),
            );
        }
        if rubric_lower.contains("panic!") && code_under_review.contains("panic!(") {
            violations.push(format!(
                "Rubric violation in `{}`: forbidden `panic!()` detected",
                staged.relative_path
            ));
            hints.push("Return a `Result::Err` rather than panicking.".to_string());
        }

        // 4. Optional LLM-backed Gatekeeper Critic using the configured subagent model.
        // Strictly gated on `critic_enabled && endpoint/transport ready && non-empty model`.
        // Otherwise degrades cleanly to deterministic pre-checks with zero extra network calls.
        let mut live_reason = String::new();
        let sub_model = self.effective_subagent_model_id();
        let llm_critic_ready = self.critic_enabled
            && (self.uses_custom_transport || self.is_endpoint_configured())
            && !sub_model.is_empty();
        if violations.is_empty() && llm_critic_ready && !trimmed_rubric.starts_with('{') {
            let fallback_epoch = self
                .storage
                .get_thread(thread_id)
                .ok()
                .flatten()
                .map(|t| t.engine_compaction_epoch)
                .unwrap_or(0);
            let diff_excerpt: String = critic_diff.chars().take(24_000).collect();
            let file_header_prefix = if !critic_diff.contains("@@ -1,")
                && !critic_diff.contains("@@ -0,")
                && !new_content.is_empty()
            {
                let header_lines: Vec<&str> = new_content.lines().take(40).collect();
                let hdr_count = header_lines.len();
                format!(
                    "<file_header lines=\"1-{hdr_count}\">\n{}\n</file_header>\n",
                    header_lines.join("\n")
                )
            } else {
                String::new()
            };
            let critic_role_directive = "You are a Spell IDE Gatekeeper Critic subagent. \
                Evaluate the staged unified diff against <rubric> for security, credential leaks, \
                syntax integrity, and rubric violations. Reply ONLY with strict JSON: \
                {\"passed\": true|false, \"reason\": \"...\"}.";
            let critic_payload = format!(
                "<file>{}</file>\n<rubric>{}</rubric>\n{}<unified_diff>\n{}\n</unified_diff>",
                staged.relative_path, trimmed_rubric, file_header_prefix, diff_excerpt
            );
            let (parent_cache_key, aligned_messages, aligned_tools) = self
                .build_critic_subagent_prompt(
                    thread_id,
                    fallback_epoch,
                    critic_role_directive,
                    critic_role_directive,
                    &critic_payload,
                );
            let critic_req = SubagentCompletionRequest {
                model: sub_model,
                messages: aligned_messages,
                stream: false,
                max_tokens: Some(768),
                temperature: Some(0.0),
                prompt_cache_key: Some(critic_cache_key),
                parent_cache_key: Some(parent_cache_key),
                reasoning_effort: None,
                thinking_budget: None,
                tools: aligned_tools,
                enable_thinking: Some(false),
                ..Default::default()
            };

            match self.active_transport().run_subagent_completion(&critic_req) {
                Ok(resp) => {
                    self.record_live_subagent_completion(&resp);
                    if matches!(resp.finish_reason.as_deref(), Some("length" | "max_tokens")) {
                        violations.push(
                            "Gatekeeper Critic output truncated at max_tokens=768 (fail-closed)"
                                .to_string(),
                        );
                        hints.push(
                            "Break the edit into a smaller surgical hunk so Gatekeeper Critic review completes within token budget."
                                .to_string(),
                        );
                    } else {
                        match CriticVerdict::parse_from_subagent_output(&resp.content) {
                            Some(sub_verdict) if !sub_verdict.passed => {
                                if !sub_verdict.violations.is_empty() {
                                    violations.extend(sub_verdict.violations);
                                } else if sub_verdict.reason.trim().is_empty() {
                                    violations.push(
                                        "Gatekeeper Critic rejected staged diff."
                                            .to_string(),
                                    );
                                } else {
                                    violations.push(format!(
                                        "Gatekeeper Critic rejected: {}",
                                        sub_verdict.reason.trim()
                                    ));
                                }
                                let hint_text = if !sub_verdict.fix_hint.trim().is_empty() {
                                    sub_verdict.fix_hint.trim().to_string()
                                } else {
                                    sub_verdict.reason.trim().to_string()
                                };
                                if !hint_text.is_empty() {
                                    hints.push(hint_text);
                                }
                            }
                            Some(sub_verdict) => {
                                live_reason = if sub_verdict.reason.trim().is_empty() {
                                    "Gatekeeper Critic: passed".to_string()
                                } else if sub_verdict.reason.trim().starts_with("Gatekeeper Critic:") {
                                    sub_verdict.reason.trim().to_string()
                                } else {
                                    format!("Gatekeeper Critic: {}", sub_verdict.reason.trim())
                                };
                            }
                            None => {
                                violations.push(
                                    "Gatekeeper Critic returned unparseable or missing JSON verdict (fail-closed)"
                                        .to_string(),
                                );
                                hints.push(
                                    "Retry staging or inspect subagent output formatting."
                                        .to_string(),
                                );
                            }
                        }
                    }
                }
                Err(TransportError::ModelRoleViolation(msg)) => {
                    let _ = self.snapshot_store.rollback_staged(Some(canonical_path));
                    return Err(format!("ModelRoleViolation: {msg}"));
                }
                Err(err) if err.is_subagent_unavailable() => {
                    live_reason = "deterministic_precheck_pass".to_string();
                }
                Err(err) => {
                    let _ = self.snapshot_store.rollback_staged(Some(canonical_path));
                    return Err(err.to_string());
                }
            }
        } else if violations.is_empty() && !trimmed_rubric.starts_with('{') {
            live_reason = "deterministic_precheck_pass".to_string();
        }

        let verdict = if !violations.is_empty() {
            CriticVerdict::reject(violations, hints.join(" "))
        } else {
            CriticVerdict::pass(if live_reason.is_empty() {
                "Deterministic Gatekeeper precheck passed.".to_string()
            } else {
                live_reason
            })
        };

        if verdict.status == "REJECT" {
            self.critic_bounce_count = self.critic_bounce_count.saturating_add(1);
            let _ = self
                .snapshot_store
                .rollback_staged(Some(canonical_path))
                .map_err(|e| e.to_string())?;
            self.last_critic_verdict = Some(verdict.clone());

            if self.critic_bounce_count > self.max_critic_bounces {
                self.loop_state = LoopState::PausedOnCriticReject;

                let target_tid = if !thread_id.trim().is_empty() {
                    Some(thread_id.trim().to_string())
                } else {
                    self.selected_thread_id.clone()
                };
                if let Some(tid) = target_tid {
                    let thread_opt = self.storage.get_thread(&tid).ok().flatten().or_else(|| {
                        self.selected_thread_id
                            .as_deref()
                            .and_then(|sel| self.storage.get_thread(sel).ok().flatten())
                    });
                    if let Some(mut th) = thread_opt {
                        let notice = format!(
                            "[GATEKEEPER CRITIC CIRCUIT BREAKER — PAUSED ON CRITIC REJECT] \
                             Bounce #{} exceeded max_critic_bounces ({}) on `{}`. Violations: {}. Fix Hint: {}",
                            self.critic_bounce_count,
                            self.max_critic_bounces,
                            staged.relative_path,
                            verdict.violations.join("; "),
                            verdict.fix_hint
                        );
                        th.ui_events
                            .push(UiChatEvent::assistant_message(notice, None, None));
                        th.updated_at = now_unix_secs();
                        let _ = self.storage.upsert_thread(&th);
                    }
                }
            } else {
                self.loop_state = LoopState::Active;
            }
        } else {
            self.critic_bounce_count = 0;
            let _ = self.snapshot_store.mark_staged_verified(canonical_path);
            self.last_critic_verdict = Some(verdict.clone());
            self.loop_state = LoopState::Active;
        }

        Ok(verdict)
    }

    /// Controls the autonomous loop state machine (`"pause"`, `"resume"`, `"start"`, `"step"`, `"abort"`, `"rollback_git"`).
    pub fn control_autonomous_loop(&mut self, action: &str) -> Result<LoopState, String> {
        let normalized = action.trim().to_ascii_lowercase();
        match normalized.as_str() {
            "pause" => {
                self.loop_state = LoopState::Paused;
                Ok(self.loop_state)
            }
            "resume" | "start" => {
                self.loop_state = LoopState::Active;
                Ok(self.loop_state)
            }
            "step" => {
                self.loop_cycle_count += 1;
                self.loop_state = LoopState::Paused;
                Ok(self.loop_state)
            }
            "abort" => {
                self.loop_state = LoopState::Aborted;
                let _ = self.cancel_active_turn();
                Ok(self.loop_state)
            }
            "rollback_git" => {
                let project_root = self
                    .selected_project_id
                    .as_deref()
                    .and_then(|pid| self.storage.get_project(pid).ok().flatten())
                    .map(|p| std::path::PathBuf::from(p.root_path))
                    .unwrap_or_else(|| self.locator.primary_workspace_root());
                let _ = self.snapshot_store.rollback_staged(None);
                if let Some(ref sha) = self.last_git_snapshot_sha.clone() {
                    let _ = rollback_to_git_loop_snapshot(&project_root, sha);
                }
                self.loop_state = LoopState::Paused;
                Ok(self.loop_state)
            }
            other => Err(format!("Unknown autonomous loop control action '{other}'")),
        }
    }

    /// Runs an unrestricted autonomous self-evaluation loop without the legacy 8-step cap,
    /// iterating up to `max_safety_limit` cycles until `step_fn` returns `Ok(true)` (`<GOAL_REACHED>`)
    /// or `self.loop_state` transitions to `Paused`, `PausedOnCriticReject`, `Aborted`, or `Completed`.
    pub fn run_unrestricted_self_eval_loop<F>(
        &mut self,
        thread_id: &str,
        config: &LoopOrchestratorConfig,
        max_safety_limit: usize,
        mut step_fn: F,
    ) -> Result<u64, String>
    where
        F: FnMut(u64, &mut Self) -> Result<bool, String>,
    {
        if !thread_id.trim().is_empty() {
            self.selected_thread_id = Some(thread_id.trim().to_string());
        }
        self.max_critic_bounces = config.max_critic_bounces;
        self.loop_state = LoopState::Active;

        for _ in 0..max_safety_limit {
            if matches!(
                self.loop_state,
                LoopState::Paused
                    | LoopState::PausedOnCriticReject
                    | LoopState::Aborted
                    | LoopState::Completed
            ) {
                break;
            }

            self.loop_cycle_count += 1;
            let goal_reached = step_fn(self.loop_cycle_count, self)?;
            if goal_reached {
                self.loop_state = LoopState::Completed;
                break;
            }

            if matches!(
                self.loop_state,
                LoopState::Paused
                    | LoopState::PausedOnCriticReject
                    | LoopState::Aborted
                    | LoopState::Completed
            ) {
                break;
            }
        }

        Ok(self.loop_cycle_count)
    }

    // ========================================================================
    // 3. Snapshot Serialization (`build_full_state_snapshot_json`) & FFI Router
}
