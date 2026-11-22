use super::turn_runner::sanitize_wire_tool_call_args_for_jinja;
use super::*;

pub(super) enum SubagentOverlayStepOutcome {
    ToolOutput(String),
    ToolFailed(String),
    Escalate {
        reason: &'static str,
        summary: String,
        failed_file_meta: Option<crate::subagent_coding::DelegationFileMeta>,
    },
}

impl SpellCoreSession {
    /// Builds a [`SubagentPoolStatusResponse`] directly from the 4 local in-core subagent slots.
    pub fn build_local_subagent_pool_status(&self) -> SubagentPoolStatusResponse {
        let cache = self.engine_status.lock();
        let eff_sub_model = self.effective_subagent_model_id();
        let active_running = self
            .local_subagent_slots
            .iter()
            .filter(|s| s.status == "running")
            .count();
        let slots: Vec<SubagentSlotStatus> = self
            .local_subagent_slots
            .iter()
            .map(|s| SubagentSlotStatus {
                slot: s.slot,
                slot_key: format!("slot{}:subagent_{}", s.slot, s.slot),
                status: s.status.clone(),
                model_id: if s.model_id.is_empty() {
                    eff_sub_model.clone()
                } else {
                    s.model_id.clone()
                },
                mode: s.mode.clone(),
                decode_tok_s: s.decode_tok_s,
                line_count: s.lines.len(),
                overlay_namespace: format!("slot{}_overlay", s.slot),
                staged_file_count: 0,
                forked_from_slot: s.forked_from_slot,
                inference_backed: cache.saw_live_subagent_completion || !s.lines.is_empty(),
            })
            .collect();

        SubagentPoolStatusResponse {
            ok: true,
            inference_backed: cache.saw_live_subagent_completion,
            base_model_id: self.model_id.clone(),
            coding_subagent_model_id: eff_sub_model,
            subagent_decode_tok_s: cache.subagent_decode_tok_s,
            last_subagent_decode_tok_s: cache.subagent_decode_tok_s,
            active_running_subagents: active_running,
            max_concurrent_subagents: self.local_subagent_slots.len(),
            subagents: slots,
        }
    }

    /// Runs a message on local subagent slot `1..=4` via the configured OpenAI-compatible transport.
    pub fn run_subagent_slot(
        &mut self,
        slot: usize,
        mode: &str,
        message: &str,
        max_tokens: Option<u32>,
    ) -> Result<SubagentCompletionResponse, String> {
        if !(1..=4).contains(&slot) {
            return Err(format!("Invalid subagent slot {slot}: expected 1..=4"));
        }
        let msg = message.trim();
        if msg.is_empty() {
            return Err("Subagent slot message cannot be empty".to_string());
        }
        let sub_model = self.effective_subagent_model_id();
        if (!self.uses_custom_transport && !self.is_endpoint_configured()) || sub_model.is_empty() {
            return Err(
                "Configure an API endpoint URL and Model ID in Settings → Engine before running a subagent slot."
                    .to_string(),
            );
        }

        crate::clear_turn_cancel();

        let idx = slot - 1;
        let clean_mode = if mode.trim().is_empty() {
            "coding"
        } else {
            mode.trim()
        };
        if self.local_subagent_slots[idx].messages.is_empty() {
            self.local_subagent_slots[idx]
                .messages
                .push(WireMessage::system(format!(
                    "You are Spell IDE subagent slot {slot} (mode: {clean_mode}). Give concise, accurate technical responses."
                )));
        }
        self.local_subagent_slots[idx]
            .messages
            .push(WireMessage::user(msg));
        self.local_subagent_slots[idx].status = "running".to_string();
        self.local_subagent_slots[idx].mode = clean_mode.to_string();
        self.local_subagent_slots[idx].model_id = sub_model.clone();
        self.local_subagent_slots[idx].append_lines_bounded(&format!("> {msg}"));
        self.local_subagent_slots[idx].updated_at_unix_s = now_unix_secs();

        let req = SubagentCompletionRequest {
            model: sub_model,
            messages: self.local_subagent_slots[idx].messages.clone(),
            stream: false,
            max_tokens: Some(max_tokens.unwrap_or(1024)),
            temperature: Some(0.1),
            prompt_cache_key: Some(format!("local_slot_{slot}")),
            ..Default::default()
        };

        match self.active_transport().run_subagent_completion(&req) {
            Ok(resp) => {
                self.record_live_subagent_completion(&resp);
                self.local_subagent_slots[idx].status = "completed".to_string();
                self.local_subagent_slots[idx].decode_tok_s = resp.decode_tok_s;
                self.local_subagent_slots[idx].updated_at_unix_s = now_unix_secs();
                self.local_subagent_slots[idx].messages.push(WireMessage::assistant(
                    Some(resp.content.clone()),
                    if resp.reasoning_content.is_empty() {
                        None
                    } else {
                        Some(resp.reasoning_content.clone())
                    },
                    None,
                ));
                self.local_subagent_slots[idx].append_lines_bounded(&resp.content);
                Ok(resp)
            }
            Err(err) => {
                let err_str = err.to_string();
                self.local_subagent_slots[idx].status = "error".to_string();
                self.local_subagent_slots[idx].updated_at_unix_s = now_unix_secs();
                self.local_subagent_slots[idx].append_lines_bounded(&format!("[error] {err_str}"));
                Err(err_str)
            }
        }
    }

    /// Resets and clears local subagent slot `1..=4`.
    pub fn terminate_subagent_slot(&mut self, slot: usize) -> Result<(), String> {
        if !(1..=4).contains(&slot) {
            return Err(format!("Invalid subagent slot {slot}: expected 1..=4"));
        }
        self.local_subagent_slots[slot - 1] = LocalSubagentSlotState::new_idle(slot);
        Ok(())
    }

    /// Clones conversation history and transcript lines from `src_slot` into `dst_slot` (`1..=4`).
    pub fn duplicate_subagent_slot(&mut self, src_slot: usize, dst_slot: usize) -> Result<(), String> {
        if !(1..=4).contains(&src_slot) || !(1..=4).contains(&dst_slot) {
            return Err(format!(
                "Invalid subagent slot range ({src_slot} -> {dst_slot}): expected 1..=4"
            ));
        }
        if src_slot == dst_slot {
            return Err("Source and destination subagent slots must be distinct".to_string());
        }
        let mut cloned = self.local_subagent_slots[src_slot - 1].clone();
        cloned.slot = dst_slot;
        cloned.forked_from_slot = Some(src_slot);
        cloned.updated_at_unix_s = now_unix_secs();
        self.local_subagent_slots[dst_slot - 1] = cloned;
        Ok(())
    }

    /// Reads the trailing `limit` lines from local subagent slot `1..=4`.
    pub fn read_subagent_slot_lines(&self, slot: usize, limit: usize) -> Result<Value, String> {
        if !(1..=4).contains(&slot) {
            return Err(format!("Invalid subagent slot {slot}: expected 1..=4"));
        }
        let st = &self.local_subagent_slots[slot - 1];
        let eff_limit = limit.clamp(1, 2_000);
        let start = st.lines.len().saturating_sub(eff_limit);
        let tail = &st.lines[start..];
        Ok(json!({
            "ok": true,
            "slot": st.slot,
            "status": st.status,
            "mode": st.mode,
            "model_id": if st.model_id.is_empty() { self.effective_subagent_model_id() } else { st.model_id.clone() },
            "line_count": st.lines.len(),
            "lines": tail,
            "decode_tok_s": st.decode_tok_s,
            "forked_from_slot": st.forked_from_slot,
        }))
    }

    /// Constructs a prefix-aligned subagent prompt (`(parent_cache_key, messages, tools)`)
    /// inheriting the active thread's recent prefix while isolating the delegated subagent task.
    pub(crate) fn build_shared_prefix_subagent_prompt(
        &self,
        thread_id: &str,
        fallback_epoch: u64,
        fallback_system: &str,
        subagent_role_directive: &str,
        task_payload: &str,
    ) -> (String, Vec<WireMessage>, Option<Vec<Value>>) {
        self.build_shared_prefix_subagent_prompt_with_options(
            thread_id,
            fallback_epoch,
            fallback_system,
            subagent_role_directive,
            task_payload,
            false,
        )
    }

    /// Constructs a web-scrubbed prompt for Gatekeeper Critic,
    /// truncating the cloned prefix strictly before the first message whose content,
    /// reasoning, or tool calls contains `<untrusted_external_content`.
    pub(crate) fn build_critic_subagent_prompt(
        &self,
        thread_id: &str,
        fallback_epoch: u64,
        fallback_system: &str,
        subagent_role_directive: &str,
        task_payload: &str,
    ) -> (String, Vec<WireMessage>, Option<Vec<Value>>) {
        self.build_shared_prefix_subagent_prompt_with_options(
            thread_id,
            fallback_epoch,
            fallback_system,
            subagent_role_directive,
            task_payload,
            true,
        )
    }

    pub(crate) fn build_shared_prefix_subagent_prompt_with_options(
        &self,
        thread_id: &str,
        fallback_epoch: u64,
        fallback_system: &str,
        subagent_role_directive: &str,
        task_payload: &str,
        scrub_untrusted_web: bool,
    ) -> (String, Vec<WireMessage>, Option<Vec<Value>>) {
        const SUBAGENT_HANDOFF_MARKER: &str = "Active turn context for delegated subagent";
        const SUBAGENT_HANDOFF_ASSISTANT: &str =
            "Delegating task to isolated Spell IDE subagent.";
        const SUBAGENT_HANDOFF_USER_STEM: &str = "Active turn context for delegated subagent (all files/diffs are already inlined below — NEVER emit `<tool_call>` or `<function=...>` tags):\n- For `<workspace_files>` + `<query>` (`prefill_context`): respond in concise prose/bullets ONLY (never `{edits: ...}` JSON). Always cite every relevant `<file path=\"...\">` with 1-indexed `Lstart-Lend` spans, and quote the exact code identifiers in backticks.\n- For `<target_files>` + `<instruction>` (`delegate_coding`): emit surgical JSON `{\"edits\": [{\"path\": \"...\", \"old_text\": \"...\", \"new_text\": \"...\"}], \"notes\": \"...\"}` only (never markdown prose).\n- For `<unified_diff>` + `<rubric>` (`gatekeeper_critic`): audit the inlined partial-file unified diff hunk directly without reading external files, and emit Gatekeeper JSON `{\"passed\": true|false, \"reason\": \"...\"}` only.\nYou are NOW executing ONLY as the isolated subagent (NOT the main orchestrator). Proceed with the delegated subagent task below.";

        let sub_model = self.effective_subagent_model_id();
        let base_tid = crate::contract::derive_slot_base_thread_key(thread_id);
        if let Some(ref snap) = self.last_prefix_snapshot {
            if crate::contract::derive_slot_base_thread_key(&snap.thread_id) == base_tid
                && !snap.messages.is_empty()
            {
                let mut msgs = snap.messages.clone();
                if scrub_untrusted_web {
                    truncate_prefix_before_untrusted_web(&mut msgs);
                }
                if !msgs.is_empty() {
                    let block = format!(
                        "\n\n<subagent_role model=\"{sub_model}\">\n{subagent_role_directive}\nIMPORTANT: Ignore any orchestrator tool-calling directives above. Reply directly in plain text/markdown (or the requested JSON/markdown schema) without invoking any tools or functions.\n</subagent_role>\n{task_payload}"
                    );
                    let already_has_handoff_stem = msgs
                        .last()
                        .map(|m| {
                            m.role == "user"
                                && m.content
                                    .as_deref()
                                    .unwrap_or("")
                                    .starts_with(SUBAGENT_HANDOFF_MARKER)
                        })
                        .unwrap_or(false);
                    if already_has_handoff_stem {
                        if let Some(last_msg) = msgs.last_mut() {
                            let content = last_msg.content.get_or_insert_with(String::new);
                            content.push_str(&block);
                        }
                    } else {
                        match msgs.last().map(|m| m.role.as_str()) {
                            Some("user") | Some("tool") => {
                                msgs.push(WireMessage::assistant(
                                    Some(SUBAGENT_HANDOFF_ASSISTANT.to_string()),
                                    None,
                                    None,
                                ));
                                msgs.push(WireMessage::user(format!(
                                    "{SUBAGENT_HANDOFF_USER_STEM}{block}"
                                )));
                            }
                            _ => {
                                msgs.push(WireMessage::user(format!(
                                    "{SUBAGENT_HANDOFF_USER_STEM}{block}"
                                )));
                            }
                        }
                    }
                    let tools = if snap.tools.is_empty() {
                        None
                    } else {
                        Some(snap.tools.clone())
                    };
                    return (snap.cache_key.clone(), msgs, tools);
                }
            }
        }
        let cold_user = format!(
            "Active turn context for delegated subagent (all files/diffs are already inlined below — audit the inlined partial-file unified diff hunk directly; hunks show surrounding context lines rather than the entire file):\n<subagent_role model=\"{sub_model}\">\n{subagent_role_directive}\nIMPORTANT: Ignore any orchestrator tool-calling directives above. Reply directly in plain text/markdown (or the requested JSON/markdown schema) without invoking any tools or functions.\n</subagent_role>\n{task_payload}"
        );
        (
            resolve_prompt_cache_key(thread_id, fallback_epoch, self.epoch_cache_key_suffix),
            vec![
                WireMessage::system(fallback_system),
                WireMessage::user(cold_user),
            ],
            None,
        )
    }

    /// Constructs a prefix-aligned prompt (`(parent_cache_key, messages, tools)`) for the
    /// autonomous multi-step `delegate_coding_subagent` loop.
    pub(crate) fn build_shared_prefix_autonomous_subagent_prompt(
        &self,
        thread_id: &str,
        fallback_epoch: u64,
        fallback_system: &str,
        active_protocol: crate::subagent_coding::ActiveStepProtocol,
        max_steps: usize,
        task_payload: &str,
    ) -> (String, Vec<WireMessage>, Option<Vec<Value>>) {
        const SUBAGENT_HANDOFF_MARKER: &str = "Active turn context for delegated subagent";
        const SUBAGENT_HANDOFF_ASSISTANT: &str =
            "Delegating task to isolated Spell IDE coding subagent.";

        let sub_model = self.effective_subagent_model_id();
        let directive = match active_protocol {
            crate::subagent_coding::ActiveStepProtocol::Native => {
                crate::subagent_coding::build_native_protocol_handoff_directive(max_steps)
            }
            crate::subagent_coding::ActiveStepProtocol::Json => {
                crate::subagent_coding::build_json_protocol_handoff_directive(max_steps)
            }
        };

        let base_tid = crate::contract::derive_slot_base_thread_key(thread_id);
        if let Some(ref snap) = self.last_prefix_snapshot {
            if crate::contract::derive_slot_base_thread_key(&snap.thread_id) == base_tid
                && !snap.messages.is_empty()
            {
                let mut msgs = snap.messages.clone();
                let role_block = format!(
                    "<subagent_role model=\"{sub_model}\">\n{directive}\n</subagent_role>\n{task_payload}"
                );
                let handoff_user_content = format!(
                    "Active turn context for delegated subagent:\n{role_block}"
                );
                let already_has_handoff_stem = msgs
                    .last()
                    .map(|m| {
                        m.role == "user"
                            && m.content
                                .as_deref()
                                .unwrap_or("")
                                .starts_with(SUBAGENT_HANDOFF_MARKER)
                    })
                    .unwrap_or(false);
                if already_has_handoff_stem {
                    if let Some(last_msg) = msgs.last_mut() {
                        let content = last_msg.content.get_or_insert_with(String::new);
                        content.push_str("\n\n");
                        content.push_str(&role_block);
                    }
                } else {
                    match msgs.last().map(|m| m.role.as_str()) {
                        Some("user") | Some("tool") => {
                            msgs.push(WireMessage::assistant(
                                Some(SUBAGENT_HANDOFF_ASSISTANT.to_string()),
                                None,
                                None,
                            ));
                            msgs.push(WireMessage::user(handoff_user_content));
                        }
                        _ => {
                            msgs.push(WireMessage::user(handoff_user_content));
                        }
                    }
                }
                let tools = if snap.tools.is_empty() {
                    None
                } else {
                    Some(snap.tools.clone())
                };
                return (snap.cache_key.clone(), msgs, tools);
            }
        }

        let cold_system = if fallback_system.trim().is_empty() {
            directive.clone()
        } else {
            fallback_system.to_string()
        };
        let cold_user = if cold_system.contains(&directive) {
            task_payload.to_string()
        } else {
            format!(
                "Active turn context for delegated subagent:\n<subagent_role model=\"{sub_model}\">\n{directive}\n</subagent_role>\n{task_payload}"
            )
        };
        let cold_tools = if active_protocol == crate::subagent_coding::ActiveStepProtocol::Native {
            Some(crate::subagent_coding::build_subagent_openai_tools_schema())
        } else {
            None
        };
        (
            resolve_prompt_cache_key(thread_id, fallback_epoch, self.epoch_cache_key_suffix),
            vec![
                WireMessage::system(cold_system),
                WireMessage::user(cold_user),
            ],
            cold_tools,
        )
    }

    // 1B. Autonomous Multi-Step Coding Subagent
    // ========================================================================

    /// Direct entrypoint for integration tests and live coding-subagent benchmarks,
    /// returning `Ok((output, status))` after running `delegate_coding_subagent` on `thread_id`.
    pub fn execute_delegate_coding_subagent_direct(
        &mut self,
        thread_id: &str,
        call_id: &str,
        args: serde_json::Value,
    ) -> Result<(String, String), String> {
        let resolved_tid = if thread_id.trim().is_empty() {
            self.selected_thread_id
                .clone()
                .unwrap_or_else(|| "direct-thread".to_string())
        } else {
            thread_id.trim().to_string()
        };
        let thread_opt = self.storage.get_thread(&resolved_tid)?;
        let project = if let Some(ref th) = thread_opt {
            self.storage.get_project(&th.project_id)?.unwrap_or_else(|| {
                StoredProject::new(
                    &th.project_id,
                    "Workspace",
                    self.workspace_root.to_string_lossy().into_owned(),
                    false,
                )
            })
        } else if let Some(ref pid) = self.selected_project_id {
            self.storage.get_project(pid)?.unwrap_or_else(|| {
                StoredProject::new(
                    pid,
                    "Workspace",
                    self.workspace_root.to_string_lossy().into_owned(),
                    false,
                )
            })
        } else {
            StoredProject::new(
                "proj-direct",
                "Workspace",
                self.workspace_root.to_string_lossy().into_owned(),
                false,
            )
        };
        let profile = if project.is_read_only
            || thread_opt
                .as_ref()
                .map(|t| t.profile.eq_ignore_ascii_case("ask"))
                .unwrap_or(false)
        {
            AgentProfile::Ask
        } else {
            AgentProfile::Write
        };

        let (output, status, _is_mcp) =
            self.run_autonomous_coding_delegation(&resolved_tid, call_id, &project, profile, &args);
        Ok((output, status))
    }

    /// Executes one-click transactional Undo for a committed `delegate_coding_subagent` run
    /// (`undo_id` e.g. `"del-1"`), restoring prior disk contents, deleting newly created files/dirs,
    /// updating `snapshot_store` + `turn_pin_cache`, recording `"undo_delegation"` in `mutation_journal`,
    /// and appending `undo_id` to `self.undone_delegation_ids`.
    pub fn undo_delegation(
        &mut self,
        undo_id: &str,
        thread_id_override: Option<&str>,
    ) -> Result<Vec<String>, String> {
        let clean_id = undo_id.trim();
        if clean_id.is_empty() {
            return Err("INVALID_ARGUMENTS: undo_delegation requires a non-empty 'undo_id'.".to_string());
        }
        let pos = self
            .delegation_undo_log
            .iter()
            .rposition(|r| r.undo_id == clean_id)
            .ok_or_else(|| format!("NOT_FOUND: Delegation undo record '{clean_id}' not found."))?;
        if self.delegation_undo_log[pos].undone {
            return Err(format!("Delegation '{clean_id}' has already been undone."));
        }

        let record_tid = self.delegation_undo_log[pos].thread_id.clone();
        let record_project_root = self.delegation_undo_log[pos].project_root.clone();
        let restored_rel_paths = self.delegation_undo_log[pos]
            .execute_undo(&mut self.snapshot_store, &mut self.turn_pin_cache)?;

        let effective_tid = thread_id_override
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .map(str::to_string)
            .unwrap_or(record_tid);
        let resolved_paths: Vec<String> = restored_rel_paths
            .iter()
            .map(|rel| {
                resolve_workspace_path(&record_project_root, rel)
                    .map(|p| p.to_string_lossy().to_string())
                    .unwrap_or_else(|_| rel.clone())
            })
            .collect();
        self.record_mutation(
            &effective_tid,
            clean_id,
            "undo_delegation",
            resolved_paths,
            "completed",
            None,
        );
        if !self.undone_delegation_ids.iter().any(|id| id == clean_id) {
            self.undone_delegation_ids.push(clean_id.to_string());
        }
        self.persist_delegation_undo_state();
        Ok(restored_rel_paths)
    }

    pub(super) fn prepare_autonomous_delegation_task_payload(
        &mut self,
        thread_id: &str,
        project: &StoredProject,
        target_files: &[String],
        instruction: &str,
        context_notes: Option<&str>,
    ) -> Result<(usize, String), String> {
        if target_files.len() > 32 {
            return Err(
                "INVALID_ARGUMENTS: target_files supports at most 32 workspace-relative seed paths."
                    .to_string(),
            );
        }
        let project_root = Path::new(&project.root_path);
        let extra_roots = self.extra_readable_roots.clone();
        let mut existing_targets: Vec<String> = Vec::new();
        let mut missing_targets: Vec<String> = Vec::new();

        for raw in target_files {
            let trimmed = raw.trim();
            if trimmed.is_empty() {
                continue;
            }
            let resolved = resolve_workspace_path_with_roots(project_root, &extra_roots, trimmed)
                .map_err(|e| e.to_string())?;
            if resolved.exists() || self.snapshot_store.has_staged(&resolved) {
                existing_targets.push(trimmed.to_string());
            } else {
                missing_targets.push(trimmed.to_string());
            }
        }

        let (files_read, numbered_bundle) = if existing_targets.is_empty() {
            (0usize, String::new())
        } else {
            let (count, _approx_tokens, bundle) = collect_numbered_subagent_context_with_roots(
                project_root,
                &extra_roots,
                &existing_targets,
                Some(&mut self.snapshot_store),
                512_000,
                true,
            )
            .map_err(|e| e.to_string())?;
            self.record_workspace_reads_for_subagent_paths(
                thread_id,
                project_root,
                &existing_targets,
            );
            (count, bundle)
        };

        let mut task_payload = format!(
            "<workspace project=\"{}\" />\n<instruction>\n{instruction}\n</instruction>\n",
            project.name
        );
        if let Some(notes) = context_notes {
            task_payload.push_str(&format!("\n<context_notes>\n{notes}\n</context_notes>\n"));
        }
        if files_read > 0 {
            task_payload.push_str(&format!(
                "\n<target_files count=\"{files_read}\">\n{numbered_bundle}\n</target_files>"
            ));
        }
        if !missing_targets.is_empty() {
            task_payload.push_str(&format!(
                "\n<new_target_files>\nPaths not yet on disk (create with `write_file` / `create_directory` as needed): {}\n</new_target_files>\n",
                missing_targets.join(", ")
            ));
        }
        if target_files.is_empty() {
            task_payload.push_str(
                "\nNo seed `target_files` were pre-loaded. Inspect relevant files using `find_path`, `grep`, `read_file`, `outline_symbols`, or `get_symbol_body` before editing.\n",
            );
        }
        Ok((files_read, task_payload))
    }

    /// Routes `delegate_coding_subagent` between `DelegationMode::Propose` (single-shot non-mutating
    /// proposal synthesis) and `DelegationMode::Auto` (autonomous multi-step coding loop + per-write
    /// Gatekeeper Critic + transactional auto-commit).
    pub(super) fn run_autonomous_coding_delegation(
        &mut self,
        thread_id: &str,
        call_id: &str,
        project: &StoredProject,
        profile: AgentProfile,
        args: &Value,
    ) -> (String, String, bool) {
        if project.is_read_only || profile == AgentProfile::Ask {
            return (
                "PERMISSION_DENIED: delegate_coding_subagent is blocked in Ask/read-only mode."
                    .to_string(),
                "denied".to_string(),
                false,
            );
        }

        let target_files: Vec<String> = match args
            .get("target_files")
            .or_else(|| args.get("targetFiles"))
        {
            Some(Value::Array(arr)) => arr
                .iter()
                .filter_map(|v| {
                    v.as_str()
                        .map(str::trim)
                        .filter(|s| !s.is_empty())
                        .map(str::to_string)
                })
                .collect(),
            Some(Value::String(single)) if !single.trim().is_empty() => {
                vec![single.trim().to_string()]
            }
            _ => Vec::new(),
        };

        let instruction = args
            .get("instruction")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .trim()
            .to_string();
        if instruction.is_empty() {
            return (
                "INVALID_ARGUMENTS: delegate_coding_subagent requires a non-empty 'instruction'."
                    .to_string(),
                "failed".to_string(),
                false,
            );
        }

        let context_notes = args
            .get("context_notes")
            .or_else(|| args.get("contextNotes"))
            .and_then(|v| v.as_str())
            .map(str::trim)
            .filter(|s| !s.is_empty());

        let project_root = Path::new(&project.root_path);
        let base_tid = derive_thread_key(Some(thread_id));
        let fallback_epoch = self
            .storage
            .get_thread(thread_id)
            .ok()
            .flatten()
            .map(|t| t.engine_compaction_epoch)
            .unwrap_or(0);

        let mode =
            crate::subagent_coding::DelegationMode::from_args_and_instruction(args, &instruction);

        if mode == crate::subagent_coding::DelegationMode::Propose {
            let extra_roots = self.extra_readable_roots.clone();
            let (files_read, _approx_tokens, numbered_bundle) = if target_files.is_empty() {
                (0usize, 0usize, String::new())
            } else {
                match collect_numbered_subagent_context_with_roots(
                    project_root,
                    &extra_roots,
                    &target_files,
                    Some(&mut self.snapshot_store),
                    512_000,
                    false,
                ) {
                    Ok(res) => {
                        self.record_workspace_reads_for_subagent_paths(
                            thread_id,
                            project_root,
                            &target_files,
                        );
                        res
                    }
                    Err(e) => return (e.to_string(), "failed".to_string(), false),
                }
            };

            let subagent_cache_key = format!("subagent0:delegate:{base_tid}");
            let mut user_prompt = format!("<instruction>\n{instruction}\n</instruction>\n");
            if let Some(notes) = context_notes {
                user_prompt.push_str(&format!("\n<context_notes>\n{notes}\n</context_notes>\n"));
            }
            user_prompt.push_str(&format!(
                "\n<target_files count=\"{files_read}\">\n{numbered_bundle}\n</target_files>"
            ));

            let sub_model = self.effective_subagent_model_id();
            let role_directive = "You are a coding synthesis subagent for Spell IDE. \
                Synthesize surgical old_text / new_text edit hunks grounded in exact numbered lines of <target_files>, \
                followed by concise verification notes for review.";
            let (parent_cache_key, aligned_messages, aligned_tools) = self
                .build_shared_prefix_subagent_prompt(
                    thread_id,
                    fallback_epoch,
                    role_directive,
                    role_directive,
                    &user_prompt,
                );

            let sub_req = SubagentCompletionRequest {
                model: sub_model,
                messages: aligned_messages,
                stream: false,
                max_tokens: Some(2048),
                temperature: Some(0.1),
                prompt_cache_key: Some(subagent_cache_key),
                parent_cache_key: Some(parent_cache_key),
                reasoning_effort: None,
                thinking_budget: None,
                tools: aligned_tools,
                tool_choice: None,
                enable_thinking: None,
                ..Default::default()
            };

            return match self.active_transport().run_subagent_completion(&sub_req) {
                Ok(resp) => {
                    self.record_live_subagent_completion(&resp);
                    let cached = self.extract_and_cache_subagent_proposals(
                        project_root,
                        &target_files,
                        &resp.content,
                    );
                    let proposal = Self::format_coding_proposal_packet(&resp, files_read, &cached);
                    (proposal, "completed".to_string(), false)
                }
                Err(crate::transport::TransportError::Cancelled) => (
                    "Stopped by user (zero files committed).".to_string(),
                    "cancelled".to_string(),
                    false,
                ),
                Err(err) => (err.to_string(), "failed".to_string(), false),
            };
        }

        let max_steps = crate::subagent_coding::clamp_delegation_max_steps(
            args.get("max_steps")
                .or_else(|| args.get("maxSteps"))
                .and_then(|v| v.as_u64()),
        );
        let (files_read, task_payload) = match self.prepare_autonomous_delegation_task_payload(
            thread_id,
            project,
            &target_files,
            &instruction,
            context_notes,
        ) {
            Ok(res) => res,
            Err(err_msg) => return (err_msg, "failed".to_string(), false),
        };

        let tracker = crate::subagent_coding::ProtocolFallbackTracker::new(
            self.subagent_protocol,
            args.get("protocol").and_then(|v| v.as_str()),
        );
        let directive = match tracker.active {
            crate::subagent_coding::ActiveStepProtocol::Native => {
                crate::subagent_coding::build_native_protocol_handoff_directive(max_steps)
            }
            crate::subagent_coding::ActiveStepProtocol::Json => {
                crate::subagent_coding::build_json_protocol_handoff_directive(max_steps)
            }
        };
        let (parent_cache_key, loop_messages, aligned_tools) = self
            .build_shared_prefix_autonomous_subagent_prompt(
                thread_id,
                fallback_epoch,
                &directive,
                tracker.active,
                max_steps,
                &task_payload,
            );

        self.continue_autonomous_coding_delegation(
            thread_id,
            call_id,
            project,
            args,
            &target_files,
            files_read,
            max_steps,
            0,
            tracker,
            parent_cache_key,
            loop_messages,
            aligned_tools,
            None,
        )
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) fn continue_autonomous_coding_delegation(
        &mut self,
        thread_id: &str,
        call_id: &str,
        project: &StoredProject,
        _args: &Value,
        target_files: &[String],
        files_read: usize,
        max_steps: usize,
        slot_idx: usize,
        mut tracker: crate::subagent_coding::ProtocolFallbackTracker,
        parent_cache_key: String,
        mut loop_messages: Vec<WireMessage>,
        aligned_tools: Option<Vec<Value>>,
        mut precomputed_step0: Option<SubagentCompletionResponse>,
    ) -> (String, String, bool) {
        let project_root = Path::new(&project.root_path);
        let base_tid = derive_thread_key(Some(thread_id));
        self.last_delegation_touched_paths.clear();
        let mut overlay = crate::subagent_coding::DelegationOverlay::fork_from(&self.snapshot_store);

        let mut steps_executed: usize = 0;
        let mut last_prefix_cache_reused = false;
        let mut last_decode_tok_s = 0.0_f64;
        let mut final_summary: Option<String> = None;
        let mut verification_notes: Option<String> = None;

        for step_idx in 0..max_steps {
            if crate::is_turn_cancel_requested() {
                return (
                    "Stopped by user (zero files committed).".to_string(),
                    "cancelled".to_string(),
                    false,
                );
            }

            let resp = if let Some(step0) =
                (step_idx == 0).then(|| precomputed_step0.take()).flatten()
            {
                step0
            } else {
                let sub_model = self.effective_subagent_model_id();
                let is_native =
                    tracker.active == crate::subagent_coding::ActiveStepProtocol::Native;
                let sub_req = SubagentCompletionRequest {
                    model: sub_model,
                    messages: loop_messages.clone(),
                    stream: is_native,
                    max_tokens: Some(2048),
                    temperature: Some(0.1),
                    prompt_cache_key: Some(format!("subagent{slot_idx}:delegate:{base_tid}")),
                    parent_cache_key: Some(parent_cache_key.clone()),
                    reasoning_effort: None,
                    thinking_budget: None,
                    tools: aligned_tools.clone(),
                    tool_choice: if is_native {
                        Some(json!("auto"))
                    } else {
                        None
                    },
                    enable_thinking: None,
                    ..Default::default()
                };
                match self.active_transport().run_subagent_completion(&sub_req) {
                    Ok(r) => r,
                    Err(crate::transport::TransportError::Cancelled) => {
                        return (
                            "Stopped by user (zero files committed).".to_string(),
                            "cancelled".to_string(),
                            false,
                        );
                    }
                    Err(err) => return (err.to_string(), "failed".to_string(), false),
                }
            };

            self.record_live_subagent_completion(&resp);
            steps_executed += 1;
            if resp.prefix_cache_reused || resp.cached_tokens > 0 {
                last_prefix_cache_reused = true;
            }
            if resp.decode_tok_s > 0.0 {
                last_decode_tok_s = resp.decode_tok_s;
            }

            // Step-0 Seamless Legacy Single-Shot Compatibility
            if step_idx == 0
                && tracker.active == crate::subagent_coding::ActiveStepProtocol::Native
                && overlay.touched_files.is_empty()
                && overlay.virtual_dirs.is_empty()
                && resp.tool_calls.is_empty()
                && crate::subagent_coding::parse_json_action_step(&resp.content, 0).is_none()
                && resp.finish_reason.as_deref() != Some("tool_calls")
                && !crate::subagent_coding::contains_malformed_tool_attempt(&resp.content)
            {
                let cached = self.extract_and_cache_subagent_proposals(
                    project_root,
                    target_files,
                    &resp.content,
                );
                return (
                    Self::format_coding_proposal_packet(&resp, files_read, &cached),
                    "completed".to_string(),
                    false,
                );
            }

            let reasoning_opt = if resp.reasoning_content.trim().is_empty() {
                None
            } else {
                Some(resp.reasoning_content.clone())
            };

            match tracker.active {
                crate::subagent_coding::ActiveStepProtocol::Native => {
                    if !resp.tool_calls.is_empty() {
                        // Validate all tool calls in this step first to detect any MalformedStep.
                        let mut first_malformed_err: Option<String> = None;
                        let mut validated_calls: Vec<(
                            WireToolCall,
                            Result<Value, crate::subagent_coding::SubagentStepValidationError>,
                        )> = Vec::with_capacity(resp.tool_calls.len());

                        for (tc_idx, raw_tc) in resp.tool_calls.iter().enumerate() {
                            let mut tc = raw_tc.clone();
                            if tc.id.trim().is_empty() {
                                tc.id = format!("sub_tc_{step_idx}_{tc_idx}");
                            }
                            let vres = crate::subagent_coding::validate_subagent_tool_call(&tc);
                            if let Err(ref err) = vres {
                                if err.is_malformed_step() && first_malformed_err.is_none() {
                                    first_malformed_err = Some(err.to_tool_error_message());
                                }
                            }
                            validated_calls.push((tc, vres));
                        }

                        if let Some(err_msg) = first_malformed_err {
                            let switched = tracker.on_malformed_native_step();
                            if switched {
                                let asst_text = if resp.content.trim().is_empty() {
                                    format!("[Malformed tool call: {err_msg}]")
                                } else {
                                    resp.content.clone()
                                };
                                loop_messages.push(WireMessage::assistant(
                                    Some(asst_text),
                                    reasoning_opt,
                                    None,
                                ));
                                loop_messages.push(WireMessage::user(
                                    crate::subagent_coding::build_sticky_fallback_switch_prompt(
                                        &err_msg,
                                    ),
                                ));
                            } else {
                                // Sanitize any non-JSON arguments to "{}" so Jinja chat templates never fail `fromjson`.
                                let safe_echo_tcs: Vec<WireToolCall> = validated_calls
                                    .iter()
                                    .map(|(tc, _)| {
                                        let mut echoed = tc.clone();
                                        sanitize_wire_tool_call_args_for_jinja(&mut echoed);
                                        echoed
                                    })
                                    .collect();
                                loop_messages.push(WireMessage::assistant(
                                    if resp.content.is_empty() {
                                        None
                                    } else {
                                        Some(resp.content.clone())
                                    },
                                    reasoning_opt,
                                    Some(safe_echo_tcs),
                                ));
                                for (tc, vres) in validated_calls {
                                    let fb = match vres {
                                        Ok(_) => "SKIPPED: Another tool call in this batch had malformed arguments; re-emit valid tool call(s).".to_string(),
                                        Err(e) => e.to_tool_error_message(),
                                    };
                                    loop_messages.push(WireMessage::tool(
                                        &tc.id,
                                        Some(tc.function.name),
                                        fb,
                                    ));
                                }
                            }
                            continue;
                        }

                        // Valid native tool-call step (`Ok` or `DisallowedTool`)
                        tracker.on_valid_native_step();
                        let echoed_tcs: Vec<WireToolCall> = validated_calls
                            .iter()
                            .map(|(tc, _)| {
                                let mut echoed = tc.clone();
                                sanitize_wire_tool_call_args_for_jinja(&mut echoed);
                                echoed
                            })
                            .collect();
                        loop_messages.push(WireMessage::assistant(
                            if resp.content.is_empty() {
                                None
                            } else {
                                Some(resp.content.clone())
                            },
                            reasoning_opt,
                            Some(echoed_tcs),
                        ));

                        let mut finished_this_step = false;
                        let mut step_had_failure = false;

                        for (tc, vres) in validated_calls {
                            match vres {
                                Err(err) => {
                                    step_had_failure = true;
                                    loop_messages.push(WireMessage::tool(
                                        &tc.id,
                                        Some(tc.function.name),
                                        err.to_tool_error_message(),
                                    ));
                                }
                                Ok(norm_args) => {
                                    if tc.function.name.trim() == "finish" {
                                        if step_had_failure {
                                            loop_messages.push(WireMessage::tool(
                                                &tc.id,
                                                Some("finish".to_string()),
                                                "FINISH_DEFERRED: An earlier tool call in this step failed or was rejected by Gatekeeper Critic. Fix the reported issue and call `finish` again.".to_string(),
                                            ));
                                        } else {
                                            let summary = norm_args
                                                .get("summary")
                                                .and_then(|v| v.as_str())
                                                .unwrap_or("Completed requested changes.")
                                                .to_string();
                                            verification_notes = norm_args
                                                .get("verification_notes")
                                                .and_then(|v| v.as_str())
                                                .map(str::trim)
                                                .filter(|s| !s.is_empty())
                                                .map(str::to_string);
                                            final_summary = Some(summary);
                                            finished_this_step = true;
                                            loop_messages.push(WireMessage::tool(
                                                &tc.id,
                                                Some("finish".to_string()),
                                                "Finalizing delegation and committing verified overlay.".to_string(),
                                            ));
                                        }
                                    } else {
                                        match self.execute_subagent_overlay_tool(
                                            thread_id,
                                            project,
                                            &tc,
                                            &norm_args,
                                            &mut overlay,
                                        ) {
                                            SubagentOverlayStepOutcome::ToolOutput(out) => {
                                                loop_messages.push(WireMessage::tool(
                                                    &tc.id,
                                                    Some(tc.function.name),
                                                    out,
                                                ));
                                            }
                                            SubagentOverlayStepOutcome::ToolFailed(err_out) => {
                                                step_had_failure = true;
                                                loop_messages.push(WireMessage::tool(
                                                    &tc.id,
                                                    Some(tc.function.name),
                                                    err_out,
                                                ));
                                            }
                                            SubagentOverlayStepOutcome::Escalate {
                                                reason,
                                                summary,
                                                failed_file_meta,
                                            } => {
                                                return self.finalize_delegation_escalation(
                                                    &overlay,
                                                    &tracker,
                                                    steps_executed,
                                                    max_steps,
                                                    reason,
                                                    summary,
                                                    failed_file_meta,
                                                    last_prefix_cache_reused,
                                                    last_decode_tok_s,
                                                );
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if finished_this_step {
                            break;
                        }
                    } else {
                        // `resp.tool_calls` is empty on a `Native` protocol step
                        match crate::subagent_coding::parse_json_action_step(
                            &resp.content,
                            step_idx,
                        ) {
                            Some(Ok(tc)) => {
                                // Sticky Fallback Trigger #1: JSON action emitted in native content
                                tracker.on_json_action_in_native_content();
                                let norm_args: Value =
                                    serde_json::from_str(&tc.function.arguments)
                                        .unwrap_or_else(|_| json!({}));
                                if tc.function.name.trim() == "finish" {
                                    let summary = norm_args
                                        .get("summary")
                                        .and_then(|v| v.as_str())
                                        .unwrap_or("Completed requested changes.")
                                        .to_string();
                                    verification_notes = norm_args
                                        .get("verification_notes")
                                        .and_then(|v| v.as_str())
                                        .map(str::trim)
                                        .filter(|s| !s.is_empty())
                                        .map(str::to_string);
                                    final_summary = Some(summary);
                                    break;
                                }
                                let tool_output = match self.execute_subagent_overlay_tool(
                                    thread_id,
                                    project,
                                    &tc,
                                    &norm_args,
                                    &mut overlay,
                                ) {
                                    SubagentOverlayStepOutcome::ToolOutput(out)
                                    | SubagentOverlayStepOutcome::ToolFailed(out) => out,
                                    SubagentOverlayStepOutcome::Escalate {
                                        reason,
                                        summary,
                                        failed_file_meta,
                                    } => {
                                        return self.finalize_delegation_escalation(
                                            &overlay,
                                            &tracker,
                                            steps_executed,
                                            max_steps,
                                            reason,
                                            summary,
                                            failed_file_meta,
                                            last_prefix_cache_reused,
                                            last_decode_tok_s,
                                        );
                                    }
                                };
                                loop_messages.push(WireMessage::assistant(
                                    Some(resp.content.clone()),
                                    None,
                                    None,
                                ));
                                loop_messages.push(WireMessage::user(format!(
                                    "<tool_result tool=\"{}\">\n{}\n</tool_result>\nContinue using single JSON action objects, or {{\"action\":\"finish\",\"summary\":\"...\"}} when done.",
                                    tc.function.name, tool_output
                                )));
                            }
                            Some(Err(err_msg)) => {
                                if err_msg.starts_with("PERMISSION_DENIED:") {
                                    // Valid JSON action syntax targeting a disallowed parent tool
                                    tracker.on_json_action_in_native_content();
                                    loop_messages.push(WireMessage::assistant(
                                        Some(resp.content.clone()),
                                        None,
                                        None,
                                    ));
                                    loop_messages.push(WireMessage::user(format!(
                                        "<tool_result>\n{err_msg}\n</tool_result>\nContinue using single JSON action objects from the 9 allowed tools, or {{\"action\":\"finish\",\"summary\":\"...\"}} when done."
                                    )));
                                } else {
                                    let switched = tracker.on_malformed_native_step();
                                    loop_messages.push(WireMessage::assistant(
                                        Some(resp.content.clone()),
                                        reasoning_opt,
                                        None,
                                    ));
                                    let prompt = if switched {
                                        crate::subagent_coding::build_sticky_fallback_switch_prompt(
                                            &err_msg,
                                        )
                                    } else {
                                        format!(
                                            "{err_msg}\nEmit a structured OpenAI tool call from `{}`, or invoke `finish` when done.",
                                            crate::subagent_coding::SUBAGENT_ALLOWED_TOOLS.join("`, `")
                                        )
                                    };
                                    loop_messages.push(WireMessage::user(prompt));
                                }
                            }
                            None => {
                                if (!overlay.touched_files.is_empty()
                                    || !overlay.virtual_dirs.is_empty())
                                    && resp.finish_reason.as_deref() == Some("stop")
                                    && !crate::subagent_coding::contains_malformed_tool_attempt(
                                        &resp.content,
                                    )
                                {
                                    let cleaned =
                                        crate::subagent_coding::strip_subagent_think_blocks(
                                            &resp.content,
                                        );
                                    let summary = if cleaned.trim().is_empty() {
                                        "Completed requested changes.".to_string()
                                    } else {
                                        cleaned.trim().to_string()
                                    };
                                    final_summary = Some(summary);
                                    break;
                                }

                                let reason = if crate::subagent_coding::contains_malformed_tool_attempt(&resp.content) {
                                    "Detected raw XML tool tag or broken JSON action instead of structured OpenAI tool_calls"
                                } else {
                                    "Response contained no structured tool_calls and no finish call"
                                };
                                let switched = tracker.on_malformed_native_step();
                                loop_messages.push(WireMessage::assistant(
                                    Some(resp.content.clone()),
                                    reasoning_opt,
                                    None,
                                ));
                                let prompt = if switched {
                                    crate::subagent_coding::build_sticky_fallback_switch_prompt(
                                        reason,
                                    )
                                } else {
                                    format!(
                                        "MALFORMED_STEP: {reason}. Emit a structured OpenAI tool call from `{}`, or invoke `finish` when all edits are verified.",
                                        crate::subagent_coding::SUBAGENT_ALLOWED_TOOLS.join("`, `")
                                    )
                                };
                                loop_messages.push(WireMessage::user(prompt));
                            }
                        }
                    }
                }
                crate::subagent_coding::ActiveStepProtocol::Json => {
                    match crate::subagent_coding::parse_json_action_step(
                        &resp.content,
                        step_idx,
                    ) {
                        Some(Ok(tc)) => {
                            let norm_args: Value = serde_json::from_str(&tc.function.arguments)
                                .unwrap_or_else(|_| json!({}));
                            if tc.function.name.trim() == "finish" {
                                let summary = norm_args
                                    .get("summary")
                                    .and_then(|v| v.as_str())
                                    .unwrap_or("Completed requested changes.")
                                    .to_string();
                                verification_notes = norm_args
                                    .get("verification_notes")
                                    .and_then(|v| v.as_str())
                                    .map(str::trim)
                                    .filter(|s| !s.is_empty())
                                    .map(str::to_string);
                                final_summary = Some(summary);
                                break;
                            }
                            let tool_output = match self.execute_subagent_overlay_tool(
                                thread_id,
                                project,
                                &tc,
                                &norm_args,
                                &mut overlay,
                            ) {
                                SubagentOverlayStepOutcome::ToolOutput(out)
                                | SubagentOverlayStepOutcome::ToolFailed(out) => out,
                                SubagentOverlayStepOutcome::Escalate {
                                    reason,
                                    summary,
                                    failed_file_meta,
                                } => {
                                    return self.finalize_delegation_escalation(
                                        &overlay,
                                        &tracker,
                                        steps_executed,
                                        max_steps,
                                        reason,
                                        summary,
                                        failed_file_meta,
                                        last_prefix_cache_reused,
                                        last_decode_tok_s,
                                    );
                                }
                            };
                            loop_messages.push(WireMessage::assistant(
                                Some(resp.content.clone()),
                                None,
                                None,
                            ));
                            loop_messages.push(WireMessage::user(format!(
                                "<tool_result tool=\"{}\">\n{}\n</tool_result>\nEmit the next single JSON action object, or {{\"action\":\"finish\",\"summary\":\"...\"}} when done.",
                                tc.function.name, tool_output
                            )));
                        }
                        Some(Err(err_msg)) => {
                            loop_messages.push(WireMessage::assistant(
                                Some(resp.content.clone()),
                                None,
                                None,
                            ));
                            loop_messages.push(WireMessage::user(format!(
                                "<tool_error>\n{err_msg}\n</tool_error>\nReply with ONLY a single valid JSON object containing an \"action\" key from `{}`, e.g. {{\"action\":\"finish\",\"summary\":\"...\"}}.",
                                crate::subagent_coding::SUBAGENT_ALLOWED_TOOLS.join("`, `")
                            )));
                        }
                        None => {
                            if (!overlay.touched_files.is_empty()
                                || !overlay.virtual_dirs.is_empty())
                                && resp.finish_reason.as_deref() == Some("stop")
                                && !crate::subagent_coding::contains_malformed_tool_attempt(
                                    &resp.content,
                                )
                                && resp.tool_calls.is_empty()
                            {
                                let cleaned = crate::subagent_coding::strip_subagent_think_blocks(
                                    &resp.content,
                                );
                                let summary = if cleaned.trim().is_empty() {
                                    "Completed requested changes.".to_string()
                                } else {
                                    cleaned.trim().to_string()
                                };
                                final_summary = Some(summary);
                                break;
                            }

                            if !resp.tool_calls.is_empty() {
                                let first_tc = &resp.tool_calls[0];
                                match crate::subagent_coding::validate_subagent_tool_call(first_tc)
                                {
                                    Ok(norm_args) => {
                                        if first_tc.function.name.trim() == "finish" {
                                            let summary = norm_args
                                                .get("summary")
                                                .and_then(|v| v.as_str())
                                                .unwrap_or("Completed requested changes.")
                                                .to_string();
                                            verification_notes = norm_args
                                                .get("verification_notes")
                                                .and_then(|v| v.as_str())
                                                .map(str::trim)
                                                .filter(|s| !s.is_empty())
                                                .map(str::to_string);
                                            final_summary = Some(summary);
                                            break;
                                        }
                                        let mut tc_with_id = first_tc.clone();
                                        if tc_with_id.id.trim().is_empty() {
                                            tc_with_id.id = format!("sub_json_tc_{step_idx}");
                                        }
                                        let tool_output = match self.execute_subagent_overlay_tool(
                                            thread_id,
                                            project,
                                            &tc_with_id,
                                            &norm_args,
                                            &mut overlay,
                                        ) {
                                            SubagentOverlayStepOutcome::ToolOutput(out)
                                            | SubagentOverlayStepOutcome::ToolFailed(out) => out,
                                            SubagentOverlayStepOutcome::Escalate {
                                                reason,
                                                summary,
                                                failed_file_meta,
                                            } => {
                                                return self.finalize_delegation_escalation(
                                                    &overlay,
                                                    &tracker,
                                                    steps_executed,
                                                    max_steps,
                                                    reason,
                                                    summary,
                                                    failed_file_meta,
                                                    last_prefix_cache_reused,
                                                    last_decode_tok_s,
                                                );
                                            }
                                        };
                                        let asst_repr = if resp.content.trim().is_empty() {
                                            format!(
                                                "{{\"action\":\"{}\"}}",
                                                tc_with_id.function.name
                                            )
                                        } else {
                                            resp.content.clone()
                                        };
                                        loop_messages.push(WireMessage::assistant(
                                            Some(asst_repr),
                                            None,
                                            None,
                                        ));
                                        loop_messages.push(WireMessage::user(format!(
                                            "<tool_result tool=\"{}\">\n{}\n</tool_result>\nEmit the next single JSON action object, or {{\"action\":\"finish\",\"summary\":\"...\"}} when done.",
                                            tc_with_id.function.name, tool_output
                                        )));
                                    }
                                    Err(err) => {
                                        loop_messages.push(WireMessage::assistant(
                                            Some(if resp.content.trim().is_empty() {
                                                "[Invalid tool call]".to_string()
                                            } else {
                                                resp.content.clone()
                                            }),
                                            None,
                                            None,
                                        ));
                                        loop_messages.push(WireMessage::user(format!(
                                            "<tool_error>\n{}\n</tool_error>\nReply with ONLY a single valid JSON object containing an \"action\" key.",
                                            err.to_tool_error_message()
                                        )));
                                    }
                                }
                            } else {
                                loop_messages.push(WireMessage::assistant(
                                    Some(resp.content.clone()),
                                    None,
                                    None,
                                ));
                                loop_messages.push(WireMessage::user(format!(
                                    "INVALID_FORMAT: Reply with ONLY a single valid JSON object containing a top-level \"action\" key from `{}` (e.g. {{\"action\":\"finish\",\"summary\":\"...\"}}).",
                                    crate::subagent_coding::SUBAGENT_ALLOWED_TOOLS.join("`, `")
                                )));
                            }
                        }
                    }
                }
            }
        }

        // Escalation Guard #4: Step budget exhausted without `finish`
        let Some(summary) = final_summary else {
            let esc_summary = format!(
                "Delegation reached max_steps ({max_steps}) without invoking `finish`; uncommitted overlay changes were preserved as proposals without writing to disk."
            );
            return self.finalize_delegation_escalation(
                &overlay,
                &tracker,
                steps_executed,
                max_steps,
                "step_budget_exhausted",
                esc_summary,
                None,
                last_prefix_cache_reused,
                last_decode_tok_s,
            );
        };

        self.commit_delegation_overlay_transactionally(
            thread_id,
            call_id,
            project_root,
            &mut overlay,
            &tracker,
            steps_executed,
            max_steps,
            summary,
            verification_notes,
            last_prefix_cache_reused,
            last_decode_tok_s,
        )
    }

    pub(super) fn execute_subagent_overlay_tool(
        &mut self,
        thread_id: &str,
        project: &StoredProject,
        tc: &WireToolCall,
        args: &Value,
        overlay: &mut crate::subagent_coding::DelegationOverlay,
    ) -> SubagentOverlayStepOutcome {
        let project_root = Path::new(&project.root_path);
        let tool_name = tc.function.name.trim();

        match tool_name {
            "find_path" => {
                let pattern = args
                    .get("pattern")
                    .or_else(|| args.get("glob"))
                    .or_else(|| args.get("needle"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let raw_base = args
                    .get("path")
                    .or_else(|| args.get("base_dir"))
                    .and_then(|v| v.as_str())
                    .map(str::trim)
                    .filter(|s| !s.is_empty())
                    .unwrap_or(".");
                let search_base = match resolve_workspace_path_with_roots(
                    project_root,
                    &self.extra_readable_roots,
                    raw_base,
                ) {
                    Ok(p) => p,
                    Err(e) => return SubagentOverlayStepOutcome::ToolFailed(e.to_string()),
                };
                self.record_workspace_read_if_outside_research_cache(thread_id, &search_base);
                let out = crate::subagent_coding::find_paths_with_overlay(
                    project_root,
                    &search_base,
                    pattern,
                    overlay,
                    Self::FIND_PATH_MAX_RESULTS,
                );
                if out.starts_with("SECURITY_BLOCK:") {
                    SubagentOverlayStepOutcome::ToolFailed(out)
                } else {
                    SubagentOverlayStepOutcome::ToolOutput(out)
                }
            }
            "grep" => {
                let pattern = args
                    .get("pattern")
                    .or_else(|| args.get("query"))
                    .or_else(|| args.get("regex"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let max_results = args
                    .get("max_results")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(100) as usize;
                let raw_base = args
                    .get("path")
                    .and_then(|v| v.as_str())
                    .map(str::trim)
                    .filter(|s| !s.is_empty())
                    .unwrap_or(".");
                let search_base = match resolve_workspace_path_with_roots(
                    project_root,
                    &self.extra_readable_roots,
                    raw_base,
                ) {
                    Ok(p) => p,
                    Err(e) => return SubagentOverlayStepOutcome::ToolFailed(e.to_string()),
                };
                self.record_workspace_read_if_outside_research_cache(thread_id, &search_base);
                match crate::subagent_coding::grep_with_overlay(
                    project_root,
                    &search_base,
                    pattern,
                    max_results,
                    overlay,
                ) {
                    Ok(out) => SubagentOverlayStepOutcome::ToolOutput(out),
                    Err(err) => SubagentOverlayStepOutcome::ToolFailed(err),
                }
            }
            "read_file" | "outline_symbols" | "get_symbol_body" => {
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or("");
                if let Err(e) = resolve_workspace_path_with_roots(
                    project_root,
                    &self.extra_readable_roots,
                    raw_path,
                ) {
                    return SubagentOverlayStepOutcome::ToolFailed(e.to_string());
                }
                self.record_workspace_reads_for_subagent_paths(
                    thread_id,
                    project_root,
                    &[raw_path.to_string()],
                );

                let normalized_tc = WireToolCall {
                    id: tc.id.clone(),
                    r#type: tc.r#type.clone(),
                    function: WireToolFunction {
                        name: tool_name.to_string(),
                        arguments: serde_json::to_string(args)
                            .unwrap_or_else(|_| tc.function.arguments.clone()),
                    },
                    index: tc.index,
                };

                std::mem::swap(&mut self.snapshot_store, &mut overlay.store);
                std::mem::swap(&mut self.turn_pin_cache, &mut overlay.pin_cache);
                let mut dummy_tags = Vec::new();
                let (out, status, _) = self.execute_single_tool_call_inner(
                    thread_id,
                    &mut dummy_tags,
                    project,
                    AgentProfile::Write,
                    &normalized_tc,
                );
                std::mem::swap(&mut self.snapshot_store, &mut overlay.store);
                std::mem::swap(&mut self.turn_pin_cache, &mut overlay.pin_cache);

                if status == "completed" {
                    SubagentOverlayStepOutcome::ToolOutput(out)
                } else {
                    SubagentOverlayStepOutcome::ToolFailed(out)
                }
            }
            "create_directory" => {
                if project.is_read_only {
                    return SubagentOverlayStepOutcome::ToolFailed(
                        "READ_ONLY_PROJECT: Directory creation blocked.".to_string(),
                    );
                }
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or("").trim();
                let resolved = match resolve_workspace_path(project_root, raw_path) {
                    Ok(p) => p,
                    Err(e) => return SubagentOverlayStepOutcome::ToolFailed(e.to_string()),
                };
                if let Err(e) = crate::tools::enforce_protected_globs(&resolved, raw_path) {
                    return SubagentOverlayStepOutcome::ToolFailed(e.to_string());
                }
                if let Err(e) = enforce_research_cache_write_guard(&resolved) {
                    return SubagentOverlayStepOutcome::ToolFailed(e.to_string());
                }
                let rel_path = Self::relative_workspace_candidate(project_root, raw_path)
                    .trim()
                    .trim_start_matches("./")
                    .replace('\\', "/");
                if self.web_tainted_threads.contains(thread_id) {
                    if let Some(sens_target) = Self::detect_web_taint_sensitive_workspace_path(
                        project_root,
                        raw_path,
                        Some(&resolved),
                    ) {
                        return SubagentOverlayStepOutcome::Escalate {
                            reason: "web_taint_sensitive_file",
                            summary: format!(
                                "Web-tainted thread attempted to create sensitive workspace path '{sens_target}'; escalating to user for explicit confirmation."
                            ),
                            failed_file_meta: None,
                        };
                    }
                }
                if let Some(sib_conflict) = self.detect_batch_sibling_conflict(&resolved) {
                    return SubagentOverlayStepOutcome::Escalate {
                        reason: "mutation_conflict",
                        summary: sib_conflict,
                        failed_file_meta: None,
                    };
                }
                if let Some(conflict_msg) = self.detect_mutation_conflict(
                    thread_id,
                    &[resolved.to_string_lossy().to_string()],
                ) {
                    return SubagentOverlayStepOutcome::Escalate {
                        reason: "mutation_conflict",
                        summary: conflict_msg,
                        failed_file_meta: None,
                    };
                }
                match overlay.stage_virtual_dir(&resolved) {
                    Ok(()) => SubagentOverlayStepOutcome::ToolOutput(format!(
                        "Staged directory creation for '{}' in delegation overlay.",
                        if rel_path.is_empty() { raw_path } else { &rel_path }
                    )),
                    Err(e) => SubagentOverlayStepOutcome::ToolFailed(e.to_string()),
                }
            }
            "write_file" | "edit_file" => {
                if project.is_read_only {
                    return SubagentOverlayStepOutcome::ToolFailed(format!(
                        "READ_ONLY_PROJECT: Project '{}' is locked as read-only.",
                        project.name
                    ));
                }
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or("").trim();
                let resolved = match resolve_workspace_path(project_root, raw_path) {
                    Ok(p) => p,
                    Err(e) => return SubagentOverlayStepOutcome::ToolFailed(e.to_string()),
                };
                if let Err(e) = crate::tools::enforce_protected_globs(&resolved, raw_path) {
                    return SubagentOverlayStepOutcome::ToolFailed(e.to_string());
                }
                if let Err(e) = enforce_research_cache_write_guard(&resolved) {
                    return SubagentOverlayStepOutcome::ToolFailed(e.to_string());
                }

                let rel_path = {
                    let cand = Self::relative_workspace_candidate(project_root, raw_path)
                        .trim()
                        .trim_start_matches("./")
                        .replace('\\', "/");
                    if cand.is_empty() {
                        resolved
                            .file_name()
                            .and_then(|s| s.to_str())
                            .unwrap_or(raw_path)
                            .to_string()
                    } else {
                        cand
                    }
                };

                // Escalation Guard #3: Web-Taint Sensitive File (checking BOTH raw candidate AND canonicalized symlink target!)
                if self.web_tainted_threads.contains(thread_id) {
                    if let Some(sens_target) = Self::detect_web_taint_sensitive_workspace_path(
                        project_root,
                        raw_path,
                        Some(&resolved),
                    ) {
                        return SubagentOverlayStepOutcome::Escalate {
                            reason: "web_taint_sensitive_file",
                            summary: format!(
                                "Web-tainted thread attempted to modify protected workspace rule/build-hook file '{sens_target}'; escalating to user for explicit confirmation."
                            ),
                            failed_file_meta: None,
                        };
                    }
                }

                // Escalation Guard #2: Same-Batch Sibling OR Cross-Thread Conflict
                if let Some(sib_conflict) = self.detect_batch_sibling_conflict(&resolved) {
                    return SubagentOverlayStepOutcome::Escalate {
                        reason: "mutation_conflict",
                        summary: sib_conflict,
                        failed_file_meta: None,
                    };
                }
                if let Some(conflict_msg) = self.detect_mutation_conflict(
                    thread_id,
                    &[resolved.to_string_lossy().to_string()],
                ) {
                    return SubagentOverlayStepOutcome::Escalate {
                        reason: "mutation_conflict",
                        summary: conflict_msg,
                        failed_file_meta: None,
                    };
                }

                let baseline_before_attempt = overlay
                    .store
                    .read_file_text(&resolved)
                    .unwrap_or_default();

                let (updated_content, op_label) = if tool_name == "write_file" {
                    let content = args.get("content").and_then(|v| v.as_str()).unwrap_or("");
                    let append = args.get("append").and_then(|v| v.as_bool()).unwrap_or(false);
                    let final_text = if append {
                        format!("{baseline_before_attempt}{content}")
                    } else {
                        content.to_string()
                    };
                    let label = format!("Staged {} bytes to {rel_path}", final_text.len());
                    (final_text, label)
                } else {
                    let mut current = match overlay.store.read_file_text(&resolved) {
                        Ok(c) => c,
                        Err(e) => {
                            return SubagentOverlayStepOutcome::ToolFailed(format!(
                                "IO_ERROR: Failed to read '{rel_path}': {e}. Inspect with `find_path` / `read_file` or create with `write_file`."
                            ));
                        }
                    };
                    let mut applied = 0usize;
                    if let Some(edits_arr) = args
                        .get("edits")
                        .and_then(|v| v.as_array())
                        .filter(|a| !a.is_empty())
                    {
                        for edit_item in edits_arr {
                            let old_t = edit_item
                                .get("old_text")
                                .or_else(|| edit_item.get("oldText"))
                                .and_then(|v| v.as_str())
                                .unwrap_or("");
                            let new_t = edit_item
                                .get("new_text")
                                .or_else(|| edit_item.get("newText"))
                                .and_then(|v| v.as_str())
                                .unwrap_or("");
                            if old_t.is_empty() {
                                return SubagentOverlayStepOutcome::ToolFailed(
                                    "INVALID_EDIT: old_text cannot be empty.".to_string(),
                                );
                            }
                            let count = current.matches(old_t).count();
                            if count == 0 {
                                return SubagentOverlayStepOutcome::ToolFailed(format!(
                                    "INVALID_EDIT: old_text not found in '{rel_path}'. Re-read '{rel_path}' via `read_file` (never include line-number gutters) and retry."
                                ));
                            }
                            if count > 1 {
                                return SubagentOverlayStepOutcome::ToolFailed(format!(
                                    "AMBIGUOUS_EDIT: old_text matched {count} times in '{rel_path}'. Include more surrounding context lines."
                                ));
                            }
                            current = current.replacen(old_t, new_t, 1);
                            applied += 1;
                        }
                    } else {
                        let old_t = args
                            .get("old_text")
                            .or_else(|| args.get("oldText"))
                            .and_then(|v| v.as_str())
                            .unwrap_or("");
                        let new_t = args
                            .get("new_text")
                            .or_else(|| args.get("newText"))
                            .and_then(|v| v.as_str())
                            .unwrap_or("");
                        if old_t.is_empty() {
                            return SubagentOverlayStepOutcome::ToolFailed(
                                "INVALID_EDIT: old_text cannot be empty.".to_string(),
                            );
                        }
                        let count = current.matches(old_t).count();
                        if count == 0 {
                            return SubagentOverlayStepOutcome::ToolFailed(format!(
                                "INVALID_EDIT: old_text not found in '{rel_path}'. Re-read '{rel_path}' via `read_file` (never include line-number gutters) and retry."
                            ));
                        }
                        if count > 1 {
                            return SubagentOverlayStepOutcome::ToolFailed(format!(
                                "AMBIGUOUS_EDIT: old_text matched {count} times in '{rel_path}'. Include more surrounding context lines."
                            ));
                        }
                        current = current.replacen(old_t, new_t, 1);
                        applied = 1;
                    }
                    let label = format!("Staged {applied} surgical edit(s) to {rel_path}");
                    (current, label)
                };

                // Per-Write Gatekeeper Critic (isolated from parent loop_state & circuit-breaker)
                let max_bounces_eff = self.max_critic_bounces.max(1);
                let prev_loop_state = self.loop_state;
                let prev_bounces = self.critic_bounce_count;
                let prev_max_bounces = self.max_critic_bounces;
                let prev_stage_mode = self.stage_write_mode;

                self.stage_write_mode = true;
                self.critic_bounce_count = 0;
                self.max_critic_bounces = max_bounces_eff;
                std::mem::swap(&mut self.snapshot_store, &mut overlay.store);

                let stage_res = self.stage_mutation_with_live_critic(
                    thread_id,
                    &resolved,
                    &rel_path,
                    updated_content.clone(),
                    &op_label,
                );

                std::mem::swap(&mut self.snapshot_store, &mut overlay.store);
                self.loop_state = prev_loop_state;
                self.critic_bounce_count = prev_bounces;
                self.max_critic_bounces = prev_max_bounces;
                self.stage_write_mode = prev_stage_mode;

                match stage_res {
                    Ok(ok_msg) => {
                        overlay.touched_files.insert(resolved.clone());
                        overlay.pin_cache.invalidate_path(&resolved);
                        SubagentOverlayStepOutcome::ToolOutput(ok_msg)
                    }
                    Err(reject_msg) => {
                        let b = overlay.record_critic_bounce(&resolved);
                        if (b as u32) >= (max_bounces_eff as u32) {
                            let (_, added, removed) = crate::tools::generate_unified_diff(
                                &rel_path,
                                &baseline_before_attempt,
                                &updated_content,
                            );
                            let esc_summary = format!(
                                "Gatekeeper Critic rejected `{rel_path}` {b}/{max_bounces_eff} time(s); escalating without disk writes. Last rejection: {reject_msg}"
                            );
                            SubagentOverlayStepOutcome::Escalate {
                                reason: "critic_bounces_exhausted",
                                summary: esc_summary,
                                failed_file_meta: Some(crate::subagent_coding::DelegationFileMeta {
                                    path: rel_path,
                                    added,
                                    removed,
                                    critic_passed: false,
                                    bounces: b,
                                }),
                            }
                        } else {
                            SubagentOverlayStepOutcome::ToolFailed(format!(
                                "[Critic bounce {b}/{max_bounces_eff}] {reject_msg}"
                            ))
                        }
                    }
                }
            }
            other => SubagentOverlayStepOutcome::ToolFailed(format!(
                "PERMISSION_DENIED: Unsupported subagent overlay tool '{other}'."
            )),
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) fn finalize_delegation_escalation(
        &mut self,
        overlay: &crate::subagent_coding::DelegationOverlay,
        tracker: &crate::subagent_coding::ProtocolFallbackTracker,
        steps_executed: usize,
        max_steps: usize,
        reason: &str,
        escalation_summary: String,
        failed_file_meta: Option<crate::subagent_coding::DelegationFileMeta>,
        prefix_cache_reused: bool,
        decode_tok_s: f64,
    ) -> (String, String, bool) {
        let mut proposal_ids: Vec<String> = Vec::new();
        let mut files: Vec<crate::subagent_coding::DelegationFileMeta> = Vec::new();
        let created_at = now_unix_secs();

        for p in &overlay.touched_files {
            let staged_opt = overlay.store.staged_overlay.get(p).or_else(|| {
                overlay
                    .store
                    .staged_overlay
                    .iter()
                    .find(|(k, v)| Self::paths_equivalent(k, p) || Self::paths_equivalent(&v.path, p))
                    .map(|(_, v)| v)
            });
            if let Some(st) = staged_opt {
                if st.status == "verified" || st.status == "staged" {
                    let is_verified = st.status == "verified";
                    let can_cache_proposal =
                        is_verified || (!self.critic_enabled && st.status == "staged");
                    files.push(crate::subagent_coding::DelegationFileMeta {
                        path: st.relative_path.clone(),
                        added: st.added_lines,
                        removed: st.removed_lines,
                        critic_passed: is_verified,
                        bounces: overlay.bounces_for(p),
                    });
                    if can_cache_proposal {
                        if let Some(ref orig) = st.original_content {
                            if !orig.is_empty() && orig != &st.staged_content {
                                self.next_proposal_seq = self.next_proposal_seq.saturating_add(1);
                                let prop_id = format!("prop-{}", self.next_proposal_seq);
                                let proposal = SubagentCodingProposal {
                                    proposal_id: prop_id.clone(),
                                    path: st.relative_path.clone(),
                                    edits: vec![ProposalEditHunk {
                                        old_text: orig.clone(),
                                        new_text: st.staged_content.clone(),
                                    }],
                                    created_at,
                                };
                                while self.proposal_cache.len() >= MAX_CACHED_SUBAGENT_PROPOSALS {
                                    self.proposal_cache.pop_front();
                                }
                                self.proposal_cache.push_back(proposal);
                                proposal_ids.push(prop_id);
                            }
                        }
                    }
                }
            }
        }

        if let Some(ffm) = failed_file_meta {
            if !files.iter().any(|f| f.path == ffm.path) {
                files.push(ffm);
            }
        }

        let meta = crate::subagent_coding::DelegationResultMeta {
            undo_id: None,
            protocol_used: tracker.active.as_str().to_string(),
            fallback_triggered: tracker.fallback_triggered,
            steps: steps_executed,
            max_steps,
            status: "escalated".to_string(),
            escalation_reason: Some(reason.to_string()),
            summary: escalation_summary,
            proposal_ids,
            files,
        };
        let sub_model = self.effective_subagent_model_id();
        (
            crate::subagent_coding::format_delegation_result_output(
                &sub_model,
                &meta,
                prefix_cache_reused,
                decode_tok_s,
                &[],
            ),
            "completed".to_string(),
            false,
        )
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) fn commit_delegation_overlay_transactionally(
        &mut self,
        thread_id: &str,
        call_id: &str,
        project_root: &Path,
        overlay: &mut crate::subagent_coding::DelegationOverlay,
        tracker: &crate::subagent_coding::ProtocolFallbackTracker,
        steps_executed: usize,
        max_steps: usize,
        summary: String,
        verification_notes: Option<String>,
        prefix_cache_reused: bool,
        decode_tok_s: f64,
    ) -> (String, String, bool) {
        let mut detail_lines = Vec::new();
        if let Some(vn) = verification_notes {
            detail_lines.push(format!("Verification Notes: {vn}"));
        }

        if overlay.touched_files.is_empty() && overlay.virtual_dirs.is_empty() {
            let meta = crate::subagent_coding::DelegationResultMeta {
                undo_id: None,
                protocol_used: tracker.active.as_str().to_string(),
                fallback_triggered: tracker.fallback_triggered,
                steps: steps_executed,
                max_steps,
                status: "committed".to_string(),
                escalation_reason: None,
                summary,
                proposal_ids: Vec::new(),
                files: Vec::new(),
            };
            let sub_model = self.effective_subagent_model_id();
            return (
                crate::subagent_coding::format_delegation_result_output(
                    &sub_model,
                    &meta,
                    prefix_cache_reused,
                    decode_tok_s,
                    &detail_lines,
                ),
                "completed".to_string(),
                false,
            );
        }

        for p in &overlay.touched_files {
            if let Some(sib_conflict) = self.detect_batch_sibling_conflict(p) {
                return self.finalize_delegation_escalation(
                    overlay,
                    tracker,
                    steps_executed,
                    max_steps,
                    "mutation_conflict",
                    sib_conflict,
                    None,
                    prefix_cache_reused,
                    decode_tok_s,
                );
            }
        }
        for dir in &overlay.virtual_dirs {
            if let Some(sib_conflict) = self.detect_batch_sibling_conflict(dir) {
                return self.finalize_delegation_escalation(
                    overlay,
                    tracker,
                    steps_executed,
                    max_steps,
                    "mutation_conflict",
                    sib_conflict,
                    None,
                    prefix_cache_reused,
                    decode_tok_s,
                );
            }
        }

        let touched_resolved_strings: Vec<String> = overlay
            .touched_files
            .iter()
            .map(|p| p.to_string_lossy().to_string())
            .collect();
        if let Some(conflict_msg) =
            self.detect_mutation_conflict(thread_id, &touched_resolved_strings)
        {
            return self.finalize_delegation_escalation(
                overlay,
                tracker,
                steps_executed,
                max_steps,
                "mutation_conflict",
                conflict_msg,
                None,
                prefix_cache_reused,
                decode_tok_s,
            );
        }

        // Pre-validate all file and virtual directory targets before touching snapshot_store or disk.
        let canon_root = project_root
            .canonicalize()
            .unwrap_or_else(|_| project_root.to_path_buf());
        for dir in &overlay.virtual_dirs {
            if let Err(e) = crate::tools::enforce_path_guardrail(dir) {
                return (e.to_string(), "failed".to_string(), false);
            }
            if let Err(e) = enforce_research_cache_write_guard(dir) {
                return (e.to_string(), "denied".to_string(), false);
            }
            if let Err(e) = crate::tools::canonicalize_target_path(project_root, dir) {
                return (e.to_string(), "failed".to_string(), false);
            }
        }
        for p in &overlay.touched_files {
            if let Err(e) = crate::tools::enforce_path_guardrail(p) {
                return (e.to_string(), "failed".to_string(), false);
            }
            if let Err(e) = enforce_research_cache_write_guard(p) {
                return (e.to_string(), "denied".to_string(), false);
            }
            if let Err(e) = crate::tools::canonicalize_target_path(project_root, p) {
                return (e.to_string(), "failed".to_string(), false);
            }
        }

        // Shadow-Overlay Hold on `web_tainted_threads` (and explicit `stage_write_mode` hold):
        // Never write directly to physical disk! Copy Critic-verified `StagedFileMutation` entries into
        // `self.snapshot_store.staged_overlay` + `self.snapshot_store.snapshots` so `StagedDiffsTray`
        // surfaces them for explicit human Commit / Discard.
        let web_tainted_hold = self.web_tainted_threads.contains(thread_id);
        let explicit_stage_hold = self.delegation_stage_write_hold && self.stage_write_mode;
        if web_tainted_hold || explicit_stage_hold {
            let mut staged_count = 0usize;
            for p in &overlay.touched_files {
                let staged_opt = overlay
                    .store
                    .staged_overlay
                    .get(p)
                    .or_else(|| {
                        overlay
                            .store
                            .staged_overlay
                            .iter()
                            .find(|(k, v)| {
                                Self::paths_equivalent(k, p) || Self::paths_equivalent(&v.path, p)
                            })
                            .map(|(_, v)| v)
                    })
                    .cloned();
                if let Some(st) = staged_opt {
                    if st.status == "verified" || (!web_tainted_hold && st.status == "staged") {
                        self.snapshot_store.staged_overlay.retain(|k, v| {
                            !Self::paths_equivalent(k, p) && !Self::paths_equivalent(&v.path, p)
                        });
                        let snap = crate::tools::FileSnapshot::from_content(
                            st.path.clone(),
                            st.staged_content.clone(),
                        );
                        self.snapshot_store.snapshots.insert(st.path.clone(), snap);
                        let joined_rel = project_root.join(&st.relative_path);
                        self.snapshot_store
                            .staged_overlay
                            .insert(st.path.clone(), st);
                        self.turn_pin_cache.invalidate_path(p);
                        if joined_rel != *p {
                            self.turn_pin_cache.invalidate_path(&joined_rel);
                        }
                        self.last_delegation_touched_paths.push(p.clone());
                        staged_count += 1;
                    }
                }
            }
            let hold_reason = if web_tainted_hold {
                "web_taint_staged_review"
            } else {
                "stage_write_hold"
            };
            let hold_summary = format!(
                "Staged {staged_count} Critic-verified file(s) for review without writing to disk ({summary})."
            );
            return self.finalize_delegation_escalation(
                overlay,
                tracker,
                steps_executed,
                max_steps,
                hold_reason,
                hold_summary,
                None,
                prefix_cache_reused,
                decode_tok_s,
            );
        }

        // Collect newly created directories (virtual_dirs + non-existent parent dirs inside project_root) BEFORE mutating disk.
        let mut created_dirs_set: std::collections::BTreeSet<std::path::PathBuf> =
            std::collections::BTreeSet::new();
        let mut record_missing_ancestors = |leaf_dir: &Path| {
            let mut cur = leaf_dir;
            while (cur.starts_with(project_root) && cur != project_root)
                || (cur.starts_with(&canon_root) && cur != canon_root)
            {
                if !cur.exists() {
                    created_dirs_set.insert(cur.to_path_buf());
                }
                let Some(parent) = cur.parent() else {
                    break;
                };
                if parent == cur {
                    break;
                }
                cur = parent;
            }
        };
        for dir in &overlay.virtual_dirs {
            record_missing_ancestors(dir);
        }
        for p in &overlay.touched_files {
            if let Some(parent) = p.parent() {
                record_missing_ancestors(parent);
            }
        }
        let created_dirs: Vec<std::path::PathBuf> = created_dirs_set.into_iter().collect();

        // Capture prior disk state for each touched file strictly BEFORE committing to disk.
        let mut files_restore: Vec<crate::subagent_coding::DelegationFileRestore> =
            Vec::with_capacity(overlay.touched_files.len());
        for p in &overlay.touched_files {
            let prior_disk_content = fs::read_to_string(p).ok();
            let staged_rel = overlay
                .store
                .staged_overlay
                .get(p)
                .or_else(|| {
                    overlay
                        .store
                        .staged_overlay
                        .iter()
                        .find(|(k, v)| {
                            Self::paths_equivalent(k, p) || Self::paths_equivalent(&v.path, p)
                        })
                        .map(|(_, v)| v)
                })
                .map(|st| st.relative_path.clone())
                .unwrap_or_else(|| {
                    Self::relative_workspace_candidate(&canon_root, &p.to_string_lossy())
                        .trim()
                        .trim_start_matches("./")
                        .replace('\\', "/")
                });
            files_restore.push(crate::subagent_coding::DelegationFileRestore {
                canonical_path: p.clone(),
                relative_path: staged_rel,
                prior_disk_content,
            });
        }

        let prospective_undo_id = format!("del-{}", self.next_delegation_undo_seq);
        let git_snapshot_sha = create_git_loop_snapshot(&canon_root, &prospective_undo_id, 1).ok();

        // Materialize virtual directories first.
        for dir in &overlay.virtual_dirs {
            if let Err(e) = fs::create_dir_all(dir) {
                // Roll back any newly created empty directories in reverse depth order.
                let mut rev_dirs = created_dirs.clone();
                rev_dirs.sort_by(|a, b| b.components().count().cmp(&a.components().count()));
                for d in rev_dirs {
                    let _ = fs::remove_dir(&d);
                }
                return (
                    format!("IO_ERROR: Failed creating directory '{}': {e}", dir.display()),
                    "failed".to_string(),
                    false,
                );
            }
        }

        // Commit each touched file to disk, rolling back any earlier writes if an I/O error occurs.
        let mut committed_files_meta: Vec<crate::subagent_coding::DelegationFileMeta> =
            Vec::with_capacity(overlay.touched_files.len());
        let mut written_count = 0usize;

        for (idx, p) in overlay.touched_files.iter().enumerate() {
            match overlay.store.commit_staged(Some(p)) {
                Ok(committed_entries) => {
                    for cm in committed_entries {
                        written_count = idx + 1;
                        let prior_text = files_restore[idx]
                            .prior_disk_content
                            .as_deref()
                            .unwrap_or("");
                        let (_, added, removed) = crate::tools::generate_unified_diff(
                            &cm.relative_path,
                            prior_text,
                            &cm.staged_content,
                        );
                        committed_files_meta.push(crate::subagent_coding::DelegationFileMeta {
                            path: cm.relative_path.clone(),
                            added,
                            removed,
                            critic_passed: true,
                            bounces: overlay.bounces_for(p),
                        });

                        let snap = crate::tools::FileSnapshot::from_content(
                            cm.path.clone(),
                            cm.staged_content.clone(),
                        );
                        self.snapshot_store.snapshots.insert(cm.path.clone(), snap);
                        self.snapshot_store.staged_overlay.retain(|k, v| {
                            !Self::paths_equivalent(k, p) && !Self::paths_equivalent(&v.path, p)
                        });
                        self.turn_pin_cache.invalidate_path(p);
                        let joined_rel = project_root.join(&cm.relative_path);
                        if joined_rel != *p {
                            self.turn_pin_cache.invalidate_path(&joined_rel);
                        }
                    }
                }
                Err(err) => {
                    // Roll back already-written files and newly created directories so zero partial state remains!
                    for restore in &files_restore[..written_count] {
                        match &restore.prior_disk_content {
                            Some(prev) => {
                                let _ = fs::write(&restore.canonical_path, prev);
                            }
                            None => {
                                let _ = fs::remove_file(&restore.canonical_path);
                            }
                        }
                    }
                    let mut rev_dirs = created_dirs.clone();
                    rev_dirs.sort_by(|a, b| b.components().count().cmp(&a.components().count()));
                    for d in rev_dirs {
                        let _ = fs::remove_dir(&d);
                    }
                    return (err.to_string(), "failed".to_string(), false);
                }
            }
        }

        let undo_id_opt = if !committed_files_meta.is_empty() || !created_dirs.is_empty() {
            let undo_id = prospective_undo_id;
            self.next_delegation_undo_seq = self.next_delegation_undo_seq.saturating_add(1);

            for p in &overlay.touched_files {
                self.last_delegation_touched_paths.push(p.clone());
            }
            for d in &overlay.virtual_dirs {
                self.last_delegation_touched_paths.push(d.clone());
            }

            let committed_resolved_paths: Vec<String> = if !touched_resolved_strings.is_empty() {
                touched_resolved_strings
            } else {
                created_dirs
                    .iter()
                    .map(|d| d.to_string_lossy().to_string())
                    .collect()
            };
            self.record_mutation(
                thread_id,
                call_id,
                "delegate_coding_subagent",
                committed_resolved_paths,
                "completed",
                None,
            );

            while self.delegation_undo_log.len()
                >= crate::subagent_coding::MAX_DELEGATION_UNDO_RECORDS
            {
                self.delegation_undo_log.pop_front();
            }
            self.delegation_undo_log
                .push_back(crate::subagent_coding::DelegationUndoRecord {
                    undo_id: undo_id.clone(),
                    thread_id: thread_id.to_string(),
                    timestamp: now_unix_secs(),
                    project_root: project_root.to_path_buf(),
                    git_snapshot_sha,
                    files: files_restore,
                    created_dirs,
                    undone: false,
                });
            self.persist_delegation_undo_state();
            Some(undo_id)
        } else {
            None
        };

        let meta = crate::subagent_coding::DelegationResultMeta {
            undo_id: undo_id_opt,
            protocol_used: tracker.active.as_str().to_string(),
            fallback_triggered: tracker.fallback_triggered,
            steps: steps_executed,
            max_steps,
            status: "committed".to_string(),
            escalation_reason: None,
            summary,
            proposal_ids: Vec::new(),
            files: committed_files_meta,
        };
        let sub_model = self.effective_subagent_model_id();
        (
            crate::subagent_coding::format_delegation_result_output(
                &sub_model,
                &meta,
                prefix_cache_reused,
                decode_tok_s,
                &detail_lines,
            ),
            "completed".to_string(),
            false,
        )
    }

    pub(super) fn apply_surgical_edit(path: &Path, old_text: &str, new_text: &str) -> Result<String, String> {
        let content =
            fs::read_to_string(path).map_err(|e| format!("IO_ERROR reading {}: {e}", path.display()))?;
        let matches: Vec<_> = content.match_indices(old_text).collect();
        if matches.is_empty() {
            return Err(format!("NOT_FOUND: Target block not found in {}", path.display()));
        }
        if matches.len() > 1 {
            return Err(format!(
                "AMBIGUOUS_EDIT: Target block matched {} times in {}",
                matches.len(),
                path.display()
            ));
        }
        let updated = content.replacen(old_text, new_text, 1);
        fs::write(path, &updated)
            .map_err(|e| format!("IO_ERROR writing {}: {e}", path.display()))?;
        Ok(format!(
            "Surgically updated {} ({} -> {} bytes)",
            path.display(),
            content.len(),
            updated.len()
        ))
    }

    pub(super) fn list_dir_formatted(dir: &Path) -> Result<String, String> {
        let entries =
            fs::read_dir(dir).map_err(|e| format!("IO_ERROR reading dir {}: {e}", dir.display()))?;
        let mut rows = Vec::new();
        for entry in entries.flatten() {
            let p = entry.path();
            let name = entry.file_name().to_string_lossy().to_string();
            if p.is_dir() {
                rows.push(format!("[DIR]  {name}/"));
            } else {
                let sz = entry.metadata().map(|m| m.len()).unwrap_or(0);
                rows.push(format!("[FILE] {name} ({sz} bytes)"));
            }
        }
        rows.sort();
        Ok(rows.join("\n"))
    }

    /// Maximum `find_path` results before the listing is truncated.
    const FIND_PATH_MAX_RESULTS: usize = 2_000;

    pub(super) fn find_paths_under(root: &Path, needle: &str) -> Vec<String> {
        let needle_lower = needle
            .trim_matches('*')
            .to_ascii_lowercase();
        let mut out = Vec::new();
        let mut scanned = 0usize;
        let mut truncated = false;
        for entry in walkdir::WalkDir::new(root)
            .into_iter()
            .filter_entry(|e| {
                let name = e.file_name().to_string_lossy();
                name != ".git" && name != "target" && name != ".build"
            })
            .flatten()
        {
            if entry.path() == root {
                continue;
            }
            scanned += 1;
            let file_name = entry.file_name().to_string_lossy().to_ascii_lowercase();
            if needle_lower.is_empty() || file_name.contains(&needle_lower) {
                if out.len() >= Self::FIND_PATH_MAX_RESULTS {
                    truncated = true;
                    continue;
                }
                out.push(entry.path().to_string_lossy().into_owned());
            }
        }
        out.sort();
        let footer = if truncated {
            format!(
                "[find_path: scanned {scanned} entries; showing first {} matches (truncated) — narrow the pattern or path]",
                out.len()
            )
        } else {
            format!("[find_path: scanned {scanned} entries, {} matches]", out.len())
        };
        out.push(footer);
        out
    }

    pub(super) fn grep_under(root: &Path, pattern: &str, max_results: usize) -> Result<String, String> {
        let (re, literal_fallback) = match regex::Regex::new(pattern) {
            Ok(r) => (r, false),
            Err(_) => (
                regex::Regex::new(&regex::escape(pattern))
                    .map_err(|e| format!("Invalid regex '{pattern}': {e}"))?,
                true,
            ),
        };
        let mut matches = Vec::new();
        let mut files_scanned = 0usize;
        let mut unreadable = 0usize;
        let mut truncated = false;
        'walk: for entry in walkdir::WalkDir::new(root)
            .into_iter()
            .filter_entry(|e| {
                let name = e.file_name().to_string_lossy();
                name != ".git" && name != "target" && name != ".build"
            })
            .flatten()
        {
            if entry.file_type().is_file() {
                let bytes = match fs::read(entry.path()) {
                    Ok(b) => b,
                    Err(_) => {
                        unreadable += 1;
                        continue;
                    }
                };
                if bytes.contains(&0u8) {
                    continue; // binary file
                }
                let Ok(content) = String::from_utf8(bytes) else {
                    unreadable += 1;
                    continue;
                };
                files_scanned += 1;
                for (idx, line) in content.lines().enumerate() {
                    if re.is_match(line) {
                        if matches.len() >= max_results {
                            truncated = true;
                            break 'walk;
                        }
                        matches.push(format!(
                            "{}:{}:{}",
                            entry.path().display(),
                            idx + 1,
                            line
                        ));
                    }
                }
            }
        }
        let mut notes = Vec::new();
        if literal_fallback {
            notes.push("pattern was not a valid regex; searched it literally".to_string());
        }
        if truncated {
            notes.push(format!("results truncated at {max_results} matches — narrow the pattern or path"));
        }
        if unreadable > 0 {
            notes.push(format!("{unreadable} file(s) skipped (unreadable or non-UTF-8)"));
        }
        if matches.is_empty() || !notes.is_empty() {
            notes.insert(0, format!("{} matches in {files_scanned} files", matches.len()));
            matches.push(format!("[grep: {}]", notes.join("; ")));
        }
        Ok(matches.join("\n"))
    }

    // ========================================================================
    // 2. Local Control-Plane Actions (Context Reset & SQLite Wall-Clock Timers)

    /// Runs a prefill context subagent completion with deterministic retry on empty or leaked tool-call output.
    pub(super) fn run_prefill_subagent_with_empty_body_fallback(
        tr: &dyn EngineTransport,
        req: &SubagentCompletionRequest,
    ) -> Result<SubagentCompletionResponse, TransportError> {
        let resp = tr.run_subagent_completion(req)?;
        let is_length_truncated =
            matches!(resp.finish_reason.as_deref(), Some("length" | "max_tokens"));
        let cleaned = sanitize_prefill_context_body(&resp.content);
        if !cleaned.trim().is_empty() || is_length_truncated {
            return Ok(resp);
        }

        // Deterministic 1-shot plain-markdown retry when the isolated prefill subagent
        // produced no prose body (e.g. drifted into a disabled `<tool_call>` on cross-file symbols).
        let mut retry_req = req.clone();
        retry_req.temperature = Some(0.0);
        retry_req.enable_thinking = Some(false);
        retry_req.thinking_budget = None;
        retry_req.reasoning_effort = None;
        retry_req.prompt_cache_key = req
            .prompt_cache_key
            .as_ref()
            .map(|k| format!("{k}:retry1"));
        if let Some(last_msg) = retry_req.messages.last_mut() {
            let content = last_msg.content.get_or_insert_with(String::new);
            content.push_str(
                "\n\n[RETRY DIRECTIVE: Your previous response contained no plain-text summary \
                 (tool calls are strictly disabled for isolated prefill subagents, and external \
                 files cannot be read). Summarize ONLY the inlined <workspace_files> above in concise \
                 markdown bullets citing exact file paths, backticked symbols, and 1-indexed line \
                 numbers (e.g. L1-L9). Output prose/bullets immediately with ZERO <tool_call> or \
                 <function=...> tags.]",
            );
        }
        tr.run_subagent_completion(&retry_req)
    }

    pub(super) fn format_prefill_context_packet(
        resp: &SubagentCompletionResponse,
        files_read: usize,
        approx_tokens: usize,
    ) -> String {
        let prefill_tokens = resp.prompt_tokens.max(approx_tokens as u32);
        let finish_reason = resp.finish_reason.as_deref().unwrap_or("stop");
        let is_truncated = matches!(finish_reason, "length" | "max_tokens");
        let trunc_attrs = if is_truncated {
            format!(" truncated=\"true\" finish_reason=\"{finish_reason}\"")
        } else {
            String::new()
        };
        let trunc_warning = if is_truncated {
            "\n[WARNING: Subagent output hit max_tokens limit and was truncated; narrow paths/query or increase max_summary_tokens (up to 4096) before relying on partial context.]"
        } else {
            ""
        };
        let cleaned_body = sanitize_prefill_context_body(&resp.content);
        let model_label = if resp.model.trim().is_empty() {
            "subagent"
        } else {
            resp.model.trim()
        };
        format!(
            "<subagent_context_packet model=\"{model_label}\" files_read=\"{files_read}\" prefill_tokens=\"{prefill_tokens}\" tok_s=\"{:.1}\"{trunc_attrs}>\n{}{trunc_warning}\n</subagent_context_packet>",
            resp.decode_tok_s,
            cleaned_body.trim()
        )
    }
}
