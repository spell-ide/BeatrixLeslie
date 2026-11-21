//! OpenAI-Compatible Wire Contract & Local Session Types (`contract.rs`) for BeatrixLeslie IDE (`spell_core`).
//!
//! Defines standard OpenAI `/chat/completions` & `/models` wire models, SSE streaming
//! delta/usage deserializers resilient to provider-specific JSON `null` values, local timer
//! records, and subagent slot status types.

#[cfg(any(test, feature = "test-support"))]
use std::collections::{HashMap, HashSet};

use serde::{Deserialize, Deserializer, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};

// ============================================================================
// Endpoint-Agnostic Contract Constants
// ============================================================================

/// Default base URL is empty (unconfigured) until the user configures an endpoint in Settings.
pub const DEFAULT_BASE_URL: &str = "";
/// Default model ID is empty until the user selects or enters a model ID in Settings.
pub const DEFAULT_MODEL_ID: &str = "";

pub const ENGINE_COMPACT_TOKEN_THRESHOLD: u64 = 40_000;
pub const ENGINE_INTERCEPTOR_COMPACT_TOKENS: u64 = 36_000;
pub const ENGINE_MIN_DECODE_TPS: f64 = 35.0;
pub const COMPACTION_SUMMARY_MARKER: &str = "[SPELL CONTEXT RESET]";
pub const TIMER_ALARM_MARKER: &str = "[SPELL AGENT TIMER ALARM FIRED]";

// ============================================================================
// Null-Resilient Serde Helpers (OpenAI / OpenRouter / vLLM / Ollama / Gemini)
// ============================================================================

/// Deserializes an explicit JSON `null` (or absent field when paired with `#[serde(default)]`)
/// as `T::default()`, preventing streaming chunk parse failures on providers that emit `null`
/// for optional strings, arrays, or counts.
pub fn deserialize_null_default<'de, D, T>(deserializer: D) -> Result<T, D::Error>
where
    T: Default + Deserialize<'de>,
    D: Deserializer<'de>,
{
    let opt = Option::<T>::deserialize(deserializer)?;
    Ok(opt.unwrap_or_default())
}

/// Deserializes a tool call `type` field where explicit JSON `null` or empty string defaults to `"function"`.
pub fn deserialize_null_tool_type<'de, D>(deserializer: D) -> Result<String, D::Error>
where
    D: Deserializer<'de>,
{
    let opt = Option::<String>::deserialize(deserializer)?;
    Ok(match opt {
        Some(s) if !s.is_empty() => s,
        _ => default_tool_type(),
    })
}

pub fn default_tool_type() -> String {
    "function".to_string()
}

pub fn default_min_tps() -> f64 {
    ENGINE_MIN_DECODE_TPS
}

pub fn default_tok_thresh() -> u64 {
    ENGINE_COMPACT_TOKEN_THRESHOLD
}

pub fn default_preserve_system_prefix() -> bool {
    true
}

// ============================================================================
// Execution Roles & Model Validation
// ============================================================================

/// Execution role distinguishing interactive chat orchestrator turns from subagent tasks.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AgentExecutionRole {
    MainOrchestrator,
    CodingSubagent,
}

impl AgentExecutionRole {
    pub const fn role_name(self) -> &'static str {
        match self {
            AgentExecutionRole::MainOrchestrator => "main",
            AgentExecutionRole::CodingSubagent => "subagent",
        }
    }
}

/// Validates that `model_id` is non-empty for any execution role.
///
/// BeatrixLeslie IDE is endpoint- and model-agnostic: any arbitrary non-empty model ID
/// (e.g. `gpt-4o`, `anthropic/claude-sonnet-4`, `gemini-2.5-pro`, `llama3.2`, `qwen2.5-coder`)
/// is accepted.
pub fn validate_model_for_role(role: AgentExecutionRole, model_id: &str) -> Result<(), String> {
    if model_id.trim().is_empty() {
        return Err(format!(
            "Model ID is required for {} requests. Configure a Model ID in Settings → Engine.",
            role.role_name()
        ));
    }
    Ok(())
}

/// Endpoint-agnostic no-op compatibility helper (any server architecture is accepted).
pub fn validate_routed_experts_for_role(
    _role: AgentExecutionRole,
    _routed_experts: usize,
) -> Result<(), String> {
    Ok(())
}

// ============================================================================
// Model Discovery Descriptor (`GET /models`)
// ============================================================================

/// Descriptor of an available model returned by an OpenAI-compatible `/models` endpoint
/// (or Ollama's `{"models": [...]}` / bare JSON array response).
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct ModelDescriptor {
    pub id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub owned_by: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub context_window: Option<u64>,
}

impl ModelDescriptor {
    fn from_json_item(item: &Value) -> Option<Self> {
        match item {
            Value::String(s) => {
                let id = s.trim();
                if id.is_empty() {
                    None
                } else {
                    Some(Self {
                        id: id.to_string(),
                        owned_by: None,
                        context_window: None,
                    })
                }
            }
            Value::Object(map) => {
                let id = map
                    .get("id")
                    .or_else(|| map.get("name"))
                    .or_else(|| map.get("model"))
                    .and_then(Value::as_str)
                    .map(str::trim)
                    .filter(|s| !s.is_empty())?
                    .to_string();

                let owned_by = map
                    .get("owned_by")
                    .or_else(|| map.get("provider"))
                    .or_else(|| map.get("details").and_then(|d| d.get("family")))
                    .and_then(Value::as_str)
                    .map(str::trim)
                    .filter(|s| !s.is_empty())
                    .map(str::to_string);

                let context_window = map
                    .get("context_window")
                    .or_else(|| map.get("context_length"))
                    .or_else(|| map.get("max_model_len"))
                    .or_else(|| map.get("max_context_length"))
                    .or_else(|| map.get("input_token_limit"))
                    .or_else(|| map.get("top_provider").and_then(|tp| tp.get("context_length")))
                    .and_then(Value::as_u64)
                    .filter(|&n| n > 0);

                Some(Self {
                    id,
                    owned_by,
                    context_window,
                })
            }
            _ => None,
        }
    }

    /// Parses standard OpenAI `{"data": [{"id": ...}]}`, Ollama `{"models": [{"name": ...}]}`,
    /// or bare array `[...]` model listing responses into deduplicated [`ModelDescriptor`] entries.
    pub fn parse_models_response(body: &str) -> Result<Vec<ModelDescriptor>, String> {
        let trimmed = body.trim();
        if trimmed.is_empty() {
            return Err("Empty models response body".to_string());
        }
        let val: Value = serde_json::from_str(trimmed)
            .map_err(|e| format!("Failed to parse models JSON payload: {e}"))?;

        let items: &[Value] = if let Some(arr) = val.as_array() {
            arr.as_slice()
        } else if let Some(obj) = val.as_object() {
            if let Some(arr) = obj.get("data").and_then(Value::as_array) {
                arr.as_slice()
            } else if let Some(arr) = obj.get("models").and_then(Value::as_array) {
                arr.as_slice()
            } else if let Some(err_val) = obj.get("error") {
                return Err(format!("Endpoint returned error on /models: {err_val}"));
            } else {
                return Err(
                    "Unexpected /models response shape: expected 'data', 'models', or JSON array"
                        .to_string(),
                );
            }
        } else {
            return Err("Unexpected /models JSON root type".to_string());
        };

        let mut out = Vec::with_capacity(items.len());
        let mut seen = std::collections::HashSet::with_capacity(items.len());
        for item in items {
            if let Some(desc) = Self::from_json_item(item) {
                if seen.insert(desc.id.clone()) {
                    out.push(desc);
                }
            }
        }
        Ok(out)
    }
}

// ============================================================================
// Standard OpenAI Wire Chat Models
// ============================================================================

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct WireToolFunction {
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub name: String,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub arguments: String,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct WireToolCall {
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub id: String,
    #[serde(
        default = "default_tool_type",
        deserialize_with = "deserialize_null_tool_type"
    )]
    pub r#type: String,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub function: WireToolFunction,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub index: Option<usize>,
}

impl Default for WireToolCall {
    fn default() -> Self {
        Self {
            id: String::new(),
            r#type: default_tool_type(),
            function: WireToolFunction::default(),
            index: None,
        }
    }
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct WireMessage {
    pub role: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub content: Option<String>,
    /// Preserved locally in SQLite for UI display; stripped by [`WireMessage::for_outbound_openai`]
    /// when sending multi-turn history to strict OpenAI-compatible endpoints.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reasoning_content: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tool_calls: Option<Vec<WireToolCall>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tool_call_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
}

impl WireMessage {
    pub fn system(content: impl Into<String>) -> Self {
        Self {
            role: "system".to_string(),
            content: Some(content.into()),
            ..Default::default()
        }
    }

    pub fn user(content: impl Into<String>) -> Self {
        Self {
            role: "user".to_string(),
            content: Some(content.into()),
            ..Default::default()
        }
    }

    pub fn assistant(
        content: Option<String>,
        reasoning_content: Option<String>,
        tool_calls: Option<Vec<WireToolCall>>,
    ) -> Self {
        Self {
            role: "assistant".to_string(),
            content,
            reasoning_content,
            tool_calls,
            tool_call_id: None,
            name: None,
        }
    }

    pub fn tool(
        tool_call_id: impl Into<String>,
        name: Option<String>,
        content: impl Into<String>,
    ) -> Self {
        Self {
            role: "tool".to_string(),
            content: Some(content.into()),
            reasoning_content: None,
            tool_calls: None,
            tool_call_id: Some(tool_call_id.into()),
            name,
        }
    }

    /// Returns a strict OpenAI-compatible outbound copy of this message:
    /// - Clears `reasoning_content = None` (keeping `reasoning_content` intact in SQLite for UI display)
    /// - Clears `index = None` on all `tool_calls` (OpenAI request schema rejects SSE delta `index`)
    pub fn for_outbound_openai(&self) -> WireMessage {
        let sanitized_tool_calls = self.tool_calls.as_ref().map(|calls| {
            calls
                .iter()
                .map(|tc| WireToolCall {
                    id: tc.id.clone(),
                    r#type: if tc.r#type.is_empty() {
                        default_tool_type()
                    } else {
                        tc.r#type.clone()
                    },
                    function: tc.function.clone(),
                    index: None,
                })
                .collect()
        });
        WireMessage {
            role: self.role.clone(),
            content: self.content.clone(),
            reasoning_content: None,
            tool_calls: sanitized_tool_calls,
            tool_call_id: self.tool_call_id.clone(),
            name: self.name.clone(),
        }
    }
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct StreamOptions {
    pub include_usage: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct ChatCompletionRequest {
    pub model: String,
    pub messages: Vec<WireMessage>,
    pub stream: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub stream_options: Option<StreamOptions>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub temperature: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub top_p: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub presence_penalty: Option<f64>,
    /// Retained in-memory only; never serialized onto standard OpenAI wire requests.
    #[serde(default, skip_serializing)]
    pub repetition_penalty: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_tokens: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reasoning_effort: Option<String>,
    /// Retained in-memory only; never serialized onto standard OpenAI wire requests.
    #[serde(default, skip_serializing)]
    pub thinking_budget: Option<u64>,
    /// Retained in-memory only; never serialized onto standard OpenAI wire requests.
    #[serde(default, skip_serializing)]
    pub chat_template_kwargs: Option<serde_json::Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tools: Option<Vec<Value>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tool_choice: Option<Value>,
    /// Retained in-memory only for local session/test tracking; never serialized on wire.
    #[serde(default, skip_serializing)]
    pub prompt_cache_key: Option<String>,
    /// Retained in-memory only for local epoch tracking; never serialized on wire.
    #[serde(default, skip_serializing)]
    pub client_compaction_epoch: Option<u64>,
    /// Retained in-memory only; never serialized on wire.
    #[serde(default, skip_serializing)]
    pub auto_compact_min_tps: Option<f64>,
}

impl Default for ChatCompletionRequest {
    fn default() -> Self {
        Self {
            model: DEFAULT_MODEL_ID.to_string(),
            messages: Vec::new(),
            stream: true,
            stream_options: Some(StreamOptions {
                include_usage: true,
            }),
            temperature: None,
            top_p: None,
            presence_penalty: None,
            repetition_penalty: None,
            max_tokens: None,
            reasoning_effort: None,
            thinking_budget: None,
            chat_template_kwargs: None,
            tools: None,
            tool_choice: None,
            prompt_cache_key: None,
            client_compaction_epoch: Some(0),
            auto_compact_min_tps: None,
        }
    }
}

impl ChatCompletionRequest {
    pub fn new(model: impl Into<String>, messages: Vec<WireMessage>) -> Self {
        Self {
            model: model.into(),
            messages,
            ..Default::default()
        }
    }

    pub fn with_sampling(
        mut self,
        temperature: Option<f64>,
        top_p: Option<f64>,
        presence_penalty: Option<f64>,
        repetition_penalty: Option<f64>,
    ) -> Self {
        self.temperature = temperature;
        self.top_p = top_p;
        self.presence_penalty = presence_penalty;
        self.repetition_penalty = repetition_penalty;
        self
    }

    pub fn with_thinking_budget(
        mut self,
        thinking_budget: Option<u64>,
        chat_template_kwargs: Option<serde_json::Value>,
    ) -> Self {
        self.thinking_budget = thinking_budget;
        self.chat_template_kwargs = chat_template_kwargs;
        self
    }

    /// Returns a cloned request with all `messages` sanitized via [`WireMessage::for_outbound_openai`].
    pub fn sanitized_for_openai(&self) -> Self {
        let mut cloned = self.clone();
        cloned.messages = cloned
            .messages
            .iter()
            .map(WireMessage::for_outbound_openai)
            .collect();
        cloned
    }
}

/// Resolves a canonical local thread cache key, stripping any `:epoch<N>` suffix by default.
pub fn resolve_prompt_cache_key(
    thread_id: &str,
    epoch: u64,
    epoch_cache_key_suffix: bool,
) -> String {
    let base = thread_id
        .split_once(":epoch")
        .map(|(prefix, _)| prefix)
        .unwrap_or(thread_id);
    if epoch_cache_key_suffix && epoch > 0 {
        format!("{base}:epoch{epoch}")
    } else {
        base.to_string()
    }
}

// ============================================================================
// Slot Key Partitioning Helpers
// ============================================================================

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum KvCacheSlot {
    Slot0InteractiveChat,
    Slot1BackgroundTimerEval { seq: u64 },
    Slot2CriticReviewer { seq: u64 },
}

/// Strips any slot prefix (`slot0:`, `slot1:`, `slot2:`) and any `:epoch<N>` / `:tick_<N>` / `:review_<N>`
/// suffix to recover the canonical base thread key.
pub fn derive_slot_base_thread_key(thread_id: &str) -> String {
    let trimmed = thread_id.trim();
    if trimmed.is_empty() {
        return "default".to_string();
    }
    let without_slot = trimmed
        .strip_prefix("slot0:")
        .or_else(|| trimmed.strip_prefix("slot1:"))
        .or_else(|| trimmed.strip_prefix("slot2:"))
        .unwrap_or(trimmed);
    let without_epoch = without_slot
        .split_once(":epoch")
        .map(|(p, _)| p)
        .unwrap_or(without_slot);
    let without_tick = without_epoch
        .split_once(":tick_")
        .map(|(p, _)| p)
        .unwrap_or(without_epoch);
    let without_review = without_tick
        .split_once(":review_")
        .map(|(p, _)| p)
        .unwrap_or(without_tick);
    if without_review.is_empty() {
        "default".to_string()
    } else {
        without_review.to_string()
    }
}

pub fn resolve_slot_prompt_cache_key(
    thread_id: &str,
    compaction_epoch: u64,
    epoch_cache_key_suffix: bool,
    slot: KvCacheSlot,
) -> String {
    let base_thread = derive_slot_base_thread_key(thread_id);
    match slot {
        KvCacheSlot::Slot0InteractiveChat => {
            let base_key = resolve_prompt_cache_key(
                &base_thread,
                compaction_epoch,
                epoch_cache_key_suffix,
            );
            format!("slot0:{base_key}")
        }
        KvCacheSlot::Slot1BackgroundTimerEval { seq } => {
            format!("slot1:{base_thread}:tick_{seq}")
        }
        KvCacheSlot::Slot2CriticReviewer { seq } => {
            format!("slot2:{base_thread}:review_{seq}")
        }
    }
}

/// Computes a deterministic 64-bit content-addressed block hash chained from `parent_hash` and `block_text`.
pub fn compute_block_hash(parent_hash: u64, block_text: &str) -> u64 {
    let mut hasher = Sha256::new();
    hasher.update(parent_hash.to_le_bytes());
    hasher.update(block_text.as_bytes());
    let digest = hasher.finalize();
    let mut bytes = [0u8; 8];
    bytes.copy_from_slice(&digest[..8]);
    u64::from_le_bytes(bytes)
}

#[cfg(any(test, feature = "test-support"))]
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct SlotTurnEvalResult {
    pub prompt_cache_key: String,
    pub prev_tokens: u64,
    pub current_tokens: u64,
    pub false_compaction_triggered: bool,
    pub shared_prefix_blocks_hit: usize,
    pub total_prefix_blocks: usize,
    pub block_hashes: Vec<u64>,
}

#[cfg(any(test, feature = "test-support"))]
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct CompactionDropTracker {
    pub min_tokens_before_drop_check: u64,
    pub token_drop_ratio: f64,
    pub slot_high_water_tokens: HashMap<String, u64>,
    pub slot_compaction_wipes: HashMap<String, u64>,
    pub global_block_hash_table: HashSet<u64>,
}

#[cfg(any(test, feature = "test-support"))]
impl Default for CompactionDropTracker {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(any(test, feature = "test-support"))]
impl CompactionDropTracker {
    pub fn new() -> Self {
        Self {
            min_tokens_before_drop_check: 2048,
            token_drop_ratio: 0.65,
            slot_high_water_tokens: HashMap::new(),
            slot_compaction_wipes: HashMap::new(),
            global_block_hash_table: HashSet::new(),
        }
    }

    pub fn compute_chain_hashes(blocks: &[&str]) -> Vec<u64> {
        let mut parent = 0u64;
        let mut out = Vec::with_capacity(blocks.len());
        for block in blocks {
            let h = compute_block_hash(parent, block);
            out.push(h);
            parent = h;
        }
        out
    }

    pub fn record_turn(
        &mut self,
        prompt_cache_key: &str,
        prompt_tokens: u64,
        prefix_blocks: &[&str],
    ) -> SlotTurnEvalResult {
        let block_hashes = Self::compute_chain_hashes(prefix_blocks);
        let mut shared_prefix_blocks_hit = 0usize;
        for &h in &block_hashes {
            if self.global_block_hash_table.contains(&h) {
                shared_prefix_blocks_hit += 1;
            } else {
                self.global_block_hash_table.insert(h);
            }
        }

        let prev_tokens = self
            .slot_high_water_tokens
            .get(prompt_cache_key)
            .copied()
            .unwrap_or(0);

        let drop_threshold = (prev_tokens as f64) * self.token_drop_ratio;
        let false_compaction_triggered = prev_tokens >= self.min_tokens_before_drop_check
            && (prompt_tokens as f64) < drop_threshold;

        if false_compaction_triggered {
            *self
                .slot_compaction_wipes
                .entry(prompt_cache_key.to_string())
                .or_insert(0) += 1;
            self.slot_high_water_tokens
                .insert(prompt_cache_key.to_string(), prompt_tokens);
        } else {
            self.slot_high_water_tokens.insert(
                prompt_cache_key.to_string(),
                prev_tokens.max(prompt_tokens),
            );
        }

        SlotTurnEvalResult {
            prompt_cache_key: prompt_cache_key.to_string(),
            prev_tokens,
            current_tokens: prompt_tokens,
            false_compaction_triggered,
            shared_prefix_blocks_hit,
            total_prefix_blocks: block_hashes.len(),
            block_hashes,
        }
    }

    pub fn wipes_for_key(&self, prompt_cache_key: &str) -> u64 {
        self.slot_compaction_wipes
            .get(prompt_cache_key)
            .copied()
            .unwrap_or(0)
    }

    pub fn high_water_tokens_for_key(&self, prompt_cache_key: &str) -> u64 {
        self.slot_high_water_tokens
            .get(prompt_cache_key)
            .copied()
            .unwrap_or(0)
    }

    pub fn has_block_hash(&self, block_hash: u64) -> bool {
        self.global_block_hash_table.contains(&block_hash)
    }
}

// ============================================================================
// Context Advice & Streaming Chunk Models
// ============================================================================

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct EngineContextAdvice {
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub action: String,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub reason: String,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub decode_tok_s: f64,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub decode_tok_s_ewma: f64,
    #[serde(default = "default_min_tps")]
    pub threshold_tok_s: f64,
    #[serde(default = "default_min_tps")]
    pub min_tps_threshold: f64,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub prompt_tokens: u64,
    #[serde(default = "default_tok_thresh")]
    pub token_threshold: u64,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub hot_cache_cleared: bool,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub freed_blocks: u64,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub blocks_freed: u64,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub thread_key: String,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub compaction_epoch: u64,
}

impl Default for EngineContextAdvice {
    fn default() -> Self {
        Self {
            action: "ok".to_string(),
            reason: "healthy".to_string(),
            decode_tok_s: 0.0,
            decode_tok_s_ewma: 0.0,
            threshold_tok_s: ENGINE_MIN_DECODE_TPS,
            min_tps_threshold: ENGINE_MIN_DECODE_TPS,
            prompt_tokens: 0,
            token_threshold: ENGINE_COMPACT_TOKEN_THRESHOLD,
            hot_cache_cleared: false,
            freed_blocks: 0,
            blocks_freed: 0,
            thread_key: String::new(),
            compaction_epoch: 0,
        }
    }
}

impl EngineContextAdvice {
    pub fn should_compact(&self) -> bool {
        self.action == "compact_now"
            || self.action == "hot_cache_cleared"
            || self.action == "context_reset"
            || self.hot_cache_cleared
    }

    pub fn effective_freed_blocks(&self) -> u64 {
        self.freed_blocks.max(self.blocks_freed)
    }
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct ChatCompletionDelta {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub role: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub content: Option<String>,
    #[serde(default, alias = "reasoning", skip_serializing_if = "Option::is_none")]
    pub reasoning_content: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tool_calls: Option<Vec<WireToolCall>>,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct ChatCompletionChunkChoice {
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub index: usize,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub delta: ChatCompletionDelta,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub finish_reason: Option<String>,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct PromptTokensDetails {
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub cached_tokens: u64,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct ChatCompletionUsage {
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub prompt_tokens: u64,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub completion_tokens: u64,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub total_tokens: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub prompt_tokens_details: Option<PromptTokensDetails>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub prompt_tokens_per_second: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub generation_tokens_per_second: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub engine_context_advice: Option<EngineContextAdvice>,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct ChatCompletionChunk {
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub id: String,
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub model: String,
    /// Empty `vec![]` on final usage-only SSE chunks.
    #[serde(default, deserialize_with = "deserialize_null_default")]
    pub choices: Vec<ChatCompletionChunkChoice>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub usage: Option<ChatCompletionUsage>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub engine_context_advice: Option<EngineContextAdvice>,
}

impl ChatCompletionChunk {
    pub fn effective_context_advice(&self) -> Option<&EngineContextAdvice> {
        self.engine_context_advice
            .as_ref()
            .or_else(|| self.usage.as_ref().and_then(|u| u.engine_context_advice.as_ref()))
    }
}

// ============================================================================
// Local Cache Reset & Local SQLite Timer Records
// ============================================================================

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct ClearThreadCacheRequest {
    pub prompt_cache_key: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub compaction_epoch: Option<u64>,
    #[serde(default = "default_preserve_system_prefix")]
    pub preserve_system_prefix: bool,
}

impl Default for ClearThreadCacheRequest {
    fn default() -> Self {
        Self {
            prompt_cache_key: String::new(),
            compaction_epoch: None,
            preserve_system_prefix: true,
        }
    }
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct ClearThreadCacheResponse {
    #[serde(default)]
    pub ok: bool,
    #[serde(default)]
    pub thread_key: String,
    #[serde(default)]
    pub hot_cache_cleared: bool,
    #[serde(default)]
    pub freed_blocks: u64,
    #[serde(default = "default_preserve_system_prefix")]
    pub preserve_system_prefix: bool,
    #[serde(default)]
    pub compaction_epoch: u64,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct SetTimerRequest {
    pub duration_s: f64,
    #[serde(default)]
    pub note: String,
    #[serde(default)]
    pub label: String,
    #[serde(default)]
    pub thread_id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub watch_target: Option<Value>,
}

/// Local SQLite-backed timer record for autonomous wakeup alarms.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct TimerRecord {
    #[serde(default, alias = "timer_id")]
    pub id: String,
    #[serde(default)]
    pub thread_id: String,
    #[serde(default)]
    pub label: String,
    #[serde(default)]
    pub note: String,
    #[serde(default)]
    pub duration_s: f64,
    #[serde(default)]
    pub remaining_s: f64,
    #[serde(default)]
    pub status: String,
    #[serde(default)]
    pub delivered: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub watch_target: Option<Value>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(untagged)]
pub enum TimersListEnvelope {
    Timers {
        timers: Vec<TimerRecord>,
        #[serde(default)]
        ok: Option<bool>,
        #[serde(default)]
        count: Option<usize>,
    },
    DueTimers {
        due_timers: Vec<TimerRecord>,
        #[serde(default)]
        ok: Option<bool>,
        #[serde(default)]
        count: Option<usize>,
    },
    SingleWrapped {
        timer: TimerRecord,
        #[serde(default)]
        ok: Option<bool>,
    },
    BareList(Vec<TimerRecord>),
    Single(TimerRecord),
}

impl TimersListEnvelope {
    pub fn into_timers(self) -> Vec<TimerRecord> {
        match self {
            Self::Timers { timers, .. } => timers,
            Self::DueTimers { due_timers, .. } => due_timers,
            Self::SingleWrapped { timer, .. } => vec![timer],
            Self::BareList(list) => list,
            Self::Single(timer) => {
                if timer.id.is_empty()
                    && timer.thread_id.is_empty()
                    && timer.label.is_empty()
                    && timer.note.is_empty()
                    && timer.status.is_empty()
                {
                    Vec::new()
                } else {
                    vec![timer]
                }
            }
        }
    }
}

impl TimerRecord {
    pub fn effective_id(&self) -> &str {
        &self.id
    }

    fn normalize_timer_value_aliases(val: &mut Value) {
        match val {
            Value::Array(items) => {
                for item in items {
                    Self::normalize_timer_value_aliases(item);
                }
            }
            Value::Object(map) => {
                if map.contains_key("id") && map.contains_key("timer_id") {
                    let timer_id_val = map.remove("timer_id");
                    let id_is_empty = map
                        .get("id")
                        .and_then(|v| v.as_str())
                        .map(|s| s.is_empty())
                        .unwrap_or(true);
                    if id_is_empty {
                        if let Some(tid) = timer_id_val {
                            map.insert("id".to_string(), tid);
                        }
                    }
                }
                for child in map.values_mut() {
                    Self::normalize_timer_value_aliases(child);
                }
            }
            _ => {}
        }
    }

    pub fn parse_timers_response(body: &str) -> Result<Vec<TimerRecord>, String> {
        let trimmed = body.trim();
        if trimmed.is_empty() {
            return Err("Empty timers response body".to_string());
        }

        let mut val: Value = serde_json::from_str(trimmed)
            .map_err(|e| format!("Failed to parse timers JSON payload: {e}"))?;

        if let Some(obj) = val.as_object() {
            if obj.contains_key("error")
                && !obj.contains_key("timers")
                && !obj.contains_key("due_timers")
                && !obj.contains_key("timer")
            {
                return Err(format!("Timer error payload: {trimmed}"));
            }
        }

        Self::normalize_timer_value_aliases(&mut val);
        let env: TimersListEnvelope = serde_json::from_value(val)
            .map_err(|e| format!("Unexpected timers envelope `{trimmed}`: {e}"))?;
        Ok(env.into_timers())
    }

    /// Returns `true` if `path_str` is permitted to be read by [`Self::format_engine_alarm_banner`].
    pub fn is_watch_target_path_allowed(path_str: &str) -> bool {
        let trimmed = path_str.trim();
        if trimmed.is_empty() || trimmed.starts_with('~') {
            return false;
        }
        let lower = trimmed.to_ascii_lowercase();
        let forbidden_patterns = [
            "..",
            ".ssh",
            ".aws",
            ".env",
            ".git/config",
            ".git/hooks",
            "api_key",
            "/etc/passwd",
            "/etc/shadow",
            "/private/etc",
            ".gnupg",
            ".kube",
        ];
        for pat in forbidden_patterns {
            if lower.contains(pat) {
                return false;
            }
        }
        true
    }

    /// Formats the deterministic wakeup banner (`[SPELL AGENT TIMER ALARM FIRED]`)
    /// injected when a scheduled timer expires, enforcing capability guardrails on `watch_target`.
    pub fn format_engine_alarm_banner(&self) -> String {
        let label_display = if self.label.is_empty() {
            "unnamed-timer"
        } else {
            self.label.as_str()
        };
        let mut banner = format!(
            "{TIMER_ALARM_MARKER}\n\
             Timer ID: {}\n\
             Thread ID: {}\n\
             Label: {}\n\
             Duration: {:.2}s (status: {})\n\
             Alarm Note: {}",
            self.effective_id(),
            self.thread_id,
            label_display,
            self.duration_s,
            self.status,
            self.note
        );
        if let Some(ref target) = self.watch_target {
            banner.push_str(&format!("\nWatch Target: {}", target));
            if let Some(path_str) = target.as_str().or_else(|| {
                target
                    .get("path")
                    .and_then(|v| v.as_str())
                    .or_else(|| target.get("file").and_then(|v| v.as_str()))
            }) {
                if !Self::is_watch_target_path_allowed(path_str) {
                    banner.push_str(
                        "\n[SECURITY_BLOCK: watch_target path denied by capability guardrail]",
                    );
                } else if let Ok(content) = std::fs::read_to_string(path_str) {
                    let tail: Vec<&str> = content.lines().rev().take(40).collect();
                    let tail_text: Vec<&str> = tail.into_iter().rev().collect();
                    banner.push_str(&format!(
                        "\nWatched Target Tail ({path_str}):\n```\n{}\n```",
                        tail_text.join("\n")
                    ));
                }
            }
        }
        banner
    }
}

/// Structured alarm metadata object.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct AlarmBannerMeta {
    #[serde(default)]
    pub id: String,
    #[serde(default)]
    pub timer_id: String,
    #[serde(default)]
    pub message: String,
    #[serde(default)]
    pub prompt: String,
    #[serde(default)]
    pub note: String,
    #[serde(default)]
    pub label: String,
    #[serde(default)]
    pub thread_id: String,
    #[serde(default)]
    pub status: String,
    #[serde(default)]
    pub delivered: bool,
    #[serde(default)]
    pub fired_at_unix_ms: i64,
    #[serde(default)]
    pub banner_text: String,
}

impl AlarmBannerMeta {
    pub fn effective_id(&self) -> &str {
        if !self.id.is_empty() {
            &self.id
        } else {
            &self.timer_id
        }
    }

    pub fn effective_message(&self) -> &str {
        if !self.message.is_empty() {
            &self.message
        } else if !self.prompt.is_empty() {
            &self.prompt
        } else {
            &self.note
        }
    }

    pub fn into_banner_text(self) -> String {
        if !self.banner_text.is_empty() {
            self.banner_text
        } else {
            format!(
                "{TIMER_ALARM_MARKER}\n\
                 - timer_id: `{}` (label: `{}`)\n\
                 - note: {}\n\
                 Please handle these due timer alarms immediately in your response.",
                self.effective_id(),
                if self.label.is_empty() {
                    "timer"
                } else {
                    &self.label
                },
                self.effective_message()
            )
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(untagged)]
pub enum AlarmHeaderEntry {
    Banner(String),
    Meta(AlarmBannerMeta),
}

impl AlarmHeaderEntry {
    pub fn into_banner_text(self) -> String {
        match self {
            Self::Banner(banner) => banner,
            Self::Meta(meta) => meta.into_banner_text(),
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct CompactionStatusResponse {
    #[serde(default)]
    pub ok: bool,
    #[serde(default)]
    pub hot_cache_only: bool,
    #[serde(default)]
    pub ssd_cold_cache_skipped: bool,
    #[serde(default)]
    pub ssd_writes: u64,
    #[serde(default = "default_min_tps")]
    pub min_decode_tps: f64,
    #[serde(default = "default_tok_thresh")]
    pub token_threshold: u64,
    #[serde(default)]
    pub total_compactions: u64,
    #[serde(default)]
    pub total_hot_resets: u64,
    #[serde(default)]
    pub total_freed_blocks: u64,
    #[serde(default)]
    pub active_threads_count: usize,
}

impl Default for CompactionStatusResponse {
    fn default() -> Self {
        Self {
            ok: true,
            hot_cache_only: true,
            ssd_cold_cache_skipped: true,
            ssd_writes: 0,
            min_decode_tps: ENGINE_MIN_DECODE_TPS,
            token_threshold: ENGINE_COMPACT_TOKEN_THRESHOLD,
            total_compactions: 0,
            total_hot_resets: 0,
            total_freed_blocks: 0,
            active_threads_count: 0,
        }
    }
}

/// Client-side endpoint health and throughput telemetry summary.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct TelemetryResponse {
    #[serde(default)]
    pub engine_online: bool,
    #[serde(default)]
    pub engine_version: String,
    #[serde(default)]
    pub api_schema_version: String,
    #[serde(default)]
    pub decode_tok_s: f64,
    #[serde(default)]
    pub decode_tok_s_ewma: f64,
    #[serde(default)]
    pub compaction_epoch: u64,
    #[serde(default)]
    pub hot_cache_blocks_freed: u64,
    #[serde(default)]
    pub active_timers_count: usize,
    #[serde(default)]
    pub transport_mode: String,
    #[serde(default)]
    pub kv_cache_used_tokens: u64,
    #[serde(default)]
    pub kv_cache_capacity_tokens: u64,
    #[serde(default)]
    pub kv_cache_fill_pct: f64,
}

impl TelemetryResponse {
    pub fn from_raw_json(raw: &str) -> Result<Self, serde_json::Error> {
        serde_json::from_str(raw)
    }
}

// ============================================================================
// Subagent Completion & Local Slot Status Models
// ============================================================================

/// Standard OpenAI-compatible subagent completion request (`POST /chat/completions`).
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct SubagentCompletionRequest {
    pub model: String,
    pub messages: Vec<WireMessage>,
    #[serde(default)]
    pub stream: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_tokens: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub temperature: Option<f32>,
    /// Retained in-memory only for local slot tracking; never serialized on wire.
    #[serde(default, skip_serializing)]
    pub prompt_cache_key: Option<String>,
    /// Retained in-memory only for local slot tracking; never serialized on wire.
    #[serde(default, skip_serializing)]
    pub parent_cache_key: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reasoning_effort: Option<String>,
    /// Retained in-memory only; never serialized on wire.
    #[serde(default, skip_serializing)]
    pub thinking_budget: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tools: Option<Vec<serde_json::Value>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tool_choice: Option<serde_json::Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub stream_options: Option<StreamOptions>,
    /// Retained in-memory only; never serialized on wire.
    #[serde(default, skip_serializing)]
    pub enable_thinking: Option<bool>,
}

impl Default for SubagentCompletionRequest {
    fn default() -> Self {
        Self {
            model: DEFAULT_MODEL_ID.to_string(),
            messages: Vec::new(),
            stream: false,
            max_tokens: Some(1024),
            temperature: Some(0.1),
            prompt_cache_key: None,
            parent_cache_key: None,
            reasoning_effort: None,
            thinking_budget: None,
            tools: None,
            tool_choice: None,
            stream_options: Some(StreamOptions {
                include_usage: true,
            }),
            enable_thinking: None,
        }
    }
}

impl SubagentCompletionRequest {
    pub fn new(messages: Vec<WireMessage>, prompt_cache_key: impl Into<String>) -> Self {
        Self {
            messages,
            prompt_cache_key: Some(prompt_cache_key.into()),
            ..Self::default()
        }
    }

    pub fn with_model(
        model: impl Into<String>,
        messages: Vec<WireMessage>,
        prompt_cache_key: impl Into<String>,
    ) -> Self {
        Self {
            model: model.into(),
            messages,
            prompt_cache_key: Some(prompt_cache_key.into()),
            ..Self::default()
        }
    }
}

/// Subagent completion response returned by [`crate::transport::EngineTransport::run_subagent_completion`].
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct SubagentCompletionResponse {
    pub response_id: String,
    pub model: String,
    pub content: String,
    #[serde(default)]
    pub reasoning_content: String,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub tool_calls: Vec<WireToolCall>,
    #[serde(default)]
    pub finish_reason: Option<String>,
    #[serde(default)]
    pub prompt_tokens: u32,
    #[serde(default)]
    pub completion_tokens: u32,
    #[serde(default)]
    pub cached_tokens: u32,
    #[serde(default)]
    pub decode_tok_s: f64,
    #[serde(default)]
    pub routed_experts: Option<usize>,
    #[serde(default)]
    pub prefix_cache_reused: bool,
}

/// Status of an individual local subagent slot (`slot 1..=4`).
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct SubagentSlotStatus {
    #[serde(default, alias = "slot_id", alias = "slotId")]
    pub slot: usize,
    #[serde(default, alias = "role", alias = "slotKey")]
    pub slot_key: String,
    #[serde(default, alias = "state")]
    pub status: String,
    #[serde(default)]
    pub model_id: String,
    #[serde(default)]
    pub mode: String,
    #[serde(default, alias = "decodeTokS")]
    pub decode_tok_s: f64,
    #[serde(default, alias = "tokens_generated", alias = "last_generated_tokens")]
    pub line_count: usize,
    #[serde(default)]
    pub overlay_namespace: String,
    #[serde(default)]
    pub staged_file_count: usize,
    #[serde(default)]
    pub forked_from_slot: Option<usize>,
    #[serde(default)]
    pub inference_backed: bool,
}

/// Local subagent pool status snapshot surfaced to the UI.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct SubagentPoolStatusResponse {
    #[serde(default)]
    pub ok: bool,
    #[serde(default)]
    pub inference_backed: bool,
    #[serde(default)]
    pub base_model_id: String,
    #[serde(default, alias = "subagent_model_id", alias = "subagentModelId")]
    pub coding_subagent_model_id: String,
    #[serde(default)]
    pub subagent_decode_tok_s: f64,
    #[serde(default)]
    pub last_subagent_decode_tok_s: f64,
    #[serde(default)]
    pub active_running_subagents: usize,
    #[serde(default)]
    pub max_concurrent_subagents: usize,
    #[serde(default)]
    pub subagents: Vec<SubagentSlotStatus>,
}

impl SubagentPoolStatusResponse {
    pub fn effective_subagent_model_id(&self) -> &str {
        let trimmed = self.coding_subagent_model_id.trim();
        if trimmed.is_empty() {
            self.base_model_id.trim()
        } else {
            trimmed
        }
    }

    pub fn effective_decode_tok_s(&self) -> f64 {
        let slot_max = self
            .subagents
            .iter()
            .map(|s| s.decode_tok_s)
            .fold(0.0_f64, f64::max);
        self.subagent_decode_tok_s
            .max(self.last_subagent_decode_tok_s)
            .max(slot_max)
    }
}

impl Default for SubagentPoolStatusResponse {
    fn default() -> Self {
        let subagents = (1..=4)
            .map(|slot| SubagentSlotStatus {
                slot,
                slot_key: format!("slot{slot}:subagent_{slot}"),
                status: "idle".to_string(),
                model_id: String::new(),
                mode: "coding".to_string(),
                decode_tok_s: 0.0,
                line_count: 0,
                overlay_namespace: format!("slot{slot}_overlay"),
                staged_file_count: 0,
                forked_from_slot: None,
                inference_backed: false,
            })
            .collect();
        Self {
            ok: true,
            inference_backed: false,
            base_model_id: String::new(),
            coding_subagent_model_id: String::new(),
            subagent_decode_tok_s: 0.0,
            last_subagent_decode_tok_s: 0.0,
            active_running_subagents: 0,
            max_concurrent_subagents: 4,
            subagents,
        }
    }
}

// ============================================================================
// Active Subagents Indicator Types
// ============================================================================

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ActiveSubagentRole {
    Prefill,
    Coding,
    Critic,
    TimerTriage,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ActiveSubagentIndicator {
    pub id: String,
    pub role: ActiveSubagentRole,
    pub model: String,
    pub slot_label: String,
    pub label: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub step: Option<usize>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_steps: Option<usize>,
    pub started_at_unix_ms: i64,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_defaults_are_unconfigured() {
        assert_eq!(DEFAULT_BASE_URL, "");
        assert_eq!(DEFAULT_MODEL_ID, "");
        assert_eq!(COMPACTION_SUMMARY_MARKER, "[SPELL CONTEXT RESET]");
        assert_eq!(TIMER_ALARM_MARKER, "[SPELL AGENT TIMER ALARM FIRED]");
        let req = ChatCompletionRequest::default();
        assert_eq!(req.model, "");
        assert_eq!(req.reasoning_effort, None);
        assert_eq!(
            req.stream_options,
            Some(StreamOptions {
                include_usage: true
            })
        );
    }

    #[test]
    fn test_validate_model_accepts_any_non_empty_model_id() {
        for model in [
            "gpt-4o",
            "anthropic/claude-sonnet-4",
            "gemini-2.5-pro",
            "llama3.2:latest",
            "qwen2.5-coder-32b",
        ] {
            assert!(validate_model_for_role(AgentExecutionRole::MainOrchestrator, model).is_ok());
            assert!(validate_model_for_role(AgentExecutionRole::CodingSubagent, model).is_ok());
        }
        assert!(validate_model_for_role(AgentExecutionRole::MainOrchestrator, "").is_err());
        assert!(validate_model_for_role(AgentExecutionRole::CodingSubagent, "   ").is_err());
        assert!(validate_routed_experts_for_role(AgentExecutionRole::MainOrchestrator, 0).is_ok());
        assert!(validate_routed_experts_for_role(AgentExecutionRole::CodingSubagent, 999).is_ok());
    }

    #[test]
    fn test_wire_message_outbound_openai_sanitization_preserves_local_reasoning() {
        let msg = WireMessage::assistant(
            Some("Final answer".to_string()),
            Some("Step-by-step chain of thought".to_string()),
            Some(vec![WireToolCall {
                id: "call_1".to_string(),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "read_file".to_string(),
                    arguments: "{\"path\":\"src/lib.rs\"}".to_string(),
                },
                index: Some(0),
            }]),
        );
        // Local SQLite serialization preserves reasoning_content
        let local_serialized = serde_json::to_string(&msg).unwrap();
        assert!(local_serialized.contains("\"reasoning_content\":\"Step-by-step chain of thought\""));
        let round_trip: WireMessage = serde_json::from_str(&local_serialized).unwrap();
        assert_eq!(round_trip, msg);

        // Outbound OpenAI sanitization strips reasoning_content and tool_call.index
        let outbound = msg.for_outbound_openai();
        assert_eq!(outbound.reasoning_content, None);
        assert_eq!(outbound.tool_calls.as_ref().unwrap()[0].index, None);
        let outbound_json = serde_json::to_value(&outbound).unwrap();
        assert!(outbound_json.get("reasoning_content").is_none());
        assert!(outbound_json["tool_calls"][0].get("index").is_none());
    }

    #[test]
    fn test_chat_request_strips_proprietary_fields_on_serialization() {
        let req = ChatCompletionRequest {
            model: "gpt-4o".to_string(),
            messages: vec![WireMessage::user("hi")],
            stream: true,
            repetition_penalty: Some(1.1),
            thinking_budget: Some(512),
            chat_template_kwargs: Some(serde_json::json!({ "enable_thinking": true })),
            prompt_cache_key: Some("thread-1".to_string()),
            client_compaction_epoch: Some(2),
            auto_compact_min_tps: Some(35.0),
            ..Default::default()
        };
        let v = serde_json::to_value(&req).unwrap();
        let obj = v.as_object().unwrap();
        for forbidden in [
            "repetition_penalty",
            "thinking_budget",
            "chat_template_kwargs",
            "prompt_cache_key",
            "client_compaction_epoch",
            "auto_compact_min_tps",
            "reasoning_effort",
        ] {
            assert!(
                !obj.contains_key(forbidden),
                "proprietary or unset key '{forbidden}' must not serialize: {v}"
            );
        }
        assert_eq!(obj["model"], "gpt-4o");
        assert_eq!(obj["stream"], true);
        assert_eq!(obj["stream_options"], serde_json::json!({ "include_usage": true }));
    }

    #[test]
    fn test_sse_chunk_null_fields_and_reasoning_alias() {
        let raw_chunk = r#"{
            "id": null,
            "model": null,
            "choices": [{
                "index": null,
                "delta": {
                    "role": "assistant",
                    "content": null,
                    "reasoning": "Thinking via OpenRouter alias",
                    "tool_calls": [{
                        "index": 0,
                        "id": null,
                        "type": null,
                        "function": { "name": null, "arguments": "{\"path\":\"Cargo.toml\"}" }
                    }]
                },
                "finish_reason": null
            }],
            "usage": {
                "prompt_tokens": null,
                "completion_tokens": 12,
                "total_tokens": null,
                "prompt_tokens_details": { "cached_tokens": null }
            }
        }"#;
        let chunk: ChatCompletionChunk = serde_json::from_str(raw_chunk).unwrap();
        assert_eq!(chunk.id, "");
        assert_eq!(chunk.model, "");
        assert_eq!(chunk.choices.len(), 1);
        assert_eq!(
            chunk.choices[0].delta.reasoning_content.as_deref(),
            Some("Thinking via OpenRouter alias")
        );
        let tc = &chunk.choices[0].delta.tool_calls.as_ref().unwrap()[0];
        assert_eq!(tc.id, "");
        assert_eq!(tc.r#type, "function");
        assert_eq!(tc.function.name, "");
        assert_eq!(tc.function.arguments, "{\"path\":\"Cargo.toml\"}");
        let u = chunk.usage.unwrap();
        assert_eq!(u.prompt_tokens, 0);
        assert_eq!(u.completion_tokens, 12);
        assert_eq!(u.total_tokens, 0);
        assert_eq!(u.prompt_tokens_details.unwrap().cached_tokens, 0);
    }

    #[test]
    fn test_model_descriptor_parse_openai_ollama_and_bare_array() {
        // 1. OpenAI format
        let openai = r#"{"object":"list","data":[{"id":"gpt-4o","owned_by":"openai","context_length":128000},{"id":"gpt-4o-mini","owned_by":"openai"}]}"#;
        let m1 = ModelDescriptor::parse_models_response(openai).unwrap();
        assert_eq!(m1.len(), 2);
        assert_eq!(m1[0].id, "gpt-4o");
        assert_eq!(m1[0].owned_by.as_deref(), Some("openai"));
        assert_eq!(m1[0].context_window, Some(128_000));

        // 2. Ollama format
        let ollama = r#"{"models":[{"name":"llama3.2:latest","details":{"family":"llama"}},{"name":"qwen2.5-coder:32b"}]}"#;
        let m2 = ModelDescriptor::parse_models_response(ollama).unwrap();
        assert_eq!(m2.len(), 2);
        assert_eq!(m2[0].id, "llama3.2:latest");
        assert_eq!(m2[0].owned_by.as_deref(), Some("llama"));
        assert_eq!(m2[1].id, "qwen2.5-coder:32b");

        // 3. Bare array of objects and strings
        let bare = r#"[{"id":"claude-sonnet-4","context_window":200000},"mistral-large"]"#;
        let m3 = ModelDescriptor::parse_models_response(bare).unwrap();
        assert_eq!(m3.len(), 2);
        assert_eq!(m3[0].id, "claude-sonnet-4");
        assert_eq!(m3[0].context_window, Some(200_000));
        assert_eq!(m3[1].id, "mistral-large");
    }

    #[test]
    fn test_timer_record_envelopes_and_watch_target_guardrails() {
        let list_json = r#"{"ok": true, "timers": [{"timer_id": "tmr_1", "thread_id": "t1", "duration_s": 15.0, "remaining_s": 10.0, "note": "check", "label": "audit", "status": "armed"}], "count": 1}"#;
        let parsed = TimerRecord::parse_timers_response(list_json).unwrap();
        assert_eq!(parsed.len(), 1);
        assert_eq!(parsed[0].id, "tmr_1");

        for forbidden_path in [
            "../../etc/passwd",
            "/etc/shadow",
            "~/.ssh/id_rsa",
            "/Users/admin/.aws/credentials",
            ".env",
            ".git/config",
            ".git/hooks/pre-commit",
            "/tmp/my_api_key.txt",
        ] {
            let rec = TimerRecord {
                id: "tmr-sec".to_string(),
                thread_id: "t-sec".to_string(),
                label: "sec-check".to_string(),
                note: "audit".to_string(),
                duration_s: 1.0,
                remaining_s: 0.0,
                status: "fired".to_string(),
                delivered: true,
                watch_target: Some(serde_json::json!({ "path": forbidden_path })),
            };
            let banner = rec.format_engine_alarm_banner();
            assert!(banner.starts_with(TIMER_ALARM_MARKER));
            assert!(
                banner.contains("[SECURITY_BLOCK: watch_target path denied by capability guardrail]"),
                "Expected SECURITY_BLOCK for path {forbidden_path}, got: {banner}"
            );
        }
    }

    #[test]
    fn test_resolve_prompt_cache_key_stability_and_legacy_compat() {
        assert_eq!(resolve_prompt_cache_key("thread-42", 0, false), "thread-42");
        assert_eq!(resolve_prompt_cache_key("thread-42", 3, false), "thread-42");
        assert_eq!(resolve_prompt_cache_key("thread-42", 0, true), "thread-42");
        assert_eq!(
            resolve_prompt_cache_key("thread-42", 3, true),
            "thread-42:epoch3"
        );
        assert_eq!(
            resolve_prompt_cache_key("thread-42:epoch1", 2, false),
            "thread-42"
        );
    }

    #[test]
    fn test_active_subagent_indicator_serde_roundtrip() {
        let ind = ActiveSubagentIndicator {
            id: "sub-coding-2".to_string(),
            role: ActiveSubagentRole::Coding,
            model: "gpt-4o-mini".to_string(),
            slot_label: "subagent1".to_string(),
            label: "Implement parser helper".to_string(),
            step: Some(3),
            max_steps: Some(12),
            started_at_unix_ms: 1_763_600_005_456,
        };
        let encoded = serde_json::to_string(&ind).unwrap();
        let decoded: ActiveSubagentIndicator = serde_json::from_str(&encoded).unwrap();
        assert_eq!(decoded, ind);
    }
}
