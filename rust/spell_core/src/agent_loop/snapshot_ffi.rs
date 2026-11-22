use super::*;

impl SpellCoreSession {
    // ========================================================================

    /// Returns a strongly-typed `WorkspaceSnapshot` with normalized `telemetry.kv_cache_fill_pct`.
    /// When `telemetry.engine_online` is `false`, `kv_cache_fill_pct` is strictly `0.0` even if
    /// `kv_cache_used_tokens` / `kv_cache_capacity_tokens` or `active_thread.prompt_tokens` are non-zero.
    pub fn workspace_snapshot(&self) -> WorkspaceSnapshot {
        let projects = self.storage.list_projects().unwrap_or_default();
        let selected_pid = self
            .selected_project_id
            .clone()
            .or_else(|| projects.first().map(|p| p.id.clone()));

        let threads = self.storage.list_threads(None).unwrap_or_default();
        let selected_tid = self
            .selected_thread_id
            .clone()
            .filter(|tid| threads.iter().any(|t| &t.id == tid))
            .or_else(|| {
                threads
                    .iter()
                    .find(|t| selected_pid.as_deref() == Some(t.project_id.as_str()))
                    .map(|t| t.id.clone())
            })
            .or_else(|| threads.first().map(|t| t.id.clone()));
        let active_thread = selected_tid
            .as_deref()
            .and_then(|tid| threads.iter().find(|t| t.id == tid).cloned());

        let active_timers = self.storage.list_timers(None, false).unwrap_or_default();
        let local_pool = self.build_local_subagent_pool_status();

        let (
            cached_telemetry,
            engine_error,
            sticky_tok_s,
            sticky_reuses,
            saw_live_subagent,
            cached_decode_tok_s,
            cached_decode_ewma,
        ) = {
            let cache = self.engine_status.lock();
            (
                cache.telemetry.clone(),
                cache.last_error.clone(),
                cache.subagent_decode_tok_s,
                cache.prefix_cache_reuses,
                cache.saw_live_subagent_completion,
                cache.decode_tok_s,
                cache.decode_tok_s_ewma,
            )
        };
        let subagent_telemetry = EngineStatusSnapshot {
            subagent_model_id: self.effective_subagent_model_id(),
            subagent_inference_backed: local_pool.inference_backed || saw_live_subagent,
            subagent_decode_tok_s: local_pool.effective_decode_tok_s().max(sticky_tok_s),
            prefix_cache_reuses: sticky_reuses,
        };
        let mut telemetry = cached_telemetry.unwrap_or_default();
        if cached_decode_tok_s > 0.0 && telemetry.decode_tok_s <= 0.0 {
            telemetry.decode_tok_s = cached_decode_tok_s;
        }
        if cached_decode_ewma > 0.0 && telemetry.decode_tok_s_ewma <= 0.0 {
            telemetry.decode_tok_s_ewma = cached_decode_ewma;
        }
        if let Some(ref th) = active_thread {
            telemetry.compaction_epoch = telemetry.compaction_epoch.max(th.engine_compaction_epoch);
            if let Some(ref adv) = th.last_context_advice {
                if adv.decode_tok_s > 0.0 {
                    telemetry.decode_tok_s = adv.decode_tok_s;
                }
                if adv.decode_tok_s_ewma > 0.0 {
                    telemetry.decode_tok_s_ewma = adv.decode_tok_s_ewma;
                }
                telemetry.hot_cache_blocks_freed = telemetry
                    .hot_cache_blocks_freed
                    .max(adv.effective_freed_blocks());
            }
        }
        telemetry.active_timers_count = active_timers
            .iter()
            .filter(|t| !t.delivered && t.status != "cancelled")
            .count();
        let base_url = self.http_transport.base_url();
        telemetry.transport_mode = if base_url.trim().is_empty() {
            "Unconfigured".to_string()
        } else if base_url.starts_with("unix://") {
            format!("Unix {base_url}")
        } else {
            format!("HTTP {base_url}")
        };

        telemetry.kv_cache_fill_pct = if telemetry.engine_online {
            if telemetry.kv_cache_capacity_tokens > 0 {
                (((telemetry.kv_cache_used_tokens as f64)
                    / (telemetry.kv_cache_capacity_tokens as f64))
                    * 100.0)
                    .clamp(0.0, 100.0)
            } else {
                active_thread
                    .as_ref()
                    .map(|th| (((th.prompt_tokens as f64) / 262_144.0) * 100.0).clamp(0.0, 100.0))
                    .unwrap_or(0.0)
            }
        } else {
            0.0
        };

        WorkspaceSnapshot {
            telemetry,
            subagent_telemetry,
            subagent_pool_status: Some(local_pool),
            selected_project_id: selected_pid,
            selected_thread_id: selected_tid,
            active_timers,
            engine_error,
        }
    }

    /// Builds the complete, strongly-typed JSON state snapshot of `projects`, `threads`,
    /// `active_thread`, `tags` (`availableTags`), `telemetry`, `available_models`,
    /// `active_timers`, `tool_permissions`, `staged_mutations`, and `diagnostics`.
    pub fn build_full_state_snapshot_json(&self) -> String {
        let projects = self.storage.list_projects().unwrap_or_default();
        let selected_pid = self
            .selected_project_id
            .clone()
            .or_else(|| projects.first().map(|p| p.id.clone()));

        let threads = self
            .storage
            .list_threads(None)
            .unwrap_or_default();
        let selected_tid = self
            .selected_thread_id
            .clone()
            .filter(|tid| threads.iter().any(|t| &t.id == tid))
            .or_else(|| {
                threads
                    .iter()
                    .find(|t| selected_pid.as_deref() == Some(t.project_id.as_str()))
                    .map(|t| t.id.clone())
            })
            .or_else(|| threads.first().map(|t| t.id.clone()));
        let active_thread = selected_tid
            .as_deref()
            .and_then(|tid| threads.iter().find(|t| t.id == tid).cloned());

        let available_tags = self.tag_registry.list_tags();
        let available_steering = self.tag_registry.list_steering_directives();
        let ws = self.workspace_snapshot();
        let telemetry = ws.telemetry;
        let sub_tel = ws.subagent_telemetry;
        let subagent_pool_status = ws.subagent_pool_status;
        let active_timers = ws.active_timers;
        let engine_error = ws.engine_error;
        let kv_cache_fill_pct = telemetry.kv_cache_fill_pct;
        let enable_thinking = self
            .last_turn_preprocessed
            .as_ref()
            .and_then(|p| p.enable_thinking)
            .unwrap_or(true);

        let endpoint_configured = self.is_endpoint_configured();
        let endpoint_base_url = self.http_transport.base_url();
        let effective_subagent_model_id = self.effective_subagent_model_id();

        let (
            available_models,
            endpoint_latency_ms,
            total_prompt_tokens,
            total_completion_tokens,
            total_cached_tokens,
        ) = {
            let cache = self.engine_status.lock();
            (
                cache.available_models.clone(),
                cache.endpoint_latency_ms,
                cache.total_prompt_tokens,
                cache.total_completion_tokens,
                cache.total_cached_tokens,
            )
        };

        let mut telemetry_json = serde_json::to_value(&telemetry).unwrap_or_else(|_| json!({}));
        if let Some(obj) = telemetry_json.as_object_mut() {
            obj.insert("endpoint_configured".to_string(), json!(endpoint_configured));
            obj.insert("endpointConfigured".to_string(), json!(endpoint_configured));
            obj.insert("model_id".to_string(), json!(self.model_id));
            obj.insert("modelId".to_string(), json!(self.model_id));
            obj.insert(
                "main_orchestrator_model_id".to_string(),
                json!(self.model_id),
            );
            obj.insert(
                "mainOrchestratorModelId".to_string(),
                json!(self.model_id),
            );
            obj.insert(
                "subagent_model_id".to_string(),
                json!(sub_tel.subagent_model_id),
            );
            obj.insert(
                "subagentModelId".to_string(),
                json!(sub_tel.subagent_model_id),
            );
            obj.insert(
                "effective_subagent_model_id".to_string(),
                json!(effective_subagent_model_id),
            );
            obj.insert(
                "effectiveSubagentModelId".to_string(),
                json!(effective_subagent_model_id),
            );
            obj.insert(
                "coding_subagent_model_id".to_string(),
                json!(effective_subagent_model_id),
            );
            obj.insert(
                "codingSubagentModelId".to_string(),
                json!(effective_subagent_model_id),
            );
            obj.insert(
                "endpoint_latency_ms".to_string(),
                json!(endpoint_latency_ms),
            );
            obj.insert(
                "endpointLatencyMs".to_string(),
                json!(endpoint_latency_ms),
            );
            obj.insert("decodeTokS".to_string(), json!(telemetry.decode_tok_s));
            obj.insert(
                "decodeTokSEwma".to_string(),
                json!(telemetry.decode_tok_s_ewma),
            );
            obj.insert(
                "total_prompt_tokens".to_string(),
                json!(total_prompt_tokens),
            );
            obj.insert(
                "totalPromptTokens".to_string(),
                json!(total_prompt_tokens),
            );
            obj.insert(
                "total_completion_tokens".to_string(),
                json!(total_completion_tokens),
            );
            obj.insert(
                "totalCompletionTokens".to_string(),
                json!(total_completion_tokens),
            );
            obj.insert(
                "total_cached_tokens".to_string(),
                json!(total_cached_tokens),
            );
            obj.insert(
                "totalCachedTokens".to_string(),
                json!(total_cached_tokens),
            );
            obj.insert(
                "critic_enabled".to_string(),
                json!(self.critic_enabled),
            );
            obj.insert(
                "criticEnabled".to_string(),
                json!(self.critic_enabled),
            );
            obj.insert("engine_error".to_string(), json!(engine_error));
            obj.insert("engineError".to_string(), json!(engine_error));
            obj.insert("kv_cache_fill_pct".to_string(), json!(kv_cache_fill_pct));
            obj.insert("kvCacheFillPct".to_string(), json!(kv_cache_fill_pct));
            obj.insert("enable_thinking".to_string(), json!(enable_thinking));
            obj.insert("enableThinking".to_string(), json!(enable_thinking));
            obj.insert(
                "subagent_inference_backed".to_string(),
                json!(sub_tel.subagent_inference_backed),
            );
            obj.insert(
                "subagentInferenceBacked".to_string(),
                json!(sub_tel.subagent_inference_backed),
            );
            obj.insert(
                "subagent_decode_tok_s".to_string(),
                json!(sub_tel.subagent_decode_tok_s),
            );
            obj.insert(
                "subagentDecodeTokS".to_string(),
                json!(sub_tel.subagent_decode_tok_s),
            );
            obj.insert(
                "last_subagent_decode_tok_s".to_string(),
                json!(sub_tel.subagent_decode_tok_s),
            );
            obj.insert(
                "lastSubagentDecodeTokS".to_string(),
                json!(sub_tel.subagent_decode_tok_s),
            );
            obj.insert(
                "prefix_cache_reuses".to_string(),
                json!(sub_tel.prefix_cache_reuses),
            );
            obj.insert(
                "prefixCacheReuses".to_string(),
                json!(sub_tel.prefix_cache_reuses),
            );
            obj.insert("loop_state".to_string(), json!(self.loop_state.as_str()));
            obj.insert("loopState".to_string(), json!(self.loop_state.as_str()));
            obj.insert("loop_cycle_count".to_string(), json!(self.loop_cycle_count));
            obj.insert("loopCycleCount".to_string(), json!(self.loop_cycle_count));
            obj.insert(
                "critic_bounce_count".to_string(),
                json!(self.critic_bounce_count),
            );
            obj.insert(
                "criticBounceCount".to_string(),
                json!(self.critic_bounce_count),
            );
            obj.insert(
                "max_critic_bounces".to_string(),
                json!(self.max_critic_bounces),
            );
            obj.insert(
                "maxCriticBounces".to_string(),
                json!(self.max_critic_bounces),
            );
            if let Some(ref verdict) = self.last_critic_verdict {
                obj.insert("last_critic_verdict".to_string(), json!(verdict));
                obj.insert("lastCriticVerdict".to_string(), json!(verdict));
            }
            if let Some(ref sha) = self.last_git_snapshot_sha {
                obj.insert("last_git_snapshot_sha".to_string(), json!(sha));
                obj.insert("lastGitSnapshotSha".to_string(), json!(sha));
            }
            obj.insert("stage_write_mode".to_string(), json!(self.stage_write_mode));
            obj.insert("stageWriteMode".to_string(), json!(self.stage_write_mode));
            obj.insert(
                "message_terminal_open".to_string(),
                json!(self.message_terminal.is_open),
            );
            obj.insert(
                "messageTerminalOpen".to_string(),
                json!(self.message_terminal.is_open),
            );
            obj.insert(
                "message_terminal_cwd".to_string(),
                json!(self.message_terminal.working_directory),
            );
            obj.insert(
                "messageTerminalCwd".to_string(),
                json!(self.message_terminal.working_directory),
            );
            obj.insert(
                "message_terminal_reason".to_string(),
                json!(self.message_terminal.reason),
            );
            obj.insert(
                "messageTerminalReason".to_string(),
                json!(self.message_terminal.reason),
            );
            obj.insert(
                "message_terminal_session_id".to_string(),
                json!(self.message_terminal.session_id),
            );
            obj.insert(
                "messageTerminalSessionId".to_string(),
                json!(self.message_terminal.session_id),
            );
        }

        let tool_permissions_map: HashMap<String, String> = self
            .tool_permissions
            .iter()
            .map(|(k, v)| {
                let s = match v {
                    ToolPermissionPolicy::Allow => "allow",
                    ToolPermissionPolicy::Confirm => "confirm",
                    ToolPermissionPolicy::Deny => "deny",
                };
                (k.clone(), s.to_string())
            })
            .collect();

        let staged_mutations = self.snapshot_store.list_staged_mutations();
        let diagnostics = self.diagnostics_bus.all();

        let mut payload = json!({
            "ok": true,
            "projects": projects,
            "selected_project_id": selected_pid,
            "selectedProjectId": selected_pid,
            "threads": threads,
            "selected_thread_id": selected_tid,
            "selectedThreadId": selected_tid,
            "active_thread": active_thread,
            "activeThread": active_thread,
            "tags": available_tags,
            "available_tags": available_tags,
            "availableTags": available_tags,
            "steering": available_steering,
            "available_steering": available_steering,
            "availableSteering": available_steering,
            "active_steering": self.active_steering_ids,
            "activeSteering": self.active_steering_ids,
            "telemetry": telemetry_json,
            "subagent_pool_status": subagent_pool_status,
            "subagentPoolStatus": subagent_pool_status,
            "subagent_decode_tok_s": sub_tel.subagent_decode_tok_s,
            "subagentDecodeTokS": sub_tel.subagent_decode_tok_s,
            "prefix_cache_reuses": sub_tel.prefix_cache_reuses,
            "prefixCacheReuses": sub_tel.prefix_cache_reuses,
            "active_timers": active_timers,
            "activeTimers": active_timers,
            "tool_permissions": tool_permissions_map,
            "toolPermissions": tool_permissions_map,
            "staged_mutations": staged_mutations,
            "stagedMutations": staged_mutations,
            "diagnostics": diagnostics,
            "message_terminal": self.message_terminal,
            "messageTerminal": self.message_terminal,
            "epoch_cache_key_suffix": self.epoch_cache_key_suffix,
            "epochCacheKeySuffix": self.epoch_cache_key_suffix,
            "stage_write_mode": self.stage_write_mode,
            "stageWriteMode": self.stage_write_mode,
        });
        if let Some(obj) = payload.as_object_mut() {
            obj.insert("endpoint_configured".to_string(), json!(endpoint_configured));
            obj.insert("endpointConfigured".to_string(), json!(endpoint_configured));
            obj.insert("endpoint_base_url".to_string(), json!(endpoint_base_url));
            obj.insert("endpointBaseUrl".to_string(), json!(endpoint_base_url));
            obj.insert("engine_base_url".to_string(), json!(endpoint_base_url));
            obj.insert("engineBaseUrl".to_string(), json!(endpoint_base_url));
            obj.insert("model_id".to_string(), json!(self.model_id));
            obj.insert("modelId".to_string(), json!(self.model_id));
            obj.insert("subagent_model_id".to_string(), json!(self.subagent_model_id));
            obj.insert("subagentModelId".to_string(), json!(self.subagent_model_id));
            obj.insert(
                "effective_subagent_model_id".to_string(),
                json!(effective_subagent_model_id),
            );
            obj.insert(
                "effectiveSubagentModelId".to_string(),
                json!(effective_subagent_model_id),
            );
            obj.insert("available_models".to_string(), json!(available_models));
            obj.insert("availableModels".to_string(), json!(available_models));
            obj.insert(
                "critic_enabled".to_string(),
                json!(self.critic_enabled),
            );
            obj.insert(
                "criticEnabled".to_string(),
                json!(self.critic_enabled),
            );
            obj.insert(
                "subagent_protocol".to_string(),
                json!(self.subagent_protocol.as_str()),
            );
            obj.insert(
                "subagentProtocol".to_string(),
                json!(self.subagent_protocol.as_str()),
            );
            obj.insert(
                "undone_delegation_ids".to_string(),
                json!(self.undone_delegation_ids),
            );
            obj.insert(
                "undoneDelegationIds".to_string(),
                json!(self.undone_delegation_ids),
            );
            let pending_val =
                serde_json::to_value(&self.pending_confirmations).unwrap_or_else(|_| json!([]));
            obj.insert("pending_confirmations".to_string(), pending_val.clone());
            obj.insert("pendingConfirmations".to_string(), pending_val);
            obj.insert(
                "critic_bounce_count".to_string(),
                json!(self.critic_bounce_count),
            );
            obj.insert(
                "criticBounceCount".to_string(),
                json!(self.critic_bounce_count),
            );
            if let Some(ref verdict) = self.last_critic_verdict {
                obj.insert("last_critic_verdict".to_string(), json!(verdict));
                obj.insert("lastCriticVerdict".to_string(), json!(verdict));
                obj.insert("critic".to_string(), json!(verdict));
            }
            let online_status = self.online_research_status_json();
            obj.insert("online_research".to_string(), online_status.clone());
            obj.insert("onlineResearch".to_string(), online_status);
        }

        let out = serde_json::to_string(&payload).unwrap_or_else(|_| "{\"ok\":false}".to_string());
        *crate::SPELL_LIVE_SNAPSHOT_CACHE.write() = out.clone();
        let _ = crate::spell_bump_patch_gen();
        out
    }

    /// Exports a complete, strongly-typed JSON snapshot of `projects`, `threads`,
    /// `active_thread`, `tags` (`availableTags`), `telemetry`, `active_timers`, and
    /// `tool_permissions` so the host UI can render all 3 panes from a single source
    /// of truth in Rust.
    pub fn export_workspace_snapshot_json(&self) -> String {
        self.build_full_state_snapshot_json()
    }

    /// Alias for [`Self::dispatch_json_command`].
    pub fn dispatch_json(&mut self, cmd_json: &str) -> String {
        self.dispatch_json_command(cmd_json)
    }

    /// Dispatches a JSON command from the C-ABI bridge (`spell_session_dispatch_json`)
    /// and returns the updated workspace snapshot JSON (or sanitized Online Research status JSON).
    pub fn dispatch_json_command(&mut self, cmd_json: &str) -> String {
        let parsed: Value = match serde_json::from_str(cmd_json) {
            Ok(v) => v,
            Err(e) => {
                return json!({
                    "ok": false,
                    "error": format!("Invalid command JSON: {e}")
                })
                .to_string();
            }
        };

        let cmd = parsed
            .get("cmd")
            .or_else(|| parsed.get("action"))
            .or_else(|| parsed.get("type"))
            .and_then(|v| v.as_str())
            .unwrap_or("get_snapshot");

        match cmd {
            "configure_online_research" => {
                let status = self.configure_online_research_from_json(&parsed);
                let _ = self.build_full_state_snapshot_json();
                return status.to_string();
            }
            "get_online_research_status" => {
                return self.online_research_status_json().to_string();
            }
            "get_mutation_journal" => {
                let tid = parsed.get("thread_id").and_then(|v| v.as_str());
                let path = parsed.get("path").and_then(|v| v.as_str());
                return self.mutation_journal_json(tid, path).to_string();
            }
            "air_gap_now" => {
                let status = self.air_gap_now();
                let _ = self.build_full_state_snapshot_json();
                return status.to_string();
            }
            "get_subagent_protocol" => {
                return json!({
                    "ok": true,
                    "subagent_protocol": self.subagent_protocol.as_str(),
                    "subagentProtocol": self.subagent_protocol.as_str(),
                })
                .to_string();
            }
            "fetch_models" => {
                let base_url_override = parsed
                    .get("base_url")
                    .or_else(|| parsed.get("baseUrl"))
                    .and_then(|v| v.as_str());
                let api_key_override = parsed
                    .get("api_key")
                    .or_else(|| parsed.get("apiKey"))
                    .and_then(|v| v.as_str());
                match self.fetch_models(base_url_override, api_key_override) {
                    Ok((models, latency_ms)) => {
                        let snap_str = self.build_full_state_snapshot_json();
                        if let Ok(mut snap_val) = serde_json::from_str::<Value>(&snap_str) {
                            if let Some(obj) = snap_val.as_object_mut() {
                                obj.insert("models".to_string(), json!(models));
                                obj.insert("endpoint_latency_ms".to_string(), json!(latency_ms));
                                obj.insert("endpointLatencyMs".to_string(), json!(latency_ms));
                            }
                            return snap_val.to_string();
                        }
                        return json!({
                            "ok": true,
                            "models": models,
                            "endpoint_latency_ms": latency_ms,
                            "endpointLatencyMs": latency_ms,
                        })
                        .to_string();
                    }
                    Err(err_msg) => {
                        let _ = self.build_full_state_snapshot_json();
                        return json!({
                            "ok": false,
                            "error": err_msg,
                            "models": []
                        })
                        .to_string();
                    }
                }
            }
            "list_timers" => {
                let tid = parsed
                    .get("thread_id")
                    .or_else(|| parsed.get("threadId"))
                    .and_then(|v| v.as_str());
                let include_delivered = parsed
                    .get("include_delivered")
                    .or_else(|| parsed.get("includeDelivered"))
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                match self.storage.list_timers(tid, include_delivered) {
                    Ok(timers) => {
                        let snap_str = self.build_full_state_snapshot_json();
                        if let Ok(mut snap_val) = serde_json::from_str::<Value>(&snap_str) {
                            if let Some(obj) = snap_val.as_object_mut() {
                                obj.insert("timers".to_string(), json!(timers));
                            }
                            return snap_val.to_string();
                        }
                        return json!({ "ok": true, "timers": timers }).to_string();
                    }
                    Err(err_msg) => {
                        return json!({ "ok": false, "error": err_msg }).to_string();
                    }
                }
            }
            "run_subagent_slot" => {
                let slot = parsed
                    .get("slot")
                    .or_else(|| parsed.get("slot_id"))
                    .or_else(|| parsed.get("slotId"))
                    .and_then(|v| v.as_u64())
                    .unwrap_or(1) as usize;
                let mode = parsed
                    .get("mode")
                    .and_then(|v| v.as_str())
                    .unwrap_or("coding");
                let message = parsed
                    .get("message")
                    .or_else(|| parsed.get("prompt"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let max_tokens = parsed
                    .get("max_tokens")
                    .or_else(|| parsed.get("maxTokens"))
                    .and_then(|v| v.as_u64())
                    .map(|n| n as u32);
                match self.run_subagent_slot(slot, mode, message, max_tokens) {
                    Ok(resp) => {
                        let snap_str = self.build_full_state_snapshot_json();
                        if let Ok(mut snap_val) = serde_json::from_str::<Value>(&snap_str) {
                            if let Some(obj) = snap_val.as_object_mut() {
                                obj.insert("slot".to_string(), json!(slot));
                                obj.insert("content".to_string(), json!(resp.content));
                                obj.insert("subagent_response".to_string(), json!(resp));
                            }
                            return snap_val.to_string();
                        }
                        return json!({
                            "ok": true,
                            "slot": slot,
                            "content": resp.content,
                            "subagent_response": resp,
                        })
                        .to_string();
                    }
                    Err(err_msg) => {
                        let _ = self.build_full_state_snapshot_json();
                        return json!({ "ok": false, "error": err_msg }).to_string();
                    }
                }
            }
            "read_subagent_slot_lines" => {
                let slot = parsed
                    .get("slot")
                    .or_else(|| parsed.get("slot_id"))
                    .or_else(|| parsed.get("slotId"))
                    .and_then(|v| v.as_u64())
                    .unwrap_or(1) as usize;
                let limit = parsed
                    .get("limit")
                    .and_then(|v| v.as_u64())
                    .unwrap_or(200) as usize;
                match self.read_subagent_slot_lines(slot, limit) {
                    Ok(val) => return val.to_string(),
                    Err(err_msg) => {
                        return json!({ "ok": false, "error": err_msg }).to_string();
                    }
                }
            }
            "get_secret" | "secret_get" => {
                let service = parsed
                    .get("service")
                    .and_then(|v| v.as_str())
                    .unwrap_or(crate::platform::DEFAULT_KEYRING_SERVICE);
                let account = parsed
                    .get("account")
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                match crate::platform::get_secret(service, account) {
                    Ok(val) => {
                        let found = val.is_some();
                        return json!({
                            "ok": true,
                            "value": val,
                            "found": found,
                        })
                        .to_string();
                    }
                    Err(err_msg) => {
                        return json!({ "ok": false, "error": err_msg }).to_string();
                    }
                }
            }
            "set_secret" | "secret_set" => {
                let service = parsed
                    .get("service")
                    .and_then(|v| v.as_str())
                    .unwrap_or(crate::platform::DEFAULT_KEYRING_SERVICE);
                let account = parsed
                    .get("account")
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let Some(value) = parsed
                    .get("value")
                    .or_else(|| parsed.get("secret"))
                    .and_then(|v| v.as_str())
                else {
                    return json!({
                        "ok": false,
                        "error": "set_secret requires a string 'value' (or 'secret') field"
                    })
                    .to_string();
                };
                match crate::platform::set_secret(service, account, value) {
                    Ok(()) => return json!({ "ok": true }).to_string(),
                    Err(err_msg) => {
                        return json!({ "ok": false, "error": err_msg }).to_string();
                    }
                }
            }
            "delete_secret" | "secret_delete" => {
                let service = parsed
                    .get("service")
                    .and_then(|v| v.as_str())
                    .unwrap_or(crate::platform::DEFAULT_KEYRING_SERVICE);
                let account = parsed
                    .get("account")
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                match crate::platform::delete_secret(service, account) {
                    Ok(()) => return json!({ "ok": true }).to_string(),
                    Err(err_msg) => {
                        return json!({ "ok": false, "error": err_msg }).to_string();
                    }
                }
            }
            "get_system_info" => {
                let info = crate::platform::collect_system_info(
                    &self.locator.data_dir,
                    &self.locator.logs_dir,
                );
                return json!({
                    "ok": true,
                    "os": info.os,
                    "os_version": info.os_version,
                    "arch": info.arch,
                    "cpu_brand": info.cpu_brand,
                    "cpu_cores": info.cpu_cores,
                    "total_memory_bytes": info.total_memory_bytes,
                    "available_memory_bytes": info.available_memory_bytes,
                    "hostname": info.hostname,
                    "data_dir": info.data_dir,
                    "logs_dir": info.logs_dir,
                })
                .to_string();
            }
            "extract_pdf_text" => {
                let raw_path = parsed
                    .get("path")
                    .or_else(|| parsed.get("file_path"))
                    .or_else(|| parsed.get("filePath"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("")
                    .trim();
                if raw_path.is_empty() {
                    return json!({
                        "ok": false,
                        "error": "extract_pdf_text requires a non-empty 'path'"
                    })
                    .to_string();
                }
                match crate::platform::extract_pdf_text(Path::new(raw_path)) {
                    Ok(res) => {
                        return json!({
                            "ok": true,
                            "text": res.text,
                            "pages": res.pages,
                        })
                        .to_string();
                    }
                    Err(err_msg) => {
                        return json!({ "ok": false, "error": err_msg }).to_string();
                    }
                }
            }
            _ => {}
        }

        let result: Result<(), String> = match cmd {
            "get_snapshot" => Ok(()),
            "select_project" => {
                let pid = parsed
                    .get("project_id")
                    .or_else(|| parsed.get("projectId"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                self.select_project(pid)
            }
            "add_project" => {
                let name = parsed
                    .get("name")
                    .and_then(|v| v.as_str())
                    .unwrap_or("New Project");
                let root_path = parsed
                    .get("root_path")
                    .or_else(|| parsed.get("rootPath"))
                    .and_then(|v| v.as_str())
                    .unwrap_or(".");
                let is_ro = parsed
                    .get("is_read_only")
                    .or_else(|| parsed.get("isReadOnly"))
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                self.add_project(name, root_path, is_ro).map(|_| ())
            }
            "toggle_project_read_only" => {
                let pid = parsed
                    .get("project_id")
                    .or_else(|| parsed.get("projectId"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                self.toggle_project_read_only(pid).map(|_| ())
            }
            "create_thread" => {
                let pid = parsed
                    .get("project_id")
                    .or_else(|| parsed.get("projectId"))
                    .and_then(|v| v.as_str());
                let title = parsed.get("title").and_then(|v| v.as_str());
                let profile = parsed.get("profile").and_then(|v| v.as_str());
                let tags: Vec<String> = parsed
                    .get("active_tags")
                    .or_else(|| parsed.get("activeTags"))
                    .or_else(|| parsed.get("tags"))
                    .and_then(|v| v.as_array())
                    .map(|arr| {
                        arr.iter()
                            .filter_map(|x| x.as_str().map(str::to_string))
                            .collect()
                    })
                    .unwrap_or_default();
                self.create_thread(pid, title, profile, tags).map(|_| ())
            }
            "delete_thread" => {
                let tid = parsed
                    .get("thread_id")
                    .or_else(|| parsed.get("threadId"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                self.delete_thread(tid)
            }
            "rename_project" => {
                let pid = parsed
                    .get("project_id")
                    .or_else(|| parsed.get("projectId"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let name = parsed.get("name").and_then(|v| v.as_str()).unwrap_or("");
                self.rename_project(pid, name).map(|_| ())
            }
            "rename_thread" => {
                let tid = parsed
                    .get("thread_id")
                    .or_else(|| parsed.get("threadId"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let title = parsed.get("title").and_then(|v| v.as_str()).unwrap_or("");
                self.rename_thread(tid, title).map(|_| ())
            }
            "select_thread" => {
                let tid = parsed
                    .get("thread_id")
                    .or_else(|| parsed.get("threadId"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                self.select_thread(tid).map(|_| ())
            }
            "set_thread_profile" => {
                let tid = parsed
                    .get("thread_id")
                    .or_else(|| parsed.get("threadId"))
                    .and_then(|v| v.as_str())
                    .map(str::to_string)
                    .or_else(|| self.selected_thread_id.clone())
                    .unwrap_or_default();
                let profile = parsed
                    .get("profile")
                    .and_then(|v| v.as_str())
                    .unwrap_or("write");
                self.set_thread_profile(&tid, profile).map(|_| ())
            }
            "toggle_tag" => {
                let tid = parsed
                    .get("thread_id")
                    .or_else(|| parsed.get("threadId"))
                    .and_then(|v| v.as_str())
                    .map(str::to_string)
                    .or_else(|| self.selected_thread_id.clone())
                    .unwrap_or_default();
                let tag_name = parsed
                    .get("tag_name")
                    .or_else(|| parsed.get("tagName"))
                    .or_else(|| parsed.get("name"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                self.toggle_tag_on_thread(&tid, tag_name).map(|_| ())
            }
            "create_tag" | "upsert_tag" => {
                let explicit_id = parsed.get("id").and_then(|v| v.as_str());
                let name = parsed
                    .get("name")
                    .or_else(|| parsed.get("id"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let category = parsed
                    .get("category")
                    .and_then(|v| v.as_str())
                    .unwrap_or("general");
                let description = parsed
                    .get("description")
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let instruction = parsed
                    .get("instruction")
                    .or_else(|| parsed.get("rubric_prompt"))
                    .or_else(|| parsed.get("rubricPrompt"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                self.upsert_tag_with_id(explicit_id, name, category, description, instruction)
                    .map(|_| ())
            }
            "create_steering" | "upsert_steering" => {
                let explicit_id = parsed.get("id").and_then(|v| v.as_str());
                let name = parsed
                    .get("name")
                    .or_else(|| parsed.get("id"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let instruction = parsed
                    .get("instruction")
                    .or_else(|| parsed.get("system_append"))
                    .or_else(|| parsed.get("systemAppend"))
                    .or_else(|| parsed.get("rubric_prompt"))
                    .or_else(|| parsed.get("rubricPrompt"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                self.create_or_update_steering(explicit_id, name, instruction)
                    .map(|_| ())
            }
            "toggle_steering" => {
                let sid = parsed
                    .get("steering_id")
                    .or_else(|| parsed.get("steeringId"))
                    .or_else(|| parsed.get("id"))
                    .or_else(|| parsed.get("trigger"))
                    .or_else(|| parsed.get("name"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                self.toggle_steering(sid).map(|_| ())
            }
            "send_prompt" => {
                let tid = parsed
                    .get("thread_id")
                    .or_else(|| parsed.get("threadId"))
                    .and_then(|v| v.as_str())
                    .map(str::to_string)
                    .or_else(|| self.selected_thread_id.clone())
                    .unwrap_or_default();
                let prompt = parsed
                    .get("prompt")
                    .or_else(|| parsed.get("text"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                self.send_user_prompt(&tid, prompt).map(|_| ())
            }
            "edit_or_regenerate_turn" | "regenerate_turn" => {
                let tid = parsed
                    .get("thread_id")
                    .or_else(|| parsed.get("threadId"))
                    .and_then(|v| v.as_str())
                    .map(str::to_string)
                    .or_else(|| self.selected_thread_id.clone())
                    .unwrap_or_default();
                let event_id = parsed
                    .get("event_id")
                    .or_else(|| parsed.get("eventId"))
                    .and_then(|v| v.as_str());
                let new_prompt = parsed
                    .get("new_prompt")
                    .or_else(|| parsed.get("newPrompt"))
                    .or_else(|| parsed.get("prompt"))
                    .and_then(|v| v.as_str());
                self.edit_or_regenerate_turn(&tid, event_id, new_prompt)
                    .map(|_| ())
            }
            "request_hot_cache_clear" => {
                let tid = parsed
                    .get("thread_id")
                    .or_else(|| parsed.get("threadId"))
                    .and_then(|v| v.as_str())
                    .map(str::to_string)
                    .or_else(|| self.selected_thread_id.clone())
                    .unwrap_or_default();
                self.request_engine_hot_cache_clear(&tid).map(|_| ())
            }
            "schedule_timer" => {
                let tid = parsed
                    .get("thread_id")
                    .or_else(|| parsed.get("threadId"))
                    .and_then(|v| v.as_str())
                    .map(str::to_string)
                    .or_else(|| self.selected_thread_id.clone())
                    .unwrap_or_default();
                let duration_s = parsed
                    .get("duration_s")
                    .or_else(|| parsed.get("durationS"))
                    .and_then(|v| v.as_f64())
                    .unwrap_or(10.0);
                let label = parsed
                    .get("label")
                    .and_then(|v| v.as_str())
                    .unwrap_or("agent-timer");
                let note = parsed
                    .get("note")
                    .and_then(|v| v.as_str())
                    .unwrap_or("Check background progress");
                let watch_path = parsed
                    .get("watch_path")
                    .or_else(|| parsed.get("watchPath"))
                    .and_then(|v| v.as_str());
                self.schedule_engine_timer(&tid, duration_s, label, note, watch_path)
                    .map(|_| ())
            }
            "fire_due_timers" => {
                let tid = parsed
                    .get("thread_id")
                    .or_else(|| parsed.get("threadId"))
                    .and_then(|v| v.as_str())
                    .map(str::to_string)
                    .or_else(|| self.selected_thread_id.clone())
                    .unwrap_or_default();
                self.poll_and_fire_due_engine_timers(&tid).map(|_| ())
            }
            "cancel_timer" => {
                let timer_id = parsed
                    .get("timer_id")
                    .or_else(|| parsed.get("timerId"))
                    .or_else(|| parsed.get("id"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                self.cancel_engine_timer(timer_id).map(|_| ())
            }
            "resolve_tool_confirmation" | "approve_tool_call" | "confirm_tool_call" => {
                let call_id = parsed
                    .get("call_id")
                    .or_else(|| parsed.get("callId"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let explicit_domain = parsed
                    .get("domain")
                    .and_then(|v| v.as_str())
                    .map(str::trim)
                    .filter(|s| !s.is_empty());
                let scope_str = parsed
                    .get("decision")
                    .or_else(|| parsed.get("scope"))
                    .or_else(|| parsed.get("approval_scope"))
                    .or_else(|| parsed.get("domain_scope"))
                    .or_else(|| parsed.get("domainScope"))
                    .and_then(|v| v.as_str());
                let decision = if let Some(s) = scope_str {
                    s
                } else if parsed
                    .get("allow_always")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false)
                {
                    "always_allow"
                } else if parsed
                    .get("allow_session")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false)
                {
                    "allow_session"
                } else if parsed
                    .get("allow_once")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false)
                {
                    "allow_once"
                } else if parsed
                    .get("approved")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false)
                {
                    "allow_once"
                } else {
                    "deny"
                };
                self.confirm_tool_call(call_id, decision, explicit_domain)
                    .map(|_| ())
            }
            "set_tool_permission" => {
                let tool_name = parsed
                    .get("tool_name")
                    .or_else(|| parsed.get("toolName"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let policy_str = parsed
                    .get("policy")
                    .and_then(|v| v.as_str())
                    .unwrap_or("allow");
                let policy = match policy_str.trim().to_ascii_lowercase().as_str() {
                    "confirm" => ToolPermissionPolicy::Confirm,
                    "deny" => ToolPermissionPolicy::Deny,
                    _ => ToolPermissionPolicy::Allow,
                };
                self.set_tool_permission_policy(tool_name, policy)
            }
            "cancel_active_turn" => self.cancel_active_turn(),
            "configure_endpoint" | "configure_engine" => {
                let base_url_opt = parsed
                    .get("base_url")
                    .or_else(|| parsed.get("baseUrl"))
                    .and_then(|v| v.as_str());
                let api_key_opt =
                    if let Some(val) = parsed.get("api_key").or_else(|| parsed.get("apiKey")) {
                        Some(val.as_str())
                    } else {
                        None
                    };
                let model_id_opt = parsed
                    .get("model_id")
                    .or_else(|| parsed.get("modelId"))
                    .and_then(|v| v.as_str());
                let subagent_model_id_opt = parsed
                    .get("subagent_model_id")
                    .or_else(|| parsed.get("subagentModelId"))
                    .and_then(|v| v.as_str());
                self.configure_endpoint(
                    base_url_opt,
                    api_key_opt,
                    model_id_opt,
                    subagent_model_id_opt,
                )
            }
            "refresh_engine_status" => {
                self.refresh_engine_status();
                Ok(())
            }
            "set_critic_enabled" => {
                let enabled = parsed
                    .get("enabled")
                    .or_else(|| parsed.get("critic_enabled"))
                    .or_else(|| parsed.get("criticEnabled"))
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                self.set_critic_enabled(enabled)
            }
            "terminate_subagent_slot" => {
                let slot = parsed
                    .get("slot")
                    .or_else(|| parsed.get("slot_id"))
                    .or_else(|| parsed.get("slotId"))
                    .and_then(|v| v.as_u64())
                    .unwrap_or(1) as usize;
                self.terminate_subagent_slot(slot)
            }
            "duplicate_subagent_slot" => {
                let src_slot = parsed
                    .get("src_slot")
                    .or_else(|| parsed.get("srcSlot"))
                    .or_else(|| parsed.get("from_slot"))
                    .or_else(|| parsed.get("source_slot"))
                    .and_then(|v| v.as_u64())
                    .unwrap_or(1) as usize;
                let dst_slot = parsed
                    .get("dst_slot")
                    .or_else(|| parsed.get("dstSlot"))
                    .or_else(|| parsed.get("to_slot"))
                    .or_else(|| parsed.get("target_slot"))
                    .and_then(|v| v.as_u64())
                    .unwrap_or(2) as usize;
                self.duplicate_subagent_slot(src_slot, dst_slot)
            }
            "set_epoch_cache_key_suffix" => {
                let enabled = parsed
                    .get("enabled")
                    .or_else(|| parsed.get("epoch_cache_key_suffix"))
                    .or_else(|| parsed.get("epochCacheKeySuffix"))
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false);
                self.epoch_cache_key_suffix = enabled;
                Ok(())
            }
            "set_stage_write_mode" => {
                let enabled = parsed
                    .get("enabled")
                    .or_else(|| parsed.get("stage_write_mode"))
                    .or_else(|| parsed.get("stageWriteMode"))
                    .and_then(|v| v.as_bool())
                    .unwrap_or(true);
                self.stage_write_mode = enabled;
                self.delegation_stage_write_hold = enabled;
                Ok(())
            }
            "set_subagent_protocol" => {
                let raw_proto = parsed
                    .get("protocol")
                    .or_else(|| parsed.get("subagent_protocol"))
                    .or_else(|| parsed.get("subagentProtocol"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("auto");
                self.subagent_protocol = crate::subagent_coding::SubagentProtocol::from_wire(raw_proto);
                let _ = self.storage.put_kv_document(
                    "settings",
                    "subagent_protocol",
                    self.subagent_protocol.as_str(),
                );
                Ok(())
            }
            "undo_delegation" | "undo_subagent_delegation" => {
                let undo_id = parsed
                    .get("undo_id")
                    .or_else(|| parsed.get("undoId"))
                    .or_else(|| parsed.get("delegation_id"))
                    .or_else(|| parsed.get("delegationId"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let tid_override = parsed
                    .get("thread_id")
                    .or_else(|| parsed.get("threadId"))
                    .and_then(|v| v.as_str());
                self.undo_delegation(undo_id, tid_override).map(|_| ())
            }
            "loop_control" => {
                let action = parsed
                    .get("action")
                    .and_then(|v| v.as_str())
                    .filter(|&a| a != "loop_control")
                    .or_else(|| parsed.get("loop_action").and_then(|v| v.as_str()))
                    .or_else(|| parsed.get("sub_action").and_then(|v| v.as_str()))
                    .unwrap_or("pause");
                self.control_autonomous_loop(action).map(|_| ())
            }
            "stage_write_action" => {
                let action = parsed
                    .get("action")
                    .and_then(|v| v.as_str())
                    .filter(|&a| a != "stage_write_action")
                    .or_else(|| parsed.get("stage_action").and_then(|v| v.as_str()))
                    .or_else(|| parsed.get("sub_action").and_then(|v| v.as_str()))
                    .unwrap_or("stage");
                let raw_path = parsed
                    .get("path")
                    .and_then(|v| v.as_str())
                    .unwrap_or("")
                    .trim();
                let content = parsed
                    .get("content")
                    .and_then(|v| v.as_str())
                    .unwrap_or("")
                    .to_string();

                let project_root = self
                    .selected_project_id
                    .as_deref()
                    .and_then(|pid| self.storage.get_project(pid).ok().flatten())
                    .map(|p| std::path::PathBuf::from(p.root_path))
                    .unwrap_or_else(|| self.locator.primary_workspace_root());

                let resolved_path_opt = if raw_path.is_empty() {
                    None
                } else if Path::new(raw_path).is_absolute() {
                    let p = std::path::PathBuf::from(raw_path);
                    if let Err(e) = crate::tools::enforce_path_guardrail(&p) {
                        return json!({ "ok": false, "error": e.to_string() }).to_string();
                    }
                    Some(p)
                } else {
                    match resolve_workspace_path(&project_root, raw_path) {
                        Ok(p) => Some(p),
                        Err(e) => {
                            return json!({ "ok": false, "error": e.to_string() }).to_string();
                        }
                    }
                };

                match action.trim().to_ascii_lowercase().as_str() {
                    "stage" => {
                        let Some(target) = resolved_path_opt else {
                            return json!({
                                "ok": false,
                                "error": "stage_write_action `stage` requires a non-empty `path`"
                            })
                            .to_string();
                        };
                        self.snapshot_store
                            .stage_write(&target, raw_path, content)
                            .map(|_| ())
                            .map_err(|e| e.to_string())
                    }
                    "verify" => {
                        let Some(target) = resolved_path_opt else {
                            return json!({
                                "ok": false,
                                "error": "stage_write_action `verify` requires a non-empty `path`"
                            })
                            .to_string();
                        };
                        let Some((_, existing)) = self.find_active_staged_clone(&target) else {
                            return json!({
                                "ok": false,
                                "error": format!("No staged mutation found for '{}'", target.display())
                            })
                            .to_string();
                        };
                        let tid = self
                            .selected_thread_id
                            .clone()
                            .unwrap_or_default();
                        if self.critic_enabled || self.web_tainted_threads.contains(&tid) {
                            let rubric = parsed
                                .get("rubric")
                                .and_then(|v| v.as_str())
                                .map(str::trim)
                                .filter(|s| !s.is_empty())
                                .map(str::to_string)
                                .unwrap_or_else(|| self.resolve_active_critic_rubric(&tid));
                            self.critic_review_seq = self.critic_review_seq.saturating_add(1);
                            let seq = self.critic_review_seq;
                            match self.evaluate_diff_with_gatekeeper_critic(
                                &tid,
                                &existing.path,
                                &existing.relative_path,
                                existing.staged_content,
                                &rubric,
                                &[],
                                seq,
                            ) {
                                Ok(verdict) if verdict.passed => Ok(()),
                                Ok(verdict) => {
                                    return json!({
                                        "ok": false,
                                        "error": format!(
                                            "Gatekeeper Critic rejected '{}': {}",
                                            existing.relative_path, verdict.reason
                                        ),
                                        "critic": verdict,
                                        "critic_bounce_count": self.critic_bounce_count
                                    })
                                    .to_string();
                                }
                                Err(e) => Err(e),
                            }
                        } else {
                            self.snapshot_store
                                .mark_staged_verified(&target)
                                .map(|_| ())
                                .ok_or_else(|| {
                                    format!("No staged mutation found for '{}'", target.display())
                                })
                        }
                    }
                    "commit" => {
                        let active_tid = self
                            .selected_thread_id
                            .clone()
                            .unwrap_or_default();
                        if self.critic_enabled
                            || self.web_tainted_threads.contains(&active_tid)
                        {
                            let unverified_entries: Vec<crate::tools::StagedFileMutation> = self
                                .snapshot_store
                                .list_staged_mutations()
                                .into_iter()
                                .filter(|st| {
                                    st.status == "staged"
                                        && resolved_path_opt
                                            .as_deref()
                                            .map_or(true, |f| Self::paths_equivalent(&st.path, f))
                                })
                                .collect();
                            if !unverified_entries.is_empty() {
                                let tid = self
                                    .selected_thread_id
                                    .clone()
                                    .unwrap_or_default();
                                let rubric = self.resolve_active_critic_rubric(&tid);
                                for entry in unverified_entries {
                                    self.critic_review_seq =
                                        self.critic_review_seq.saturating_add(1);
                                    let seq = self.critic_review_seq;
                                    match self.evaluate_diff_with_gatekeeper_critic(
                                        &tid,
                                        &entry.path,
                                        &entry.relative_path,
                                        entry.staged_content,
                                        &rubric,
                                        &[],
                                        seq,
                                    ) {
                                        Ok(verdict) if verdict.passed => {}
                                        Ok(verdict) => {
                                            return json!({
                                                "ok": false,
                                                "error": format!(
                                                    "Commit blocked by Gatekeeper Critic on '{}': {}",
                                                    entry.relative_path, verdict.reason
                                                ),
                                                "critic": verdict,
                                                "critic_bounce_count": self.critic_bounce_count
                                            })
                                            .to_string();
                                        }
                                        Err(e) => {
                                            return json!({
                                                "ok": false,
                                                "error": e
                                            })
                                            .to_string();
                                        }
                                    }
                                }
                            }
                        }
                        match self.snapshot_store.commit_staged(resolved_path_opt.as_deref()) {
                            Ok(committed) => {
                                for item in &committed {
                                    self.turn_pin_cache.invalidate_path(&item.path);
                                }
                                if committed.is_empty() {
                                    if let Some(ref target) = resolved_path_opt {
                                        if target.exists() {
                                            self.turn_pin_cache.invalidate_path(target);
                                        } else {
                                            return json!({
                                                "ok": false,
                                                "error": format!(
                                                    "No staged mutation found for '{}'",
                                                    target.display()
                                                )
                                            })
                                            .to_string();
                                        }
                                    }
                                }
                                Ok(())
                            }
                            Err(e) => Err(e.to_string()),
                        }
                    }
                    "rollback" => self
                        .snapshot_store
                        .rollback_staged(resolved_path_opt.as_deref())
                        .map(|_| ())
                        .map_err(|e| e.to_string()),
                    other => Err(format!("Unknown stage_write_action '{other}'")),
                }
            }
            "open_message_terminal" => {
                let raw_path = parsed
                    .get("path")
                    .or_else(|| parsed.get("folder"))
                    .or_else(|| parsed.get("cwd"))
                    .or_else(|| parsed.get("directory"))
                    .and_then(|v| v.as_str())
                    .unwrap_or(".");
                let reason = parsed
                    .get("reason")
                    .or_else(|| parsed.get("note"))
                    .or_else(|| parsed.get("prompt"))
                    .and_then(|v| v.as_str())
                    .map(str::to_string);
                self.open_message_terminal(raw_path, reason).map(|_| ())
            }
            "close_message_terminal" => {
                self.close_message_terminal();
                Ok(())
            }
            "run_message_terminal_command" => {
                let command = parsed
                    .get("command")
                    .or_else(|| parsed.get("cmd_text"))
                    .and_then(|v| v.as_str())
                    .unwrap_or("");
                let cwd = parsed.get("cwd").and_then(|v| v.as_str());
                self.run_message_terminal_command(command, cwd).map(|_| ())
            }
            other => Err(format!("Unknown session command '{other}'")),
        };

        if let Err(err_msg) = result {
            return json!({
                "ok": false,
                "error": err_msg
            })
            .to_string();
        }

        self.build_full_state_snapshot_json()
    }
}
