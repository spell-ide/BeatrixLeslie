use super::*;

/// Ensures `tc.function.arguments` is a valid serialized JSON object (`"{...}"`) before echoing
/// an assistant `WireMessage` back to the engine, preventing Jinja `| fromjson` template errors
/// on 0-arg `DisallowedTool` calls (`""`), JSON arrays (`"[]"`), or truncated argument strings.
pub(super) fn sanitize_wire_tool_call_args_for_jinja(tc: &mut WireToolCall) {
    let is_valid_obj = serde_json::from_str::<Value>(&tc.function.arguments)
        .map(|v| v.is_object())
        .unwrap_or(false);
    if !is_valid_obj {
        tc.function.arguments = "{}".to_string();
    }
}

/// Returns `true` if `msg` contains `<untrusted_external_content` in `content`, `reasoning_content`,
/// or any serialized `tool_calls` entry (Gatekeeper Critic isolation).
pub(crate) fn wire_message_contains_untrusted_web(msg: &WireMessage) -> bool {
    const MARKER: &str = "<untrusted_external_content";
    if msg
        .content
        .as_deref()
        .map(|s| s.to_ascii_lowercase().contains(MARKER))
        .unwrap_or(false)
    {
        return true;
    }
    if msg
        .reasoning_content
        .as_deref()
        .map(|s| s.to_ascii_lowercase().contains(MARKER))
        .unwrap_or(false)
    {
        return true;
    }
    if let Some(ref calls) = msg.tool_calls {
        for tc in calls {
            if tc.function.name.to_ascii_lowercase().contains(MARKER)
                || tc.function.arguments.to_ascii_lowercase().contains(MARKER)
            {
                return true;
            }
        }
    }
    false
}

/// Truncates `messages` strictly before the first message containing `<untrusted_external_content`,
/// and rewinds any trailing incomplete `assistant(tool_calls)` turn whose `tool` result was truncated.
/// Returns `true` if truncation occurred.
pub(crate) fn truncate_prefix_before_untrusted_web(messages: &mut Vec<WireMessage>) -> bool {
    if let Some(first_idx) = messages.iter().position(wire_message_contains_untrusted_web) {
        messages.truncate(first_idx);
        let trailing_tools = messages
            .iter()
            .rev()
            .take_while(|m| m.role == "tool")
            .count();
        let cand_idx = messages.len().saturating_sub(trailing_tools);
        if cand_idx > 0 {
            let prev = &messages[cand_idx - 1];
            if prev.role == "assistant" {
                let expected_calls = prev.tool_calls.as_ref().map(|c| c.len()).unwrap_or(0);
                if expected_calls > trailing_tools {
                    messages.truncate(cand_idx - 1);
                }
            }
        }
        true
    } else {
        false
    }
}

pub(super) fn parse_url_domain_and_repo_key(raw_url: &str) -> Option<(String, Option<String>)> {
    let trimmed = raw_url.trim();
    let lower = trimmed.to_ascii_lowercase();
    let rest = lower
        .strip_prefix("https://")
        .or_else(|| lower.strip_prefix("http://"))?;
    let (authority, path_and_query) = match rest.find(['/', '?', '#']) {
        Some(idx) => (&rest[..idx], &rest[idx..]),
        None => (rest, ""),
    };
    let host_port = authority.rsplit_once('@').map(|(_, hp)| hp).unwrap_or(authority);
    let host = host_port
        .split(':')
        .next()?
        .trim()
        .trim_end_matches('.')
        .to_string();
    if host.is_empty() {
        return None;
    }

    let clean_path = path_and_query.split(['?', '#']).next().unwrap_or("");
    let segs: Vec<&str> = clean_path.split('/').filter(|s| !s.is_empty()).collect();
    let canonical_repo_host = match host.as_str() {
        "github.com" | "api.github.com" | "raw.githubusercontent.com" | "codeload.github.com" => {
            Some("github.com")
        }
        "gitlab.com" => Some("gitlab.com"),
        "codeberg.org" => Some("codeberg.org"),
        "huggingface.co" => Some("huggingface.co"),
        _ => None,
    };
    let repo_key = canonical_repo_host.and_then(|rh| {
        if host == "api.github.com" && segs.first() == Some(&"repos") && segs.len() >= 3 {
            let repo = segs[2].trim_end_matches(".git");
            Some(format!("{rh}/{}/{repo}", segs[1]))
        } else if rh == "huggingface.co"
            && matches!(segs.first().copied(), Some("datasets" | "spaces"))
            && segs.len() >= 3
        {
            let repo = segs[2].trim_end_matches(".git");
            Some(format!("{rh}/{}/{repo}", segs[1]))
        } else if segs.len() >= 2 {
            let repo = segs[1].trim_end_matches(".git");
            Some(format!("{rh}/{}/{repo}", segs[0]))
        } else {
            None
        }
    });

    Some((host, repo_key))
}

pub(super) fn is_code_hosting_repo_domain(domain: &str) -> bool {
    matches!(
        domain,
        "github.com"
            | "api.github.com"
            | "raw.githubusercontent.com"
            | "codeload.github.com"
            | "gitlab.com"
            | "codeberg.org"
            | "huggingface.co"
    ) || domain.ends_with(".githubusercontent.com")
}

pub(super) fn normalize_domain_entry(raw: &str) -> Option<String> {
    let trimmed = raw.trim().to_ascii_lowercase();
    let without_scheme = trimmed
        .strip_prefix("https://")
        .or_else(|| trimmed.strip_prefix("http://"))
        .unwrap_or(&trimmed);
    let cleaned = without_scheme
        .trim_matches('/')
        .trim_end_matches('.')
        .to_string();
    if cleaned.is_empty() {
        None
    } else {
        Some(cleaned)
    }
}


impl SpellCoreSession {

    /// Appends a user prompt to `thread_id` and runs the multi-turn agent loop
    /// (up to `DEFAULT_MAX_AGENT_STEPS = 8`), streaming from `EngineTransport`,
    /// recording engine-owned compaction and timer telemetry, and executing headless
    /// or MCP tools until completion or a `Confirm` permission pause.
    pub fn send_user_prompt(
        &mut self,
        thread_id: &str,
        prompt: &str,
    ) -> Result<StoredThread, String> {
        crate::clear_turn_cancel();
        if (!self.uses_custom_transport && !self.is_endpoint_configured())
            || self.model_id.trim().is_empty()
        {
            return Err(
                "Configure an API endpoint URL and Model ID in Settings → Engine before sending a message."
                    .to_string(),
            );
        }
        let resolved_tid = if thread_id.trim().is_empty() {
            self.selected_thread_id
                .clone()
                .ok_or_else(|| "No active thread selected".to_string())?
        } else {
            thread_id.trim().to_string()
        };

        let mut thread = self
            .storage
            .get_thread(&resolved_tid)?
            .ok_or_else(|| format!("Thread '{resolved_tid}' not found"))?;
        self.selected_thread_id = Some(thread.id.clone());
        self.selected_project_id = Some(thread.project_id.clone());

        let mut preprocessed = self.tag_registry.preprocess_prompt(prompt);
        for active_sid in &self.active_steering_ids {
            if let Some(directive) = self.tag_registry.get_steering(active_sid) {
                if !preprocessed
                    .matched_steering
                    .iter()
                    .any(|d| d.id == directive.id)
                {
                    if let Some(t) = directive.temperature {
                        preprocessed.temperature = Some(t);
                    }
                    if let Some(p) = directive.top_p {
                        preprocessed.top_p = Some(p);
                    }
                    if let Some(pp) = directive.presence_penalty {
                        preprocessed.presence_penalty = Some(pp);
                    }
                    if let Some(rp) = directive.repetition_penalty {
                        preprocessed.repetition_penalty = Some(rp);
                    }
                    if let Some(mt) = directive.max_tokens {
                        preprocessed.max_tokens = Some(mt);
                    }
                    if let Some(et) = directive.enable_thinking {
                        preprocessed.enable_thinking = Some(et);
                    }
                    if let Some(tb) = directive.thinking_budget {
                        preprocessed.thinking_budget = Some(tb);
                    }
                    if !directive.system_append.trim().is_empty() {
                        if !preprocessed.tail_system_append.is_empty() {
                            preprocessed.tail_system_append.push('\n');
                        }
                        preprocessed
                            .tail_system_append
                            .push_str(directive.system_append.trim());
                    }
                    preprocessed.matched_steering.push(directive);
                }
            }
        }
        if preprocessed.matched_steering.iter().any(|d| d.id == "spell") {
            self.message_terminal.is_open = false;
        }
        for matched_tag in &preprocessed.matched_tags {
            if !thread
                .active_tags
                .iter()
                .any(|t| t.eq_ignore_ascii_case(&matched_tag.name))
            {
                thread.active_tags.push(matched_tag.name.clone());
            }
        }

        let mut tail_prompt = preprocessed.build_tail_injected_user_message();
        // Inject any active thread-level rubric tags into the tail user message (so `messages[0]`
        // never mutate messages[0] so the prefix cache remains 100% byte-identical).
        let extra_thread_tags: Vec<ConversationTag> = thread
            .active_tags
            .iter()
            .filter(|name| {
                !preprocessed
                    .matched_tags
                    .iter()
                    .any(|m| m.name.eq_ignore_ascii_case(name) || m.id.eq_ignore_ascii_case(name))
            })
            .filter_map(|name| self.tag_registry.get_tag(name))
            .collect();
        if !extra_thread_tags.is_empty() {
            tail_prompt.push_str("\n\n<conversation_rubric_tags>\n");
            for t in &extra_thread_tags {
                tail_prompt.push_str(&format!(
                    "- **{}** (`{}`): {}\n",
                    t.name, t.category, t.rubric_prompt
                ));
            }
            tail_prompt.push_str("</conversation_rubric_tags>");
        }

        let wire_prompt = tail_prompt.clone();
        self.last_turn_preprocessed = Some(preprocessed);

        // Append user message to both UI presentation events and exact OpenAI wire history
        thread.ui_events.push(UiChatEvent::user_message(prompt));
        thread.wire_messages.push(WireMessage::user(wire_prompt));
        thread.updated_at = now_unix_secs();
        self.storage.upsert_thread(&thread)?;

        self.run_agent_continuation_loop(&mut thread, DEFAULT_MAX_AGENT_STEPS)?;
        Ok(thread)
    }

    /// Edits or regenerates an earlier user turn in `thread_id` by truncating `ui_events` and
    /// `wire_messages` back to the target user turn and re-running the multi-turn agent loop.
    pub fn edit_or_regenerate_turn(
        &mut self,
        thread_id: &str,
        event_id: Option<&str>,
        new_prompt: Option<&str>,
    ) -> Result<StoredThread, String> {
        crate::clear_turn_cancel();
        let resolved_tid = if thread_id.trim().is_empty() {
            self.selected_thread_id
                .clone()
                .ok_or_else(|| "No active thread selected".to_string())?
        } else {
            thread_id.trim().to_string()
        };

        let mut thread = self
            .storage
            .get_thread(&resolved_tid)?
            .ok_or_else(|| format!("Thread '{resolved_tid}' not found"))?;

        // Locate the target user event index (by event_id if provided, else the last user message)
        let target_ui_idx = if let Some(ev_id) = event_id.map(str::trim).filter(|s| !s.is_empty()) {
            thread
                .ui_events
                .iter()
                .position(|e| e.id == ev_id && e.kind == "user")
        } else {
            thread.ui_events.iter().rposition(|e| e.kind == "user")
        };

        let Some(ui_idx) = target_ui_idx else {
            return Err("No user message turn found to edit or regenerate".to_string());
        };

        // Count how many user turns occurred up to and including `ui_idx` (1-based)
        let user_turn_ordinal = thread.ui_events[..=ui_idx]
            .iter()
            .filter(|e| e.kind == "user")
            .count();

        let original_prompt = thread.ui_events[ui_idx].content.clone();
        let prompt_to_run = new_prompt
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .map(str::to_string)
            .unwrap_or(original_prompt);

        // Truncate `ui_events` before `ui_idx`
        thread.ui_events.truncate(ui_idx);

        // Truncate `wire_messages` before the corresponding `user_turn_ordinal`-th user message
        let mut seen_users = 0usize;
        let mut wire_cut_idx = thread.wire_messages.len();
        for (idx, wm) in thread.wire_messages.iter().enumerate() {
            if wm.role == "user" {
                seen_users += 1;
                if seen_users == user_turn_ordinal {
                    wire_cut_idx = idx;
                    break;
                }
            }
        }
        thread.wire_messages.truncate(wire_cut_idx);
        thread.updated_at = now_unix_secs();
        self.storage.upsert_thread(&thread)?;

        self.send_user_prompt(&resolved_tid, &prompt_to_run)
    }

    /// Runs up to `max_steps` turns of the agent loop for `thread`.
    pub fn run_agent_continuation_loop(
        &mut self,
        thread: &mut StoredThread,
        max_steps: usize,
    ) -> Result<(), String> {
        if (!self.uses_custom_transport && !self.is_endpoint_configured())
            || self.model_id.trim().is_empty()
        {
            return Err(
                "Configure an API endpoint URL and Model ID in Settings → Engine before sending a message."
                    .to_string(),
            );
        }

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

        // Generated tokens of tool-call steps are credited to the response that follows them.
        let mut pending_completion_tokens: u64 = 0;
        for _step in 0..max_steps {
            // Keep `messages[0]` immutable across all turns and tag toggles
            // (dynamic tags/steering are injected at the tail user message).
            let system_prompt = build_clean_system_prompt(
                &project.name,
                Path::new(&project.root_path),
                project.is_read_only,
                profile,
                &[],
            );

            let mut request_messages = Vec::with_capacity(thread.wire_messages.len() + 1);
            request_messages.push(WireMessage::system(system_prompt));
            request_messages.extend(thread.wire_messages.iter().cloned());

            let cache_key = resolve_prompt_cache_key(
                &thread.id,
                thread.engine_compaction_epoch,
                self.epoch_cache_key_suffix,
            );

            let pp = self.last_turn_preprocessed.as_ref();
            let mut req = ChatCompletionRequest {
                model: self.model_id.clone(),
                messages: request_messages,
                stream: true,
                stream_options: Some(StreamOptions {
                    include_usage: true,
                }),
                temperature: pp.and_then(|p| p.temperature),
                max_tokens: pp.and_then(|p| p.max_tokens),
                reasoning_effort: None,
                tools: Some({
                    // Headless tools + the user-terminal tools. `type_in_terminal` is only offered in
                    // the Write profile (read-only projects already map to Ask above), and it is also
                    // policy-forced to Confirm / Deny in `effective_tool_permission`.
                    let mut schemas = build_openai_tools_schema(profile);
                    schemas.extend(crate::terminal_bridge::terminal_tool_schemas(
                        profile == AgentProfile::Write,
                    ));
                    schemas
                }),
                tool_choice: None,
                prompt_cache_key: Some(cache_key),
                client_compaction_epoch: Some(thread.engine_compaction_epoch),
                auto_compact_min_tps: None,
                top_p: pp.and_then(|p| p.top_p),
                presence_penalty: pp.and_then(|p| p.presence_penalty),
                repetition_penalty: pp.and_then(|p| p.repetition_penalty),
                thinking_budget: pp.and_then(|p| p.thinking_budget),
                chat_template_kwargs: pp.and_then(|p| p.chat_template_kwargs.clone()),
            };

            self.last_prefix_snapshot = Some(LastPrefixSnapshot {
                thread_id: thread.id.clone(),
                cache_key: req.prompt_cache_key.clone().unwrap_or_default(),
                messages: req.messages.clone(),
                tools: req.tools.clone().unwrap_or_default(),
            });

            // Prepare base live snapshot JSON for incremental ~50 ms streaming updates into
            // `SPELL_LIVE_SNAPSHOT_CACHE` so the UI streaming poller renders tokens as they arrive.
            let base_snap_str = self.build_full_state_snapshot_json();
            let mut live_snap_val: Option<Value> = serde_json::from_str(&base_snap_str).ok();
            let stream_event_id = next_storage_id("ev-asst");
            let stream_started_ts = now_unix_secs();
            let active_tid_for_stream = thread.id.clone();
            let mut streaming_content = String::new();
            let mut streaming_reasoning = String::new();
            let mut last_live_flush = Instant::now() - Duration::from_millis(100);
            let turn_start = Instant::now();

            let transport = Arc::clone(&self.transport);
            let summary = transport.stream_chat_completion(&mut req, &mut |chunk| {
                let mut changed = false;
                for ch in &chunk.choices {
                    if let Some(ref c) = ch.delta.content {
                        if !c.is_empty() {
                            streaming_content.push_str(c);
                            changed = true;
                        }
                    }
                    if let Some(ref r) = ch.delta.reasoning_content {
                        if !r.is_empty() {
                            streaming_reasoning.push_str(r);
                            changed = true;
                        }
                    }
                }
                if changed && last_live_flush.elapsed() >= Duration::from_millis(50) {
                    if let Some(ref mut snap) = live_snap_val {
                        let live_event = json!({
                            "id": stream_event_id,
                            "kind": "assistant",
                            "role": "assistant",
                            "content": streaming_content,
                            "reasoning_content": if streaming_reasoning.is_empty() { Value::Null } else { json!(streaming_reasoning) },
                            "timestamp": stream_started_ts,
                        });
                        let patch_thread_events = |thread_obj: &mut Value| {
                            if let Some(arr) = thread_obj
                                .get_mut("ui_events")
                                .and_then(Value::as_array_mut)
                            {
                                if let Some(last) = arr.last_mut().filter(|e| {
                                    e.get("id").and_then(Value::as_str)
                                        == Some(stream_event_id.as_str())
                                }) {
                                    *last = live_event.clone();
                                } else {
                                    arr.push(live_event.clone());
                                }
                            }
                        };
                        if let Some(at) = snap.get_mut("active_thread") {
                            patch_thread_events(at);
                        }
                        if let Some(at) = snap.get_mut("activeThread") {
                            patch_thread_events(at);
                        }
                        if let Some(threads_arr) =
                            snap.get_mut("threads").and_then(Value::as_array_mut)
                        {
                            for th in threads_arr.iter_mut() {
                                if th.get("id").and_then(Value::as_str)
                                    == Some(active_tid_for_stream.as_str())
                                {
                                    patch_thread_events(th);
                                }
                            }
                        }
                        if let Ok(serialized) = serde_json::to_string(snap) {
                            crate::update_live_snapshot_cache(&serialized);
                        }
                    } else {
                        let _ = crate::spell_bump_patch_gen();
                    }
                    last_live_flush = Instant::now();
                } else {
                    let _ = crate::spell_bump_patch_gen();
                }
            })?;

            let turn_latency_ms = turn_start.elapsed().as_millis() as u64;
            self.record_main_turn_telemetry(&summary, turn_latency_ms);

            // Surface any injected alarm banners as UI events without adding duplicate messages.
            for alarm_banner in &summary.engine_injected_alarms {
                thread
                    .ui_events
                    .push(UiChatEvent::EngineTimerAlarm(alarm_banner.clone()));
            }

            // Never slice or drop `thread.wire_messages` silently; record epoch transitions if signalled.
            if summary.context_advice.hot_cache_cleared
                || summary.context_advice.compaction_epoch > thread.engine_compaction_epoch
            {
                let new_epoch = summary
                    .context_advice
                    .compaction_epoch
                    .max(thread.engine_compaction_epoch + 1);
                thread.engine_compaction_epoch = new_epoch;
                self.turn_pin_cache.clear_seen_files_for_new_epoch();

                let mut notice_advice = summary.context_advice.clone();
                notice_advice.compaction_epoch = new_epoch;
                thread
                    .ui_events
                    .push(UiChatEvent::EngineCompactionNotice(&notice_advice));
            }

            if summary.usage.prompt_tokens > 0 {
                thread.prompt_tokens = summary.usage.prompt_tokens;
            } else if summary.context_advice.prompt_tokens > 0 {
                thread.prompt_tokens = summary.context_advice.prompt_tokens;
            }
            thread.last_context_advice = Some(summary.context_advice.clone());

            // Preserve exact assistant WireMessage locally in SQLite (including reasoning_content
            // and tool_calls) while outbound requests sanitize via `WireMessage::for_outbound_openai`.
            let reasoning_opt = if summary.reasoning_content.trim().is_empty() {
                None
            } else {
                Some(summary.reasoning_content.clone())
            };
            let tool_calls_opt = if summary.tool_calls.is_empty() {
                None
            } else {
                let mut sanitized_tcs = summary.tool_calls.clone();
                for tc in &mut sanitized_tcs {
                    sanitize_wire_tool_call_args_for_jinja(tc);
                }
                Some(sanitized_tcs)
            };
            let content_opt = if summary.content.is_empty() && tool_calls_opt.is_some() {
                Some(String::new())
            } else {
                Some(summary.content.clone())
            };

            thread.wire_messages.push(WireMessage {
                role: "assistant".to_string(),
                content: content_opt,
                reasoning_content: reasoning_opt.clone(),
                tool_calls: tool_calls_opt,
                tool_call_id: None,
                name: None,
            });

            pending_completion_tokens += summary.usage.completion_tokens;
            if !summary.content.trim().is_empty() || reasoning_opt.is_some() {
                let mut event = UiChatEvent::assistant_message(
                    summary.content.clone(),
                    reasoning_opt,
                    Some(summary.context_advice.clone()),
                );
                event.id = stream_event_id;
                if pending_completion_tokens > 0 {
                    event.completion_tokens = Some(pending_completion_tokens);
                }
                pending_completion_tokens = 0;
                thread.ui_events.push(event);
            }

            // If turn cancellation was requested or no tool calls were requested, persist to SQLite and finish the loop
            if crate::is_turn_cancel_requested()
                || summary.tool_calls.is_empty()
                || summary.finish_reason != "tool_calls"
            {
                thread.updated_at = now_unix_secs();
                self.storage.upsert_thread(thread)?;
                let snap = self.build_full_state_snapshot_json();
                crate::update_live_snapshot_cache(&snap);
                break;
            }

            // Execute tool calls (or pause if a tool requires user confirmation)
            let paused_for_confirmation = self.process_tool_calls_batch(
                thread,
                &project,
                profile,
                &summary.tool_calls,
            )?;
            if self.loop_state == LoopState::PausedOnCriticReject {
                self.sync_critic_circuit_breaker_banner(thread);
            }
            thread.updated_at = now_unix_secs();
            self.storage.upsert_thread(thread)?;
            let snap = self.build_full_state_snapshot_json();
            crate::update_live_snapshot_cache(&snap);

            if paused_for_confirmation
                || self.loop_state == LoopState::PausedOnCriticReject
                || crate::is_turn_cancel_requested()
            {
                return Ok(());
            }
        }

        Ok(())
    }

    pub fn effective_tool_permission(
        &self,
        tool_name: &str,
        profile: AgentProfile,
        project_is_read_only: bool,
    ) -> ToolPermissionPolicy {
        if let Some(allowed) = self
            .last_turn_preprocessed
            .as_ref()
            .and_then(|p| p.allowed_tools.as_ref())
        {
            if !allowed.iter().any(|a| a.eq_ignore_ascii_case(tool_name)) {
                return ToolPermissionPolicy::Deny;
            }
        }
        // Typing into the user's own shell is never auto-approved: always Confirm (per call),
        // and never allowed in the Ask profile or on read-only projects, regardless of the
        // persisted tool-permission map.
        if tool_name == "spawn_agent" || tool_name == "skill" {
            return ToolPermissionPolicy::Deny;
        }
        if tool_name == "type_in_terminal" {
            return if project_is_read_only || profile == AgentProfile::Ask {
                ToolPermissionPolicy::Deny
            } else {
                ToolPermissionPolicy::Confirm
            };
        }
        if (tool_name == "delegate_coding_subagent" || tool_name == "apply_subagent_proposal")
            && (project_is_read_only || profile == AgentProfile::Ask)
        {
            return ToolPermissionPolicy::Deny;
        }
        let is_mutating = matches!(
            tool_name,
            "edit_file"
                | "write_file"
                | "apply_subagent_proposal"
                | "create_directory"
                | "delete_path"
                | "move_path"
                | "copy_path"
                | "terminal"
        );
        if project_is_read_only && is_mutating {
            return ToolPermissionPolicy::Deny;
        }
        if let Some(&explicit) = self.tool_permissions.get(tool_name) {
            if profile == AgentProfile::Ask && is_mutating && explicit == ToolPermissionPolicy::Allow
            {
                return ToolPermissionPolicy::Deny;
            }
            return explicit;
        }
        if profile == AgentProfile::Ask && is_mutating {
            ToolPermissionPolicy::Deny
        } else if tool_name == "apply_subagent_proposal" {
            ToolPermissionPolicy::Confirm
        } else {
            ToolPermissionPolicy::Allow
        }
    }

    pub(super) fn sync_critic_circuit_breaker_banner(&self, thread: &mut StoredThread) {
        if self.loop_state != LoopState::PausedOnCriticReject {
            return;
        }
        let marker = format!("Bounce #{} exceeded max_critic_bounces", self.critic_bounce_count);
        let already_present = thread
            .ui_events
            .iter()
            .any(|e| e.content.contains("PAUSED ON CRITIC REJECT") && e.content.contains(&marker));
        if !already_present {
            let verdict = self.last_critic_verdict.clone().unwrap_or_default();
            let notice = format!(
                "[GATEKEEPER CRITIC CIRCUIT BREAKER — PAUSED ON CRITIC REJECT] \
                 Bounce #{} exceeded max_critic_bounces ({}). Violations: {}. Fix Hint: {}",
                self.critic_bounce_count,
                self.max_critic_bounces,
                verdict.violations.join("; "),
                verdict.fix_hint
            );
            thread
                .ui_events
                .push(UiChatEvent::assistant_message(notice, None, None));
        }
    }

    pub(super) fn record_workspace_read_if_outside_research_cache(
        &mut self,
        thread_id: &str,
        resolved: &Path,
    ) {
        if !crate::online_fetch::is_research_cache_path(resolved) {
            self.workspace_read_threads.insert(thread_id.to_string());
        }
    }

    pub(super) fn record_workspace_reads_for_subagent_paths(
        &mut self,
        thread_id: &str,
        project_root: &Path,
        paths: &[String],
    ) {
        if paths.is_empty() {
            if !crate::online_fetch::is_research_cache_path(project_root) {
                self.workspace_read_threads.insert(thread_id.to_string());
            }
            return;
        }
        let extra = self.extra_readable_roots.clone();
        for raw in paths {
            if let Ok(resolved) = resolve_workspace_path_with_roots(project_root, &extra, raw) {
                if !crate::online_fetch::is_research_cache_path(&resolved) {
                    self.workspace_read_threads.insert(thread_id.to_string());
                    return;
                }
            }
        }
    }

    pub(super) fn register_extra_readable_root(&mut self, root: &Path) {
        let canonical = root.canonicalize().unwrap_or_else(|_| root.to_path_buf());
        if !self
            .extra_readable_roots
            .iter()
            .any(|existing| Self::paths_equivalent(existing, &canonical))
        {
            self.extra_readable_roots.push(canonical);
        }
    }

    pub(super) fn relative_workspace_candidate(project_root: &Path, raw_path: &str) -> String {
        let trimmed = raw_path.trim();
        if trimmed.is_empty() {
            return String::new();
        }
        let p = Path::new(trimmed);
        if p.is_absolute() {
            if let Ok(rel) = p.strip_prefix(project_root) {
                return rel.to_string_lossy().into_owned();
            }
            if let (Ok(cp), Ok(cr)) = (
                p.canonicalize().or_else(|_| {
                    p.parent()
                        .and_then(|par| par.canonicalize().ok())
                        .and_then(|cpar| p.file_name().map(|f| cpar.join(f)))
                        .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::NotFound, "no parent"))
                }),
                project_root.canonicalize(),
            ) {
                if let Ok(rel) = cp.strip_prefix(&cr) {
                    return rel.to_string_lossy().into_owned();
                }
            }
        }
        trimmed.to_string()
    }

    pub(super) fn resolved_relative_workspace_candidate(project_root: &Path, resolved: &Path) -> String {
        let canon_root = project_root
            .canonicalize()
            .unwrap_or_else(|_| project_root.to_path_buf());
        if let Ok(rel) = resolved.strip_prefix(&canon_root) {
            let s = rel.to_string_lossy().replace('\\', "/");
            if !s.is_empty() {
                return s;
            }
        }
        if let Ok(rel) = resolved.strip_prefix(project_root) {
            let s = rel.to_string_lossy().replace('\\', "/");
            if !s.is_empty() {
                return s;
            }
        }
        if let Ok(canon_resolved) = resolved.canonicalize().or_else(|_| {
            resolved
                .parent()
                .and_then(|par| par.canonicalize().ok())
                .and_then(|cpar| resolved.file_name().map(|f| cpar.join(f)))
                .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::NotFound, "no parent"))
        }) {
            if let Ok(rel) = canon_resolved.strip_prefix(&canon_root) {
                let s = rel.to_string_lossy().replace('\\', "/");
                if !s.is_empty() {
                    return s;
                }
            }
        }
        String::new()
    }

    pub(super) fn detect_web_taint_sensitive_workspace_path(
        project_root: &Path,
        raw_path: &str,
        resolved_opt: Option<&Path>,
    ) -> Option<String> {
        let rel_raw = Self::relative_workspace_candidate(project_root, raw_path)
            .trim()
            .trim_start_matches("./")
            .replace('\\', "/");
        if !rel_raw.is_empty() && is_web_taint_sensitive_workspace_file(&rel_raw) {
            return Some(rel_raw);
        }

        let resolved_buf = if let Some(r) = resolved_opt {
            Some(r.to_path_buf())
        } else if let Ok(r) = resolve_workspace_path(project_root, raw_path) {
            Some(r)
        } else {
            let trimmed = raw_path.trim();
            if trimmed.is_empty() {
                None
            } else {
                let cand = if Path::new(trimmed).is_absolute() {
                    std::path::PathBuf::from(trimmed)
                } else {
                    project_root.join(trimmed)
                };
                cand.canonicalize()
                    .or_else(|_| {
                        fs::read_link(&cand).and_then(|target| {
                            let joined = if target.is_absolute() {
                                target
                            } else {
                                cand.parent().unwrap_or(project_root).join(target)
                            };
                            joined.canonicalize().or(Ok(joined))
                        })
                    })
                    .ok()
            }
        };

        if let Some(ref resolved) = resolved_buf {
            let rel_resolved = Self::resolved_relative_workspace_candidate(project_root, resolved)
                .trim()
                .trim_start_matches("./")
                .replace('\\', "/");
            if !rel_resolved.is_empty() && is_web_taint_sensitive_workspace_file(&rel_resolved) {
                return Some(rel_resolved);
            }
        }
        None
    }

    pub(super) fn persist_delegation_undo_state(&self) {
        let state = PersistedDelegationUndoState {
            next_seq: self.next_delegation_undo_seq,
            records: self.delegation_undo_log.iter().cloned().collect(),
            undone_ids: self.undone_delegation_ids.clone(),
        };
        if let Ok(json) = serde_json::to_string(&state) {
            let _ = self
                .storage
                .put_kv_document("delegation_undo", "state_v1", &json);
        }
    }

    pub(super) fn detect_batch_sibling_conflict(&self, candidate: &Path) -> Option<String> {
        if self.active_batch_committed_paths.is_empty() {
            return None;
        }
        let conflicted = self.active_batch_committed_paths.contains(candidate)
            || self
                .active_batch_committed_paths
                .iter()
                .any(|existing| Self::paths_equivalent(existing, candidate));
        if conflicted {
            Some(format!(
                "CONFLICT_WARNING: '{}' was already modified by an earlier sibling delegation in the same parallel batch; escalating rather than overwriting with a stale Step-0 snapshot.",
                candidate.display()
            ))
        } else {
            None
        }
    }

    pub(crate) fn find_web_taint_sensitive_target(
        &self,
        project_root: &Path,
        tool_name: &str,
        args: &Value,
    ) -> Option<String> {
        let check_raw = |raw: &str| -> Option<String> {
            Self::detect_web_taint_sensitive_workspace_path(project_root, raw, None)
        };

        match tool_name {
            "write_file" | "delete_path" | "create_directory" => {
                let raw = args.get("path").and_then(|v| v.as_str()).unwrap_or("");
                check_raw(raw)
            }
            "edit_file" | "apply_subagent_proposal" => {
                if let Some(raw) = args.get("path").and_then(|v| v.as_str()) {
                    if let Some(hit) = check_raw(raw) {
                        return Some(hit);
                    }
                }
                if let Some(prop_id) = args
                    .get("proposal_id")
                    .or_else(|| args.get("proposalId"))
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
                        if let Some(hit) = check_raw(&prop.path) {
                            return Some(hit);
                        }
                    }
                }
                None
            }
            "copy_path" => {
                let dst = args
                    .get("destination_path")
                    .or_else(|| args.get("dst"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                check_raw(dst)
            }
            "move_path" => {
                let src = args
                    .get("source_path")
                    .or_else(|| args.get("src"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let dst = args
                    .get("destination_path")
                    .or_else(|| args.get("dst"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                check_raw(src).or_else(|| check_raw(dst))
            }
            _ => None,
        }
    }

    pub(super) fn thread_user_prompts_reference_url_or_repo(
        wire_messages: &[WireMessage],
        raw_url: &str,
        repo_key: Option<&str>,
    ) -> bool {
        let url_lower = raw_url.trim().to_ascii_lowercase();
        if url_lower.is_empty() {
            return false;
        }
        let has_query = url_lower.contains('?');
        for msg in wire_messages {
            if msg.role != "user" {
                continue;
            }
            let Some(ref text) = msg.content else {
                continue;
            };
            let lower_text = text.to_ascii_lowercase();
            if lower_text.contains(&url_lower) {
                return true;
            }
            if !has_query {
                if let Some(rk) = repo_key {
                    if !rk.is_empty() && lower_text.contains(rk) {
                        return true;
                    }
                }
            }
        }
        false
    }

    pub(super) fn is_domain_ephemerally_approved(
        cfg: &crate::online_fetch::OnlineResearchConfig,
        domain: &str,
        repo_key: Option<&str>,
    ) -> bool {
        let d = domain.trim().to_ascii_lowercase();
        if cfg.session_allowed_domains.contains(&d) || cfg.once_allowed_domains.contains(&d) {
            return true;
        }
        if matches!(
            d.as_str(),
            "api.github.com" | "raw.githubusercontent.com" | "codeload.github.com"
        ) && (cfg.session_allowed_domains.contains("github.com")
            || cfg.once_allowed_domains.contains("github.com"))
        {
            return true;
        }
        if let Some(rk) = repo_key {
            let rkl = rk.to_ascii_lowercase();
            if cfg.session_allowed_domains.contains(&rkl) || cfg.once_allowed_domains.contains(&rkl)
            {
                return true;
            }
        }
        false
    }

    /// Returns sanitized Online Research status JSON (never echoing raw secret tokens/API keys).
    pub fn online_research_status_json(&self) -> Value {
        let cfg = &self.online_research_config;
        let custom_domains: Vec<String> = cfg.custom_allowed_domains.iter().cloned().collect();
        let session_domains: Vec<String> = cfg.session_allowed_domains.iter().cloned().collect();
        let has_github_token = cfg
            .github_token
            .as_deref()
            .map(|s| !s.trim().is_empty())
            .unwrap_or(false);
        let has_huggingface_token = cfg
            .huggingface_token
            .as_deref()
            .map(|s| !s.trim().is_empty())
            .unwrap_or(false);
        let has_openalex_api_key = cfg
            .openalex_api_key
            .as_deref()
            .map(|s| !s.trim().is_empty())
            .unwrap_or(false);
        let has_search_api_key = cfg
            .search_api_key
            .as_deref()
            .map(|s| !s.trim().is_empty())
            .unwrap_or(false);
        let search_provider_val = match cfg.search_provider {
            Some(crate::online_fetch::SearchProviderKind::Brave) => json!("brave"),
            Some(crate::online_fetch::SearchProviderKind::Exa) => json!("exa"),
            Some(crate::online_fetch::SearchProviderKind::Tavily) => json!("tavily"),
            Some(crate::online_fetch::SearchProviderKind::Kagi) => json!("kagi"),
            None => Value::Null,
        };
        json!({
            "ok": true,
            "mode": cfg.mode.as_wire(),
            "custom_allowed_domains": custom_domains,
            "customAllowedDomains": custom_domains,
            "session_allowed_domains": session_domains,
            "sessionAllowedDomains": session_domains,
            "has_github_token": has_github_token,
            "hasGitHubToken": has_github_token,
            "has_huggingface_token": has_huggingface_token,
            "hasHuggingFaceToken": has_huggingface_token,
            "has_openalex_api_key": has_openalex_api_key,
            "hasOpenAlexAPIKey": has_openalex_api_key,
            "search_provider": search_provider_val,
            "searchProvider": search_provider_val,
            "has_search_api_key": has_search_api_key,
            "hasSearchAPIKey": has_search_api_key,
            "crossref_mailto": cfg.crossref_mailto,
            "crossrefMailto": cfg.crossref_mailto,
            "isNativeRustSupported": true,
        })
    }

    /// Updates `self.online_research_config` from a `"configure_online_research"` JSON command
    /// and returns the sanitized status JSON.
    pub fn configure_online_research_from_json(&mut self, parsed: &Value) -> Value {
        if let Some(mode_str) = parsed
            .get("mode")
            .or_else(|| parsed.get("online_research_mode"))
            .or_else(|| parsed.get("onlineResearchMode"))
            .and_then(|v| v.as_str())
        {
            self.online_research_config.mode =
                crate::online_fetch::OnlineResearchMode::from_wire(mode_str);
        }

        if let Some(arr) = parsed
            .get("custom_allowed_domains")
            .or_else(|| parsed.get("customAllowedDomains"))
            .and_then(|v| v.as_array())
        {
            self.online_research_config.custom_allowed_domains = arr
                .iter()
                .filter_map(|v| v.as_str().and_then(normalize_domain_entry))
                .collect();
        }

        if let Some(arr) = parsed
            .get("session_allowed_domains")
            .or_else(|| parsed.get("sessionAllowedDomains"))
            .and_then(|v| v.as_array())
        {
            self.online_research_config.session_allowed_domains = arr
                .iter()
                .filter_map(|v| v.as_str().and_then(normalize_domain_entry))
                .collect();
        }

        if let Some(arr) = parsed
            .get("once_allowed_domains")
            .or_else(|| parsed.get("onceAllowedDomains"))
            .and_then(|v| v.as_array())
        {
            self.online_research_config.once_allowed_domains = arr
                .iter()
                .filter_map(|v| v.as_str().and_then(normalize_domain_entry))
                .collect();
        }

        let update_secret = |target: &mut Option<String>, primary: &str, camel: &str| {
            if let Some(val) = parsed.get(primary).or_else(|| parsed.get(camel)) {
                match val {
                    Value::Null => *target = None,
                    Value::String(s) => {
                        let t = s.trim();
                        if t.is_empty() {
                            *target = None;
                        } else {
                            *target = Some(t.to_string());
                        }
                    }
                    _ => {}
                }
            }
        };

        update_secret(
            &mut self.online_research_config.github_token,
            "github_token",
            "githubToken",
        );
        update_secret(
            &mut self.online_research_config.huggingface_token,
            "huggingface_token",
            "huggingfaceToken",
        );
        update_secret(
            &mut self.online_research_config.openalex_api_key,
            "openalex_api_key",
            "openalexApiKey",
        );
        update_secret(
            &mut self.online_research_config.search_api_key,
            "search_api_key",
            "searchApiKey",
        );
        update_secret(
            &mut self.online_research_config.crossref_mailto,
            "crossref_mailto",
            "crossrefMailto",
        );

        if let Some(prov_val) = parsed
            .get("search_provider")
            .or_else(|| parsed.get("searchProvider"))
        {
            match prov_val {
                Value::Null => self.online_research_config.search_provider = None,
                Value::String(s) => {
                    self.online_research_config.search_provider =
                        crate::online_fetch::SearchProviderKind::from_wire(s);
                }
                _ => {}
            }
        }

        if let Some(max_file) = parsed
            .get("max_single_file_bytes")
            .or_else(|| parsed.get("maxSingleFileBytes"))
            .and_then(|v| v.as_u64())
        {
            if max_file > 0 {
                self.online_research_config.max_single_file_bytes = max_file as usize;
            }
        }
        if let Some(max_tree) = parsed
            .get("max_tree_bytes")
            .or_else(|| parsed.get("maxTreeBytes"))
            .and_then(|v| v.as_u64())
        {
            if max_tree > 0 {
                self.online_research_config.max_tree_bytes = max_tree as usize;
            }
        }
        if let Some(ws_root) = parsed
            .get("workspace_root")
            .or_else(|| parsed.get("workspaceRoot"))
            .and_then(|v| v.as_str())
            .map(str::trim)
            .filter(|s| !s.is_empty())
        {
            self.online_research_config.workspace_root = Some(std::path::PathBuf::from(ws_root));
        }
        if let Some(ws_ro) = parsed
            .get("workspace_read_only")
            .or_else(|| parsed.get("workspaceReadOnly"))
            .and_then(|v| v.as_bool())
        {
            self.online_research_config.workspace_read_only = ws_ro;
        }

        self.online_research_status_json()
    }

    /// 1-click Air-Gap Kill-Switch: immediately resets `mode = Offline` and clears ephemeral
    /// `session_allowed_domains` and `once_allowed_domains`.
    pub fn air_gap_now(&mut self) -> Value {
        self.online_research_config.mode = crate::online_fetch::OnlineResearchMode::Offline;
        self.online_research_config.session_allowed_domains.clear();
        self.online_research_config.once_allowed_domains.clear();
        self.online_research_status_json()
    }

    pub(super) fn execute_fetch_for_session_internal(
        &mut self,
        thread_id: &str,
        wire_messages: &[WireMessage],
        project: &StoredProject,
        call_id: &str,
        raw_url: &str,
        user_confirmed: bool,
    ) -> Result<String, (Option<(String, String, &'static str)>, String)> {
        let project_root = Path::new(&project.root_path);
        let mut cfg = self.online_research_config.clone();
        cfg.workspace_root = Some(project_root.to_path_buf());
        cfg.workspace_read_only = project.is_read_only;
        if cfg.fallback_data_dir.is_none() {
            cfg.fallback_data_dir = Some(self.locator.data_dir.clone());
        }

        if cfg.mode == crate::online_fetch::OnlineResearchMode::Offline {
            return Err((
                None,
                "Online research is currently Offline (Air-Gapped). Enable 'Curated Research Allowlist' or 'Open Web' in BeatrixLeslie IDE Settings -> Online Research to fetch live URLs."
                    .to_string(),
            ));
        }

        let parsed_host_repo = parse_url_domain_and_repo_key(raw_url);

        // Session Taint Guard (Breaking the Lethal Trifecta):
        // web_tainted && workspace_read && code-hosting repo not yet in session/once approvals or user prompt text
        if !user_confirmed
            && self.web_tainted_threads.contains(thread_id)
            && self.workspace_read_threads.contains(thread_id)
        {
            if let Some((ref domain, ref repo_key)) = parsed_host_repo {
                if is_code_hosting_repo_domain(domain)
                    && !Self::is_domain_ephemerally_approved(&cfg, domain, repo_key.as_deref())
                    && !Self::thread_user_prompts_reference_url_or_repo(
                        wire_messages,
                        raw_url,
                        repo_key.as_deref(),
                    )
                {
                    let reason = format!(
                        "Web-tainted session read local workspace files and is now requesting outbound access to {raw_url}. Approve?"
                    );
                    let card_msg = format!("Domain approval required for '{domain}': {reason}");
                    return Err((
                        Some((domain.clone(), reason, "taint_guard")),
                        card_msg,
                    ));
                }
            }
        }

        let fetch_res = if let Some(ref custom_fetcher) = self.online_fetcher_override {
            crate::online_fetch::execute_online_fetch_with_fetcher(
                raw_url,
                &cfg,
                custom_fetcher.as_ref(),
            )
        } else {
            crate::online_fetch::execute_online_fetch(raw_url, &cfg)
        };

        // Consume single-shot `once_allowed_domains` entry immediately after execution attempt
        if let Some((ref domain, _)) = parsed_host_repo {
            self.online_research_config
                .once_allowed_domains
                .remove(domain);
        }

        match fetch_res {
            Ok(outcome) => {
                self.web_tainted_threads.insert(thread_id.to_string());
                if let Some(ref staged_root) = outcome.staged_root {
                    if cfg.workspace_read_only || !staged_root.starts_with(project_root) {
                        self.register_extra_readable_root(staged_root);
                        if let Some(parent) = staged_root.parent() {
                            if crate::online_fetch::is_research_cache_path(parent) {
                                self.register_extra_readable_root(parent);
                            }
                        }
                    }
                }
                self.last_fetch_citations
                    .insert(call_id.to_string(), outcome.citation.clone());
                self.last_fetch_citations
                    .insert(raw_url.trim().to_string(), outcome.citation.clone());
                Ok(outcome.envelope_markdown)
            }
            Err(crate::online_fetch::OnlineFetchError::OfflineAirGapped(_)) => Err((
                None,
                "Online research is currently Offline (Air-Gapped). Enable 'Curated Research Allowlist' or 'Open Web' in BeatrixLeslie IDE Settings -> Online Research to fetch live URLs."
                    .to_string(),
            )),
            Err(crate::online_fetch::OnlineFetchError::DomainApprovalRequired { domain, url }) => {
                self.online_research_config
                    .once_allowed_domains
                    .remove(&domain);
                match cfg.mode {
                    crate::online_fetch::OnlineResearchMode::OpenWebAskPerDomain => {
                        let reason = format!(
                            "Domain approval required for '{domain}' before fetching {url}. Choose Allow Once, Allow Session, Always Allow, or Deny."
                        );
                        Err((Some((domain, reason.clone(), "domain_approval")), reason))
                    }
                    _ => Err((
                        None,
                        format!(
                            "DOMAIN_NOT_ALLOWLISTED: Outbound fetch to '{domain}' ({url}) is blocked in Curated Research Allowlist mode. Add '{domain}' under Custom Allowed Domains in BeatrixLeslie IDE Settings -> Online Research, or switch to Open Web (Auto-Approve All Domains)."
                        ),
                    )),
                }
            }
            Err(crate::online_fetch::OnlineFetchError::TaintConfirmationRequired {
                domain,
                url: _,
                reason,
            }) => {
                let card_msg = format!("Domain approval required for '{domain}': {reason}");
                Err((Some((domain, reason, "taint_guard")), card_msg))
            }
            Err(other) => Err((None, other.to_string())),
        }
    }
}
