//! Test-only engine doubles for `spell_core`.
//!
//! Compiled only for `cargo test` or with the `test-support` feature (used to build the
//! `spell_test_engine` binary that standalone integration tests talk to over real HTTP). None of
//! this is part of `libspell_core.dylib` as shipped in the app.

#![allow(dead_code)]

use std::collections::{HashMap, VecDeque};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
#[cfg(unix)]
use std::os::unix::net::{UnixListener, UnixStream};
#[cfg(unix)]
use std::path::{Path, PathBuf};
#[cfg(unix)]
use std::sync::atomic::AtomicU64;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::Duration;

use parking_lot::Mutex;
use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::contract::{
    derive_slot_base_thread_key, validate_model_for_role, AgentExecutionRole, ChatCompletionChunk,
    ChatCompletionChunkChoice, ChatCompletionDelta, ChatCompletionRequest, ChatCompletionUsage,
    ClearThreadCacheRequest, CompactionStatusResponse, EngineContextAdvice, ModelDescriptor,
    PromptTokensDetails, SetTimerRequest, SubagentCompletionRequest, SubagentCompletionResponse,
    SubagentPoolStatusResponse, TelemetryResponse, TimerRecord, WireMessage, WireToolCall,
    WireToolFunction, COMPACTION_SUMMARY_MARKER, ENGINE_COMPACT_TOKEN_THRESHOLD,
    ENGINE_INTERCEPTOR_COMPACT_TOKENS, ENGINE_MIN_DECODE_TPS, TIMER_ALARM_MARKER,
};
use crate::locator::EnginePathLocator;

use crate::transport::*;

/// Default main model ID used by in-memory fixture tests.
pub const FIXTURE_MAIN_MODEL_ID: &str = "test-model";
/// Default subagent model ID used by in-memory fixture tests.
pub const FIXTURE_SUBAGENT_MODEL_ID: &str = "test-subagent-model";

// ============================================================================
// 2. `FixtureTransport` (Engine-Exact Simulation)
// ============================================================================

/// Per-thread state maintained inside the simulated engine (`compaction_cache_sync.py` parity).
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct SimulatedEngineThreadState {
    pub thread_key: String,
    pub last_prompt_tokens: u64,
    pub decode_tok_s: f64,
    pub decode_tok_s_ewma: f64,
    pub compaction_epoch: u64,
    pub needs_compaction: bool,
    pub turn_hot_cache_cleared: bool,
    pub turn_freed_blocks: u64,
    pub tool_turn_step: usize,
}

impl SimulatedEngineThreadState {
    pub fn new(thread_key: impl Into<String>) -> Self {
        Self {
            thread_key: thread_key.into(),
            last_prompt_tokens: 12_480,
            decode_tok_s: 0.0,
            decode_tok_s_ewma: 0.0,
            compaction_epoch: 0,
            needs_compaction: false,
            turn_hot_cache_cleared: false,
            turn_freed_blocks: 0,
            tool_turn_step: 0,
        }
    }
}

/// Scripted multi-step subagent turn step queued on [`FixtureTransport`] for Option A / Option B tests.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ScriptedSubagentStep {
    pub content: String,
    pub reasoning_content: String,
    pub tool_calls: Vec<WireToolCall>,
    pub finish_reason: Option<String>,
    pub prefix_cache_reused: Option<bool>,
}

/// Recorded coding/prefill/critic subagent invocation captured by [`FixtureTransport`] / [`LoopbackVerifyServer`].
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RecordedSubagentCall {
    pub model: String,
    pub stream: bool,
    pub role_header: Option<String>,
    pub prompt_cache_key: Option<String>,
    pub parent_cache_key: Option<String>,
    pub messages: Vec<WireMessage>,
    pub max_tokens: Option<u32>,
    pub tools: Option<Vec<Value>>,
    pub tool_choice: Option<Value>,
    pub enable_thinking: Option<bool>,
    pub chat_template_kwargs: Option<Value>,
    pub thinking_budget: Option<u32>,
    pub reasoning_effort: Option<String>,
}

#[derive(Debug, Default)]
struct FixtureEngineInner {
    threads: HashMap<String, SimulatedEngineThreadState>,
    timers: Vec<TimerRecord>,
    models: Vec<ModelDescriptor>,
    total_compactions: u64,
    total_hot_resets: u64,
    total_freed_blocks: u64,
    next_timer_seq: u64,
    next_turn_seq: u64,
    next_subagent_seq: u64,
    last_client_header: Option<String>,
    last_agent_role_header: Option<String>,
    raw_telemetry_override: Option<String>,
    recorded_main_requests: Vec<ChatCompletionRequest>,
    main_tool_calls_queue: Vec<Vec<WireToolCall>>,
    recorded_subagent_calls: Vec<RecordedSubagentCall>,
    pending_subagent_wire_meta: Option<(Option<Value>, Option<Value>)>,
    subagent_step_queue: VecDeque<ScriptedSubagentStep>,
    subagent_response_queue: Vec<(String, Option<String>)>,
    subagent_keyed_responses: Vec<(String, String, Option<String>)>,
    subagent_simulated_delay_ms: u64,
    subagent_unavailable: bool,
    mock_resolved_model_override: Option<String>,
    mock_routed_experts_override: Option<usize>,
    subagent_status_override: Option<SubagentPoolStatusResponse>,
    prefix_cache_reuses: u64,
}

/// Engine-exact in-memory simulator for OpenAI-compatible unit and integration tests.
pub struct FixtureTransport {
    inner: Mutex<FixtureEngineInner>,
    in_flight_subagents: AtomicUsize,
    max_concurrent_subagents: AtomicUsize,
}

impl Default for FixtureTransport {
    fn default() -> Self {
        Self::new()
    }
}

impl FixtureTransport {
    pub fn new() -> Self {
        let mut inner = FixtureEngineInner::default();
        inner.models = vec![
            ModelDescriptor {
                id: FIXTURE_MAIN_MODEL_ID.to_string(),
                owned_by: Some("spell-test".to_string()),
                context_window: Some(131_072),
            },
            ModelDescriptor {
                id: FIXTURE_SUBAGENT_MODEL_ID.to_string(),
                owned_by: Some("spell-test".to_string()),
                context_window: Some(131_072),
            },
        ];
        Self {
            inner: Mutex::new(inner),
            in_flight_subagents: AtomicUsize::new(0),
            max_concurrent_subagents: AtomicUsize::new(0),
        }
    }

    /// Replaces the model catalog served by `GET /v1/models` and [`EngineTransport::list_models`].
    pub fn set_models(&self, models: Vec<ModelDescriptor>) {
        self.inner.lock().models = models;
    }

    /// Advances the simulated engine clock by `elapsed_s` seconds, decrementing `remaining_s`
    /// on all pending timers and marking timers with `remaining_s <= 0.0` as `"expired"`.
    pub fn advance_simulated_time(&self, elapsed_s: f64) {
        let delta = elapsed_s.max(0.0);
        let mut guard = self.inner.lock();
        for timer in &mut guard.timers {
            if !timer.delivered && timer.status != "cancelled" {
                timer.remaining_s = (timer.remaining_s - delta).max(0.0);
                if timer.remaining_s <= 0.0 {
                    timer.status = "expired".to_string();
                }
            }
        }
    }

    /// Explicitly marks a thread as requiring compaction on its next turn.
    pub fn set_thread_needs_compaction(&self, thread_key: &str, needs_compaction: bool) {
        let key = derive_thread_key(Some(thread_key));
        let mut guard = self.inner.lock();
        let state = guard
            .threads
            .entry(key.clone())
            .or_insert_with(|| SimulatedEngineThreadState::new(key));
        state.needs_compaction = needs_compaction;
    }

    /// Sets simulated decode throughput metrics for a specific thread.
    pub fn set_thread_decode_tps(&self, thread_key: &str, decode_tok_s: f64, ewma: f64) {
        let key = derive_thread_key(Some(thread_key));
        let mut guard = self.inner.lock();
        let state = guard
            .threads
            .entry(key.clone())
            .or_insert_with(|| SimulatedEngineThreadState::new(key));
        state.decode_tok_s = decode_tok_s;
        state.decode_tok_s_ewma = ewma;
    }

    /// Returns a snapshot of the simulated engine state for `thread_key`.
    pub fn get_thread_state(&self, thread_key: &str) -> Option<SimulatedEngineThreadState> {
        let key = derive_thread_key(Some(thread_key));
        self.inner.lock().threads.get(&key).cloned()
    }

    /// Resets all simulated threads, timers, and counters.
    pub fn reset(&self) {
        let mut guard = self.inner.lock();
        let existing_models = guard.models.clone();
        *guard = FixtureEngineInner::default();
        guard.models = existing_models;
        self.in_flight_subagents.store(0, Ordering::SeqCst);
        self.max_concurrent_subagents.store(0, Ordering::SeqCst);
    }

    /// Returns the most recent client header value observed by [`LoopbackVerifyServer`] / [`UnixVerifyServer`].
    pub fn last_client_header(&self) -> Option<String> {
        self.inner.lock().last_client_header.clone()
    }

    /// Records the client header value observed on an incoming verification server request.
    pub fn record_client_header(&self, header: Option<String>) {
        self.inner.lock().last_client_header = header;
    }

    /// Returns the most recent agent role header observed by [`LoopbackVerifyServer`].
    pub fn last_agent_role_header(&self) -> Option<String> {
        self.inner.lock().last_agent_role_header.clone()
    }

    /// Records an agent role header observed on an incoming verification server request.
    pub fn record_agent_role_header(&self, header: Option<String>) {
        self.inner.lock().last_agent_role_header = header;
    }

    /// Returns all recorded main chat completion requests.
    pub fn recorded_main_requests(&self) -> Vec<ChatCompletionRequest> {
        self.inner.lock().recorded_main_requests.clone()
    }

    /// Queues explicit `tool_calls` to be emitted on the next main turn step.
    pub fn push_main_tool_calls_override(&self, calls: Vec<WireToolCall>) {
        self.inner.lock().main_tool_calls_queue.push(calls);
    }

    /// Returns all recorded subagent calls.
    pub fn recorded_subagent_calls(&self) -> Vec<RecordedSubagentCall> {
        self.inner.lock().recorded_subagent_calls.clone()
    }

    /// Records raw wire-level `tool_choice` and `chat_template_kwargs` observed by [`LoopbackVerifyServer`].
    pub fn record_subagent_wire_metadata(
        &self,
        tool_choice: Option<Value>,
        chat_template_kwargs: Option<Value>,
    ) {
        self.inner.lock().pending_subagent_wire_meta = Some((tool_choice, chat_template_kwargs));
    }

    /// Queues a deterministic text/JSON payload to be returned by the next `run_subagent_completion`.
    pub fn push_subagent_response_override(&self, content: impl Into<String>) {
        self.inner
            .lock()
            .subagent_response_queue
            .push((content.into(), None));
    }

    /// Queues a deterministic subagent response with an explicit `finish_reason` (e.g. `"length"`).
    pub fn push_subagent_response_with_finish_reason(
        &self,
        content: impl Into<String>,
        finish_reason: impl Into<String>,
    ) {
        self.inner
            .lock()
            .subagent_response_queue
            .push((content.into(), Some(finish_reason.into())));
    }

    /// Queues a deterministic subagent response matched against `prompt_cache_key` prefix
    /// (ideal for order-independent parallel `std::thread::scope` batch tests).
    pub fn push_subagent_response_for_key(
        &self,
        cache_key_prefix: impl Into<String>,
        content: impl Into<String>,
        finish_reason: Option<String>,
    ) {
        self.inner.lock().subagent_keyed_responses.push((
            cache_key_prefix.into(),
            content.into(),
            finish_reason,
        ));
    }

    /// Queues a scripted multi-step subagent turn step (`content`, `reasoning_content`, native `tool_calls`, `finish_reason`).
    pub fn push_subagent_step(&self, step: ScriptedSubagentStep) {
        self.inner.lock().subagent_step_queue.push_back(step);
    }

    /// Convenience helper queuing a native `tool_calls` subagent step (`finish_reason: Some("tool_calls")`, empty content).
    pub fn push_subagent_tool_calls(&self, tool_calls: Vec<WireToolCall>) {
        self.push_subagent_step(ScriptedSubagentStep {
            content: String::new(),
            reasoning_content: String::new(),
            tool_calls,
            finish_reason: Some("tool_calls".to_string()),
            prefix_cache_reused: None,
        });
    }

    /// Configures simulated per-call subagent latency (outside `inner` mutex) to verify parallel batching.
    pub fn set_subagent_simulated_delay_ms(&self, delay_ms: u64) {
        self.inner.lock().subagent_simulated_delay_ms = delay_ms;
    }

    /// Peak number of concurrently executing `run_subagent_completion` calls observed on this transport.
    pub fn max_observed_concurrent_subagents(&self) -> usize {
        self.max_concurrent_subagents.load(Ordering::SeqCst)
    }

    pub fn run_subagent_completion_with_meta(
        &self,
        req: &SubagentCompletionRequest,
        role_header_override: Option<String>,
        wire_meta: Option<(Option<Value>, Option<Value>)>,
    ) -> Result<SubagentCompletionResponse, TransportError> {
        let effective_model = if req.model.trim().is_empty() {
            FIXTURE_SUBAGENT_MODEL_ID.to_string()
        } else {
            req.model.clone()
        };
        validate_model_for_role(AgentExecutionRole::CodingSubagent, &effective_model)
            .map_err(TransportError::ModelRoleViolation)?;

        let (
            sub_seq,
            prefix_cache_reused,
            resolved_model,
            routed_experts,
            content,
            reasoning_content,
            tool_calls,
            finish_reason,
            delay_ms,
        ) = {
            let mut guard = self.inner.lock();
            if guard.subagent_unavailable {
                return Err(TransportError::HttpStatus {
                    status: 503,
                    body: "{\"error\":{\"message\":\"subagent_unavailable: service unavailable\",\"code\":503}}".to_string(),
                });
            }

            guard.next_subagent_seq += 1;
            let sub_seq = guard.next_subagent_seq;
            let req_prefix_reused = req
                .parent_cache_key
                .as_deref()
                .map(|s| !s.trim().is_empty())
                .unwrap_or(false);
            let role_header = role_header_override.or_else(|| guard.last_agent_role_header.clone());
            let effective_wire_meta = wire_meta.or_else(|| guard.pending_subagent_wire_meta.take());
            let (fallback_tool_choice, wire_chat_kwargs) = match effective_wire_meta {
                Some((wire_tc, wire_kwargs)) => (wire_tc, wire_kwargs),
                None => {
                    let tc = req.tool_choice.clone().or_else(|| {
                        if req.tools.as_ref().map(|t| !t.is_empty()).unwrap_or(false) {
                            Some(serde_json::json!("none"))
                        } else {
                            None
                        }
                    });
                    (tc, None)
                }
            };
            let last_msg_text = req
                .messages
                .last()
                .and_then(|m| m.content.as_deref())
                .unwrap_or("");
            let payload_tail = last_msg_text
                .rsplit_once("</subagent_role>")
                .map(|(_, tail)| tail)
                .unwrap_or(last_msg_text);
            let inferred_key = req
                .prompt_cache_key
                .clone()
                .filter(|k| !k.trim().is_empty())
                .unwrap_or_else(|| {
                    if payload_tail.contains("<unified_diff>")
                        || last_msg_text.contains("Gatekeeper Critic subagent")
                    {
                        format!("critic:inferred:{sub_seq}")
                    } else if payload_tail.contains("<due_timers>") {
                        format!("timer:inferred:{sub_seq}")
                    } else if payload_tail.contains("<workspace_files>") {
                        format!("inferred:prefill:{sub_seq}")
                    } else {
                        format!("inferred:delegate:{sub_seq}")
                    }
                });
            let cache_key = inferred_key.as_str();

            guard.recorded_subagent_calls.push(RecordedSubagentCall {
                model: effective_model.clone(),
                stream: req.stream,
                role_header,
                prompt_cache_key: req
                    .prompt_cache_key
                    .clone()
                    .or_else(|| Some(inferred_key.clone())),
                parent_cache_key: req.parent_cache_key.clone(),
                messages: req.messages.clone(),
                max_tokens: req.max_tokens,
                tools: req.tools.clone(),
                tool_choice: fallback_tool_choice,
                enable_thinking: req.enable_thinking,
                chat_template_kwargs: wire_chat_kwargs,
                thinking_budget: req.thinking_budget,
                reasoning_effort: req.reasoning_effort.clone(),
            });

            let resolved_model = guard
                .mock_resolved_model_override
                .clone()
                .unwrap_or(effective_model);
            let routed_experts = guard.mock_routed_experts_override;
            let delay_ms = guard.subagent_simulated_delay_ms;

            let keyed_pos = guard
                .subagent_keyed_responses
                .iter()
                .position(|(prefix, _, _)| cache_key.starts_with(prefix));
            let is_critic = cache_key.starts_with("critic:");
            let is_timer = cache_key.starts_with("timer:");
            let is_aux_slot = is_critic || is_timer;

            let (content, reasoning_content, tool_calls, custom_finish, step_cache_override) =
                if let Some(pos) = keyed_pos {
                    let (_, c, fr) = guard.subagent_keyed_responses.remove(pos);
                    (
                        c,
                        "Subagent terse pass.".to_string(),
                        Vec::new(),
                        fr,
                        None,
                    )
                } else if !is_aux_slot && !guard.subagent_step_queue.is_empty() {
                    let step = guard.subagent_step_queue.pop_front().unwrap();
                    let default_fr = if step.tool_calls.is_empty() {
                        "stop".to_string()
                    } else {
                        "tool_calls".to_string()
                    };
                    let fr = step.finish_reason.or(Some(default_fr));
                    (
                        step.content,
                        step.reasoning_content,
                        step.tool_calls,
                        fr,
                        step.prefix_cache_reused,
                    )
                } else if !guard.subagent_response_queue.is_empty() {
                    let (c, fr) = guard.subagent_response_queue.remove(0);
                    (
                        c,
                        "Subagent terse pass.".to_string(),
                        Vec::new(),
                        fr,
                        None,
                    )
                } else {
                    let last_user_text = req
                        .messages
                        .iter()
                        .rev()
                        .find(|m| m.role == "user" || m.role == "tool")
                        .and_then(|m| m.content.as_deref())
                        .unwrap_or("");
                    let generated = if is_critic {
                        serde_json::json!({
                            "passed": true,
                            "reason": "Gatekeeper Critic verified diff safety, syntax balance, and rubric invariants."
                        })
                        .to_string()
                    } else if is_timer {
                        let compact_line = last_user_text
                            .lines()
                            .find(|l| !l.trim().is_empty())
                            .unwrap_or("Timer wakeup fired")
                            .chars()
                            .take(160)
                            .collect::<String>();
                        format!("Timer triage ready: {compact_line}")
                    } else if cache_key.contains(":prefill:") {
                        let preview: String = last_user_text
                            .lines()
                            .take(8)
                            .collect::<Vec<_>>()
                            .join(" | ");
                        format!(
                            "Context Digest ({cache_key}): Indexed target workspace files without polluting main history. Key trace: {}",
                            preview.chars().take(280).collect::<String>()
                        )
                    } else {
                        let preview: String = last_user_text
                            .lines()
                            .take(6)
                            .collect::<Vec<_>>()
                            .join(" | ");
                        format!(
                            "Coding Proposal ({cache_key}): Synthesized minimal old_text/new_text hunks + verification checklist for: {}",
                            preview.chars().take(280).collect::<String>()
                        )
                    };
                    (
                        generated,
                        "Subagent terse pass.".to_string(),
                        Vec::new(),
                        None,
                        None,
                    )
                };

            let prefix_cache_reused = step_cache_override.unwrap_or(req_prefix_reused);
            if prefix_cache_reused {
                guard.prefix_cache_reuses += 1;
            }

            (
                sub_seq,
                prefix_cache_reused,
                resolved_model,
                routed_experts,
                content,
                reasoning_content,
                tool_calls,
                custom_finish.unwrap_or_else(|| "stop".to_string()),
                delay_ms,
            )
        };

        let in_flight = self.in_flight_subagents.fetch_add(1, Ordering::SeqCst) + 1;
        self.max_concurrent_subagents
            .fetch_max(in_flight, Ordering::SeqCst);
        if delay_ms > 0 {
            thread::sleep(Duration::from_millis(delay_ms));
        }
        self.in_flight_subagents.fetch_sub(1, Ordering::SeqCst);

        let prompt_chars: usize = req
            .messages
            .iter()
            .map(|m| m.content.as_deref().map(str::len).unwrap_or(0))
            .sum();
        let prompt_tokens = ((prompt_chars / 4).max(32)) as u32;
        let tc_chars: usize = tool_calls
            .iter()
            .map(|tc| tc.function.name.len() + tc.function.arguments.len())
            .sum();
        let completion_tokens = (((content.len() + tc_chars) / 4).max(8)) as u32;
        let cached_tokens = if prefix_cache_reused {
            (prompt_tokens / 2).max(16)
        } else {
            0
        };

        Ok(SubagentCompletionResponse {
            response_id: format!("chatcmpl-subagent-{sub_seq}"),
            model: resolved_model,
            content,
            reasoning_content,
            tool_calls,
            finish_reason: Some(finish_reason),
            prompt_tokens,
            completion_tokens,
            cached_tokens,
            decode_tok_s: 64.5,
            routed_experts,
            prefix_cache_reused,
        })
    }

    /// Configures whether the simulated subagent pool should return HTTP 503.
    pub fn set_subagent_unavailable(&self, unavailable: bool) {
        self.inner.lock().subagent_unavailable = unavailable;
    }

    /// Test-only hook simulating a server returning a specific resolved `model`.
    pub fn set_mock_resolved_model_override(&self, model: Option<String>) {
        self.inner.lock().mock_resolved_model_override = model;
    }

    pub fn mock_resolved_model_override(&self) -> Option<String> {
        self.inner.lock().mock_resolved_model_override.clone()
    }

    /// Test-only hook simulating optional routed-experts metadata.
    pub fn set_mock_routed_experts_override(&self, routed_experts: Option<usize>) {
        self.inner.lock().mock_routed_experts_override = routed_experts;
    }

    pub fn mock_routed_experts_override(&self) -> Option<usize> {
        self.inner.lock().mock_routed_experts_override
    }

    /// Overrides the subagent pool status payload returned by [`FixtureTransport`].
    pub fn set_subagent_status_override(&self, status: Option<SubagentPoolStatusResponse>) {
        self.inner.lock().subagent_status_override = status;
    }

    /// Overrides raw telemetry JSON for compatibility tests.
    pub fn set_raw_telemetry_override(&self, raw_json: Option<String>) {
        self.inner.lock().raw_telemetry_override = raw_json;
    }

    /// Returns the configured raw telemetry JSON override if present.
    pub fn raw_telemetry_override(&self) -> Option<String> {
        self.inner.lock().raw_telemetry_override.clone()
    }

    fn estimate_prompt_tokens(messages: &[WireMessage], force_compact_hint: bool) -> u64 {
        let total_chars: usize = messages
            .iter()
            .map(|m| {
                let c_len = m.content.as_ref().map(|s| s.len()).unwrap_or(0);
                let r_len = m.reasoning_content.as_ref().map(|s| s.len()).unwrap_or(0);
                let tc_len = m
                    .tool_calls
                    .as_ref()
                    .map(|tcs| {
                        tcs.iter()
                            .map(|tc| tc.function.name.len() + tc.function.arguments.len())
                            .sum::<usize>()
                    })
                    .unwrap_or(0);
                c_len + r_len + tc_len + 16
            })
            .sum();
        let estimated = (total_chars as u64 / 4) + 12_480;
        if force_compact_hint {
            estimated.max(41_280)
        } else {
            estimated
        }
    }

    fn compact_messages_in_flight(messages: &mut Vec<WireMessage>, new_epoch: u64, reason: &str) {
        if messages.len() <= 6 {
            return;
        }
        let tail_start = messages.len() - 4;
        let dropped_count = tail_start.saturating_sub(1);

        let mut digest_items = Vec::new();
        for msg in &messages[1..tail_start] {
            let preview: String = msg
                .content
                .as_deref()
                .unwrap_or("")
                .chars()
                .take(48)
                .collect();
            digest_items.push(format!("[{}: {}]", msg.role, preview.replace('\n', " ")));
        }

        let summary_msg = WireMessage::user(format!(
            "{COMPACTION_SUMMARY_MARKER}\n\
             Engine-owned synchronized compaction (epoch={new_epoch}, reason={reason}): \
             compacted {dropped_count} middle messages while preserving messages[0] \
             in the prefix cache. Digest: {}",
            digest_items.join(" -> ")
        ));

        let mut compacted = Vec::with_capacity(6);
        compacted.push(messages[0].clone());
        compacted.push(summary_msg);
        compacted.extend_from_slice(&messages[tail_start..]);
        *messages = compacted;
    }

    fn synthesize_tool_calls_if_requested(
        messages: &[WireMessage],
        turn_seq: u64,
        tool_turn_step: usize,
    ) -> Vec<WireToolCall> {
        let Some(last_msg) = messages.last() else {
            return Vec::new();
        };
        // When the last message is already a tool result, synthesize the final assistant answer
        // unless an explicit multi-step second tool marker is present and step == 1.
        if last_msg.role == "tool" {
            return Vec::new();
        }

        // Find the most recent user message to inspect for scenario/tool markers.
        let user_text = messages
            .iter()
            .rev()
            .find(|m| m.role == "user")
            .and_then(|m| m.content.as_deref())
            .unwrap_or("");

        let mut calls = Vec::new();

        // 1. `[SIM_TOOL:read_file]` or `[SIM_TOOL:read_file:<path>]` or `[SIM_SCENARIO:MULTI_TOOL]` or `FORCE_TOOL`
        if let Some(arg) = Self::extract_marker_arg(user_text, "[SIM_TOOL:read_file") {
            let path = if arg.is_empty() {
                "Cargo.toml".to_string()
            } else {
                arg
            };
            calls.push(WireToolCall {
                id: format!("call_read_file_{turn_seq}_{}", calls.len()),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "read_file".to_string(),
                    arguments: serde_json::json!({ "path": path }).to_string(),
                },
                index: Some(calls.len()),
            });
        } else if user_text.contains("[SIM_SCENARIO:MULTI_TOOL]") || user_text.contains("FORCE_TOOL")
        {
            calls.push(WireToolCall {
                id: format!("call_read_file_{turn_seq}_0"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "read_file".to_string(),
                    arguments: serde_json::json!({ "path": "Cargo.toml" }).to_string(),
                },
                index: Some(0),
            });
        }

        // 2. `[SIM_TOOL:edit_file]` or `[SIM_TOOL:edit_file:<path>:<old>:<new>]`
        if let Some(arg) = Self::extract_marker_arg(user_text, "[SIM_TOOL:edit_file") {
            let parts: Vec<&str> = if arg.is_empty() {
                Vec::new()
            } else {
                arg.splitn(3, ':').collect()
            };
            let (path, old_text, new_text) = if parts.len() == 3 {
                (parts[0], parts[1], parts[2])
            } else if parts.len() == 1 && !parts[0].is_empty() {
                (parts[0], "OLD_TEXT", "NEW_TEXT")
            } else {
                ("README.md", "OLD_TEXT", "NEW_TEXT")
            };
            let idx = calls.len();
            calls.push(WireToolCall {
                id: format!("call_edit_file_{turn_seq}_{idx}"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "edit_file".to_string(),
                    arguments: serde_json::json!({
                        "path": path,
                        "display_description": "Simulated surgical file edit",
                        "edits": [{
                            "old_text": old_text,
                            "new_text": new_text
                        }]
                    })
                    .to_string(),
                },
                index: Some(idx),
            });
        }

        // 3. `[SIM_TOOL:write_file:<path>:<content>]`
        if let Some(arg) = Self::extract_marker_arg(user_text, "[SIM_TOOL:write_file") {
            let (path, content) = arg
                .split_once(':')
                .unwrap_or(("sim_output.txt", "Simulated file content\n"));
            let idx = calls.len();
            calls.push(WireToolCall {
                id: format!("call_write_file_{turn_seq}_{idx}"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "write_file".to_string(),
                    arguments: serde_json::json!({
                        "path": if path.is_empty() { "sim_output.txt" } else { path },
                        "content": content
                    })
                    .to_string(),
                },
                index: Some(idx),
            });
        }

        // 4. `[SIM_TOOL:tag]` or `[SIM_TOOL:tag:<name>]` or `[SIM_SCENARIO:RUBRIC_TAG]`
        if let Some(arg) = Self::extract_marker_arg(user_text, "[SIM_TOOL:tag") {
            let tag_name = if arg.is_empty() {
                "systems-architecture"
            } else {
                arg.as_str()
            };
            let idx = calls.len();
            calls.push(WireToolCall {
                id: format!("call_tag_{turn_seq}_{idx}"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "tag".to_string(),
                    arguments: serde_json::json!({
                        "action": "apply",
                        "name": tag_name
                    })
                    .to_string(),
                },
                index: Some(idx),
            });
        } else if user_text.contains("[SIM_SCENARIO:RUBRIC_TAG]") {
            let idx = calls.len();
            calls.push(WireToolCall {
                id: format!("call_tag_{turn_seq}_{idx}"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "tag".to_string(),
                    arguments: serde_json::json!({
                        "action": "apply",
                        "name": "research-synthesis"
                    })
                    .to_string(),
                },
                index: Some(idx),
            });
        }

        // 5. `[SIM_TOOL:schedule_wakeup]` or `[SIM_SCENARIO:WAKEUP_TIMER]`
        if let Some(arg) = Self::extract_marker_arg(user_text, "[SIM_TOOL:schedule_wakeup") {
            let duration_s = arg.parse::<f64>().unwrap_or(5.0);
            let idx = calls.len();
            calls.push(WireToolCall {
                id: format!("call_wakeup_{turn_seq}_{idx}"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "schedule_wakeup".to_string(),
                    arguments: serde_json::json!({
                        "duration_s": duration_s,
                        "label": "engine-watchdog-timer",
                        "note": "Check background task progress and prefix cache status"
                    })
                    .to_string(),
                },
                index: Some(idx),
            });
        } else if user_text.contains("[SIM_SCENARIO:WAKEUP_TIMER]") {
            let idx = calls.len();
            calls.push(WireToolCall {
                id: format!("call_wakeup_{turn_seq}_{idx}"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "schedule_wakeup".to_string(),
                    arguments: serde_json::json!({
                        "duration_s": 5.0,
                        "label": "engine-watchdog-timer",
                        "note": "Check background task progress and prefix cache status"
                    })
                    .to_string(),
                },
                index: Some(idx),
            });
        }

        // 6. `[SIM_TOOL:grep]` or `[SIM_TOOL:grep:<pattern>]`
        if let Some(arg) = Self::extract_marker_arg(user_text, "[SIM_TOOL:grep") {
            let pattern = if arg.is_empty() {
                "Spell"
            } else {
                arg.as_str()
            };
            let idx = calls.len();
            calls.push(WireToolCall {
                id: format!("call_grep_{turn_seq}_{idx}"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "grep".to_string(),
                    arguments: serde_json::json!({ "pattern": pattern }).to_string(),
                },
                index: Some(idx),
            });
        }

        // 7. `[SIM_TOOL:list_directory]`
        if let Some(arg) = Self::extract_marker_arg(user_text, "[SIM_TOOL:list_directory") {
            let path = if arg.is_empty() { "." } else { arg.as_str() };
            let idx = calls.len();
            calls.push(WireToolCall {
                id: format!("call_list_dir_{turn_seq}_{idx}"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "list_directory".to_string(),
                    arguments: serde_json::json!({ "path": path }).to_string(),
                },
                index: Some(idx),
            });
        }

        // 8. `[SIM_TOOL:fetch]`
        if let Some(arg) = Self::extract_marker_arg(user_text, "[SIM_TOOL:fetch") {
            let url = if arg.is_empty() {
                "https://arxiv.org/abs/2501.12345"
            } else {
                arg.as_str()
            };
            let idx = calls.len();
            calls.push(WireToolCall {
                id: format!("call_fetch_{turn_seq}_{idx}"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "fetch".to_string(),
                    arguments: serde_json::json!({ "url": url }).to_string(),
                },
                index: Some(idx),
            });
        }

        // 9. `[SIM_TOOL:open_message_terminal]` or `[SIM_TOOL:open_message_terminal:<path>]`
        if let Some(arg) = Self::extract_marker_arg(user_text, "[SIM_TOOL:open_message_terminal") {
            let (path, reason) = if arg.is_empty() {
                (
                    ".".to_string(),
                    "Agent-requested interactive CLI debugging session".to_string(),
                )
            } else if let Some((p, r)) = arg.split_once(':') {
                (
                    if p.trim().is_empty() {
                        ".".to_string()
                    } else {
                        p.trim().to_string()
                    },
                    r.trim().to_string(),
                )
            } else {
                (
                    arg,
                    "Agent-requested interactive CLI debugging session".to_string(),
                )
            };
            let idx = calls.len();
            calls.push(WireToolCall {
                id: format!("call_open_message_terminal_{turn_seq}_{idx}"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "open_message_terminal".to_string(),
                    arguments: serde_json::json!({
                        "path": path,
                        "reason": reason
                    })
                    .to_string(),
                },
                index: Some(idx),
            });
        }

        // 10. `[SIM_TOOL:read_terminal]` (read-only user terminal scrollback)
        if Self::extract_marker_arg(user_text, "[SIM_TOOL:read_terminal").is_some() {
            let idx = calls.len();
            calls.push(WireToolCall {
                id: format!("call_read_terminal_{turn_seq}_{idx}"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "read_terminal".to_string(),
                    arguments: serde_json::json!({ "lines": 40 }).to_string(),
                },
                index: Some(idx),
            });
        }

        // 11. `[SIM_TOOL:type_in_terminal:<text>]` (always requires user approval)
        if let Some(arg) = Self::extract_marker_arg(user_text, "[SIM_TOOL:type_in_terminal") {
            let idx = calls.len();
            calls.push(WireToolCall {
                id: format!("call_type_in_terminal_{turn_seq}_{idx}"),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "type_in_terminal".to_string(),
                    arguments: serde_json::json!({
                        "text": if arg.is_empty() { "pwd".to_string() } else { arg },
                        "submit": true
                    })
                    .to_string(),
                },
                index: Some(idx),
            });
        }

        let _ = tool_turn_step;
        calls
    }

    /// Extracts optional `:<arg>` from `[PREFIX]` or `[PREFIX:<arg>]`.
    fn extract_marker_arg(text: &str, prefix: &str) -> Option<String> {
        let start = text.find(prefix)?;
        let after = &text[start + prefix.len()..];
        let end = after.find(']')?;
        let inner = &after[..end];
        if let Some(stripped) = inner.strip_prefix(':') {
            Some(stripped.trim().to_string())
        } else if inner.is_empty() {
            Some(String::new())
        } else {
            None
        }
    }
}

impl EngineTransport for FixtureTransport {
    fn transport_name(&self) -> &str {
        "FixtureTransport (Simulated Engine)"
    }

    fn list_models(&self) -> Result<Vec<ModelDescriptor>, TransportError> {
        Ok(self.inner.lock().models.clone())
    }

    fn stream_chat_completion(
        &self,
        req: &mut ChatCompletionRequest,
        on_chunk: &mut dyn FnMut(&ChatCompletionChunk),
    ) -> Result<ChatTurnSummary, String> {
        let thread_key = derive_thread_key(req.prompt_cache_key.as_deref());
        let requested_model = if req.model.trim().is_empty() {
            FIXTURE_MAIN_MODEL_ID.to_string()
        } else {
            req.model.clone()
        };
        validate_model_for_role(AgentExecutionRole::MainOrchestrator, &requested_model)
            .map_err(|e| TransportError::ModelRoleViolation(e).to_string())?;
        let model_id = self
            .mock_resolved_model_override()
            .unwrap_or(requested_model);

        let mut engine_injected_alarms = Vec::new();
        let (turn_seq, tool_calls, reasoning_parts, content_parts, finish_reason, usage, advice) = {
            let mut guard = self.inner.lock();
            guard.recorded_main_requests.push(req.clone());
            guard.next_turn_seq += 1;
            let turn_seq = guard.next_turn_seq;

            // 1. Decision 2: Engine-Owned Timer Delivery (`inject_due_agent_timers_at_end`)
            // Pop due timers for `thread_key` or `"*"` and append banners to `req.messages.last_mut()`
            // without touching `req.messages[0]`!
            let is_aux_slot =
                thread_key.starts_with("timer:") || thread_key.starts_with("critic:");
            let base_thread_key = derive_slot_base_thread_key(&thread_key);
            for timer in &mut guard.timers {
                let matches_thread = timer.thread_id == thread_key
                    || (!is_aux_slot
                        && (timer.thread_id == base_thread_key
                            || timer.thread_id == "*"
                            || timer.thread_id.is_empty()));
                let is_due = !timer.delivered
                    && timer.status != "cancelled"
                    && (timer.remaining_s <= 0.0
                        || timer.status == "expired"
                        || timer.status == "due");
                if matches_thread && is_due {
                    timer.remaining_s = 0.0;
                    timer.delivered = true;
                    timer.status = "delivered".to_string();
                    let banner = timer.format_engine_alarm_banner();
                    engine_injected_alarms.push(banner.clone());

                    if req.messages.iter().any(|m| {
                        let role = m.role.to_ascii_lowercase();
                        role == "user" || role == "tool"
                    }) {
                        apply_engine_alarm_banners_to_messages(
                            &mut req.messages,
                            std::slice::from_ref(&banner),
                        );
                    } else {
                        req.messages.push(WireMessage::user(banner));
                    }
                }
            }

            // Detect scenario hints in the latest user/tool messages
            let last_non_system_text = req
                .messages
                .iter()
                .rev()
                .find(|m| m.role != "system")
                .and_then(|m| m.content.as_deref())
                .unwrap_or("")
                .to_string();

            // `[SIM_DELAY_MS:<n>]` holds the response for n ms (max 8 s) so tests can observe the
            // IDE while a turn is in flight.
            if let Some(ms) = last_non_system_text
                .split("[SIM_DELAY_MS:")
                .nth(1)
                .and_then(|rest| rest.split(']').next())
                .and_then(|v| v.trim().parse::<u64>().ok())
            {
                std::thread::sleep(std::time::Duration::from_millis(ms.min(8_000)));
            }

            let force_compact_hint = last_non_system_text.contains("[SIM_SCENARIO:ENGINE_COMPACT]")
                || last_non_system_text.contains("FORCE_COMPACT")
                || last_non_system_text.contains("[SIM_COMPACT]");
            let low_tps_hint = last_non_system_text.contains("[SIM_SCENARIO:LOW_TPS]");

            let estimated_tokens =
                Self::estimate_prompt_tokens(&req.messages, force_compact_hint);

            let mut state = guard
                .threads
                .get(&thread_key)
                .cloned()
                .unwrap_or_else(|| SimulatedEngineThreadState::new(thread_key.clone()));

            state.turn_hot_cache_cleared = false;
            state.turn_freed_blocks = 0;
            state.last_prompt_tokens = estimated_tokens;

            let min_tps = req
                .auto_compact_min_tps
                .unwrap_or(ENGINE_MIN_DECODE_TPS);

            if low_tps_hint {
                state.decode_tok_s = 31.4;
                state.decode_tok_s_ewma = 32.8;
            } else if state.decode_tok_s < min_tps && !state.needs_compaction {
                state.decode_tok_s = 0.0;
                state.decode_tok_s_ewma = 0.0;
            }

            let req_epoch = req.client_compaction_epoch.unwrap_or(0);
            let external_epoch_bump = req_epoch > state.compaction_epoch;
            let tokens_exceeded = estimated_tokens >= ENGINE_INTERCEPTOR_COMPACT_TOKENS;
            let tps_below = state.decode_tok_s > 0.0 && state.decode_tok_s < min_tps;

            // 2. Decision 3: Engine-Owned In-Flight Compaction & Hot-Cache Clear
            let mut compaction_reason = "healthy".to_string();
            let mut compaction_action = "ok".to_string();

            if force_compact_hint
                || tokens_exceeded
                || tps_below
                || state.needs_compaction
                || external_epoch_bump
            {
                let reason = if external_epoch_bump {
                    "compaction_detected"
                } else if tps_below {
                    "decode_tps_below_threshold"
                } else {
                    "token_threshold_exceeded"
                };

                if external_epoch_bump {
                    state.compaction_epoch = req_epoch;
                } else {
                    state.compaction_epoch += 1;
                }

                Self::compact_messages_in_flight(&mut req.messages, state.compaction_epoch, reason);
                req.client_compaction_epoch = Some(state.compaction_epoch);

                state.needs_compaction = false;
                state.turn_hot_cache_cleared = true;
                state.turn_freed_blocks = 24;

                guard.total_compactions += 1;
                guard.total_hot_resets += 1;
                guard.total_freed_blocks += 24;

                compaction_reason = reason.to_string();
                compaction_action = "hot_cache_cleared".to_string();
            }

            // 3. Determine whether to emit tool calls or final assistant content
            let tool_calls = if !guard.main_tool_calls_queue.is_empty() {
                guard.main_tool_calls_queue.remove(0)
            } else {
                Self::synthesize_tool_calls_if_requested(
                    &req.messages,
                    turn_seq,
                    state.tool_turn_step,
                )
            };

            let (reasoning_parts, content_parts, finish_reason) = if !tool_calls.is_empty() {
                state.tool_turn_step += 1;
                let tool_names: Vec<&str> =
                    tool_calls.iter().map(|tc| tc.function.name.as_str()).collect();
                let r1 = format!(
                    "Evaluating request on thread `{thread_key}` (epoch={}, prompt_tokens={}). ",
                    state.compaction_epoch, state.last_prompt_tokens
                );
                let r2 = format!(
                    "Dispatching headless tool call(s): {:?} while keeping messages[0] pinned in SSD Cold Cache.",
                    tool_names
                );
                (vec![r1, r2], Vec::new(), "tool_calls".to_string())
            } else {
                state.tool_turn_step = 0;
                let tool_outputs: Vec<String> = req
                    .messages
                    .iter()
                    .rev()
                    .take_while(|m| m.role == "tool")
                    .filter_map(|m| m.content.clone())
                    .collect();

                let r1 = format!(
                    "Spell test model reasoning on thread `{thread_key}` (epoch={}, decode={:.1} tok/s). ",
                    state.compaction_epoch, state.decode_tok_s
                );
                let r2 = if !tool_outputs.is_empty() {
                    format!(
                        "Synthesizing results from {} completed tool execution(s).",
                        tool_outputs.len()
                    )
                } else if !engine_injected_alarms.is_empty() {
                    format!(
                        "Processing {} injected timer wakeup alarm(s).",
                        engine_injected_alarms.len()
                    )
                } else if state.turn_hot_cache_cleared {
                    format!(
                        "Context compaction completed (epoch #{}, freed {} blocks).",
                        state.compaction_epoch, state.turn_freed_blocks
                    )
                } else {
                    "Generating response.".to_string()
                };

                let mut body_sections = Vec::new();
                body_sections.push(format!(
                    "### Spell Test Response (`{thread_key}` • Epoch #{})\n",
                    state.compaction_epoch
                ));

                if state.turn_hot_cache_cleared {
                    body_sections.push(format!(
                        "- **Context Compaction**: Cleared `{}` blocks (`action = \"{}\"` / `reason = \"{}\"`).\n",
                        state.turn_freed_blocks, compaction_action, compaction_reason
                    ));
                }

                if !engine_injected_alarms.is_empty() {
                    body_sections.push(format!(
                        "- **Timer Wakeup (`{TIMER_ALARM_MARKER}`)**: Delivered `{}` due timer alarm(s) at message tail.\n",
                        engine_injected_alarms.len()
                    ));
                }

                if !tool_outputs.is_empty() {
                    let preview: String = tool_outputs[0].chars().take(240).collect();
                    body_sections.push(format!(
                        "- **Verified Tool Output**:\n```\n{}\n```\n",
                        preview.trim()
                    ));
                } else {
                    let prompt_preview: String = last_non_system_text.chars().take(120).collect();
                    body_sections.push(format!(
                        "Processed request: `{}` with decode velocity `{:.1} tok/s` (EWMA `{:.1} tok/s`).",
                        prompt_preview.replace('\n', " "),
                        state.decode_tok_s,
                        state.decode_tok_s_ewma
                    ));
                }

                (vec![r1, r2], body_sections, "stop".to_string())
            };

            let advice = EngineContextAdvice {
                action: compaction_action,
                reason: compaction_reason,
                decode_tok_s: state.decode_tok_s,
                decode_tok_s_ewma: state.decode_tok_s_ewma,
                threshold_tok_s: min_tps,
                min_tps_threshold: min_tps,
                prompt_tokens: state.last_prompt_tokens,
                token_threshold: ENGINE_COMPACT_TOKEN_THRESHOLD,
                hot_cache_cleared: state.turn_hot_cache_cleared,
                freed_blocks: state.turn_freed_blocks,
                blocks_freed: state.turn_freed_blocks,
                thread_key: thread_key.clone(),
                compaction_epoch: state.compaction_epoch,
            };

            let completion_tokens = 192u64;
            let cached_tokens = (state.last_prompt_tokens * 4) / 5;
            let usage = ChatCompletionUsage {
                prompt_tokens: state.last_prompt_tokens,
                completion_tokens,
                total_tokens: state.last_prompt_tokens + completion_tokens,
                prompt_tokens_details: Some(PromptTokensDetails { cached_tokens }),
                prompt_tokens_per_second: Some(1480.0),
                generation_tokens_per_second: Some(state.decode_tok_s),
                engine_context_advice: Some(advice.clone()),
            };

            guard.threads.insert(thread_key.clone(), state);

            (
                turn_seq,
                tool_calls,
                reasoning_parts,
                content_parts,
                finish_reason,
                usage,
                advice,
            )
        };

        let response_id = format!("chatcmpl-spell-{turn_seq}");
        let mut raw_sse_frames = Vec::new();
        let mut emit_chunk = |chunk: ChatCompletionChunk| -> Result<(), String> {
            let json = serde_json::to_string(&chunk)
                .map_err(|e| format!("Failed to serialize SSE chunk: {e}"))?;
            raw_sse_frames.push(format!("data: {json}\n\n"));
            on_chunk(&chunk);
            Ok(())
        };

        // Chunk 1: Role chunk (`delta.role = Some("assistant")`, `finish_reason = None`, `usage = None`)
        emit_chunk(ChatCompletionChunk {
            id: response_id.clone(),
            model: model_id.clone(),
            choices: vec![ChatCompletionChunkChoice {
                index: 0,
                delta: ChatCompletionDelta {
                    role: Some("assistant".to_string()),
                    content: None,
                    reasoning_content: None,
                    tool_calls: None,
                },
                finish_reason: None,
            }],
            usage: None,
            engine_context_advice: None,
        })?;

        // Chunk 2..K: Separate `reasoning_content` chunks (`delta.reasoning_content = Some(...)`, `delta.content = None`)
        let mut full_reasoning = String::new();
        for part in &reasoning_parts {
            if crate::is_turn_cancel_requested() {
                break;
            }
            full_reasoning.push_str(part);
            emit_chunk(ChatCompletionChunk {
                id: response_id.clone(),
                model: model_id.clone(),
                choices: vec![ChatCompletionChunkChoice {
                    index: 0,
                    delta: ChatCompletionDelta {
                        role: None,
                        content: None,
                        reasoning_content: Some(part.clone()),
                        tool_calls: None,
                    },
                    finish_reason: None,
                }],
                usage: None,
                engine_context_advice: None,
            })?;
        }

        // Chunk K+1..M: Content or Tool Call chunks
        let mut full_content = String::new();
        if !tool_calls.is_empty() && !crate::is_turn_cancel_requested() {
            emit_chunk(ChatCompletionChunk {
                id: response_id.clone(),
                model: model_id.clone(),
                choices: vec![ChatCompletionChunkChoice {
                    index: 0,
                    delta: ChatCompletionDelta {
                        role: None,
                        content: None,
                        reasoning_content: None,
                        tool_calls: Some(tool_calls.clone()),
                    },
                    finish_reason: Some("tool_calls".to_string()),
                }],
                usage: None,
                engine_context_advice: None,
            })?;
        } else {
            let last_idx = content_parts.len().saturating_sub(1);
            for (idx, part) in content_parts.iter().enumerate() {
                if crate::is_turn_cancel_requested() {
                    break;
                }
                full_content.push_str(part);
                let fr = if idx == last_idx {
                    Some(finish_reason.clone())
                } else {
                    None
                };
                emit_chunk(ChatCompletionChunk {
                    id: response_id.clone(),
                    model: model_id.clone(),
                    choices: vec![ChatCompletionChunkChoice {
                        index: 0,
                        delta: ChatCompletionDelta {
                            role: None,
                            content: Some(part.clone()),
                            reasoning_content: None,
                            tool_calls: None,
                        },
                        finish_reason: fr,
                    }],
                    usage: None,
                    engine_context_advice: None,
                })?;
            }
        }

        // Final Usage Chunk: `choices: vec![]` (exact match to `server.py:5266`)
        emit_chunk(ChatCompletionChunk {
            id: response_id.clone(),
            model: model_id.clone(),
            choices: Vec::new(),
            usage: Some(usage.clone()),
            engine_context_advice: Some(advice.clone()),
        })?;

        raw_sse_frames.push("data: [DONE]\n\n".to_string());

        Ok(ChatTurnSummary {
            response_id,
            model: model_id,
            reasoning_content: full_reasoning,
            content: full_content,
            tool_calls,
            finish_reason,
            usage,
            context_advice: advice,
            engine_injected_alarms,
            raw_sse_frames,
        })
    }

    fn clear_thread_cache(&self, req: &ClearThreadCacheRequest) -> Result<EngineContextAdvice, String> {
        let thread_key = derive_thread_key(Some(req.prompt_cache_key.as_str()));
        let mut guard = self.inner.lock();
        let mut state = guard
            .threads
            .get(&thread_key)
            .cloned()
            .unwrap_or_else(|| SimulatedEngineThreadState::new(thread_key.clone()));

        let next_epoch = req
            .compaction_epoch
            .map(|e| e.max(state.compaction_epoch + 1))
            .unwrap_or(state.compaction_epoch + 1);
        state.compaction_epoch = next_epoch;
        state.needs_compaction = false;
        state.turn_hot_cache_cleared = true;
        state.turn_freed_blocks = 24;

        guard.total_compactions += 1;
        guard.total_hot_resets += 1;
        guard.total_freed_blocks += 24;

        let advice = EngineContextAdvice {
            action: "hot_cache_cleared".to_string(),
            reason: "compaction_detected".to_string(),
            decode_tok_s: state.decode_tok_s,
            decode_tok_s_ewma: state.decode_tok_s_ewma,
            threshold_tok_s: ENGINE_MIN_DECODE_TPS,
            min_tps_threshold: ENGINE_MIN_DECODE_TPS,
            prompt_tokens: state.last_prompt_tokens,
            token_threshold: ENGINE_COMPACT_TOKEN_THRESHOLD,
            hot_cache_cleared: true,
            freed_blocks: 24,
            blocks_freed: 24,
            thread_key: thread_key.clone(),
            compaction_epoch: state.compaction_epoch,
        };

        guard.threads.insert(thread_key, state);
        Ok(advice)
    }

    fn get_compaction_status(
        &self,
        _prompt_cache_key: Option<&str>,
    ) -> Result<CompactionStatusResponse, String> {
        let guard = self.inner.lock();
        Ok(CompactionStatusResponse {
            ok: true,
            hot_cache_only: true,
            ssd_cold_cache_skipped: true,
            ssd_writes: 0,
            min_decode_tps: ENGINE_MIN_DECODE_TPS,
            token_threshold: ENGINE_COMPACT_TOKEN_THRESHOLD,
            total_compactions: guard.total_compactions,
            total_hot_resets: guard.total_hot_resets,
            total_freed_blocks: guard.total_freed_blocks,
            active_threads_count: guard.threads.len(),
        })
    }

    fn set_timer(&self, req: &SetTimerRequest) -> Result<TimerRecord, String> {
        let mut guard = self.inner.lock();
        guard.next_timer_seq += 1;
        let seq = guard.next_timer_seq;
        let duration_s = req.duration_s.max(0.0);
        let status = if duration_s <= 0.0 {
            "expired".to_string()
        } else {
            "pending".to_string()
        };
        let record = TimerRecord {
            id: format!("tmr-spell-{seq:04}"),
            thread_id: if req.thread_id.trim().is_empty() {
                "default".to_string()
            } else {
                derive_thread_key(Some(req.thread_id.as_str()))
            },
            label: if req.label.trim().is_empty() {
                format!("wakeup-{seq}")
            } else {
                req.label.clone()
            },
            note: req.note.clone(),
            duration_s,
            remaining_s: duration_s,
            status,
            delivered: false,
            watch_target: req.watch_target.clone(),
        };
        guard.timers.push(record.clone());
        Ok(record)
    }

    fn list_timers(
        &self,
        thread_id: Option<&str>,
        include_delivered: bool,
    ) -> Result<Vec<TimerRecord>, String> {
        let guard = self.inner.lock();
        let target_thread = thread_id
            .map(|t| t.trim())
            .filter(|t| !t.is_empty() && *t != "*")
            .map(|t| derive_thread_key(Some(t)));

        let out = guard
            .timers
            .iter()
            .filter(|t| {
                if !include_delivered && (t.delivered || t.status == "cancelled") {
                    return false;
                }
                if let Some(ref tid) = target_thread {
                    t.thread_id == *tid || t.thread_id == "*"
                } else {
                    true
                }
            })
            .cloned()
            .collect();
        Ok(out)
    }

    fn cancel_timer(&self, timer_id: &str) -> Result<bool, String> {
        let mut guard = self.inner.lock();
        for timer in &mut guard.timers {
            if timer.id == timer_id && timer.status != "cancelled" {
                timer.status = "cancelled".to_string();
                return Ok(true);
            }
        }
        Ok(false)
    }

    fn pop_due_timers(&self, thread_id: Option<&str>) -> Result<Vec<TimerRecord>, String> {
        let mut guard = self.inner.lock();
        let target_thread = thread_id
            .map(|t| t.trim())
            .filter(|t| !t.is_empty() && *t != "*")
            .map(|t| derive_thread_key(Some(t)));

        let mut popped = Vec::new();
        for timer in &mut guard.timers {
            let matches_thread = match target_thread {
                Some(ref tid) => {
                    timer.thread_id == *tid || timer.thread_id == "*" || timer.thread_id.is_empty()
                }
                None => true,
            };
            let is_due = !timer.delivered
                && timer.status != "cancelled"
                && (timer.remaining_s <= 0.0
                    || timer.status == "expired"
                    || timer.status == "due");
            if matches_thread && is_due {
                timer.remaining_s = 0.0;
                timer.delivered = true;
                timer.status = "fired".to_string();
                popped.push(timer.clone());
            }
        }
        Ok(popped)
    }

    fn get_telemetry(&self) -> Result<TelemetryResponse, String> {
        let guard = self.inner.lock();
        let (decode_tok_s, decode_tok_s_ewma, max_epoch) = guard
            .threads
            .values()
            .fold((0.0_f64, 0.0_f64, 0_u64), |acc, st| {
                (st.decode_tok_s, st.decode_tok_s_ewma, acc.2.max(st.compaction_epoch))
            });
        let active_timers_count = guard
            .timers
            .iter()
            .filter(|t| !t.delivered && t.status != "cancelled")
            .count();

        Ok(TelemetryResponse {
            engine_online: true,
            engine_version: FIXTURE_MAIN_MODEL_ID.to_string(),
            api_schema_version: "openai.v1".to_string(),
            decode_tok_s,
            decode_tok_s_ewma,
            compaction_epoch: max_epoch,
            hot_cache_blocks_freed: guard.total_freed_blocks,
            active_timers_count,
            transport_mode: self.transport_name().to_string(),
            ..Default::default()
        })
    }

    fn run_subagent_completion(
        &self,
        req: &SubagentCompletionRequest,
    ) -> Result<SubagentCompletionResponse, TransportError> {
        self.run_subagent_completion_with_meta(req, None, None)
    }

    fn fetch_subagents_status(&self) -> Result<SubagentPoolStatusResponse, TransportError> {
        let guard = self.inner.lock();
        if let Some(ref custom) = guard.subagent_status_override {
            return Ok(custom.clone());
        }
        let mut status = SubagentPoolStatusResponse::default();
        status.base_model_id = FIXTURE_MAIN_MODEL_ID.to_string();
        status.coding_subagent_model_id = FIXTURE_SUBAGENT_MODEL_ID.to_string();
        if guard.prefix_cache_reuses > 0 {
            status.subagent_decode_tok_s = 64.5;
            status.last_subagent_decode_tok_s = 64.5;
            status.inference_backed = true;
        }
        Ok(status)
    }

    fn get_subagent_pool_status(&self) -> Result<SubagentPoolStatusResponse, TransportError> {
        self.fetch_subagents_status()
    }
}


// ============================================================================
// 4. Shared Verification Server Handler (`LoopbackVerifyServer` & `UnixVerifyServer`)
// ============================================================================

fn serialize_timer_with_both_ids(record: &TimerRecord) -> Value {
    let mut val = serde_json::to_value(record).unwrap_or_else(|_| serde_json::json!({}));
    if let Some(obj) = val.as_object_mut() {
        obj.insert(
            "id".to_string(),
            Value::String(record.effective_id().to_string()),
        );
        obj.insert(
            "timer_id".to_string(),
            Value::String(record.effective_id().to_string()),
        );
    }
    val
}

fn parse_query_params(query: Option<&str>) -> HashMap<String, String> {
    let mut map = HashMap::new();
    let Some(q) = query else {
        return map;
    };
    for pair in q.split('&') {
        if let Some((k, v)) = pair.split_once('=') {
            map.insert(k.trim().to_string(), v.trim().to_string());
        }
    }
    map
}

fn write_verify_json<S: Write>(
    stream: &mut S,
    code: u16,
    reason: &str,
    json: &str,
) -> std::io::Result<()> {
    let resp = format!(
        "HTTP/1.1 {code} {reason}\r\n\
         Content-Type: application/json\r\n\
         Content-Length: {}\r\n\
         Connection: close\r\n\r\n{}",
        json.len(),
        json
    );
    stream.write_all(resp.as_bytes())?;
    stream.flush()
}

fn handle_verify_http_stream<S: Read + Write>(
    stream: S,
    fixture: Arc<FixtureTransport>,
    expected_key: &str,
    transport_label: &str,
) -> std::io::Result<()> {
    let mut reader = BufReader::new(stream);
    let mut raw_header_bytes = Vec::new();
    loop {
        let mut line = Vec::new();
        let n = reader.read_until(b'\n', &mut line)?;
        if n == 0 {
            return Ok(());
        }
        raw_header_bytes.extend_from_slice(&line);
        if raw_header_bytes.ends_with(b"\r\n\r\n") || raw_header_bytes.ends_with(b"\n\n") {
            break;
        }
        if raw_header_bytes.len() > 65_536 {
            return Ok(());
        }
    }

    let mut headers_storage = [httparse::EMPTY_HEADER; 64];
    let mut req = httparse::Request::new(&mut headers_storage);
    if req.parse(&raw_header_bytes).is_err() {
        return write_verify_json(
            reader.get_mut(),
            400,
            "Bad Request",
            "{\"error\":\"bad_request\"}",
        );
    }

    let method = req.method.unwrap_or("GET").to_string();
    let full_path = req.path.unwrap_or("/").to_string();

    let mut content_length = 0usize;
    let mut auth_bearer: Option<String> = None;

    for h in req.headers.iter() {
        let name_lower = h.name.to_ascii_lowercase();
        let val_str = String::from_utf8_lossy(h.value).trim().to_string();
        match name_lower.as_str() {
            "content-length" => {
                content_length = val_str.parse::<usize>().unwrap_or(0);
            }
            "authorization" => {
                if let Some(token) = val_str.strip_prefix("Bearer ") {
                    auth_bearer = Some(token.trim().to_string());
                }
            }
            _ => {}
        }
    }

    let mut body_bytes = vec![0u8; content_length];
    if content_length > 0 {
        reader.read_exact(&mut body_bytes)?;
    }
    let body_str = String::from_utf8_lossy(&body_bytes).to_string();
    let stream_out = reader.get_mut();

    // Enforce standard OpenAI `Authorization: Bearer <key>`
    if !expected_key.is_empty() && auth_bearer.as_deref() != Some(expected_key) {
        return write_verify_json(
            stream_out,
            401,
            "Unauthorized",
            "{\"error\":{\"message\":\"Missing or invalid Authorization: Bearer token\",\"type\":\"authentication_error\"}}",
        );
    }

    let (path_only, _query_str) = full_path
        .split_once('?')
        .map(|(p, q)| (p, Some(q)))
        .unwrap_or((full_path.as_str(), None));
    let _ = transport_label;

    match (method.as_str(), path_only) {
        // Test-only control route: advance the fixture's clock so armed timers become due.
        ("POST", "/test/advance_clock") => {
            let seconds = serde_json::from_str::<Value>(&body_str)
                .ok()
                .and_then(|v| v.get("seconds").and_then(|s| s.as_f64()))
                .unwrap_or(0.0);
            fixture.advance_simulated_time(seconds);
            write_verify_json(
                stream_out,
                200,
                "OK",
                &serde_json::json!({"ok": true, "advanced_s": seconds}).to_string(),
            )
        }
        ("GET", "/v1/models" | "/models") => match fixture.list_models() {
            Ok(models) => {
                let data: Vec<Value> = models
                    .iter()
                    .map(|m| {
                        let mut obj = serde_json::json!({
                            "id": m.id,
                            "object": "model",
                            "owned_by": m.owned_by.as_deref().unwrap_or("spell-test"),
                        });
                        if let Some(cw) = m.context_window {
                            if let Some(map) = obj.as_object_mut() {
                                map.insert("context_window".to_string(), serde_json::json!(cw));
                            }
                        }
                        obj
                    })
                    .collect();
                let payload = serde_json::json!({
                    "object": "list",
                    "data": data,
                })
                .to_string();
                write_verify_json(stream_out, 200, "OK", &payload)
            }
            Err(e) => {
                let msg = serde_json::json!({ "error": e.to_string() });
                write_verify_json(stream_out, 500, "Internal Server Error", &msg.to_string())
            }
        },
        ("POST", "/v1/chat/completions" | "/chat/completions") => {
            let raw_val: Value = match serde_json::from_str(&body_str) {
                Ok(v) => v,
                Err(e) => {
                    let msg = serde_json::json!({ "error": format!("Invalid JSON: {e}") });
                    return write_verify_json(stream_out, 400, "Bad Request", &msg.to_string());
                }
            };
            let stream_flag = raw_val
                .get("stream")
                .and_then(|v| v.as_bool())
                .unwrap_or(true);
            let req_model = raw_val
                .get("model")
                .and_then(|v| v.as_str())
                .unwrap_or("");
            let is_subagent_request = !stream_flag
                || req_model == FIXTURE_SUBAGENT_MODEL_ID
                || body_str.contains("<subagent_role")
                || body_str.contains("Active turn context for delegated subagent");

            if is_subagent_request {
                let sub_req: SubagentCompletionRequest = match serde_json::from_str(&body_str) {
                    Ok(r) => r,
                    Err(e) => {
                        let msg =
                            serde_json::json!({ "error": format!("Invalid subagent JSON: {e}") });
                        return write_verify_json(stream_out, 400, "Bad Request", &msg.to_string());
                    }
                };
                let wire_meta = (
                    raw_val.get("tool_choice").cloned(),
                    raw_val.get("chat_template_kwargs").cloned(),
                );
                match fixture.run_subagent_completion_with_meta(&sub_req, None, Some(wire_meta)) {
                    Ok(sub_resp) => {
                        let resolved_model = sub_resp.model.clone();
                        let final_finish_reason =
                            sub_resp.finish_reason.clone().unwrap_or_else(|| {
                                if sub_resp.tool_calls.is_empty() {
                                    "stop".to_string()
                                } else {
                                    "tool_calls".to_string()
                                }
                            });

                        if stream_flag {
                            let resp_id = sub_resp.response_id.clone();
                            let mut sse_frames: Vec<String> = Vec::new();
                            let mut push_chunk = |chunk_val: Value| {
                                sse_frames.push(format!("data: {chunk_val}\n\n"));
                            };

                            push_chunk(serde_json::json!({
                                "id": resp_id,
                                "object": "chat.completion.chunk",
                                "model": resolved_model,
                                "choices": [{
                                    "index": 0,
                                    "delta": { "role": "assistant", "content": "" },
                                    "finish_reason": Value::Null,
                                }]
                            }));

                            if !sub_resp.reasoning_content.is_empty() {
                                push_chunk(serde_json::json!({
                                    "id": resp_id,
                                    "object": "chat.completion.chunk",
                                    "model": resolved_model,
                                    "choices": [{
                                        "index": 0,
                                        "delta": { "reasoning_content": sub_resp.reasoning_content },
                                        "finish_reason": Value::Null,
                                    }]
                                }));
                            }

                            if !sub_resp.content.is_empty() {
                                push_chunk(serde_json::json!({
                                    "id": resp_id,
                                    "object": "chat.completion.chunk",
                                    "model": resolved_model,
                                    "choices": [{
                                        "index": 0,
                                        "delta": { "content": sub_resp.content },
                                        "finish_reason": Value::Null,
                                    }]
                                }));
                            }

                            for (idx, tc) in sub_resp.tool_calls.iter().enumerate() {
                                let tc_id = if tc.id.is_empty() {
                                    format!("call_sub_{idx}")
                                } else {
                                    tc.id.clone()
                                };
                                let args_str = &tc.function.arguments;
                                let mut split_idx = args_str.len() / 2;
                                while split_idx > 0 && !args_str.is_char_boundary(split_idx) {
                                    split_idx -= 1;
                                }
                                let (first_half, second_half) = args_str.split_at(split_idx);

                                push_chunk(serde_json::json!({
                                    "id": resp_id,
                                    "object": "chat.completion.chunk",
                                    "model": resolved_model,
                                    "choices": [{
                                        "index": 0,
                                        "delta": {
                                            "tool_calls": [{
                                                "index": idx,
                                                "id": tc_id,
                                                "type": "function",
                                                "function": {
                                                    "name": tc.function.name,
                                                    "arguments": first_half,
                                                }
                                            }]
                                        },
                                        "finish_reason": Value::Null,
                                    }]
                                }));

                                push_chunk(serde_json::json!({
                                    "id": resp_id,
                                    "object": "chat.completion.chunk",
                                    "model": resolved_model,
                                    "choices": [{
                                        "index": 0,
                                        "delta": {
                                            "tool_calls": [{
                                                "index": idx,
                                                "function": {
                                                    "arguments": second_half,
                                                }
                                            }]
                                        },
                                        "finish_reason": Value::Null,
                                    }]
                                }));
                            }

                            push_chunk(serde_json::json!({
                                "id": resp_id,
                                "object": "chat.completion.chunk",
                                "model": resolved_model,
                                "choices": [{
                                    "index": 0,
                                    "delta": {},
                                    "finish_reason": final_finish_reason,
                                }]
                            }));

                            push_chunk(serde_json::json!({
                                "id": resp_id,
                                "object": "chat.completion.chunk",
                                "model": resolved_model,
                                "choices": [],
                                "usage": {
                                    "prompt_tokens": sub_resp.prompt_tokens,
                                    "completion_tokens": sub_resp.completion_tokens,
                                    "total_tokens": sub_resp.prompt_tokens + sub_resp.completion_tokens,
                                    "generation_tokens_per_second": sub_resp.decode_tok_s,
                                    "prompt_tokens_details": {
                                        "cached_tokens": sub_resp.cached_tokens,
                                    },
                                }
                            }));

                            sse_frames.push("data: [DONE]\n\n".to_string());
                            let sse_body = sse_frames.concat();

                            let resp = format!(
                                "HTTP/1.1 200 OK\r\n\
                                 Content-Type: text/event-stream\r\n\
                                 Cache-Control: no-cache\r\n\
                                 Content-Length: {}\r\n\
                                 Connection: close\r\n\r\n{}",
                                sse_body.len(),
                                sse_body
                            );
                            stream_out.write_all(resp.as_bytes())?;
                            stream_out.flush()
                        } else {
                            let mut message_obj = serde_json::json!({
                                "role": "assistant",
                                "content": sub_resp.content,
                                "reasoning_content": sub_resp.reasoning_content,
                            });
                            if !sub_resp.tool_calls.is_empty() {
                                if let Some(m) = message_obj.as_object_mut() {
                                    m.insert(
                                        "tool_calls".to_string(),
                                        serde_json::to_value(&sub_resp.tool_calls)
                                            .unwrap_or(Value::Null),
                                    );
                                }
                            }
                            let json_body = serde_json::json!({
                                "id": sub_resp.response_id,
                                "object": "chat.completion",
                                "model": resolved_model,
                                "choices": [{
                                    "index": 0,
                                    "message": message_obj,
                                    "finish_reason": final_finish_reason,
                                }],
                                "usage": {
                                    "prompt_tokens": sub_resp.prompt_tokens,
                                    "completion_tokens": sub_resp.completion_tokens,
                                    "total_tokens": sub_resp.prompt_tokens + sub_resp.completion_tokens,
                                    "prompt_tokens_details": {
                                        "cached_tokens": sub_resp.cached_tokens,
                                    },
                                    "generation_tokens_per_second": sub_resp.decode_tok_s,
                                }
                            })
                            .to_string();
                            let resp = format!(
                                "HTTP/1.1 200 OK\r\n\
                                 Content-Type: application/json\r\n\
                                 Content-Length: {}\r\n\
                                 Connection: close\r\n\r\n{}",
                                json_body.len(),
                                json_body
                            );
                            stream_out.write_all(resp.as_bytes())?;
                            stream_out.flush()
                        }
                    }
                    Err(TransportError::ModelRoleViolation(msg)) => {
                        let err = serde_json::json!({ "error": msg });
                        write_verify_json(stream_out, 400, "Bad Request", &err.to_string())
                    }
                    Err(TransportError::HttpStatus { status, body }) => {
                        write_verify_json(stream_out, status, "Service Unavailable", &body)
                    }
                    Err(other) => {
                        let err = serde_json::json!({ "error": other.to_string() });
                        write_verify_json(
                            stream_out,
                            500,
                            "Internal Server Error",
                            &err.to_string(),
                        )
                    }
                }
            } else {
                let mut chat_req: ChatCompletionRequest = match serde_json::from_str(&body_str) {
                    Ok(r) => r,
                    Err(e) => {
                        let msg = serde_json::json!({ "error": format!("Invalid JSON: {e}") });
                        return write_verify_json(stream_out, 400, "Bad Request", &msg.to_string());
                    }
                };
                match fixture.stream_chat_completion(&mut chat_req, &mut |_| {}) {
                    Ok(summary) => {
                        let sse_payload = summary.raw_sse_frames.concat();
                        let resp = format!(
                            "HTTP/1.1 200 OK\r\n\
                             Content-Type: text/event-stream\r\n\
                             Cache-Control: no-cache\r\n\
                             Content-Length: {}\r\n\
                             Connection: close\r\n\r\n{}",
                            sse_payload.len(),
                            sse_payload
                        );
                        stream_out.write_all(resp.as_bytes())?;
                        stream_out.flush()
                    }
                    Err(e) => {
                        let status = if e.contains("MODEL_ROLE_VIOLATION") {
                            400
                        } else {
                            500
                        };
                        let reason = if status == 400 {
                            "Bad Request"
                        } else {
                            "Internal Server Error"
                        };
                        let msg = serde_json::json!({ "error": e });
                        write_verify_json(stream_out, status, reason, &msg.to_string())
                    }
                }
            }
        }
        _ => write_verify_json(stream_out, 404, "Not Found", "{\"error\":\"not_found\"}"),
    }
}

/// Ephemeral `127.0.0.1:0` OpenAI-compatible HTTP/1.1 + SSE server wrapping `Arc<FixtureTransport>`.
///
/// Enforces `Authorization: Bearer <key>` authentication (returning `HTTP/1.1 401 Unauthorized`
/// when missing or mismatched) so [`HttpTransport`] can be verified end-to-end over real TCP sockets.
pub struct LoopbackVerifyServer {
    addr: SocketAddr,
    expected_api_key: String,
    fixture: Arc<FixtureTransport>,
    shutdown: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
}

impl LoopbackVerifyServer {
    pub fn start(
        fixture: Arc<FixtureTransport>,
        expected_api_key: impl Into<String>,
    ) -> Result<Self, String> {
        Self::start_on_port(fixture, expected_api_key, 0)
    }

    /// Like [`Self::start`], bound to `127.0.0.1:<port>` (`0` = ephemeral).
    pub fn start_on_port(
        fixture: Arc<FixtureTransport>,
        expected_api_key: impl Into<String>,
        port: u16,
    ) -> Result<Self, String> {
        let listener = TcpListener::bind(("127.0.0.1", port))
            .map_err(|e| format!("Failed to bind LoopbackVerifyServer on 127.0.0.1:{port}: {e}"))?;
        listener
            .set_nonblocking(true)
            .map_err(|e| format!("Failed to set nonblocking listener: {e}"))?;
        let addr = listener
            .local_addr()
            .map_err(|e| format!("Failed to query local_addr: {e}"))?;

        let expected_key = expected_api_key.into();
        let shutdown = Arc::new(AtomicBool::new(false));

        let shutdown_clone = shutdown.clone();
        let fixture_clone = fixture.clone();
        let key_clone = expected_key.clone();

        let handle = thread::spawn(move || {
            while !shutdown_clone.load(Ordering::SeqCst) {
                match listener.accept() {
                    Ok((stream, _peer)) => {
                        let _ = stream.set_nonblocking(false);
                        let _ = stream.set_read_timeout(Some(Duration::from_secs(3)));
                        let _ = stream.set_write_timeout(Some(Duration::from_secs(3)));
                        let fx = fixture_clone.clone();
                        let exp_key = key_clone.clone();
                        thread::spawn(move || {
                            let _ = handle_verify_http_stream(
                                stream,
                                fx,
                                &exp_key,
                                "HttpTransport (Live :8000)",
                            );
                        });
                    }
                    Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                        thread::sleep(Duration::from_millis(4));
                    }
                    Err(_) => break,
                }
            }
        });

        Ok(Self {
            addr,
            expected_api_key: expected_key,
            fixture,
            shutdown,
            handle: Some(handle),
        })
    }

    pub fn start_with_locator(
        fixture: Arc<FixtureTransport>,
        locator: &EnginePathLocator,
    ) -> Result<Self, String> {
        let key = locator
            .resolve_api_key()
            .unwrap_or_else(|| "test-support-key".to_string());
        Self::start(fixture, key)
    }

    pub fn addr(&self) -> SocketAddr {
        self.addr
    }

    pub fn base_url(&self) -> String {
        format!("http://{}", self.addr)
    }

    pub fn expected_api_key(&self) -> &str {
        &self.expected_api_key
    }

    pub fn fixture(&self) -> &Arc<FixtureTransport> {
        &self.fixture
    }
}

impl Drop for LoopbackVerifyServer {
    fn drop(&mut self) {
        self.shutdown.store(true, Ordering::SeqCst);
        let _ = TcpStream::connect(self.addr);
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

// ============================================================================
// 5. `UnixVerifyServer` (Ephemeral `AF_UNIX` Socket Server Wrapping `FixtureTransport`)
// ============================================================================

#[cfg(unix)]
static UNIX_VERIFY_SEQ: AtomicU64 = AtomicU64::new(1);

/// Ephemeral `AF_UNIX` (`std::os::unix::net::UnixListener`) HTTP/1.1 + SSE server wrapping
/// `Arc<FixtureTransport>` for air-gapped IPC verification (`G7`).
#[cfg(unix)]
pub struct UnixVerifyServer {
    socket_path: PathBuf,
    expected_api_key: String,
    fixture: Arc<FixtureTransport>,
    shutdown: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
}

#[cfg(unix)]
impl UnixVerifyServer {
    /// Starts a `UnixVerifyServer` at an automatically generated short `/tmp/bt_ipc_<pid>_<seq>.sock` path (`< 104` bytes).
    pub fn start(
        fixture: Arc<FixtureTransport>,
        expected_api_key: impl Into<String>,
    ) -> Result<Self, String> {
        let seq = UNIX_VERIFY_SEQ.fetch_add(1, Ordering::SeqCst);
        let pid = std::process::id();
        let sock_path = PathBuf::from(format!("/tmp/bt_ipc_{pid}_{seq}.sock"));
        Self::start_at(sock_path, fixture, expected_api_key)
    }

    /// Starts a `UnixVerifyServer` bound to the specified `socket_path` (`< 104` bytes),
    /// removing any stale socket file on startup and on `Drop`.
    pub fn start_at(
        socket_path: impl Into<PathBuf>,
        fixture: Arc<FixtureTransport>,
        expected_api_key: impl Into<String>,
    ) -> Result<Self, String> {
        let path: PathBuf = socket_path.into();
        let path_str = path.to_string_lossy();
        if path_str.is_empty() || path_str.len() >= 104 {
            return Err(format!(
                "UnixVerifyServer socket path length {} violates macOS SUN_LEN (< 104 bytes) limit: {path_str}",
                path_str.len()
            ));
        }

        let _ = std::fs::remove_file(&path);
        let listener = UnixListener::bind(&path).map_err(|e| {
            format!(
                "Failed to bind UnixVerifyServer on {}: {e}",
                path.display()
            )
        })?;
        listener
            .set_nonblocking(true)
            .map_err(|e| format!("Failed to set nonblocking UnixListener: {e}"))?;

        let expected_key = expected_api_key.into();
        let shutdown = Arc::new(AtomicBool::new(false));

        let shutdown_clone = shutdown.clone();
        let fixture_clone = fixture.clone();
        let key_clone = expected_key.clone();

        let handle = thread::spawn(move || {
            while !shutdown_clone.load(Ordering::SeqCst) {
                match listener.accept() {
                    Ok((stream, _addr)) => {
                        let _ = stream.set_nonblocking(false);
                        let _ = stream.set_read_timeout(Some(Duration::from_secs(3)));
                        let _ = stream.set_write_timeout(Some(Duration::from_secs(3)));
                        let fx = fixture_clone.clone();
                        let exp_key = key_clone.clone();
                        thread::spawn(move || {
                            let _ = handle_verify_http_stream(
                                stream,
                                fx,
                                &exp_key,
                                "HttpTransport (AF_UNIX)",
                            );
                        });
                    }
                    Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                        thread::sleep(Duration::from_millis(4));
                    }
                    Err(_) => break,
                }
            }
        });

        Ok(Self {
            socket_path: path,
            expected_api_key: expected_key,
            fixture,
            shutdown,
            handle: Some(handle),
        })
    }

    pub fn socket_path(&self) -> &Path {
        &self.socket_path
    }

    pub fn base_url(&self) -> String {
        format!("unix://{}", self.socket_path.display())
    }

    pub fn http_unix_url(&self) -> String {
        format!("http+unix://{}", self.socket_path.display())
    }

    pub fn expected_api_key(&self) -> &str {
        &self.expected_api_key
    }

    pub fn fixture(&self) -> &Arc<FixtureTransport> {
        &self.fixture
    }
}

#[cfg(unix)]
impl Drop for UnixVerifyServer {
    fn drop(&mut self) {
        self.shutdown.store(true, Ordering::SeqCst);
        let _ = UnixStream::connect(&self.socket_path);
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
        let _ = std::fs::remove_file(&self.socket_path);
    }
}


/// In-memory session wired to a fresh [`FixtureTransport`] (never to a real engine).
pub fn fixture_session() -> Result<crate::agent_loop::SpellCoreSession, String> {
    fixture_session_with_handle().map(|(s, _)| s)
}

/// Like [`fixture_session`], also returning the fixture so tests can drive its clock.
pub fn fixture_session_with_handle(
) -> Result<(crate::agent_loop::SpellCoreSession, Arc<FixtureTransport>), String> {
    let fixture = Arc::new(FixtureTransport::new());
    let mut session = crate::agent_loop::SpellCoreSession::new_with_transport(
        Some(":memory:"),
        fixture.clone(),
    )?;
    // Production sessions start with no projects; tests get one project (the locator's primary
    // workspace root) with its first conversation selected.
    let root = session.locator.primary_workspace_root();
    session.add_project("BeatrixLeslie", &root.to_string_lossy(), false)?;
    Ok((session, fixture))
}
