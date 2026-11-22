use super::turn_runner::{normalize_domain_entry, parse_url_domain_and_repo_key};
use super::*;

impl SpellCoreSession {
    /// Materializes cached `SubagentCodingProposal` fields (`path`, `edits`, `old_text`, `new_text`)
    /// onto `UiToolCallItem` (`name = "edit_file"`) for Zero-UI-Change confirmation cards and
    /// `+added/-removed` diff stats while keeping compact `wire_messages` untouched.
    pub(crate) fn build_ui_tool_call_item(
        &self,
        call_id: &str,
        tool_name: &str,
        raw_arguments_json: &str,
        result_preview: Option<String>,
        status: impl Into<String>,
        is_mcp: bool,
    ) -> UiToolCallItem {
        if tool_name == "fetch" {
            let parsed_args = serde_json::from_str::<Value>(raw_arguments_json).ok();
            let req_url = parsed_args
                .as_ref()
                .and_then(|v| v.get("url").and_then(|u| u.as_str()))
                .map(str::trim);
            let citation_opt = self
                .last_fetch_citations
                .get(call_id)
                .or_else(|| req_url.and_then(|u| self.last_fetch_citations.get(u)));
            if let Some(citation) = citation_opt {
                let mut enriched_args = parsed_args.unwrap_or_else(|| json!({}));
                if let Some(obj) = enriched_args.as_object_mut() {
                    obj.insert("citation".to_string(), json!(citation));
                    obj.insert("title".to_string(), json!(citation.title));
                    obj.insert("domain".to_string(), json!(citation.domain));
                    obj.insert("tier_badge".to_string(), json!(citation.tier_badge));
                    obj.insert(
                        "cached_primary_path".to_string(),
                        json!(citation.cached_primary_path),
                    );
                    obj.insert("canonical_url".to_string(), json!(citation.canonical_url));
                    obj.insert(
                        "cached_file_count".to_string(),
                        json!(citation.cached_file_count),
                    );
                    obj.insert(
                        "est_total_tokens".to_string(),
                        json!(citation.est_total_tokens),
                    );
                }
                let enriched_output = result_preview.map(|out| {
                    if out.contains("<!-- spell_citation:") {
                        out
                    } else if let Ok(cit_json) = serde_json::to_string(citation) {
                        format!("<!-- spell_citation: {cit_json} -->\n{out}")
                    } else {
                        out
                    }
                });
                return UiToolCallItem::new(
                    call_id,
                    tool_name,
                    enriched_args.to_string(),
                    enriched_output,
                    status,
                    is_mcp,
                );
            }
        }
        if tool_name == "edit_file" || tool_name == "apply_subagent_proposal" {
            if let Ok(args_val) = serde_json::from_str::<Value>(raw_arguments_json) {
                if let Some(prop_id) = args_val
                    .get("proposal_id")
                    .or_else(|| args_val.get("proposalId"))
                    .and_then(|v| v.as_str())
                    .map(str::trim)
                    .filter(|s| !s.is_empty())
                {
                    if let Some(prop) = self
                        .proposal_cache
                        .iter()
                        .rev()
                        .find(|p| p.proposal_id == prop_id)
                    {
                        let selected = Self::select_proposal_hunks(&prop.edits, &args_val)
                            .unwrap_or_else(|_| prop.edits.clone());
                        let edits_json: Vec<Value> = selected
                            .iter()
                            .map(|h| {
                                json!({
                                    "old_text": h.old_text,
                                    "new_text": h.new_text,
                                })
                            })
                            .collect();
                        let first_old = selected
                            .first()
                            .map(|h| h.old_text.clone())
                            .unwrap_or_default();
                        let first_new = selected
                            .first()
                            .map(|h| h.new_text.clone())
                            .unwrap_or_default();
                        let desc = args_val
                            .get("display_description")
                            .or_else(|| args_val.get("displayDescription"))
                            .and_then(|v| v.as_str())
                            .map(str::to_string)
                            .unwrap_or_else(|| {
                                format!(
                                    "Apply cached subagent proposal {prop_id} ({} hunk(s) on {})",
                                    selected.len(),
                                    prop.path
                                )
                            });
                        let explicit_path = args_val
                            .get("path")
                            .and_then(|v| v.as_str())
                            .map(str::trim)
                            .filter(|s| !s.is_empty())
                            .unwrap_or(&prop.path);
                        let materialized = json!({
                            "proposal_id": prop_id,
                            "original_tool": tool_name,
                            "path": explicit_path,
                            "display_description": desc,
                            "edits": edits_json,
                            "old_text": first_old,
                            "new_text": first_new,
                        });
                        return UiToolCallItem::new(
                            call_id,
                            "edit_file",
                            &materialized.to_string(),
                            result_preview,
                            status,
                            is_mcp,
                        );
                    }
                }
            }
        }
        UiToolCallItem::new(
            call_id,
            tool_name,
            raw_arguments_json,
            result_preview,
            status,
            is_mcp,
        )
    }

    pub(super) fn is_parallelizable_subagent_tool(tool_name: &str) -> bool {
        matches!(
            tool_name,
            "prefill_context_subagent" | "delegate_coding_subagent"
        )
    }

    /// Executes `>= 2` contiguous `Allow` subagent calls (`prefill_context_subagent` / `delegate_coding_subagent`)
    /// via a 3-phase `std::thread::scope` pipeline:
    /// 1. Sequential pre-flight (`snapshot_store` context collection + unchanged `build_shared_prefix_subagent_prompt`
    ///    + distinct slot keys `subagent{idx % 4}:prefill|delegate:{base_tid}`).
    /// 2. Concurrent HTTP dispatch across `self.transport.run_subagent_completion`.
    /// 3. Deterministic post-flight in original `tool_calls` index order.
    pub(super) fn execute_parallel_subagent_batch(
        &mut self,
        thread: &mut StoredThread,
        project: &StoredProject,
        batch: &[WireToolCall],
    ) {
        enum SubagentPreFlight<'a> {
            ImmediateFailure {
                tc: &'a WireToolCall,
                error_msg: String,
            },
            ReadyPrefill {
                tc: &'a WireToolCall,
                sub_req: SubagentCompletionRequest,
                files_read: usize,
                approx_tokens: usize,
            },
            ReadyDelegatePropose {
                tc: &'a WireToolCall,
                sub_req: SubagentCompletionRequest,
                files_read: usize,
                target_files: Vec<String>,
            },
            ReadyDelegateAuto {
                tc: &'a WireToolCall,
                sub_req: SubagentCompletionRequest,
                slot_idx: usize,
                args: Value,
                target_files: Vec<String>,
                files_read: usize,
                max_steps: usize,
                tracker: crate::subagent_coding::ProtocolFallbackTracker,
                parent_cache_key: String,
                loop_messages: Vec<WireMessage>,
                aligned_tools: Option<Vec<Value>>,
            },
        }

        let project_root = Path::new(&project.root_path);
        let base_tid = derive_thread_key(Some(&thread.id));
        let fallback_epoch = thread.engine_compaction_epoch;

        // Phase 1: Sequential pre-flight
        let mut preflights: Vec<SubagentPreFlight<'_>> = Vec::with_capacity(batch.len());
        for (batch_pos, tc) in batch.iter().enumerate() {
            let slot_idx = batch_pos % 4;
            let tool_name = tc.function.name.trim();
            let args: Value =
                serde_json::from_str(&tc.function.arguments).unwrap_or_else(|_| json!({}));

            if tool_name == "prefill_context_subagent" {
                let paths: Vec<String> = match args.get("paths") {
                    Some(Value::Array(arr)) => arr
                        .iter()
                        .filter_map(|v| v.as_str().map(str::to_string))
                        .collect(),
                    Some(Value::String(single)) => vec![single.clone()],
                    _ => Vec::new(),
                };
                let query = args
                    .get("query")
                    .and_then(|v| v.as_str())
                    .unwrap_or("")
                    .trim()
                    .to_string();
                if query.is_empty() {
                    preflights.push(SubagentPreFlight::ImmediateFailure {
                        tc,
                        error_msg:
                            "INVALID_ARGUMENTS: prefill_context_subagent requires a non-empty 'query'."
                                .to_string(),
                    });
                    continue;
                }
                let max_summary_tokens = args
                    .get("max_summary_tokens")
                    .or_else(|| args.get("maxSummaryTokens"))
                    .and_then(|v| v.as_u64())
                    .map(|n| (n as u32).clamp(256, 4096))
                    .unwrap_or(2560);

                let extra_roots = self.extra_readable_roots.clone();
                let (files_read, approx_tokens, numbered_bundle) =
                    match collect_numbered_subagent_context_with_roots(
                        project_root,
                        &extra_roots,
                        &paths,
                        Some(&mut self.snapshot_store),
                        512_000,
                        true,
                    ) {
                        Ok(res) => {
                            self.record_workspace_reads_for_subagent_paths(
                                &thread.id,
                                project_root,
                                &paths,
                            );
                            res
                        }
                        Err(e) => {
                            preflights.push(SubagentPreFlight::ImmediateFailure {
                                tc,
                                error_msg: e.to_string(),
                            });
                            continue;
                        }
                    };

                let subagent_cache_key = format!("subagent{slot_idx}:prefill:{base_tid}");
                let sub_model = self.effective_subagent_model_id();
                let role_directive = "You are a read-only context prefill subagent for Spell IDE. \
                    Synthesize a dense, accurate technical summary with exact file paths and 1-indexed \
                    line numbers answering <query>. Never dump raw whole files.";
                let task_payload = format!(
                    "<query>\n{query}\n</query>\n\n<workspace_files>\n{numbered_bundle}\n</workspace_files>"
                );
                let (parent_cache_key, aligned_messages, aligned_tools) = self
                    .build_shared_prefix_subagent_prompt(
                        &thread.id,
                        fallback_epoch,
                        role_directive,
                        role_directive,
                        &task_payload,
                    );

                let sub_req = SubagentCompletionRequest {
                    model: sub_model,
                    messages: aligned_messages,
                    stream: false,
                    max_tokens: Some(max_summary_tokens),
                    temperature: Some(0.1),
                    prompt_cache_key: Some(subagent_cache_key),
                    parent_cache_key: Some(parent_cache_key),
                    reasoning_effort: None,
                    thinking_budget: None,
                    tools: aligned_tools,
                    enable_thinking: None,
                    ..Default::default()
                };

                preflights.push(SubagentPreFlight::ReadyPrefill {
                    tc,
                    sub_req,
                    files_read,
                    approx_tokens,
                });
            } else {
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
                    preflights.push(SubagentPreFlight::ImmediateFailure {
                        tc,
                        error_msg:
                            "INVALID_ARGUMENTS: delegate_coding_subagent requires a non-empty 'instruction'."
                                .to_string(),
                    });
                    continue;
                }
                let context_notes = args
                    .get("context_notes")
                    .or_else(|| args.get("contextNotes"))
                    .and_then(|v| v.as_str())
                    .map(str::trim)
                    .filter(|s| !s.is_empty());

                let mode = crate::subagent_coding::DelegationMode::from_args_and_instruction(
                    &args,
                    &instruction,
                );
                let subagent_cache_key = format!("subagent{slot_idx}:delegate:{base_tid}");

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
                                    &thread.id,
                                    project_root,
                                    &target_files,
                                );
                                res
                            }
                            Err(e) => {
                                preflights.push(SubagentPreFlight::ImmediateFailure {
                                    tc,
                                    error_msg: e.to_string(),
                                });
                                continue;
                            }
                        }
                    };

                    let mut user_prompt = format!("<instruction>\n{instruction}\n</instruction>\n");
                    if let Some(notes) = context_notes {
                        user_prompt
                            .push_str(&format!("\n<context_notes>\n{notes}\n</context_notes>\n"));
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
                            &thread.id,
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

                    preflights.push(SubagentPreFlight::ReadyDelegatePropose {
                        tc,
                        sub_req,
                        files_read,
                        target_files,
                    });
                } else {
                    let max_steps = crate::subagent_coding::clamp_delegation_max_steps(
                        args.get("max_steps")
                            .or_else(|| args.get("maxSteps"))
                            .and_then(|v| v.as_u64()),
                    );
                    let (files_read, task_payload) = match self
                        .prepare_autonomous_delegation_task_payload(
                            &thread.id,
                            project,
                            &target_files,
                            &instruction,
                            context_notes,
                        ) {
                        Ok(res) => res,
                        Err(err_msg) => {
                            preflights.push(SubagentPreFlight::ImmediateFailure {
                                tc,
                                error_msg: err_msg,
                            });
                            continue;
                        }
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
                            &thread.id,
                            fallback_epoch,
                            &directive,
                            tracker.active,
                            max_steps,
                            &task_payload,
                        );
                    let sub_model = self.effective_subagent_model_id();
                    let is_native =
                        tracker.active == crate::subagent_coding::ActiveStepProtocol::Native;
                    let sub_req = SubagentCompletionRequest {
                        model: sub_model,
                        messages: loop_messages.clone(),
                        stream: is_native,
                        max_tokens: Some(2048),
                        temperature: Some(0.1),
                        prompt_cache_key: Some(subagent_cache_key),
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

                    preflights.push(SubagentPreFlight::ReadyDelegateAuto {
                        tc,
                        sub_req,
                        slot_idx,
                        args,
                        target_files,
                        files_read,
                        max_steps,
                        tracker,
                        parent_cache_key,
                        loop_messages,
                        aligned_tools,
                    });
                }
            }
        }

        // Phase 2: Concurrent HTTP dispatch via std::thread::scope
        let transport = self.transport.clone();
        let mut results: Vec<Option<Result<SubagentCompletionResponse, TransportError>>> =
            (0..preflights.len()).map(|_| None).collect();

        std::thread::scope(|s| {
            let mut handles = Vec::new();
            for (pos, item) in preflights.iter().enumerate() {
                match item {
                    SubagentPreFlight::ReadyPrefill { sub_req, .. } => {
                        let tr = transport.clone();
                        handles.push((
                            pos,
                            s.spawn(move || {
                                Self::run_prefill_subagent_with_empty_body_fallback(
                                    tr.as_ref(),
                                    sub_req,
                                )
                            }),
                        ));
                    }
                    SubagentPreFlight::ReadyDelegatePropose { sub_req, .. }
                    | SubagentPreFlight::ReadyDelegateAuto { sub_req, .. } => {
                        let tr = transport.clone();
                        handles.push((pos, s.spawn(move || tr.run_subagent_completion(sub_req))));
                    }
                    SubagentPreFlight::ImmediateFailure { .. } => {}
                }
            }
            for (pos, handle) in handles {
                let outcome = handle.join().unwrap_or_else(|_| {
                    Err(TransportError::Message(
                        "Parallel subagent worker thread panicked".to_string(),
                    ))
                });
                results[pos] = Some(outcome);
            }
        });

        // Phase 3: Deterministic post-flight in original tool_calls index order
        let mut batch_committed_paths: HashSet<std::path::PathBuf> = HashSet::new();
        self.active_batch_committed_paths.clear();

        for (pos, preflight) in preflights.into_iter().enumerate() {
            let (tc, output, status) = match preflight {
                SubagentPreFlight::ImmediateFailure { tc, error_msg } => {
                    (tc, error_msg, "failed".to_string())
                }
                SubagentPreFlight::ReadyPrefill {
                    tc,
                    files_read,
                    approx_tokens,
                    ..
                } => match results[pos].take() {
                    Some(Ok(resp)) => {
                        self.record_live_subagent_completion(&resp);
                        let packet =
                            Self::format_prefill_context_packet(&resp, files_read, approx_tokens);
                        (tc, packet, "completed".to_string())
                    }
                    Some(Err(err)) => (tc, err.to_string(), "failed".to_string()),
                    None => (
                        tc,
                        "Internal error: missing parallel subagent result".to_string(),
                        "failed".to_string(),
                    ),
                },
                SubagentPreFlight::ReadyDelegatePropose {
                    tc,
                    files_read,
                    target_files,
                    ..
                } => match results[pos].take() {
                    Some(Ok(resp)) => {
                        self.record_live_subagent_completion(&resp);
                        let cached = self.extract_and_cache_subagent_proposals(
                            project_root,
                            &target_files,
                            &resp.content,
                        );
                        let proposal =
                            Self::format_coding_proposal_packet(&resp, files_read, &cached);
                        (tc, proposal, "completed".to_string())
                    }
                    Some(Err(err)) => (tc, err.to_string(), "failed".to_string()),
                    None => (
                        tc,
                        "Internal error: missing parallel subagent result".to_string(),
                        "failed".to_string(),
                    ),
                },
                SubagentPreFlight::ReadyDelegateAuto {
                    tc,
                    slot_idx,
                    args,
                    target_files,
                    files_read,
                    max_steps,
                    tracker,
                    parent_cache_key,
                    loop_messages,
                    aligned_tools,
                    ..
                } => match results[pos].take() {
                    Some(Ok(step0_resp)) => {
                        let extra_roots = self.extra_readable_roots.clone();
                        let mut overlapping_seed: Option<String> = None;
                        for raw_tf in &target_files {
                            let trimmed = raw_tf.trim();
                            if trimmed.is_empty() {
                                continue;
                            }
                            if let Ok(res_tf) = resolve_workspace_path_with_roots(
                                project_root,
                                &extra_roots,
                                trimmed,
                            ) {
                                if batch_committed_paths.contains(&res_tf)
                                    || batch_committed_paths
                                        .iter()
                                        .any(|existing| Self::paths_equivalent(existing, &res_tf))
                                {
                                    overlapping_seed = Some(trimmed.to_string());
                                    break;
                                }
                            }
                        }

                        if let Some(conflict_file) = overlapping_seed {
                            self.record_live_subagent_completion(&step0_resp);
                            let empty_overlay = crate::subagent_coding::DelegationOverlay::new();
                            let conflict_summary = format!(
                                "CONFLICT_WARNING: Parallel batch sibling delegation already modified '{conflict_file}' earlier in this turn; escalating rather than applying edits from a stale Step-0 snapshot."
                            );
                            let (out, st, _) = self.finalize_delegation_escalation(
                                &empty_overlay,
                                &tracker,
                                1,
                                max_steps,
                                "mutation_conflict",
                                conflict_summary,
                                None,
                                step0_resp.prefix_cache_reused || step0_resp.cached_tokens > 0,
                                step0_resp.decode_tok_s,
                            );
                            (tc, out, st)
                        } else {
                            self.active_batch_committed_paths = batch_committed_paths.clone();
                            let (out, st, _) = self.continue_autonomous_coding_delegation(
                                &thread.id,
                                &tc.id,
                                project,
                                &args,
                                &target_files,
                                files_read,
                                max_steps,
                                slot_idx,
                                tracker,
                                parent_cache_key,
                                loop_messages,
                                aligned_tools,
                                Some(step0_resp),
                            );
                            for touched in self.last_delegation_touched_paths.drain(..) {
                                batch_committed_paths.insert(touched);
                            }
                            self.active_batch_committed_paths = batch_committed_paths.clone();
                            (tc, out, st)
                        }
                    }
                    Some(Err(err)) => (tc, err.to_string(), "failed".to_string()),
                    None => (
                        tc,
                        "Internal error: missing parallel subagent result".to_string(),
                        "failed".to_string(),
                    ),
                },
            };

            let tool_name = tc.function.name.trim();
            let item = self.build_ui_tool_call_item(
                &tc.id,
                tool_name,
                &tc.function.arguments,
                Some(output.clone()),
                status,
                false,
            );
            thread.ui_events.push(UiChatEvent::ToolCallCard(item));
            thread.wire_messages.push(WireMessage::tool(
                &tc.id,
                Some(tool_name.to_string()),
                output,
            ));
        }
        self.active_batch_committed_paths.clear();
    }

    pub(super) fn process_tool_calls_batch(
        &mut self,
        thread: &mut StoredThread,
        project: &StoredProject,
        profile: AgentProfile,
        tool_calls: &[WireToolCall],
    ) -> Result<bool, String> {
        let project_root = Path::new(&project.root_path);
        let mut idx = 0usize;
        while idx < tool_calls.len() {
            let mut parallel_end = idx;
            while parallel_end < tool_calls.len() {
                let candidate_name = tool_calls[parallel_end].function.name.trim();
                if Self::is_parallelizable_subagent_tool(candidate_name)
                    && self.effective_tool_permission(
                        candidate_name,
                        profile,
                        project.is_read_only,
                    ) == ToolPermissionPolicy::Allow
                {
                    parallel_end += 1;
                } else {
                    break;
                }
            }
            if parallel_end - idx >= 2 {
                self.execute_parallel_subagent_batch(
                    thread,
                    project,
                    &tool_calls[idx..parallel_end],
                );
                idx = parallel_end;
                continue;
            }

            let tc = &tool_calls[idx];
            let tool_name = tc.function.name.trim();
            let policy =
                self.effective_tool_permission(tool_name, profile, project.is_read_only);

            match policy {
                ToolPermissionPolicy::Confirm => {
                    let item = self.build_ui_tool_call_item(
                        &tc.id,
                        tool_name,
                        &tc.function.arguments,
                        None,
                        "awaiting_confirmation",
                        Self::is_mcp_tool_name(tool_name),
                    );
                    thread.ui_events.push(UiChatEvent::ToolCallCard(item));
                    self.pending_confirmations.push(PendingToolConfirmation {
                        call_id: tc.id.clone(),
                        thread_id: thread.id.clone(),
                        tool_name: tool_name.to_string(),
                        arguments_json: tc.function.arguments.clone(),
                        remaining_tool_calls: tool_calls[idx + 1..].to_vec(),
                        created_at: now_unix_secs(),
                        confirmation_kind: None,
                        target_domain: None,
                        reason: None,
                    });
                    return Ok(true);
                }
                ToolPermissionPolicy::Deny => {
                    let msg = format!(
                        "PERMISSION_DENIED: Tool '{tool_name}' is blocked by tool permission policy or read-only/Ask profile."
                    );
                    let item = self.build_ui_tool_call_item(
                        &tc.id,
                        tool_name,
                        &tc.function.arguments,
                        Some(msg.clone()),
                        "denied",
                        Self::is_mcp_tool_name(tool_name),
                    );
                    thread.ui_events.push(UiChatEvent::ToolCallCard(item));
                    thread.wire_messages.push(WireMessage::tool(
                        &tc.id,
                        Some(tool_name.to_string()),
                        msg,
                    ));
                }
                ToolPermissionPolicy::Allow => {
                    let args_val: Value = serde_json::from_str(&tc.function.arguments)
                        .unwrap_or_else(|_| json!({}));

                    // Web-tainted session attempting to modify auto-loaded workspace rule/hook file
                    if self.web_tainted_threads.contains(&thread.id) {
                        if let Some(sensitive_rel) =
                            self.find_web_taint_sensitive_target(project_root, tool_name, &args_val)
                        {
                            let hold_msg = format!(
                                "SECURITY_HOLD: Web-tainted session attempted to modify protected workspace rule/build-hook file '{sensitive_rel}'. Explicit user confirmation required."
                            );
                            let item = self.build_ui_tool_call_item(
                                &tc.id,
                                tool_name,
                                &tc.function.arguments,
                                Some(hold_msg.clone()),
                                "awaiting_confirmation",
                                Self::is_mcp_tool_name(tool_name),
                            );
                            thread.ui_events.push(UiChatEvent::ToolCallCard(item));
                            self.pending_confirmations.push(PendingToolConfirmation {
                                call_id: tc.id.clone(),
                                thread_id: thread.id.clone(),
                                tool_name: tool_name.to_string(),
                                arguments_json: tc.function.arguments.clone(),
                                remaining_tool_calls: tool_calls[idx + 1..].to_vec(),
                                created_at: now_unix_secs(),
                                confirmation_kind: Some(
                                    "web_taint_sensitive_file".to_string(),
                                ),
                                target_domain: None,
                                reason: Some(hold_msg),
                            });
                            return Ok(true);
                        }
                    }

                    if tool_name == "fetch" {
                        let raw_url = args_val
                            .get("url")
                            .and_then(|v| v.as_str())
                            .unwrap_or("");
                        match self.execute_fetch_for_session_internal(
                            &thread.id,
                            &thread.wire_messages,
                            project,
                            &tc.id,
                            raw_url,
                            false,
                        ) {
                            Ok(envelope) => {
                                let item = self.build_ui_tool_call_item(
                                    &tc.id,
                                    tool_name,
                                    &tc.function.arguments,
                                    Some(envelope.clone()),
                                    "completed",
                                    false,
                                );
                                thread.ui_events.push(UiChatEvent::ToolCallCard(item));
                                thread.wire_messages.push(WireMessage::tool(
                                    &tc.id,
                                    Some(tool_name.to_string()),
                                    envelope,
                                ));
                            }
                            Err((Some((domain, reason, kind)), card_msg)) => {
                                let item = self.build_ui_tool_call_item(
                                    &tc.id,
                                    tool_name,
                                    &tc.function.arguments,
                                    Some(card_msg),
                                    "awaiting_confirmation",
                                    false,
                                );
                                thread.ui_events.push(UiChatEvent::ToolCallCard(item));
                                self.pending_confirmations.push(PendingToolConfirmation {
                                    call_id: tc.id.clone(),
                                    thread_id: thread.id.clone(),
                                    tool_name: tool_name.to_string(),
                                    arguments_json: tc.function.arguments.clone(),
                                    remaining_tool_calls: tool_calls[idx + 1..].to_vec(),
                                    created_at: now_unix_secs(),
                                    confirmation_kind: Some(kind.to_string()),
                                    target_domain: Some(domain),
                                    reason: Some(reason),
                                });
                                return Ok(true);
                            }
                            Err((None, err_msg)) => {
                                let item = self.build_ui_tool_call_item(
                                    &tc.id,
                                    tool_name,
                                    &tc.function.arguments,
                                    Some(err_msg.clone()),
                                    "failed",
                                    false,
                                );
                                thread.ui_events.push(UiChatEvent::ToolCallCard(item));
                                thread.wire_messages.push(WireMessage::tool(
                                    &tc.id,
                                    Some(tool_name.to_string()),
                                    err_msg,
                                ));
                            }
                        }
                        idx += 1;
                        continue;
                    }

                    let (output, status, is_mcp) = self.execute_single_tool_call(
                        &thread.id,
                        &mut thread.active_tags,
                        project,
                        profile,
                        tc,
                    );
                    let item = self.build_ui_tool_call_item(
                        &tc.id,
                        tool_name,
                        &tc.function.arguments,
                        Some(output.clone()),
                        status,
                        is_mcp,
                    );
                    thread.ui_events.push(UiChatEvent::ToolCallCard(item));
                    thread.wire_messages.push(WireMessage::tool(
                        &tc.id,
                        Some(tool_name.to_string()),
                        output,
                    ));
                    if self.loop_state == LoopState::PausedOnCriticReject {
                        self.sync_critic_circuit_breaker_banner(thread);
                        break;
                    }
                }
            }
            idx += 1;
        }
        Ok(false)
    }

    /// Approves or denies a pending tool call (`ToolPermissionPolicy::Confirm`) and resumes the agent loop.
    pub fn approve_tool_call(
        &mut self,
        call_id: &str,
        approved: bool,
    ) -> Result<StoredThread, String> {
        self.confirm_tool_call(call_id, if approved { "allow_once" } else { "deny" }, None)
    }

    /// Resolves a pending tool or domain/taint confirmation with scope
    /// `"allow_once"` (`"once"`), `"allow_session"` (`"session"`), `"always_allow"` (`"always"`), or `"deny"`.
    pub fn confirm_tool_call(
        &mut self,
        call_id: &str,
        decision: &str,
        explicit_domain: Option<&str>,
    ) -> Result<StoredThread, String> {
        let pos = self
            .pending_confirmations
            .iter()
            .position(|p| p.call_id == call_id)
            .ok_or_else(|| format!("No pending tool confirmation found for call_id '{call_id}'"))?;
        let pending = self.pending_confirmations.remove(pos);

        let normalized_decision = match decision.trim().to_ascii_lowercase().as_str() {
            "always" | "always_allow" | "allow_always" => "always_allow",
            "session" | "allow_session" => "allow_session",
            "once" | "allow_once" | "approve" | "approved" | "true" => "allow_once",
            _ => "deny",
        };
        let approved = normalized_decision != "deny";

        let mut thread = self
            .storage
            .get_thread(&pending.thread_id)?
            .ok_or_else(|| format!("Thread '{}' not found", pending.thread_id))?;
        let project = self
            .storage
            .get_project(&thread.project_id)?
            .unwrap_or_else(|| {
                StoredProject::new(
                    &thread.project_id,
                    "Workspace",
                    self.locator
                        .primary_workspace_root()
                        .to_string_lossy()
                        .into_owned(),
                    false,
                )
            });
        let profile = if project.is_read_only || thread.profile.eq_ignore_ascii_case("ask") {
            AgentProfile::Ask
        } else {
            AgentProfile::Write
        };

        let mut single_shot_domain_to_clear: Option<String> = None;
        if approved && pending.tool_name == "fetch" {
            let args_val: Value =
                serde_json::from_str(&pending.arguments_json).unwrap_or_else(|_| json!({}));
            let raw_url = args_val.get("url").and_then(|v| v.as_str()).unwrap_or("");
            let candidate_domain = explicit_domain
                .and_then(normalize_domain_entry)
                .or_else(|| pending.target_domain.as_deref().and_then(normalize_domain_entry))
                .or_else(|| parse_url_domain_and_repo_key(raw_url).map(|(d, _)| d));
            if let Some(dom) = candidate_domain {
                match normalized_decision {
                    "always_allow" => {
                        self.online_research_config
                            .custom_allowed_domains
                            .insert(dom.clone());
                        self.online_research_config
                            .session_allowed_domains
                            .insert(dom);
                    }
                    "allow_session" => {
                        self.online_research_config
                            .session_allowed_domains
                            .insert(dom);
                    }
                    _ => {
                        self.online_research_config
                            .once_allowed_domains
                            .insert(dom.clone());
                        single_shot_domain_to_clear = Some(dom);
                    }
                }
            }
        }

        let tc = WireToolCall {
            id: pending.call_id.clone(),
            r#type: "function".to_string(),
            function: WireToolFunction {
                name: pending.tool_name.clone(),
                arguments: pending.arguments_json.clone(),
            },
            index: Some(0),
        };

        let (output, status, is_mcp) = if approved {
            self.confirmed_tool_call_ids.insert(pending.call_id.clone());
            let out = self.execute_single_tool_call(
                &thread.id,
                &mut thread.active_tags,
                &project,
                profile,
                &tc,
            );
            self.confirmed_tool_call_ids.remove(&pending.call_id);
            if let Some(dom) = single_shot_domain_to_clear {
                self.online_research_config
                    .once_allowed_domains
                    .remove(&dom);
            }
            out
        } else {
            (
                format!(
                    "USER_DENIED: Tool '{}' execution was rejected by the user.",
                    pending.tool_name
                ),
                "denied".to_string(),
                Self::is_mcp_tool_name(&pending.tool_name),
            )
        };

        // Update the existing `awaiting_confirmation` card in `thread.ui_events`
        let updated_card = self.build_ui_tool_call_item(
            &pending.call_id,
            &pending.tool_name,
            &pending.arguments_json,
            Some(output.clone()),
            &status,
            is_mcp,
        );
        if let Some(ev) = thread.ui_events.iter_mut().find(|e| {
            e.tool_call
                .as_ref()
                .map(|c| c.id == pending.call_id)
                .unwrap_or(false)
        }) {
            ev.content = output.clone();
            ev.tool_call = Some(updated_card);
        } else {
            thread.ui_events.push(UiChatEvent::ToolCallCard(updated_card));
        }

        thread.wire_messages.push(WireMessage::tool(
            &pending.call_id,
            Some(pending.tool_name.clone()),
            output,
        ));

        if self.loop_state == LoopState::PausedOnCriticReject {
            self.sync_critic_circuit_breaker_banner(&mut thread);
            thread.updated_at = now_unix_secs();
            self.storage.upsert_thread(&thread)?;
            return Ok(thread);
        }

        // Process any remaining tool calls from the same batch
        let paused_again = self.process_tool_calls_batch(
            &mut thread,
            &project,
            profile,
            &pending.remaining_tool_calls,
        )?;
        if self.loop_state == LoopState::PausedOnCriticReject {
            self.sync_critic_circuit_breaker_banner(&mut thread);
        }
        thread.updated_at = now_unix_secs();
        self.storage.upsert_thread(&thread)?;

        if !paused_again && self.loop_state != LoopState::PausedOnCriticReject {
            self.run_agent_continuation_loop(&mut thread, DEFAULT_MAX_AGENT_STEPS)?;
        }
        Ok(thread)
    }

    pub(super) fn is_mcp_tool_name(name: &str) -> bool {
        name.starts_with("mcp_")
            || matches!(
                name,
                "read_full_file"
                    | "read_multiple_files"
                    | "grep_workspace"
                    | "file_stats"
                    | "fetch_html"
                    | "timer_set"
                    | "timer_wait"
                    | "timer_list"
                    | "timer_cancel"
                    | "progress_check"
            )
    }

    /// Executes a single native headless tool or MCP tool call and returns `(output, status, is_mcp)`.
    ///
    /// Wraps [`Self::execute_single_tool_call_inner`] with:
    /// - automatic retries (up to 2) for transient `io`/`timeout` failures on read-only tools,
    /// - a `[tool_error kind=... retryable=...]` trailer on failures (error taxonomy),
    /// - cross-conversation conflict warnings and a mutation journal for mutating tools.
    pub(super) fn execute_single_tool_call(
        &mut self,
        thread_id: &str,
        thread_active_tags: &mut Vec<String>,
        project: &StoredProject,
        profile: AgentProfile,
        tc: &WireToolCall,
    ) -> (String, String, bool) {
        let tool_name = tc.function.name.trim().to_string();
        let is_mutating = crate::tools::is_mutating_tool(&tool_name);
        let mutation_paths = if is_mutating {
            self.mutation_target_paths(project, tc)
        } else {
            Vec::new()
        };
        let conflict_warning = if is_mutating {
            self.detect_mutation_conflict(thread_id, &mutation_paths)
        } else {
            None
        };

        let retry_safe = crate::tools::is_auto_retry_safe_tool(&tool_name);
        let mut attempts = 0u32;
        let (mut output, status, is_mcp) = loop {
            attempts += 1;
            let res =
                self.execute_single_tool_call_inner(thread_id, thread_active_tags, project, profile, tc);
            let kind = crate::tools::classify_tool_error(&res.0, &res.1);
            let should_retry = retry_safe
                && attempts < 3
                && kind.map(crate::tools::is_retryable_error_kind).unwrap_or(false);
            if !should_retry {
                break res;
            }
            // Same tool_call_id must not short-circuit to a pinned failure on retry.
            self.turn_pin_cache.forget_tool_output(&tc.id);
            std::thread::sleep(std::time::Duration::from_millis(if attempts == 1 { 50 } else { 200 }));
        };

        if status != "cancelled" {
            if let Some(kind) = crate::tools::classify_tool_error(&output, &status) {
                output.push_str(&format!(
                    "\n[tool_error kind={kind} retryable={} attempts={attempts}]",
                    crate::tools::is_retryable_error_kind(kind)
                ));
            } else if attempts > 1 {
                output.push_str(&format!("\n[succeeded after {} retr{}]", attempts - 1, if attempts == 2 { "y" } else { "ies" }));
            }
        }

        if let Some(ref warning) = conflict_warning {
            output = format!("{warning}\n{output}");
        }

        if is_mutating && !matches!(status.as_str(), "failed" | "denied") {
            self.record_mutation(thread_id, &tc.id, &tool_name, mutation_paths, &status, conflict_warning);
        }

        (output, status, is_mcp)
    }

    /// Resolves the workspace paths a mutating tool call targets (best-effort, canonical when possible).
    pub(super) fn mutation_target_paths(&self, project: &StoredProject, tc: &WireToolCall) -> Vec<String> {
        let args: Value = serde_json::from_str(&tc.function.arguments).unwrap_or_else(|_| json!({}));
        let project_root = Path::new(&project.root_path);
        let mut out = Vec::new();
        for key in ["path", "source_path", "src", "destination_path", "dst"] {
            if let Some(raw) = args.get(key).and_then(|v| v.as_str()).map(str::trim) {
                if raw.is_empty() {
                    continue;
                }
                let p = resolve_workspace_path(project_root, raw)
                    .map(|r| r.to_string_lossy().replace('\\', "/"))
                    .unwrap_or_else(|_| raw.replace('\\', "/"));
                if !out.contains(&p) {
                    out.push(p);
                }
            }
        }
        out
    }

    /// Returns a warning when another conversation mutated any of `paths` within the conflict window.
    pub(super) fn detect_mutation_conflict(&self, thread_id: &str, paths: &[String]) -> Option<String> {
        let now = now_unix_secs();
        let mut warnings = Vec::new();
        for p in paths {
            if let Some(prev) = self.last_mutation_by_path.get(p) {
                let age = now.saturating_sub(prev.timestamp);
                if prev.thread_id != thread_id && age <= MUTATION_CONFLICT_WINDOW_SECS {
                    warnings.push(format!(
                        "CONFLICT_WARNING: '{p}' was modified {age}s ago by another conversation ({}) via {}. Re-read it before relying on earlier contents.",
                        prev.thread_id, prev.tool
                    ));
                }
            }
        }
        if warnings.is_empty() {
            None
        } else {
            Some(warnings.join("\n"))
        }
    }

    /// Appends a mutation to the bounded journal and updates the per-path index.
    pub(super) fn record_mutation(
        &mut self,
        thread_id: &str,
        call_id: &str,
        tool: &str,
        paths: Vec<String>,
        status: &str,
        conflict_warning: Option<String>,
    ) {
        let entry = MutationJournalEntry {
            timestamp: now_unix_secs(),
            thread_id: thread_id.to_string(),
            call_id: call_id.to_string(),
            tool: tool.to_string(),
            paths,
            status: status.to_string(),
            conflict_warning,
        };
        for p in &entry.paths {
            self.last_mutation_by_path.insert(p.clone(), entry.clone());
        }
        self.mutation_journal.push_back(entry);
        while self.mutation_journal.len() > MUTATION_JOURNAL_CAPACITY {
            self.mutation_journal.pop_front();
        }
    }

    /// JSON view of the mutation journal, optionally filtered by thread or path substring.
    pub fn mutation_journal_json(&self, thread_id: Option<&str>, path_contains: Option<&str>) -> Value {
        let entries: Vec<&MutationJournalEntry> = self
            .mutation_journal
            .iter()
            .filter(|e| thread_id.map(|t| e.thread_id == t).unwrap_or(true))
            .filter(|e| {
                path_contains
                    .map(|needle| {
                        let norm = needle.replace('\\', "/");
                        e.paths.iter().any(|p| p.contains(&norm))
                    })
                    .unwrap_or(true)
            })
            .collect();
        json!({ "ok": true, "count": entries.len(), "entries": entries })
    }

    pub(super) fn execute_single_tool_call_inner(
        &mut self,
        thread_id: &str,
        thread_active_tags: &mut Vec<String>,
        project: &StoredProject,
        profile: AgentProfile,
        tc: &WireToolCall,
    ) -> (String, String, bool) {
        let tool_name = tc.function.name.trim();
        let args: Value =
            serde_json::from_str(&tc.function.arguments).unwrap_or_else(|_| json!({}));
        let project_root = Path::new(&project.root_path);
        let user_confirmed = self.confirmed_tool_call_ids.contains(&tc.id);
        let web_tainted = self.web_tainted_threads.contains(thread_id);
        let force_staged_critic = self.stage_write_mode || web_tainted;

        if web_tainted && !user_confirmed {
            if let Some(sensitive_rel) =
                self.find_web_taint_sensitive_target(project_root, tool_name, &args)
            {
                return (
                    format!(
                        "SECURITY_BLOCK: Web-tainted session cannot modify protected workspace rule/build-hook file '{sensitive_rel}' without explicit user confirmation."
                    ),
                    "denied".to_string(),
                    false,
                );
            }
        }

        match tool_name {
            "read_file" => {
                let raw_path = args
                    .get("path")
                    .and_then(|v| v.as_str())
                    .unwrap_or("README.md");
                let start_line = args
                    .get("start_line")
                    .or_else(|| args.get("startLine"))
                    .and_then(|v| v.as_u64())
                    .map(|n| n as usize);
                let end_line = args
                    .get("end_line")
                    .or_else(|| args.get("endLine"))
                    .and_then(|v| v.as_u64())
                    .map(|n| n as usize);
                let expand_to_symbol = args
                    .get("expand_to_symbol")
                    .or_else(|| args.get("expandToSymbol"))
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);

                match resolve_workspace_path_with_roots(
                    project_root,
                    &self.extra_readable_roots,
                    raw_path,
                ) {
                    Ok(resolved) => {
                        let (start_line, end_line) = match (expand_to_symbol, start_line) {
                            (true, Some(s)) => self
                                .snapshot_store
                                .read_file_text(&resolved)
                                .ok()
                                .or_else(|| fs::read_to_string(&resolved).ok())
                                .and_then(|content| {
                                    crate::tools::enclosing_symbol_range(&content, s, end_line)
                                })
                                .map(|(a, b)| (Some(a), Some(b)))
                                .unwrap_or((start_line, end_line)),
                            _ => (start_line, end_line),
                        };
                        if self.snapshot_store.has_staged_mutation(&resolved) {
                            match self
                                .snapshot_store
                                .read_file_lines(&resolved, start_line, end_line)
                            {
                                Ok(content) => {
                                    self.record_workspace_read_if_outside_research_cache(
                                        thread_id, &resolved,
                                    );
                                    (content, "completed".to_string(), false)
                                }
                                Err(e) => (e.to_string(), "failed".to_string(), false),
                            }
                        } else {
                            match self.turn_pin_cache.read_file_pinned(
                                &tc.id,
                                &resolved,
                                start_line,
                                end_line,
                            ) {
                                Ok(content) => {
                                    self.record_workspace_read_if_outside_research_cache(
                                        thread_id, &resolved,
                                    );
                                    (content, "completed".to_string(), false)
                                }
                                Err(e) => (e.to_string(), "failed".to_string(), false),
                            }
                        }
                    }
                    Err(e) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "edit_file" => {
                if project.is_read_only {
                    return (
                        format!(
                            "READ_ONLY_PROJECT: Project '{}' is locked as read-only.",
                            project.name
                        ),
                        "denied".to_string(),
                        false,
                    );
                }
                if let Some(prop_id) = args
                    .get("proposal_id")
                    .or_else(|| args.get("proposalId"))
                    .and_then(|v| v.as_str())
                    .map(str::trim)
                    .filter(|s| !s.is_empty())
                {
                    let explicit_path = args
                        .get("path")
                        .and_then(|v| v.as_str())
                        .map(str::trim)
                        .filter(|s| !s.is_empty());
                    return self.apply_cached_proposal_edits(
                        thread_id,
                        project_root,
                        prop_id,
                        explicit_path,
                        &args,
                    );
                }
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or("").trim();
                if raw_path.is_empty() {
                    return (
                        "INVALID_ARGUMENTS: edit_file requires either 'path' or 'proposal_id'."
                            .to_string(),
                        "failed".to_string(),
                        false,
                    );
                }
                match resolve_workspace_path(project_root, raw_path) {
                    Ok(resolved) => {
                        if let Err(e) = enforce_research_cache_write_guard(&resolved) {
                            return (e.to_string(), "denied".to_string(), false);
                        }
                        if let Some(edits_arr) = args.get("edits").and_then(|v| v.as_array()) {
                            let mut current = match self
                                .snapshot_store
                                .read_file_text(&resolved)
                                .or_else(|_| {
                                    fs::read_to_string(&resolved).map_err(|e| {
                                        crate::tools::ToolError::IoError(e.to_string())
                                    })
                                }) {
                                Ok(c) => c,
                                Err(e) => {
                                    return (
                                        format!(
                                            "IO_ERROR: Failed to read {}: {e}",
                                            resolved.display()
                                        ),
                                        "failed".to_string(),
                                        false,
                                    )
                                }
                            };
                            let mut applied = 0usize;
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
                                    return ("INVALID_EDIT: old_text cannot be empty".to_string(), "failed".to_string(), false);
                                }
                                let count = current.matches(old_t).count();
                                if count == 0 {
                                    return (
                                        format!("NOT_FOUND: old_text not found in {}", resolved.display()),
                                        "failed".to_string(),
                                        false,
                                    );
                                }
                                if count > 1 {
                                    return (
                                        format!("AMBIGUOUS_EDIT: old_text matched {count} times in {}", resolved.display()),
                                        "failed".to_string(),
                                        false,
                                    );
                                }
                                current = current.replacen(old_t, new_t, 1);
                                applied += 1;
                            }
                            if force_staged_critic {
                                let op_label = format!(
                                    "Staged {applied} surgical edit(s) to {}",
                                    resolved.display()
                                );
                                match self.stage_mutation_with_live_critic(
                                    thread_id,
                                    &resolved,
                                    raw_path,
                                    current,
                                    &op_label,
                                ) {
                                    Ok(msg) => (msg, "completed".to_string(), false),
                                    Err(err_msg) => (err_msg, "failed".to_string(), false),
                                }
                            } else {
                                if let Err(e) = fs::write(&resolved, &current) {
                                    return (
                                        format!(
                                            "IO_ERROR: Failed to write {}: {e}",
                                            resolved.display()
                                        ),
                                        "failed".to_string(),
                                        false,
                                    );
                                }
                                self.turn_pin_cache.invalidate_path(&resolved);
                                (
                                    format!(
                                        "Applied {applied} surgical edit(s) to {}",
                                        resolved.display()
                                    ),
                                    "completed".to_string(),
                                    false,
                                )
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
                            if force_staged_critic {
                                let current = match self
                                    .snapshot_store
                                    .read_file_text(&resolved)
                                    .or_else(|_| {
                                        fs::read_to_string(&resolved).map_err(|e| {
                                            crate::tools::ToolError::IoError(e.to_string())
                                        })
                                    }) {
                                    Ok(c) => c,
                                    Err(e) => {
                                        return (
                                            format!(
                                                "IO_ERROR: Failed to read {}: {e}",
                                                resolved.display()
                                            ),
                                            "failed".to_string(),
                                            false,
                                        )
                                    }
                                };
                                if old_t.is_empty() {
                                    return (
                                        "INVALID_EDIT: old_text cannot be empty".to_string(),
                                        "failed".to_string(),
                                        false,
                                    );
                                }
                                let count = current.matches(old_t).count();
                                if count == 0 {
                                    return (
                                        format!(
                                            "NOT_FOUND: Target block not found in {}",
                                            resolved.display()
                                        ),
                                        "failed".to_string(),
                                        false,
                                    );
                                }
                                if count > 1 {
                                    return (
                                        format!(
                                            "AMBIGUOUS_EDIT: Target block matched {count} times in {}",
                                            resolved.display()
                                        ),
                                        "failed".to_string(),
                                        false,
                                    );
                                }
                                let updated = current.replacen(old_t, new_t, 1);
                                let op_label = format!(
                                    "Staged surgical edit(s) to {}",
                                    resolved.display()
                                );
                                match self.stage_mutation_with_live_critic(
                                    thread_id,
                                    &resolved,
                                    raw_path,
                                    updated,
                                    &op_label,
                                ) {
                                    Ok(msg) => (msg, "completed".to_string(), false),
                                    Err(err_msg) => (err_msg, "failed".to_string(), false),
                                }
                            } else {
                                match Self::apply_surgical_edit(&resolved, old_t, new_t) {
                                    Ok(msg) => {
                                        self.turn_pin_cache.invalidate_path(&resolved);
                                        (msg, "completed".to_string(), false)
                                    }
                                    Err(e) => (e, "failed".to_string(), false),
                                }
                            }
                        }
                    }
                    Err(e) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "apply_subagent_proposal" => {
                if project.is_read_only || profile == AgentProfile::Ask {
                    return (
                        "PERMISSION_DENIED: apply_subagent_proposal is blocked in Ask/read-only mode."
                            .to_string(),
                        "denied".to_string(),
                        false,
                    );
                }
                let prop_id = args
                    .get("proposal_id")
                    .or_else(|| args.get("proposalId"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("")
                    .trim();
                if prop_id.is_empty() {
                    return (
                        "INVALID_ARGUMENTS: apply_subagent_proposal requires a non-empty 'proposal_id'."
                            .to_string(),
                        "failed".to_string(),
                        false,
                    );
                }
                let explicit_path = args
                    .get("path")
                    .and_then(|v| v.as_str())
                    .map(str::trim)
                    .filter(|s| !s.is_empty());
                self.apply_cached_proposal_edits(
                    thread_id,
                    project_root,
                    prop_id,
                    explicit_path,
                    &args,
                )
            }
            "write_file" => {
                if project.is_read_only {
                    return (
                        format!(
                            "READ_ONLY_PROJECT: Project '{}' is locked as read-only.",
                            project.name
                        ),
                        "denied".to_string(),
                        false,
                    );
                }
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or("");
                let content = args.get("content").and_then(|v| v.as_str()).unwrap_or("");
                let append = args.get("append").and_then(|v| v.as_bool()).unwrap_or(false);
                match resolve_workspace_path(project_root, raw_path) {
                    Ok(resolved) => {
                        if let Err(e) = enforce_research_cache_write_guard(&resolved) {
                            return (e.to_string(), "denied".to_string(), false);
                        }
                        if force_staged_critic {
                            let final_content = if append {
                                let existing = self
                                    .snapshot_store
                                    .read_file_text(&resolved)
                                    .or_else(|_| {
                                        fs::read_to_string(&resolved).map_err(|e| {
                                            crate::tools::ToolError::IoError(e.to_string())
                                        })
                                    })
                                    .unwrap_or_default();
                                format!("{existing}{content}")
                            } else {
                                content.to_string()
                            };
                            let op_label = format!(
                                "Staged {} bytes to {}",
                                final_content.len(),
                                resolved.display()
                            );
                            match self.stage_mutation_with_live_critic(
                                thread_id,
                                &resolved,
                                raw_path,
                                final_content,
                                &op_label,
                            ) {
                                Ok(msg) => (msg, "completed".to_string(), false),
                                Err(err_msg) => (err_msg, "failed".to_string(), false),
                            }
                        } else {
                            if let Some(parent) = resolved.parent() {
                                let _ = fs::create_dir_all(parent);
                            }
                            let write_res = if append {
                                std::fs::OpenOptions::new()
                                    .create(true)
                                    .append(true)
                                    .open(&resolved)
                                    .and_then(|mut f| std::io::Write::write_all(&mut f, content.as_bytes()))
                            } else {
                                fs::write(&resolved, content)
                            };
                            match write_res {
                                Ok(()) => {
                                    self.turn_pin_cache.invalidate_path(&resolved);
                                    (
                                        format!(
                                            "Wrote {} bytes to {}",
                                            content.len(),
                                            resolved.display()
                                        ),
                                        "completed".to_string(),
                                        false,
                                    )
                                }
                                Err(e) => (
                                    format!(
                                        "IO_ERROR: Failed to write {}: {e}",
                                        resolved.display()
                                    ),
                                    "failed".to_string(),
                                    false,
                                ),
                            }
                        }
                    }
                    Err(e) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "outline_symbols" => {
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or("");
                match resolve_workspace_path_with_roots(
                    project_root,
                    &self.extra_readable_roots,
                    raw_path,
                ) {
                    Ok(resolved) => {
                        match self
                            .snapshot_store
                            .read_file_text(&resolved)
                            .or_else(|_| {
                                fs::read_to_string(&resolved)
                                    .map_err(|e| crate::tools::ToolError::IoError(e.to_string()))
                            }) {
                            Ok(content) => {
                                let symbols = crate::tools::extract_outline_symbols(&content);
                                let json_out = serde_json::to_string(&symbols)
                                    .unwrap_or_else(|_| "[]".to_string());
                                (json_out, "completed".to_string(), false)
                            }
                            Err(e) => (e.to_string(), "failed".to_string(), false),
                        }
                    }
                    Err(e) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "get_symbol_body" => {
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or("");
                let symbol_name = args
                    .get("symbol")
                    .or_else(|| args.get("symbol_name"))
                    .or_else(|| args.get("symbolName"))
                    .or_else(|| args.get("name"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                match resolve_workspace_path_with_roots(
                    project_root,
                    &self.extra_readable_roots,
                    raw_path,
                ) {
                    Ok(resolved) => {
                        match self
                            .snapshot_store
                            .read_file_text(&resolved)
                            .or_else(|_| {
                                fs::read_to_string(&resolved)
                                    .map_err(|e| crate::tools::ToolError::IoError(e.to_string()))
                            }) {
                            Ok(content) => {
                                match crate::tools::get_symbol_body(&content, symbol_name) {
                                    Some((_entry, body)) => (body, "completed".to_string(), false),
                                    None => (
                                        format!(
                                            "NOT_FOUND: Symbol '{symbol_name}' not found in {}",
                                            resolved.display()
                                        ),
                                        "failed".to_string(),
                                        false,
                                    ),
                                }
                            }
                            Err(e) => (e.to_string(), "failed".to_string(), false),
                        }
                    }
                    Err(e) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "list_directory" => {
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or(".");
                match resolve_workspace_path_with_roots(
                    project_root,
                    &self.extra_readable_roots,
                    raw_path,
                ) {
                    Ok(resolved) => match Self::list_dir_formatted(&resolved) {
                        Ok(out) => (out, "completed".to_string(), false),
                        Err(e) => (e, "failed".to_string(), false),
                    },
                    Err(e) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "find_path" => {
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or(".");
                let pattern = args
                    .get("glob")
                    .or_else(|| args.get("pattern"))
                    .or_else(|| args.get("needle"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                match resolve_workspace_path_with_roots(
                    project_root,
                    &self.extra_readable_roots,
                    raw_path,
                ) {
                    Ok(resolved) => {
                        self.record_workspace_read_if_outside_research_cache(
                            thread_id, &resolved,
                        );
                        let matches = Self::find_paths_under(&resolved, pattern);
                        (matches.join("\n"), "completed".to_string(), false)
                    }
                    Err(e) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "grep" => {
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or(".");
                let pattern = args
                    .get("regex")
                    .or_else(|| args.get("pattern"))
                    .or_else(|| args.get("query"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let max_results = args
                    .get("max_results")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(100) as usize;
                match resolve_workspace_path_with_roots(
                    project_root,
                    &self.extra_readable_roots,
                    raw_path,
                ) {
                    Ok(resolved) => match Self::grep_under(&resolved, pattern, max_results) {
                        Ok(out) => {
                            self.record_workspace_read_if_outside_research_cache(
                                thread_id, &resolved,
                            );
                            (out, "completed".to_string(), false)
                        }
                        Err(e) => (e, "failed".to_string(), false),
                    },
                    Err(e) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "create_directory" => {
                if project.is_read_only {
                    return (
                        "READ_ONLY_PROJECT: Directory creation blocked.".to_string(),
                        "denied".to_string(),
                        false,
                    );
                }
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or("");
                match resolve_workspace_path(project_root, raw_path) {
                    Ok(resolved) => {
                        if let Err(e) = enforce_research_cache_write_guard(&resolved) {
                            return (e.to_string(), "denied".to_string(), false);
                        }
                        match fs::create_dir_all(&resolved) {
                            Ok(()) => (
                                format!("Created directory {}", resolved.display()),
                                "completed".to_string(),
                                false,
                            ),
                            Err(e) => (format!("IO_ERROR: {e}"), "failed".to_string(), false),
                        }
                    }
                    Err(e) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "copy_path" => {
                if project.is_read_only {
                    return (
                        "READ_ONLY_PROJECT: Copy blocked.".to_string(),
                        "denied".to_string(),
                        false,
                    );
                }
                let src_raw = args
                    .get("source_path")
                    .or_else(|| args.get("src"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let dst_raw = args
                    .get("destination_path")
                    .or_else(|| args.get("dst"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                match (
                    resolve_workspace_path_with_roots(
                        project_root,
                        &self.extra_readable_roots,
                        src_raw,
                    ),
                    resolve_workspace_path(project_root, dst_raw),
                ) {
                    (Ok(src), Ok(dst)) => {
                        if let Err(e) = enforce_research_cache_write_guard(&dst) {
                            return (e.to_string(), "denied".to_string(), false);
                        }
                        if let Some(p) = dst.parent() {
                            let _ = fs::create_dir_all(p);
                        }
                        match fs::copy(&src, &dst) {
                            Ok(bytes) => {
                                self.record_workspace_read_if_outside_research_cache(
                                    thread_id, &src,
                                );
                                (
                                    format!(
                                        "Copied {} -> {} ({bytes} bytes)",
                                        src.display(),
                                        dst.display()
                                    ),
                                    "completed".to_string(),
                                    false,
                                )
                            }
                            Err(e) => (format!("IO_ERROR: {e}"), "failed".to_string(), false),
                        }
                    }
                    (Err(e), _) | (_, Err(e)) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "move_path" => {
                if project.is_read_only {
                    return (
                        "READ_ONLY_PROJECT: Move blocked.".to_string(),
                        "denied".to_string(),
                        false,
                    );
                }
                let src_raw = args
                    .get("source_path")
                    .or_else(|| args.get("src"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let dst_raw = args
                    .get("destination_path")
                    .or_else(|| args.get("dst"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                match (
                    resolve_workspace_path(project_root, src_raw),
                    resolve_workspace_path(project_root, dst_raw),
                ) {
                    (Ok(src), Ok(dst)) => {
                        if let Err(e) = enforce_research_cache_write_guard(&src) {
                            return (e.to_string(), "denied".to_string(), false);
                        }
                        if let Err(e) = enforce_research_cache_write_guard(&dst) {
                            return (e.to_string(), "denied".to_string(), false);
                        }
                        if let Some(p) = dst.parent() {
                            let _ = fs::create_dir_all(p);
                        }
                        match fs::rename(&src, &dst) {
                            Ok(()) => (
                                format!("Moved {} -> {}", src.display(), dst.display()),
                                "completed".to_string(),
                                false,
                            ),
                            Err(e) => (format!("IO_ERROR: {e}"), "failed".to_string(), false),
                        }
                    }
                    (Err(e), _) | (_, Err(e)) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "delete_path" => {
                if project.is_read_only {
                    return (
                        "READ_ONLY_PROJECT: Delete blocked.".to_string(),
                        "denied".to_string(),
                        false,
                    );
                }
                let raw_path = args.get("path").and_then(|v| v.as_str()).unwrap_or("");
                match resolve_workspace_path(project_root, raw_path) {
                    Ok(resolved) => {
                        if let Err(e) = enforce_research_cache_write_guard(&resolved) {
                            return (e.to_string(), "denied".to_string(), false);
                        }
                        let res = if resolved.is_dir() {
                            fs::remove_dir_all(&resolved)
                        } else {
                            fs::remove_file(&resolved)
                        };
                        match res {
                            Ok(()) => {
                                self.turn_pin_cache.invalidate_path(&resolved);
                                (
                                    format!("Deleted {}", resolved.display()),
                                    "completed".to_string(),
                                    false,
                                )
                            }
                            Err(e) => (format!("IO_ERROR: {e}"), "failed".to_string(), false),
                        }
                    }
                    Err(e) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "terminal" => {
                if project.is_read_only {
                    return (
                        "READ_ONLY_PROJECT: Terminal execution blocked.".to_string(),
                        "denied".to_string(),
                        false,
                    );
                }
                let cmd_str = args.get("command").and_then(|v| v.as_str()).unwrap_or("");
                let cd_dir = match args.get("cd").and_then(|v| v.as_str()) {
                    Some(raw) if !raw.trim().is_empty() => match resolve_workspace_path(project_root, raw) {
                        Ok(p) => p,
                        Err(e) => return (e.to_string(), "failed".to_string(), false),
                    },
                    _ => project_root.to_path_buf(),
                };

                match crate::tools::run_terminal_command(&cd_dir, cmd_str, 30_000, true) {
                    Ok(term_out) => {
                        if cmd_str.contains("cargo") || cmd_str.contains("rustc") {
                            self.diagnostics_bus.ingest_cargo_json_output(&term_out);
                        } else if cmd_str.contains("swift") {
                            self.diagnostics_bus.ingest_swiftc_output(&term_out);
                        }
                        let ok = term_out.starts_with("exit_code: 0\n")
                            || term_out.trim() == "exit_code: 0"
                            || term_out.contains("Exit code: 0")
                            || term_out.contains("Exit Code: 0");
                        (
                            term_out,
                            if ok {
                                "completed".to_string()
                            } else {
                                "failed".to_string()
                            },
                            false,
                        )
                    }
                    Err(e) => (e.to_string(), "failed".to_string(), false),
                }
            }
            "fetch" => {
                let raw_url = args.get("url").and_then(|v| v.as_str()).unwrap_or("");
                let wire_msgs = self
                    .storage
                    .get_thread(thread_id)
                    .ok()
                    .flatten()
                    .map(|t| t.wire_messages)
                    .unwrap_or_default();
                match self.execute_fetch_for_session_internal(
                    thread_id,
                    &wire_msgs,
                    project,
                    &tc.id,
                    raw_url,
                    user_confirmed,
                ) {
                    Ok(envelope) => (envelope, "completed".to_string(), false),
                    Err((Some((_domain, _reason, _kind)), card_msg)) => {
                        (card_msg, "awaiting_confirmation".to_string(), false)
                    }
                    Err((None, err_msg)) => (err_msg, "failed".to_string(), false),
                }
            }
            "schedule_wakeup" => {
                let duration_s = args
                    .get("duration_s")
                    .or_else(|| args.get("duration_secs"))
                    .or_else(|| args.get("durationSecs"))
                    .and_then(|v| v.as_f64())
                    .unwrap_or(5.0);
                let label = args
                    .get("label")
                    .and_then(|v| v.as_str())
                    .unwrap_or("agent-wakeup")
                    .to_string();
                let note = args
                    .get("note")
                    .or_else(|| args.get("alarm_note"))
                    .or_else(|| args.get("alarmNote"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("Resume autonomous workflow")
                    .to_string();
                let watch_path = args
                    .get("watch_path")
                    .or_else(|| args.get("watchPath"))
                    .and_then(|v| v.as_str())
                    .map(str::to_string)
                    .or_else(|| {
                        args.get("watch_target")
                            .and_then(|wt| wt.get("path").and_then(|p| p.as_str()))
                            .map(str::to_string)
                    });

                match self.schedule_engine_timer(
                    thread_id,
                    duration_s,
                    &label,
                    &note,
                    watch_path.as_deref(),
                ) {
                    Ok(rec) => (
                        serde_json::to_string(&rec).unwrap_or_else(|_| {
                            format!(
                                "{{\"id\":\"{}\",\"status\":\"armed\",\"duration_s\":{duration_s}}}",
                                rec.id
                            )
                        }),
                        "completed".to_string(),
                        false,
                    ),
                    Err(e) => (e, "failed".to_string(), false),
                }
            }
            "tag" => {
                let action = args
                    .get("action")
                    .and_then(|v| v.as_str())
                    .unwrap_or("list")
                    .to_ascii_lowercase();
                let name = args
                    .get("name")
                    .or_else(|| args.get("id"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");

                match action.as_str() {
                    "list" => {
                        let tags = self.tag_registry.list_tags();
                        let summary: Vec<String> = tags
                            .iter()
                            .map(|t| format!("- {} ({}) — {}", t.id, t.category, t.description))
                            .collect();
                        (summary.join("\n"), "completed".to_string(), false)
                    }
                    "get" => match self.tag_registry.get_tag(name) {
                        Some(tag) => (
                            format!(
                                "Tag `{}` ({}):\n{}\n\n{}",
                                tag.id, tag.category, tag.description, tag.rubric_prompt
                            ),
                            "completed".to_string(),
                            false,
                        ),
                        None => (
                            format!("Tag '{name}' not found"),
                            "failed".to_string(),
                            false,
                        ),
                    },
                    "apply" => match self.tag_registry.apply_tag(name) {
                        Some(tag) => {
                            if !thread_active_tags.iter().any(|t| t == &tag.id) {
                                thread_active_tags.push(tag.id.clone());
                            }
                            let block = self
                                .tag_registry
                                .format_active_tags_rubric_block(thread_active_tags);
                            (
                                format!(
                                    "Applied Conversation Rubric Tag `{}` ({}) to thread `{thread_id}`.\n{block}",
                                    tag.id, tag.category
                                ),
                                "completed".to_string(),
                                false,
                            )
                        }
                        None => (
                            format!("Tag '{name}' not found in TagRegistry"),
                            "failed".to_string(),
                            false,
                        ),
                    },
                    "remove" => {
                        let canonical = normalize_tag_key(name);
                        thread_active_tags.retain(|t| normalize_tag_key(t) != canonical);
                        self.tag_registry.deactivate_tag(&canonical);
                        (
                            format!("Removed tag `{canonical}` from thread `{thread_id}`."),
                            "completed".to_string(),
                            false,
                        )
                    }
                    "create" => {
                        let category = args
                            .get("category")
                            .and_then(|v| v.as_str())
                            .unwrap_or("general");
                        let description = args
                            .get("description")
                            .and_then(|v| v.as_str())
                            .unwrap_or("");
                        let rubric = args
                            .get("rubric_prompt")
                            .or_else(|| args.get("rubricPrompt"))
                            .and_then(|v| v.as_str())
                            .unwrap_or("");
                        match self.create_custom_tag(name, category, description, rubric) {
                            Ok(tag) => {
                                if !thread_active_tags.iter().any(|t| t == &tag.id) {
                                    thread_active_tags.push(tag.id.clone());
                                }
                                (
                                    format!(
                                        "Created and applied custom Conversation Rubric Tag `{}` ({})",
                                        tag.id, tag.category
                                    ),
                                    "completed".to_string(),
                                    false,
                                )
                            }
                            Err(e) => (e, "failed".to_string(), false),
                        }
                    }
                    other => (
                        format!("Unknown tag action '{other}'"),
                        "failed".to_string(),
                        false,
                    ),
                }
            }
            "read_terminal" => {
                let (out, status) = crate::terminal_bridge::read_terminal_tool_output(&args);
                (out, status, false)
            }
            "type_in_terminal" => {
                // Only reachable after `approve_tool_call(.., true)`: policy is forced to Confirm.
                let (out, status) =
                    crate::terminal_bridge::type_in_terminal_tool_output(&tc.id, &args);
                (out, status, false)
            }
            "open_message_terminal" => {
                let raw_path = args
                    .get("path")
                    .or_else(|| args.get("folder"))
                    .or_else(|| args.get("cwd"))
                    .or_else(|| args.get("directory"))
                    .and_then(|v| v.as_str())
                    .unwrap_or(".");
                let reason_opt = args
                    .get("reason")
                    .or_else(|| args.get("note"))
                    .or_else(|| args.get("prompt"))
                    .and_then(|v| v.as_str())
                    .map(str::to_string);
                match self.open_message_terminal(raw_path, reason_opt) {
                    Ok(msg) => (msg, "completed".to_string(), false),
                    Err(e) => (e, "failed".to_string(), false),
                }
            }
            "prefill_context_subagent" => {
                let paths: Vec<String> = match args.get("paths") {
                    Some(Value::Array(arr)) => arr
                        .iter()
                        .filter_map(|v| v.as_str().map(str::to_string))
                        .collect(),
                    Some(Value::String(single)) => vec![single.clone()],
                    _ => Vec::new(),
                };
                let query = args
                    .get("query")
                    .and_then(|v| v.as_str())
                    .unwrap_or("")
                    .trim()
                    .to_string();
                if query.is_empty() {
                    return (
                        "INVALID_ARGUMENTS: prefill_context_subagent requires a non-empty 'query'."
                            .to_string(),
                        "failed".to_string(),
                        false,
                    );
                }
                let max_summary_tokens = args
                    .get("max_summary_tokens")
                    .or_else(|| args.get("maxSummaryTokens"))
                    .and_then(|v| v.as_u64())
                    .map(|n| (n as u32).clamp(256, 4096))
                    .unwrap_or(2560);

                let extra_roots = self.extra_readable_roots.clone();
                let (files_read, approx_tokens, numbered_bundle) =
                    match collect_numbered_subagent_context_with_roots(
                        project_root,
                        &extra_roots,
                        &paths,
                        Some(&mut self.snapshot_store),
                        512_000,
                        true,
                    ) {
                        Ok(res) => {
                            self.record_workspace_reads_for_subagent_paths(
                                thread_id,
                                project_root,
                                &paths,
                            );
                            res
                        }
                        Err(e) => return (e.to_string(), "failed".to_string(), false),
                    };

                let base_tid = derive_thread_key(Some(thread_id));
                let fallback_epoch = self
                    .storage
                    .get_thread(thread_id)
                    .ok()
                    .flatten()
                    .map(|t| t.engine_compaction_epoch)
                    .unwrap_or(0);
                let subagent_cache_key = format!("subagent0:prefill:{base_tid}");
                let sub_model = self.effective_subagent_model_id();
                let role_directive = "You are a read-only context prefill subagent for Spell IDE. \
                    Synthesize a dense, accurate technical summary with exact file paths and 1-indexed \
                    line numbers answering <query>. Never dump raw whole files.";
                let task_payload = format!(
                    "<query>\n{query}\n</query>\n\n<workspace_files>\n{numbered_bundle}\n</workspace_files>"
                );
                let (parent_cache_key, aligned_messages, aligned_tools) = self
                    .build_shared_prefix_subagent_prompt(
                        thread_id,
                        fallback_epoch,
                        role_directive,
                        role_directive,
                        &task_payload,
                    );

                let sub_req = SubagentCompletionRequest {
                    model: sub_model,
                    messages: aligned_messages,
                    stream: false,
                    max_tokens: Some(max_summary_tokens),
                    temperature: Some(0.1),
                    prompt_cache_key: Some(subagent_cache_key),
                    parent_cache_key: Some(parent_cache_key),
                    reasoning_effort: None,
                    thinking_budget: None,
                    tools: aligned_tools,
                    enable_thinking: None,
                    ..Default::default()
                };

                match Self::run_prefill_subagent_with_empty_body_fallback(
                    self.active_transport(),
                    &sub_req,
                ) {
                    Ok(resp) => {
                        self.record_live_subagent_completion(&resp);
                        let packet =
                            Self::format_prefill_context_packet(&resp, files_read, approx_tokens);
                        (packet, "completed".to_string(), false)
                    }
                    Err(crate::transport::TransportError::Cancelled) => {
                        ("Stopped by user.".to_string(), "cancelled".to_string(), false)
                    }
                    Err(err) => (err.to_string(), "failed".to_string(), false),
                }
            }
            "delegate_coding_subagent" => {
                self.run_autonomous_coding_delegation(thread_id, &tc.id, project, profile, &args)
            }
            mcp_or_other => {
                // Dispatch via real McpStdioClient (initializing embedded responder on demand if needed)
                if self.mcp_client.is_none() {
                    if let Ok((client, _responder)) = McpStdioClient::new_embedded() {
                        let _ = client.initialize();
                        self.mcp_client = Some(Arc::new(client));
                    }
                }
                if let Some(client) = self.mcp_client.clone() {
                    let req_id = next_req_id();
                    self.active_mcp_req_id = Some(req_id);
                    let clean_name = mcp_or_other.strip_prefix("mcp_").unwrap_or(mcp_or_other);
                    let res = client.call_tool(req_id, clean_name, args);
                    self.active_mcp_req_id = None;
                    match res {
                        Ok(val) => {
                            let text = val
                                .get("content")
                                .and_then(|c| c.as_array())
                                .and_then(|arr| arr.first())
                                .and_then(|item| item.get("text"))
                                .and_then(|t| t.as_str())
                                .map(str::to_string)
                                .unwrap_or_else(|| val.to_string());
                            (text, "completed".to_string(), true)
                        }
                        Err(e) => (e, "failed".to_string(), true),
                    }
                } else {
                    (
                        format!("Unsupported tool '{mcp_or_other}'"),
                        "failed".to_string(),
                        true,
                    )
                }
            }
        }
    }

    // ========================================================================
}
