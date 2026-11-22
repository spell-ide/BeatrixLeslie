use super::*;

impl SpellCoreSession {
    // ========================================================================
    // Project, Thread & Tag Management
    // ========================================================================

    pub fn add_project(
        &mut self,
        name: &str,
        root_path: &str,
        is_read_only: bool,
    ) -> Result<StoredProject, String> {
        let id = next_storage_id("proj");
        // Projects are numbered by default ("Project 3"); the user can rename them.
        let resolved_name = if name.trim().is_empty() {
            let existing = self.storage.list_projects()?;
            format!("Project {}", next_title_number(existing.iter().map(|p| p.name.as_str()), "Project"))
        } else {
            name.trim().to_string()
        };
        let project = StoredProject::new(&id, &resolved_name, root_path, is_read_only);
        self.storage.upsert_project(&project)?;
        self.tag_registry.load_project_tags(Path::new(root_path));
        self.selected_project_id = Some(id.clone());
        self.workspace_root = std::path::PathBuf::from(root_path);
        if !self.message_terminal.is_open {
            self.message_terminal.working_directory = root_path.to_string();
        }

        let default_thread = StoredThread::new(
            next_storage_id("thread"),
            &id,
            "Conversation 1".to_string(),
            if is_read_only { "ask" } else { "write" },
            Vec::new(),
        );
        self.storage.upsert_thread(&default_thread)?;
        self.selected_thread_id = Some(default_thread.id.clone());
        Ok(project)
    }

    pub fn select_project(&mut self, project_id: &str) -> Result<(), String> {
        let Some(proj) = self.storage.get_project(project_id)? else {
            return Err(format!("Project '{project_id}' not found"));
        };
        self.selected_project_id = Some(project_id.to_string());
        self.workspace_root = std::path::PathBuf::from(&proj.root_path);
        if !self.message_terminal.is_open {
            self.message_terminal.working_directory = proj.root_path.clone();
        }
        let threads = self.storage.list_threads(Some(project_id))?;
        self.selected_thread_id = threads.first().map(|t| t.id.clone());
        Ok(())
    }

    pub fn toggle_project_read_only(&mut self, project_id: &str) -> Result<StoredProject, String> {
        let mut proj = self
            .storage
            .get_project(project_id)?
            .ok_or_else(|| format!("Project '{project_id}' not found"))?;
        proj.is_read_only = !proj.is_read_only;
        self.storage.upsert_project(&proj)?;
        Ok(proj)
    }

    pub fn create_thread(
        &mut self,
        project_id: Option<&str>,
        title: Option<&str>,
        profile: Option<&str>,
        active_tags: Vec<String>,
    ) -> Result<StoredThread, String> {
        let pid = project_id
            .map(str::to_string)
            .or_else(|| self.selected_project_id.clone())
            .ok_or_else(|| "No active project selected".to_string())?;
        let proj = self
            .storage
            .get_project(&pid)?
            .ok_or_else(|| format!("Project '{pid}' not found"))?;
        let existing = self.storage.list_threads(Some(&pid))?;
        let resolved_title = title
            .filter(|s| !s.trim().is_empty())
            .map(str::to_string)
            .unwrap_or_else(|| {
                format!(
                    "Conversation {}",
                    next_title_number(existing.iter().map(|t| t.title.as_str()), "Conversation")
                )
            });
        let resolved_profile = if proj.is_read_only {
            "ask".to_string()
        } else {
            profile
                .map(|p| p.to_ascii_lowercase())
                .filter(|p| p == "ask" || p == "write")
                .unwrap_or_else(|| "write".to_string())
        };
        let tid = next_storage_id("thread");
        let thread = StoredThread::new(tid.clone(), pid, resolved_title, resolved_profile, active_tags);
        self.storage.upsert_thread(&thread)?;
        self.selected_thread_id = Some(tid);
        Ok(thread)
    }

    pub fn select_thread(&mut self, thread_id: &str) -> Result<StoredThread, String> {
        let thread = self
            .storage
            .get_thread(thread_id)?
            .ok_or_else(|| format!("Thread '{thread_id}' not found"))?;
        self.selected_thread_id = Some(thread.id.clone());
        self.selected_project_id = Some(thread.project_id.clone());
        Ok(thread)
    }

    pub fn delete_thread(&mut self, thread_id: &str) -> Result<(), String> {
        self.storage.delete_thread(thread_id)?;
        if self
            .last_prefix_snapshot
            .as_ref()
            .map(|s| s.thread_id == thread_id)
            .unwrap_or(false)
        {
            self.last_prefix_snapshot = None;
        }
        if self.selected_thread_id.as_deref() == Some(thread_id) {
            let remaining = self
                .storage
                .list_threads(self.selected_project_id.as_deref())?;
            self.selected_thread_id = remaining.first().map(|t| t.id.clone());
        }
        Ok(())
    }

    /// Renames a project (an empty name restores its numbered default).
    pub fn rename_project(&mut self, project_id: &str, name: &str) -> Result<StoredProject, String> {
        let mut proj = self
            .storage
            .get_project(project_id)?
            .ok_or_else(|| format!("Project '{project_id}' not found"))?;
        let trimmed = name.trim();
        proj.name = if trimmed.is_empty() {
            let others = self.storage.list_projects()?;
            format!(
                "Project {}",
                next_title_number(others.iter().filter(|p| p.id != project_id).map(|p| p.name.as_str()), "Project")
            )
        } else {
            trimmed.chars().take(120).collect()
        };
        self.storage.upsert_project(&proj)?;
        Ok(proj)
    }

    /// Renames a conversation (an empty title restores its numbered default). Does not change
    /// `updated_at`, so renaming does not reorder the history.
    pub fn rename_thread(&mut self, thread_id: &str, title: &str) -> Result<StoredThread, String> {
        let mut thread = self
            .storage
            .get_thread(thread_id)?
            .ok_or_else(|| format!("Thread '{thread_id}' not found"))?;
        let trimmed = title.trim();
        thread.title = if trimmed.is_empty() {
            let siblings = self.storage.list_threads(Some(&thread.project_id))?;
            format!(
                "Conversation {}",
                next_title_number(
                    siblings.iter().filter(|t| t.id != thread_id).map(|t| t.title.as_str()),
                    "Conversation"
                )
            )
        } else {
            trimmed.chars().take(120).collect()
        };
        self.storage.upsert_thread(&thread)?;
        Ok(thread)
    }

    pub fn set_thread_profile(
        &mut self,
        thread_id: &str,
        profile: &str,
    ) -> Result<StoredThread, String> {
        let mut thread = self
            .storage
            .get_thread(thread_id)?
            .ok_or_else(|| format!("Thread '{thread_id}' not found"))?;
        let normalized = if profile.trim().eq_ignore_ascii_case("ask") {
            "ask"
        } else {
            "write"
        };
        thread.profile = normalized.to_string();
        thread.updated_at = now_unix_secs();
        self.storage.upsert_thread(&thread)?;
        Ok(thread)
    }

    pub fn toggle_tag_on_thread(
        &mut self,
        thread_id: &str,
        tag_name_or_id: &str,
    ) -> Result<StoredThread, String> {
        let mut thread = self
            .storage
            .get_thread(thread_id)?
            .ok_or_else(|| format!("Thread '{thread_id}' not found"))?;
        let canonical = self
            .tag_registry
            .get_tag(tag_name_or_id)
            .map(|t| t.id)
            .unwrap_or_else(|| normalize_tag_key(tag_name_or_id));

        if let Some(pos) = thread
            .active_tags
            .iter()
            .position(|t| normalize_tag_key(t) == canonical)
        {
            thread.active_tags.remove(pos);
            self.tag_registry.deactivate_tag(&canonical);
        } else {
            thread.active_tags.push(canonical.clone());
            let _ = self.tag_registry.apply_tag(&canonical);
        }
        thread.updated_at = now_unix_secs();
        self.storage.upsert_thread(&thread)?;
        Ok(thread)
    }

    pub fn create_custom_tag(
        &mut self,
        name: &str,
        category: &str,
        description: &str,
        rubric_prompt: &str,
    ) -> Result<ConversationTag, String> {
        self.upsert_tag_with_id(None, name, category, description, rubric_prompt)
    }

    pub fn upsert_tag_with_id(
        &mut self,
        explicit_id: Option<&str>,
        name: &str,
        category: &str,
        description: &str,
        instruction: &str,
    ) -> Result<ConversationTag, String> {
        let raw_id = explicit_id
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| name.trim())
            .strip_prefix("@tag:")
            .or_else(|| explicit_id.map(str::trim).and_then(|s| s.strip_prefix("@skill:")))
            .unwrap_or_else(|| {
                explicit_id
                    .map(str::trim)
                    .filter(|s| !s.is_empty())
                    .unwrap_or_else(|| name.trim())
            });
        let id = normalize_tag_key(raw_id);
        if id.is_empty() {
            return Err("Tag ID or name cannot be empty".to_string());
        }
        let display_name = if name.trim().is_empty() {
            id.clone()
        } else {
            name.trim().to_string()
        };
        let mut tag = self.tag_registry.get_tag(&id).unwrap_or_else(|| {
            ConversationTag::new(
                id.clone(),
                display_name.clone(),
                if category.trim().is_empty() {
                    "general"
                } else {
                    category.trim()
                },
                description.trim(),
                instruction.trim(),
                false,
            )
        });
        tag.id = id.clone();
        tag.name = display_name;
        if !category.trim().is_empty() {
            tag.category = category.trim().to_string();
        }
        if !description.trim().is_empty() {
            tag.description = description.trim().to_string();
        }
        tag.rubric_prompt = instruction.trim().to_string();
        if tag.trigger.is_none() {
            tag.trigger = Some(format!("@tag:{id}"));
        }
        self.tag_registry.upsert_tag(tag.clone());
        self.storage.upsert_tag(&tag)?;
        Ok(tag)
    }

    pub fn create_or_update_steering(
        &mut self,
        explicit_id: Option<&str>,
        name: &str,
        instruction: &str,
    ) -> Result<crate::tags::SteeringDirective, String> {
        let raw_source = explicit_id
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| name.trim());
        let stripped = raw_source
            .strip_prefix("#steer:")
            .or_else(|| raw_source.strip_prefix('#'))
            .unwrap_or(raw_source);
        let id = normalize_tag_key(stripped);
        if id.is_empty() {
            return Err("Steering ID or name cannot be empty".to_string());
        }
        let display_name = if name.trim().is_empty() {
            id.clone()
        } else {
            name.trim().to_string()
        };
        let mut directive = self
            .tag_registry
            .get_steering(&id)
            .unwrap_or_else(|| crate::tags::SteeringDirective {
                id: id.clone(),
                name: display_name.clone(),
                trigger: format!("#steer:{id}"),
                description: String::new(),
                system_append: instruction.trim().to_string(),
                temperature: None,
                top_p: None,
                presence_penalty: None,
                repetition_penalty: None,
                max_tokens: None,
                enable_thinking: None,
                thinking_budget: None,
                is_builtin: false,
            });
        directive.id = id.clone();
        directive.name = display_name;
        if directive.trigger.trim().is_empty() {
            directive.trigger = format!("#steer:{id}");
        }
        directive.system_append = instruction.trim().to_string();
        self.tag_registry.upsert_steering(directive.clone());
        self.storage.upsert_steering(&directive)?;
        Ok(directive)
    }

    pub fn toggle_steering(&mut self, id_or_trigger: &str) -> Result<Vec<String>, String> {
        let canonical = self
            .tag_registry
            .get_steering(id_or_trigger)
            .map(|d| d.id)
            .unwrap_or_else(|| {
                let s = id_or_trigger
                    .trim()
                    .strip_prefix("#steer:")
                    .or_else(|| id_or_trigger.trim().strip_prefix('#'))
                    .unwrap_or(id_or_trigger.trim());
                normalize_tag_key(s)
            });
        if canonical.is_empty() {
            return Err("Steering identifier cannot be empty".to_string());
        }
        if let Some(pos) = self
            .active_steering_ids
            .iter()
            .position(|x| normalize_tag_key(x) == canonical)
        {
            self.active_steering_ids.remove(pos);
        } else {
            self.active_steering_ids.push(canonical);
        }
        Ok(self.active_steering_ids.clone())
    }

    pub fn set_tool_permission_policy(
        &mut self,
        tool_name: &str,
        policy: ToolPermissionPolicy,
    ) -> Result<(), String> {
        self.tool_permissions.insert(tool_name.to_string(), policy);
        self.storage.set_tool_permission(tool_name, policy)
    }

    /// Opens the inline Message Terminal at `raw_path` (validated against `self.workspace_root`).
    pub fn open_message_terminal(
        &mut self,
        raw_path: &str,
        reason: Option<String>,
    ) -> Result<String, String> {
        let trimmed = raw_path.trim();
        let resolved = if trimmed.is_empty() || trimmed == "." {
            self.workspace_root.clone()
        } else {
            crate::tools::canonicalize_target_path(&self.workspace_root, Path::new(trimmed))
                .map_err(|e| e.to_string())?
        };
        let target_dir = if resolved.is_file() {
            resolved
                .parent()
                .unwrap_or(&self.workspace_root)
                .to_path_buf()
        } else {
            resolved
        };
        self.message_terminal.session_id += 1;
        self.message_terminal.is_open = true;
        self.message_terminal.working_directory = target_dir.to_string_lossy().to_string();
        self.message_terminal.reason = reason.filter(|s| !s.trim().is_empty());
        Ok(format!(
            "Opened Message Terminal in {} (session #{})",
            self.message_terminal.working_directory, self.message_terminal.session_id
        ))
    }

    /// Closes the inline Message Terminal.
    pub fn close_message_terminal(&mut self) {
        self.message_terminal.is_open = false;
    }

    /// Executes a command inside the sandboxed Message Terminal (`10s` timeout, `cd` tracking).
    pub fn run_message_terminal_command(
        &mut self,
        command: &str,
        cwd_override: Option<&str>,
    ) -> Result<String, String> {
        let base_cwd = cwd_override
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .map(str::to_string)
            .unwrap_or_else(|| {
                if self.message_terminal.working_directory.trim().is_empty() {
                    ".".to_string()
                } else {
                    self.message_terminal.working_directory.clone()
                }
            });

        let resolved_cwd = if base_cwd == "." {
            crate::tools::canonicalize_target_path(&self.workspace_root, Path::new("."))
                .unwrap_or_else(|_| self.workspace_root.clone())
        } else {
            match crate::tools::canonicalize_target_path(&self.workspace_root, Path::new(&base_cwd))
            {
                Ok(p) => p,
                Err(e) => {
                    let err_msg = e.to_string();
                    self.message_terminal.last_output = err_msg.clone();
                    return Ok(err_msg);
                }
            }
        };

        let trimmed_cmd = command.trim();
        if trimmed_cmd.is_empty() {
            return Ok(self.message_terminal.last_output.clone());
        }

        if trimmed_cmd == "cd" || trimmed_cmd.starts_with("cd ") {
            let target_sub = trimmed_cmd.strip_prefix("cd").unwrap_or("").trim();
            let candidate_dir = if target_sub.is_empty() || target_sub == "~" {
                self.workspace_root.clone()
            } else if Path::new(target_sub).is_absolute() {
                std::path::PathBuf::from(target_sub)
            } else {
                resolved_cwd.join(target_sub)
            };

            match crate::tools::canonicalize_target_path(&self.workspace_root, &candidate_dir) {
                Ok(resolved_dir) => {
                    if !resolved_dir.is_dir() {
                        let msg = format!(
                            "NOT_A_DIRECTORY: '{}' is not an existing directory",
                            resolved_dir.display()
                        );
                        self.message_terminal.last_output = msg.clone();
                        return Ok(msg);
                    }
                    self.message_terminal.working_directory =
                        resolved_dir.to_string_lossy().to_string();
                    let out = format!("cwd -> {}", self.message_terminal.working_directory);
                    self.message_terminal.last_output = out.clone();
                    return Ok(out);
                }
                Err(e) => {
                    let msg = e.to_string();
                    self.message_terminal.last_output = msg.clone();
                    return Ok(msg);
                }
            }
        }

        self.message_terminal.working_directory = resolved_cwd.to_string_lossy().to_string();
        match crate::tools::run_terminal_command(&resolved_cwd, trimmed_cmd, 10_000, true) {
            Ok(out) => {
                self.message_terminal.last_output = out.clone();
                Ok(out)
            }
            Err(e) => {
                let msg = e.to_string();
                self.message_terminal.last_output = msg.clone();
                Ok(msg)
            }
        }
    }

    // ========================================================================
    // 1. Multi-Turn Agent Loop (`send_user_prompt`)
    // ========================================================================

    // ========================================================================

    /// Advances the local `thread.engine_compaction_epoch`, resets seen-file pinning and the
    /// cached wire prefix snapshot, records an `engine_compaction_notice`, and persists to SQLite.
    /// Operates 100% locally with zero network I/O.
    pub fn request_engine_hot_cache_clear(
        &mut self,
        thread_id: &str,
    ) -> Result<EngineContextAdvice, String> {
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

        let new_epoch = thread.engine_compaction_epoch.saturating_add(1);
        let cache_key = resolve_prompt_cache_key(
            &thread.id,
            new_epoch,
            self.epoch_cache_key_suffix,
        );
        let (decode_tok_s, decode_tok_s_ewma) = {
            let cache = self.engine_status.lock();
            (cache.decode_tok_s, cache.decode_tok_s_ewma)
        };
        let advice = EngineContextAdvice {
            action: "context_reset".to_string(),
            reason: "Local context epoch reset".to_string(),
            decode_tok_s,
            decode_tok_s_ewma,
            hot_cache_cleared: true,
            thread_key: cache_key,
            compaction_epoch: new_epoch,
            ..Default::default()
        };

        thread.engine_compaction_epoch = new_epoch;
        thread.last_context_advice = Some(advice.clone());
        thread
            .ui_events
            .push(UiChatEvent::engine_compaction_notice(&advice));
        thread.updated_at = now_unix_secs();
        self.turn_pin_cache.clear_seen_files_for_new_epoch();
        if self
            .last_prefix_snapshot
            .as_ref()
            .map(|s| s.thread_id == thread.id)
            .unwrap_or(false)
        {
            self.last_prefix_snapshot = None;
        }
        self.storage.upsert_thread(&thread)?;

        Ok(advice)
    }

    /// Creates and persists a local wall-clock timer in SQLite (zero network I/O).
    pub fn schedule_engine_timer(
        &mut self,
        thread_id: &str,
        duration_s: f64,
        label: &str,
        note: &str,
        watch_path: Option<&str>,
    ) -> Result<TimerRecord, String> {
        let resolved_tid = if thread_id.trim().is_empty() {
            self.selected_thread_id
                .clone()
                .ok_or_else(|| "No active conversation selected".to_string())?
        } else {
            thread_id.trim().to_string()
        };
        let watch_target = watch_path
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .map(|p| json!({ "kind": "file_tail", "path": p }));

        let record = self.storage.create_timer(
            &resolved_tid,
            duration_s,
            label,
            note,
            watch_target.as_ref(),
        )?;
        self.engine_status.lock().timers = self.storage.list_timers(None, false).unwrap_or_default();
        Ok(record)
    }

    /// Atomically pops due local SQLite timers for `thread_id`.
    /// When no timers are due, returns `Ok(Vec::new())` immediately with ZERO network I/O.
    /// When timers are due, records alarm banners locally and — if an endpoint and model are
    /// configured (or a custom test transport is active) — runs optional background wakeup triage
    /// and continues the agent loop.
    pub fn poll_and_fire_due_engine_timers(
        &mut self,
        thread_id: &str,
    ) -> Result<Vec<TimerRecord>, String> {
        let resolved_tid = if thread_id.trim().is_empty() {
            self.selected_thread_id
                .clone()
                .ok_or_else(|| "No active thread selected".to_string())?
        } else {
            thread_id.trim().to_string()
        };

        let due_timers = self.storage.pop_due_timers(Some(&resolved_tid))?;
        self.engine_status.lock().timers = self.storage.list_timers(None, false).unwrap_or_default();

        if due_timers.is_empty() {
            return Ok(Vec::new());
        }

        self.timer_tick_seq += 1;
        let base_tid = derive_thread_key(Some(&resolved_tid));
        let first_timer_id = due_timers
            .first()
            .map(|t| t.effective_id().to_string())
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| format!("tick_{}", self.timer_tick_seq));
        let mut thread = self
            .storage
            .get_thread(&resolved_tid)?
            .ok_or_else(|| format!("Thread '{resolved_tid}' not found"))?;
        if thread.wire_messages.is_empty() {
            thread
                .wire_messages
                .push(WireMessage::user("Resume on scheduled wakeup timer."));
        }

        let banners: Vec<String> = due_timers
            .iter()
            .map(|t| t.format_engine_alarm_banner())
            .collect();
        apply_engine_alarm_banners_to_messages(&mut thread.wire_messages, &banners);
        for banner in &banners {
            thread.ui_events.push(UiChatEvent::engine_timer_alarm(banner));
        }
        thread.updated_at = now_unix_secs();
        self.storage.upsert_thread(&thread)?;

        let can_call_endpoint = (self.uses_custom_transport || self.is_endpoint_configured())
            && !self.model_id.trim().is_empty();

        if can_call_endpoint {
            let sub_model = self.effective_subagent_model_id();
            if !sub_model.is_empty() {
                let timer_key = format!("timer:{base_tid}:{first_timer_id}");
                let due_summary = due_timers
                    .iter()
                    .map(|t| {
                        format!(
                            "- timer_id={} label=\"{}\" note=\"{}\" watch={}",
                            t.effective_id(),
                            t.label,
                            t.note,
                            t.watch_target
                                .as_ref()
                                .map(Value::to_string)
                                .unwrap_or_else(|| "none".to_string())
                        )
                    })
                    .collect::<Vec<_>>()
                    .join("\n");

                let timer_role_directive = "You are a Spell IDE background wakeup triage subagent. \
                    Produce a concise, actionable 1-3 line digest of the fired timer(s) for the main orchestrator.";
                let timer_payload = format!("<due_timers>\n{due_summary}\n</due_timers>");
                let (parent_key, aligned_messages, aligned_tools) = self
                    .build_shared_prefix_subagent_prompt(
                        &resolved_tid,
                        thread.engine_compaction_epoch,
                        timer_role_directive,
                        timer_role_directive,
                        &timer_payload,
                    );

                let timer_req = SubagentCompletionRequest {
                    model: sub_model.clone(),
                    messages: aligned_messages,
                    stream: false,
                    max_tokens: Some(256),
                    temperature: Some(0.0),
                    prompt_cache_key: Some(timer_key.clone()),
                    parent_cache_key: Some(parent_key),
                    reasoning_effort: None,
                    thinking_budget: None,
                    tools: aligned_tools,
                    enable_thinking: Some(false),
                    ..Default::default()
                };

                match self.active_transport().run_subagent_completion(&timer_req) {
                    Ok(triage_resp) => {
                        self.record_live_subagent_completion(&triage_resp);
                        let cache_badge = if triage_resp.prefix_cache_reused {
                            " · SUBAGENT LIVE (PREFIX CACHED)"
                        } else {
                            " · SUBAGENT LIVE"
                        };
                        let triage_digest = format!(
                            "<timer_triage model=\"{sub_model}\" cache_key=\"{timer_key}\" tok_s=\"{:.1}\">\n{}\n</timer_triage>",
                            triage_resp.decode_tok_s,
                            triage_resp.content.trim()
                        );
                        apply_engine_alarm_banners_to_messages(
                            &mut thread.wire_messages,
                            std::slice::from_ref(&triage_digest),
                        );
                        thread.ui_events.push(UiChatEvent::engine_timer_alarm(&format!(
                            "{}{cache_badge}",
                            triage_resp.content.trim()
                        )));
                    }
                    Err(TransportError::ModelRoleViolation(msg)) => {
                        return Err(format!("ModelRoleViolation: {msg}"));
                    }
                    Err(err) if err.is_subagent_unavailable() => {
                        // Subagent endpoint temporarily unavailable; proceed with main continuation.
                    }
                    Err(err) => {
                        return Err(err.to_string());
                    }
                }
            }

            self.run_agent_continuation_loop(&mut thread, DEFAULT_MAX_AGENT_STEPS)?;
        }

        Ok(due_timers)
    }

    /// Cancels a local SQLite timer by ID (zero network I/O).
    pub fn cancel_engine_timer(&mut self, timer_id: &str) -> Result<bool, String> {
        let cancelled = self.storage.cancel_timer(timer_id)?;
        self.engine_status.lock().timers = self.storage.list_timers(None, false).unwrap_or_default();
        Ok(cancelled)
    }

    pub fn cancel_active_turn(&mut self) -> Result<(), String> {
        crate::request_turn_cancel();
        if let (Some(client), Some(req_id)) = (&self.mcp_client, self.active_mcp_req_id.take()) {
            let _ = client.cancel_request(req_id, Some("User cancelled active turn"));
        }
        Ok(())
    }
}

/// Next default number for `"<prefix> N"` titles: one past the highest number in use (so numbers
/// are never reused after a deletion within a session's history).
pub(super) fn next_title_number<'a>(titles: impl Iterator<Item = &'a str>, prefix: &str) -> usize {
    titles
        .filter_map(|t| t.strip_prefix(prefix))
        .filter_map(|rest| rest.trim().parse::<usize>().ok())
        .max()
        .unwrap_or(0)
        + 1
}
