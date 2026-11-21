//! OpenAI-Compatible Endpoint Transport Layer (`transport.rs`) for BeatrixLeslie IDE (`spell_core`).
//!
//! Provides:
//! 1. Endpoint normalization (`normalize_and_parse_endpoint`, `resolve_openai_endpoint_url`)
//!    supporting arbitrary `http://` and `https://` OpenAI-compatible APIs (cloud, LAN, localhost,
//!    custom ports/paths) as well as optional `unix://` domain sockets for local Unix tests.
//! 2. [`EngineTransport`] trait and [`ChatTurnSummary`] turn aggregation structure.
//! 3. [`HttpTransport`]: OS-portable pure-Rust HTTP/1.1 + HTTP/2 + TLS 1.2+ + SSE streaming +
//!    `GET /models` client backed by `reqwest::blocking` (`rustls-tls-native-roots`), with pure
//!    in-memory headers/payloads, `< 50 ms` cross-platform cancellation via a reader thread +
//!    bounded channel, and a `#[cfg(unix)]` `UnixStream` HTTP/1.1 helper for `unix://` tests.

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Read};
use std::net::IpAddr;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc};
use std::time::{Duration, Instant};

use once_cell::sync::OnceCell;
use parking_lot::RwLock;
use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::contract::{
    validate_model_for_role, AgentExecutionRole, ChatCompletionChunk, ChatCompletionChunkChoice,
    ChatCompletionDelta, ChatCompletionRequest, ChatCompletionUsage, ClearThreadCacheRequest,
    CompactionStatusResponse, EngineContextAdvice, ModelDescriptor,
    SetTimerRequest, SubagentCompletionRequest, SubagentCompletionResponse,
    SubagentPoolStatusResponse, TelemetryResponse, TimerRecord, WireMessage, WireToolCall,
    DEFAULT_BASE_URL,
};
use crate::locator::EnginePathLocator;

/// Canonical transport error type for [`EngineTransport`] operations.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum TransportError {
    /// Missing or invalid model identifier for the requested role.
    ModelRoleViolation(String),
    /// Non-2xx HTTP status returned by the endpoint.
    HttpStatus { status: u16, body: String },
    /// Socket/connect/DNS/TLS/timeout error.
    Connection(String),
    /// Invalid or unsupported endpoint URL/scheme on the current platform.
    InvalidUrl(String),
    /// Generic serialization, framing, or protocol error.
    Message(String),
    /// Active turn cancellation requested via `crate::is_turn_cancel_requested()`.
    Cancelled,
}

impl TransportError {
    pub fn is_model_role_violation(&self) -> bool {
        matches!(self, TransportError::ModelRoleViolation(_))
    }

    pub fn is_cancelled(&self) -> bool {
        matches!(self, TransportError::Cancelled)
    }

    pub fn is_subagent_unavailable(&self) -> bool {
        match self {
            TransportError::ModelRoleViolation(_)
            | TransportError::InvalidUrl(_)
            | TransportError::Cancelled => false,
            TransportError::HttpStatus { status, .. } => *status == 503,
            TransportError::Connection(_) => true,
            TransportError::Message(msg) => {
                msg.contains("Failed to connect") || msg.contains("Could not resolve host")
            }
        }
    }

    pub fn contains(&self, pat: &str) -> bool {
        self.to_string().contains(pat)
    }
}

impl std::fmt::Display for TransportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TransportError::ModelRoleViolation(msg) => write!(f, "{msg}"),
            TransportError::HttpStatus { status, body } => {
                write!(f, "HTTP {status} error: {body}")
            }
            TransportError::Connection(msg)
            | TransportError::InvalidUrl(msg)
            | TransportError::Message(msg) => write!(f, "{msg}"),
            TransportError::Cancelled => write!(f, "Request cancelled"),
        }
    }
}

impl std::error::Error for TransportError {}

impl From<String> for TransportError {
    fn from(s: String) -> Self {
        if s.contains("MODEL_ROLE_VIOLATION") || s.starts_with("ModelRoleViolation:") {
            TransportError::ModelRoleViolation(s)
        } else if s == "Request cancelled" {
            TransportError::Cancelled
        } else if s.contains("Failed to connect") || s.contains("Could not resolve host") {
            TransportError::Connection(s)
        } else {
            TransportError::Message(s)
        }
    }
}

impl From<&str> for TransportError {
    fn from(s: &str) -> Self {
        TransportError::from(s.to_string())
    }
}

impl From<TransportError> for String {
    fn from(err: TransportError) -> Self {
        err.to_string()
    }
}

// ============================================================================
// 1. `ChatTurnSummary` & `EngineTransport` Trait
// ============================================================================

/// Aggregated summary of a completed `/chat/completions` turn.
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct ChatTurnSummary {
    pub response_id: String,
    pub model: String,
    pub reasoning_content: String,
    pub content: String,
    pub tool_calls: Vec<WireToolCall>,
    pub finish_reason: String,
    pub usage: ChatCompletionUsage,
    pub context_advice: EngineContextAdvice,
    pub engine_injected_alarms: Vec<String>,
    pub raw_sse_frames: Vec<String>,
}

/// Abstract transport interface for communicating with any OpenAI-compatible inference endpoint.
pub trait EngineTransport: Send + Sync {
    fn transport_name(&self) -> &str;

    fn list_models(&self) -> Result<Vec<ModelDescriptor>, TransportError> {
        Ok(Vec::new())
    }

    fn stream_chat_completion(
        &self,
        req: &mut ChatCompletionRequest,
        on_chunk: &mut dyn FnMut(&ChatCompletionChunk),
    ) -> Result<ChatTurnSummary, String>;

    fn stream_chat_completion_strict(
        &self,
        req: &mut ChatCompletionRequest,
        on_chunk: &mut dyn FnMut(&ChatCompletionChunk),
    ) -> Result<ChatTurnSummary, TransportError> {
        validate_model_for_role(AgentExecutionRole::MainOrchestrator, &req.model)
            .map_err(TransportError::ModelRoleViolation)?;
        self.stream_chat_completion(req, on_chunk)
            .map_err(TransportError::from)
    }

    fn run_subagent_completion(
        &self,
        req: &SubagentCompletionRequest,
    ) -> Result<SubagentCompletionResponse, TransportError> {
        validate_model_for_role(AgentExecutionRole::CodingSubagent, &req.model)
            .map_err(TransportError::ModelRoleViolation)?;
        Err(TransportError::HttpStatus {
            status: 503,
            body: "Transport does not implement subagent completion".to_string(),
        })
    }

    fn clear_thread_cache(&self, req: &ClearThreadCacheRequest) -> Result<EngineContextAdvice, String> {
        Ok(EngineContextAdvice {
            action: "context_reset".to_string(),
            reason: "local_context_reset".to_string(),
            hot_cache_cleared: true,
            thread_key: derive_thread_key(Some(&req.prompt_cache_key)),
            compaction_epoch: req.compaction_epoch.unwrap_or(1),
            ..Default::default()
        })
    }

    fn get_compaction_status(
        &self,
        _prompt_cache_key: Option<&str>,
    ) -> Result<CompactionStatusResponse, String> {
        Ok(CompactionStatusResponse::default())
    }

    fn set_timer(&self, _req: &SetTimerRequest) -> Result<TimerRecord, String> {
        Err("Timers are managed locally in SQLite storage".to_string())
    }

    fn list_timers(
        &self,
        _thread_id: Option<&str>,
        _include_delivered: bool,
    ) -> Result<Vec<TimerRecord>, String> {
        Ok(Vec::new())
    }

    fn cancel_timer(&self, _timer_id: &str) -> Result<bool, String> {
        Ok(false)
    }

    fn pop_due_timers(&self, _thread_id: Option<&str>) -> Result<Vec<TimerRecord>, String> {
        Ok(Vec::new())
    }

    fn get_telemetry(&self) -> Result<TelemetryResponse, String> {
        Ok(TelemetryResponse::default())
    }

    fn fetch_subagents_status(&self) -> Result<SubagentPoolStatusResponse, TransportError> {
        Ok(SubagentPoolStatusResponse::default())
    }

    fn get_subagent_pool_status(&self) -> Result<SubagentPoolStatusResponse, TransportError> {
        self.fetch_subagents_status()
    }
}

/// Derives the canonical thread key from an optional cache key, stripping any `:epoch<N>` suffix.
pub fn derive_thread_key(prompt_cache_key: Option<&str>) -> String {
    let raw = prompt_cache_key.unwrap_or("default").trim();
    if raw.is_empty() {
        return "default".to_string();
    }
    raw.split_once(":epoch")
        .map(|(prefix, _)| prefix.to_string())
        .unwrap_or_else(|| raw.to_string())
}

/// Appends due timer alarm banners to the last `"user"` or `"tool"` message without duplicating
/// already-present banners and without modifying `"system"` or `"assistant"` messages.
pub fn apply_engine_alarm_banners_to_messages(messages: &mut [WireMessage], banners: &[String]) {
    if banners.is_empty() {
        return;
    }
    let Some(target_msg) = messages.iter_mut().rev().find(|m| {
        let role = m.role.to_ascii_lowercase();
        role == "user" || role == "tool"
    }) else {
        return;
    };
    for banner in banners {
        if banner.is_empty() {
            continue;
        }
        match target_msg.content {
            Some(ref mut existing) if !existing.is_empty() => {
                if !existing.contains(banner.as_str()) {
                    existing.push_str("\n\n");
                    existing.push_str(banner);
                }
            }
            _ => {
                target_msg.content = Some(banner.clone());
            }
        }
    }
}

/// Returns the built-in OpenAI function tool schemas including `open_message_terminal`.
pub fn builtin_tool_schemas() -> Vec<Value> {
    let mut schemas = crate::tools::build_openai_tools_schema(crate::tools::AgentProfile::Write);
    schemas.push(serde_json::json!({
        "type": "function",
        "function": {
            "name": "open_message_terminal",
            "description": "Opens the interactive Message Terminal CLI in the specified workspace directory.",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": { "type": "string" },
                    "reason": { "type": "string" }
                }
            }
        }
    }));
    schemas
}

// ============================================================================
// 2. Endpoint Normalization & OpenAI Resource URL Resolution
// ============================================================================

/// Validated, normalized OpenAI-compatible endpoint target.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum NormalizedEndpoint {
    Http {
        scheme: String,
        host: String,
        port: Option<u16>,
        base_url: String,
        has_path: bool,
        is_loopback: bool,
    },
    Unix {
        socket_path: PathBuf,
        base_url: String,
    },
}

impl NormalizedEndpoint {
    pub fn normalized_base_url(&self) -> &str {
        match self {
            Self::Http { base_url, .. } => base_url,
            Self::Unix { base_url, .. } => base_url,
        }
    }
}

/// Validates and normalizes an endpoint base URL:
/// - Accepts any valid `http://` or `https://` URL (remote cloud, LAN IP, localhost, custom ports,
///   custom paths) and `unix://` domain sockets.
/// - Rejects CR/LF/control chars, internal whitespace, and non-`http(s)`/`unix` schemes.
/// - Trims whitespace and trailing `/`, and strips accidental trailing `/chat/completions` or `/models`.
pub fn normalize_and_parse_endpoint(base_url: &str) -> Result<NormalizedEndpoint, String> {
    let trimmed = base_url.trim();
    if trimmed.is_empty() {
        return Err("Endpoint URL cannot be empty".to_string());
    }
    if trimmed.chars().any(|c| c.is_control() || c.is_whitespace()) {
        return Err(
            "Endpoint URL contains forbidden whitespace or control characters".to_string(),
        );
    }

    // Optional `unix://` socket target for local tests
    if let Some(raw_sock) = trimmed
        .strip_prefix("unix://")
        .or_else(|| trimmed.strip_prefix("http+unix://"))
    {
        let decoded = raw_sock.replace("%2F", "/").replace("%2f", "/");
        let clean_path = if let Some((prefix, _rest)) = decoded.split_once(".sock/") {
            format!("{prefix}.sock")
        } else {
            decoded
        };
        let normalized = if clean_path.starts_with('/') {
            clean_path
        } else {
            format!("/{clean_path}")
        };
        if normalized.len() <= 1 || normalized.len() >= 104 {
            return Err(format!(
                "Invalid unix socket path length {} (must be 2..104 bytes): {normalized}",
                normalized.len()
            ));
        }
        return Ok(NormalizedEndpoint::Unix {
            socket_path: PathBuf::from(&normalized),
            base_url: format!("unix://{normalized}"),
        });
    }

    let (scheme, rest) = if trimmed.len() >= 8 && trimmed[..8].eq_ignore_ascii_case("https://") {
        ("https", &trimmed[8..])
    } else if trimmed.len() >= 7 && trimmed[..7].eq_ignore_ascii_case("http://") {
        ("http", &trimmed[7..])
    } else {
        return Err(format!(
            "Unsupported endpoint URL scheme in '{trimmed}': expected http:// or https://"
        ));
    };

    if rest.is_empty() {
        return Err(format!("Endpoint URL '{trimmed}' is missing a host"));
    }
    if rest.contains('?') || rest.contains('#') {
        return Err(format!(
            "Endpoint base URL '{trimmed}' must not contain query parameters or fragments"
        ));
    }

    let (authority, raw_path) = match rest.find('/') {
        Some(idx) => (&rest[..idx], &rest[idx..]),
        None => (rest, ""),
    };
    if authority.is_empty() {
        return Err(format!("Endpoint URL '{trimmed}' is missing host authority"));
    }
    if authority.contains('@') {
        return Err(format!(
            "Endpoint URL '{trimmed}' must not embed userinfo credentials in authority"
        ));
    }

    let (host, port) = if authority.starts_with('[') {
        let Some(end_bracket) = authority.find(']') else {
            return Err(format!("Malformed IPv6 authority in '{trimmed}'"));
        };
        let ipv6_host = &authority[1..end_bracket];
        if ipv6_host.is_empty() {
            return Err(format!("Empty IPv6 host in '{trimmed}'"));
        }
        let after = &authority[end_bracket + 1..];
        let parsed_port = if let Some(port_str) = after.strip_prefix(':') {
            let p = port_str
                .parse::<u16>()
                .map_err(|e| format!("Invalid port '{port_str}' in '{trimmed}': {e}"))?;
            if p == 0 {
                return Err(format!("Invalid port 0 in '{trimmed}'"));
            }
            Some(p)
        } else if after.is_empty() {
            None
        } else {
            return Err(format!("Unexpected authority suffix '{after}' in '{trimmed}'"));
        };
        (ipv6_host.to_ascii_lowercase(), parsed_port)
    } else if let Some((h, port_str)) = authority.rsplit_once(':') {
        if h.is_empty() || h.contains(':') {
            return Err(format!("Malformed host:port authority '{authority}' in '{trimmed}'"));
        }
        let p = port_str
            .parse::<u16>()
            .map_err(|e| format!("Invalid port '{port_str}' in '{trimmed}': {e}"))?;
        if p == 0 {
            return Err(format!("Invalid port 0 in '{trimmed}'"));
        }
        (h.to_ascii_lowercase(), Some(p))
    } else {
        (authority.to_ascii_lowercase(), None)
    };

    if host.is_empty()
        || !host
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b':'))
    {
        return Err(format!("Invalid host '{host}' in endpoint URL '{trimmed}'"));
    }

    // Trim trailing slashes and strip accidental `/chat/completions` or `/models` suffixes
    let mut path = raw_path.trim_end_matches('/').to_string();
    loop {
        let lower = path.to_ascii_lowercase();
        if lower.ends_with("/chat/completions") {
            path.truncate(path.len() - "/chat/completions".len());
            path = path.trim_end_matches('/').to_string();
        } else if lower.ends_with("/models") {
            path.truncate(path.len() - "/models".len());
            path = path.trim_end_matches('/').to_string();
        } else {
            break;
        }
    }

    let is_loopback = host.eq_ignore_ascii_case("localhost")
        || host == "127.0.0.1"
        || host == "::1"
        || host
            .parse::<IpAddr>()
            .map(|ip| ip.is_loopback())
            .unwrap_or(false);

    let normalized_base = format!("{scheme}://{authority}{path}");
    let has_path = !path.is_empty();

    Ok(NormalizedEndpoint::Http {
        scheme: scheme.to_string(),
        host,
        port,
        base_url: normalized_base,
        has_path,
        is_loopback,
    })
}

/// Resolves an OpenAI API resource (`"chat/completions"`, `"models"`, etc.) against `base_url`:
/// - If `base_url` has NO path component (e.g. `https://api.openai.com`, `http://localhost:11434`,
///   `http://127.0.0.1:8000`) -> `{base}/v1/{resource}`
/// - If `base_url` ALREADY has a path component (e.g. `https://api.openai.com/v1`,
///   `https://openrouter.ai/api/v1`, `https://generativelanguage.googleapis.com/v1beta/openai`,
///   `http://localhost:11434/v1`) -> `{base}/{resource}`
pub fn resolve_openai_endpoint_url(base_url: &str, resource: &str) -> Result<String, String> {
    let parsed = normalize_and_parse_endpoint(base_url)?;
    let clean_resource = resource.trim().trim_start_matches('/');
    if clean_resource.is_empty() {
        return Err("Resource path cannot be empty".to_string());
    }
    match parsed {
        NormalizedEndpoint::Http {
            base_url, has_path, ..
        } => {
            if has_path {
                Ok(format!("{base_url}/{clean_resource}"))
            } else {
                Ok(format!("{base_url}/v1/{clean_resource}"))
            }
        }
        NormalizedEndpoint::Unix { .. } => Ok(format!("http://localhost/v1/{clean_resource}")),
    }
}

// ============================================================================
// 3. Pure-Rust HTTP (`reqwest::blocking`) & `#[cfg(unix)]` Socket Helpers
// ============================================================================

static SHARED_HTTP_CLIENT: OnceCell<reqwest::blocking::Client> = OnceCell::new();
static SHARED_LOOPBACK_HTTP_CLIENT: OnceCell<reqwest::blocking::Client> = OnceCell::new();

fn build_reqwest_client(
    is_loopback: bool,
    connect_timeout: Duration,
) -> Result<reqwest::blocking::Client, TransportError> {
    let mut builder = reqwest::blocking::Client::builder()
        .connect_timeout(connect_timeout)
        .timeout(None)
        .min_tls_version(reqwest::tls::Version::TLS_1_2)
        .user_agent("SpellIDE/1.0");
    if is_loopback {
        builder = builder.no_proxy();
    }
    builder.build().map_err(|e| {
        TransportError::Connection(format!("Failed to initialize HTTP client: {e}"))
    })
}

fn get_reqwest_client(
    is_loopback: bool,
    connect_timeout: Duration,
) -> Result<&'static reqwest::blocking::Client, TransportError> {
    let cell = if is_loopback {
        &SHARED_LOOPBACK_HTTP_CLIENT
    } else {
        &SHARED_HTTP_CLIENT
    };
    cell.get_or_try_init(|| build_reqwest_client(is_loopback, connect_timeout))
}

fn map_reqwest_error(err: reqwest::Error) -> TransportError {
    if err.is_builder() {
        return TransportError::InvalidUrl(format!("Invalid HTTP request or endpoint URL: {err}"));
    }
    if err.is_connect() || err.is_timeout() || err.is_request() {
        return TransportError::Connection(format!("Failed to connect: {err}"));
    }
    if err.is_body() || err.is_decode() {
        return TransportError::Connection(format!("HTTP stream error: {err}"));
    }
    TransportError::Connection(format!("Failed to connect: {err}"))
}

/// Unified streaming HTTP response across `reqwest::blocking` (`http://`/`https://`)
/// and `#[cfg(unix)]` domain sockets (`unix://`/`http+unix://`).
struct HttpStreamResponse {
    status: u16,
    headers: HashMap<String, String>,
    body: Box<dyn Read + Send>,
    cancel_flag: Arc<AtomicBool>,
}

/// Cross-platform `< 50 ms` cancellable streaming line reader backed by a worker thread
/// wrapping `BufReader::new(resp)` over a bounded `sync_channel(64)`.
struct LineStreamHandle {
    rx: mpsc::Receiver<Result<String, std::io::Error>>,
    cancel_flag: Arc<AtomicBool>,
}

impl Drop for LineStreamHandle {
    fn drop(&mut self) {
        self.cancel_flag.store(true, Ordering::Relaxed);
    }
}

impl LineStreamHandle {
    fn next_line(&self, check_cancel: bool) -> Result<Option<String>, TransportError> {
        self.next_line_with(|| check_cancel && crate::is_turn_cancel_requested())
    }

    fn next_line_with(
        &self,
        is_cancelled: impl Fn() -> bool,
    ) -> Result<Option<String>, TransportError> {
        loop {
            if is_cancelled() {
                self.cancel_flag.store(true, Ordering::Relaxed);
                return Err(TransportError::Cancelled);
            }
            match self.rx.recv_timeout(Duration::from_millis(50)) {
                Ok(Ok(line)) => return Ok(Some(line)),
                Ok(Err(e)) => {
                    if is_cancelled() {
                        self.cancel_flag.store(true, Ordering::Relaxed);
                        return Err(TransportError::Cancelled);
                    }
                    return Err(TransportError::Connection(format!(
                        "HTTP stream read error: {e}"
                    )));
                }
                Err(mpsc::RecvTimeoutError::Timeout) => continue,
                Err(mpsc::RecvTimeoutError::Disconnected) => return Ok(None),
            }
        }
    }

    fn collect_all(&self, prefix: String, check_cancel: bool) -> Result<String, TransportError> {
        let mut out = prefix;
        while let Some(line) = self.next_line(check_cancel)? {
            out.push_str(&line);
        }
        Ok(out.trim().to_string())
    }
}

fn spawn_line_reader(
    body: Box<dyn Read + Send>,
    cancel_flag: Arc<AtomicBool>,
) -> Result<LineStreamHandle, TransportError> {
    let (tx, rx) = mpsc::sync_channel::<Result<String, std::io::Error>>(64);
    let stop_flag = Arc::clone(&cancel_flag);
    std::thread::Builder::new()
        .name("spell-http-stream".to_string())
        .spawn(move || {
            let mut reader = BufReader::new(body);
            let mut line_buf: Vec<u8> = Vec::with_capacity(512);
            loop {
                if stop_flag.load(Ordering::Relaxed) {
                    break;
                }
                line_buf.clear();
                match reader.read_until(b'\n', &mut line_buf) {
                    Ok(0) => break,
                    Ok(_) => {
                        let line = match String::from_utf8(std::mem::take(&mut line_buf)) {
                            Ok(s) => s,
                            Err(e) => String::from_utf8_lossy(e.as_bytes()).into_owned(),
                        };
                        if tx.send(Ok(line)).is_err() {
                            break;
                        }
                    }
                    Err(e) => {
                        if !stop_flag.load(Ordering::Relaxed) {
                            let _ = tx.send(Err(e));
                        }
                        break;
                    }
                }
            }
        })
        .map_err(|e| {
            TransportError::Connection(format!("Failed to spawn HTTP stream reader thread: {e}"))
        })?;
    Ok(LineStreamHandle { rx, cancel_flag })
}

fn read_body_to_string(mut body: Box<dyn Read + Send>) -> Result<String, TransportError> {
    let mut bytes = Vec::new();
    body.read_to_end(&mut bytes).map_err(|e| {
        TransportError::Connection(format!("Failed reading HTTP response body: {e}"))
    })?;
    let text = match String::from_utf8(bytes) {
        Ok(s) => s,
        Err(e) => String::from_utf8_lossy(e.as_bytes()).into_owned(),
    };
    Ok(text.trim().to_string())
}

#[cfg(any(unix, test))]
fn find_header_delimiter(buf: &[u8]) -> Option<(usize, usize)> {
    for i in 0..buf.len() {
        if i + 4 <= buf.len() && &buf[i..i + 4] == b"\r\n\r\n" {
            return Some((i, i + 4));
        }
        if i + 2 <= buf.len() && &buf[i..i + 2] == b"\n\n" {
            return Some((i, i + 2));
        }
    }
    None
}

/// Parses both `HTTP/1.1 200 OK` and `HTTP/2 200` status lines and lowercased headers.
#[cfg(unix)]
pub(crate) fn parse_http_status_and_headers(
    raw_block: &[u8],
) -> Result<(u16, String, HashMap<String, String>), TransportError> {
    let text = String::from_utf8_lossy(raw_block);
    let mut lines = text.split('\n').map(|l| l.trim_end_matches('\r'));
    let status_line = lines
        .find(|l| !l.trim().is_empty())
        .ok_or_else(|| TransportError::Message("Empty HTTP response status line".to_string()))?;
    if !status_line.starts_with("HTTP/") {
        return Err(TransportError::Message(format!(
            "Malformed HTTP status line: '{status_line}'"
        )));
    }
    let code_str = status_line.split_ascii_whitespace().nth(1).ok_or_else(|| {
        TransportError::Message(format!("Missing HTTP status code in '{status_line}'"))
    })?;
    let status: u16 = code_str.parse().map_err(|_| {
        TransportError::Message(format!("Invalid HTTP status code '{code_str}'"))
    })?;

    let mut headers = HashMap::new();
    for line in lines {
        if line.is_empty() {
            continue;
        }
        if let Some((name, val)) = line.split_once(':') {
            let key = name.trim().to_ascii_lowercase();
            if !key.is_empty() {
                headers.insert(key, val.trim().to_string());
            }
        }
    }
    Ok((status, status_line.to_string(), headers))
}

#[cfg(unix)]
mod unix_http {
    use super::{
        find_header_delimiter, parse_http_status_and_headers, HttpStreamResponse, TransportError,
    };
    use std::collections::HashMap;
    use std::io::{self, BufRead, BufReader, Cursor, Read, Write};
    use std::os::unix::net::UnixStream;
    use std::path::Path;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::time::{Duration, Instant};

    struct PrefixReader<R> {
        prefix: Cursor<Vec<u8>>,
        inner: R,
    }

    impl<R: Read> Read for PrefixReader<R> {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            let n = self.prefix.read(buf)?;
            if n > 0 {
                return Ok(n);
            }
            self.inner.read(buf)
        }
    }

    struct CancelAwareUnixReader {
        stream: UnixStream,
        cancel_flag: Arc<AtomicBool>,
        check_turn_cancel: bool,
        deadline: Option<Instant>,
    }

    impl Read for CancelAwareUnixReader {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            loop {
                if self.cancel_flag.load(Ordering::Relaxed)
                    || (self.check_turn_cancel && crate::is_turn_cancel_requested())
                {
                    return Err(io::Error::new(
                        io::ErrorKind::Interrupted,
                        "Request cancelled",
                    ));
                }
                if let Some(dl) = self.deadline {
                    if Instant::now() >= dl {
                        return Err(io::Error::new(
                            io::ErrorKind::TimedOut,
                            "Unix socket request timed out",
                        ));
                    }
                }
                match self.stream.read(buf) {
                    Ok(n) => return Ok(n),
                    Err(e)
                        if matches!(
                            e.kind(),
                            io::ErrorKind::WouldBlock
                                | io::ErrorKind::TimedOut
                                | io::ErrorKind::Interrupted
                        ) =>
                    {
                        continue;
                    }
                    Err(e) => return Err(e),
                }
            }
        }
    }

    enum ChunkState {
        ReadSize,
        ReadData(usize),
        ConsumeCrlf,
        Done,
    }

    /// Streaming HTTP/1.1 `Transfer-Encoding: chunked` body decoder for Unix domain sockets.
    pub(super) struct ChunkedBodyReader<R: Read> {
        reader: BufReader<R>,
        state: ChunkState,
    }

    impl<R: Read> ChunkedBodyReader<R> {
        pub(super) fn new(inner: R) -> Self {
            Self {
                reader: BufReader::new(inner),
                state: ChunkState::ReadSize,
            }
        }

        fn read_size_line(&mut self) -> io::Result<String> {
            let mut line = Vec::new();
            let n = self.reader.read_until(b'\n', &mut line)?;
            if n == 0 {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "Unexpected EOF while reading HTTP chunk size",
                ));
            }
            Ok(String::from_utf8_lossy(&line)
                .trim_end_matches(&['\r', '\n'][..])
                .to_string())
        }

        fn consume_chunk_crlf(&mut self) -> io::Result<()> {
            let mut line = Vec::new();
            let _ = self.reader.read_until(b'\n', &mut line)?;
            Ok(())
        }

        fn drain_trailers(&mut self) -> io::Result<()> {
            loop {
                let mut line = Vec::new();
                let n = self.reader.read_until(b'\n', &mut line)?;
                if n == 0 || line == b"\r\n" || line == b"\n" {
                    return Ok(());
                }
            }
        }
    }

    impl<R: Read> Read for ChunkedBodyReader<R> {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            if buf.is_empty() {
                return Ok(0);
            }
            loop {
                match self.state {
                    ChunkState::Done => return Ok(0),
                    ChunkState::ConsumeCrlf => {
                        self.consume_chunk_crlf()?;
                        self.state = ChunkState::ReadSize;
                    }
                    ChunkState::ReadSize => {
                        let size_line = self.read_size_line()?;
                        let size_hex = size_line.split(';').next().unwrap_or("").trim();
                        let chunk_size = usize::from_str_radix(size_hex, 16).map_err(|e| {
                            io::Error::new(
                                io::ErrorKind::InvalidData,
                                format!("Invalid HTTP chunk size '{size_hex}': {e}"),
                            )
                        })?;
                        if chunk_size == 0 {
                            self.drain_trailers()?;
                            self.state = ChunkState::Done;
                            return Ok(0);
                        }
                        self.state = ChunkState::ReadData(chunk_size);
                    }
                    ChunkState::ReadData(remaining) => {
                        let to_read = buf.len().min(remaining);
                        let n = self.reader.read(&mut buf[..to_read])?;
                        if n == 0 {
                            return Err(io::Error::new(
                                io::ErrorKind::UnexpectedEof,
                                "Unexpected EOF inside HTTP chunk payload",
                            ));
                        }
                        let left = remaining - n;
                        if left == 0 {
                            self.state = ChunkState::ConsumeCrlf;
                        } else {
                            self.state = ChunkState::ReadData(left);
                        }
                        return Ok(n);
                    }
                }
            }
        }
    }

    fn extract_http_request_target(full_url: &str) -> &str {
        let rest = full_url
            .strip_prefix("http://")
            .or_else(|| full_url.strip_prefix("https://"))
            .unwrap_or(full_url);
        match rest.find('/') {
            Some(idx) => &rest[idx..],
            None => "/",
        }
    }

    fn read_unix_headers(
        stream: &mut UnixStream,
        deadline: Option<Instant>,
        check_cancel: bool,
    ) -> Result<(u16, HashMap<String, String>, Vec<u8>), TransportError> {
        const MAX_HEADER_BYTES: usize = 64 * 1024;
        let mut buf: Vec<u8> = Vec::with_capacity(4096);
        let mut scratch = [0u8; 4096];

        loop {
            if let Some((hdr_end, body_start)) = find_header_delimiter(&buf) {
                let (status, status_line, headers) =
                    parse_http_status_and_headers(&buf[..hdr_end])?;
                buf.drain(..body_start);

                let is_informational = (100..200).contains(&status);
                let is_proxy_tunnel = status == 200
                    && status_line
                        .to_ascii_lowercase()
                        .contains("connection established");
                if is_informational || is_proxy_tunnel {
                    continue;
                }
                return Ok((status, headers, buf));
            }

            if buf.len() > MAX_HEADER_BYTES {
                return Err(TransportError::Message(
                    "HTTP response headers exceeded 64 KB limit".to_string(),
                ));
            }

            if check_cancel && crate::is_turn_cancel_requested() {
                return Err(TransportError::Cancelled);
            }
            if let Some(dl) = deadline {
                if Instant::now() >= dl {
                    return Err(TransportError::Connection(
                        "Failed to connect: Unix socket header read timed out".to_string(),
                    ));
                }
            }

            match stream.read(&mut scratch) {
                Ok(0) => {
                    return Err(TransportError::Connection(
                        "Failed to connect: Unix socket closed before HTTP response headers"
                            .to_string(),
                    ));
                }
                Ok(n) => buf.extend_from_slice(&scratch[..n]),
                Err(e)
                    if matches!(
                        e.kind(),
                        io::ErrorKind::WouldBlock
                            | io::ErrorKind::TimedOut
                            | io::ErrorKind::Interrupted
                    ) =>
                {
                    continue;
                }
                Err(e) => {
                    return Err(TransportError::Connection(format!(
                        "Failed to connect: Unix socket read error: {e}"
                    )));
                }
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) fn open_unix_response(
        socket_path: &Path,
        full_url: &str,
        method: &str,
        accept: &str,
        auth_headers: &[(String, String)],
        body_json: Option<&str>,
        connect_timeout: Duration,
        request_timeout: Option<Duration>,
        check_cancel: bool,
    ) -> Result<HttpStreamResponse, TransportError> {
        if check_cancel && crate::is_turn_cancel_requested() {
            return Err(TransportError::Cancelled);
        }

        let mut stream = UnixStream::connect(socket_path).map_err(|e| {
            TransportError::Connection(format!(
                "Failed to connect to Unix socket {}: {e}",
                socket_path.display()
            ))
        })?;
        let _ = stream.set_write_timeout(Some(connect_timeout));
        let _ = stream.set_read_timeout(Some(Duration::from_millis(50)));

        let request_target = extract_http_request_target(full_url);
        let mut raw_req = String::with_capacity(256 + body_json.map_or(0, str::len));
        raw_req.push_str(&format!("{method} {request_target} HTTP/1.1\r\n"));
        raw_req.push_str("Host: localhost\r\n");
        raw_req.push_str("User-Agent: SpellIDE/1.0\r\n");
        raw_req.push_str(&format!("Accept: {accept}\r\n"));
        raw_req.push_str("Connection: close\r\n");

        for (k, v) in auth_headers {
            if k.bytes().any(|b| b == b'\r' || b == b'\n' || b == 0)
                || v.bytes().any(|b| b == b'\r' || b == b'\n' || b == 0)
            {
                return Err(TransportError::Message(
                    "HTTP auth header contains forbidden CR/LF/NUL characters".to_string(),
                ));
            }
            raw_req.push_str(&format!("{k}: {v}\r\n"));
        }

        if let Some(payload) = body_json {
            raw_req.push_str("Content-Type: application/json\r\n");
            raw_req.push_str(&format!("Content-Length: {}\r\n", payload.len()));
            raw_req.push_str("\r\n");
            raw_req.push_str(payload);
        } else {
            raw_req.push_str("\r\n");
        }

        stream.write_all(raw_req.as_bytes()).map_err(|e| {
            TransportError::Connection(format!(
                "Failed writing HTTP request to Unix socket {}: {e}",
                socket_path.display()
            ))
        })?;
        let _ = stream.flush();

        let deadline = request_timeout.map(|d| Instant::now() + d);
        let (status, headers, initial_body) =
            read_unix_headers(&mut stream, deadline, check_cancel)?;

        let cancel_flag = Arc::new(AtomicBool::new(false));
        let socket_reader = CancelAwareUnixReader {
            stream,
            cancel_flag: Arc::clone(&cancel_flag),
            check_turn_cancel: check_cancel,
            deadline,
        };
        let prefixed = PrefixReader {
            prefix: Cursor::new(initial_body),
            inner: socket_reader,
        };

        let is_chunked = headers
            .get("transfer-encoding")
            .map(|v| v.to_ascii_lowercase().contains("chunked"))
            .unwrap_or(false);
        let content_length = headers
            .get("content-length")
            .and_then(|v| v.trim().parse::<u64>().ok());

        let body: Box<dyn Read + Send> = if status == 204 || status == 304 {
            Box::new(io::empty())
        } else if is_chunked {
            Box::new(ChunkedBodyReader::new(prefixed))
        } else if let Some(len) = content_length {
            Box::new(prefixed.take(len))
        } else {
            Box::new(prefixed)
        };

        Ok(HttpStreamResponse {
            status,
            headers,
            body,
            cancel_flag,
        })
    }
}

// ============================================================================
// 4. `HttpTransport` (OpenAI-Compatible Client via `reqwest::blocking`)
// ============================================================================

/// Flexible second-argument trait for [`HttpTransport::new`] accepting either an [`EnginePathLocator`]
/// or a direct API key string (`&str` / `String`).
pub trait IntoHttpTransportConfig {
    fn into_locator_and_key(self) -> (EnginePathLocator, Option<String>);
}

impl IntoHttpTransportConfig for EnginePathLocator {
    fn into_locator_and_key(self) -> (EnginePathLocator, Option<String>) {
        (self, None)
    }
}

impl IntoHttpTransportConfig for &EnginePathLocator {
    fn into_locator_and_key(self) -> (EnginePathLocator, Option<String>) {
        (self.clone(), None)
    }
}

impl IntoHttpTransportConfig for &str {
    fn into_locator_and_key(self) -> (EnginePathLocator, Option<String>) {
        (EnginePathLocator::discover(), Some(self.to_string()))
    }
}

impl IntoHttpTransportConfig for String {
    fn into_locator_and_key(self) -> (EnginePathLocator, Option<String>) {
        (EnginePathLocator::discover(), Some(self))
    }
}

impl IntoHttpTransportConfig for Option<String> {
    fn into_locator_and_key(self) -> (EnginePathLocator, Option<String>) {
        (EnginePathLocator::discover(), self)
    }
}

/// Real HTTP/1.1 + HTTP/2 + TLS 1.2+ + SSE client backed by pure-Rust `reqwest::blocking`.
pub struct HttpTransport {
    base_url: RwLock<String>,
    locator: EnginePathLocator,
    api_key_override: RwLock<Option<String>>,
    timeout: Duration,
}

impl Default for HttpTransport {
    fn default() -> Self {
        Self::new(DEFAULT_BASE_URL, EnginePathLocator::discover())
    }
}

impl HttpTransport {
    pub fn new(
        base_url: impl Into<String>,
        auth_or_locator: impl IntoHttpTransportConfig,
    ) -> Self {
        let (locator, api_key_override) = auth_or_locator.into_locator_and_key();
        Self {
            base_url: RwLock::new(base_url.into()),
            locator,
            api_key_override: RwLock::new(api_key_override),
            timeout: Duration::from_secs(15),
        }
    }

    pub fn with_default_locator(base_url: impl Into<String>) -> Self {
        Self::new(base_url, EnginePathLocator::discover())
    }

    pub fn with_api_key(base_url: impl Into<String>, api_key: impl Into<String>) -> Self {
        let transport = Self::with_default_locator(base_url);
        *transport.api_key_override.write() = Some(api_key.into());
        transport
    }

    /// Creates an `HttpTransport` that deliberately omits the `Authorization` header.
    pub fn without_auth(base_url: impl Into<String>) -> Self {
        let transport = Self::with_default_locator(base_url);
        *transport.api_key_override.write() = Some(String::new());
        transport
    }

    pub fn base_url(&self) -> String {
        self.base_url.read().clone()
    }

    pub fn set_base_url(&self, base_url: impl Into<String>) {
        *self.base_url.write() = base_url.into();
    }

    pub fn api_key_override(&self) -> Option<String> {
        self.api_key_override.read().clone()
    }

    pub fn set_api_key_override(&self, api_key: Option<String>) {
        *self.api_key_override.write() = api_key;
    }

    pub fn validate_endpoint(&self) -> Result<NormalizedEndpoint, String> {
        normalize_and_parse_endpoint(&self.base_url())
    }

    pub fn stream_chat(&self, req: &mut ChatCompletionRequest) -> Result<ChatTurnSummary, String> {
        self.stream_chat_completion(req, &mut |_| {})
    }

    fn resolve_auth_headers(&self) -> Vec<(String, String)> {
        if let Some(ref key_override) = *self.api_key_override.read() {
            let trimmed = key_override.trim();
            if trimmed.is_empty() {
                return Vec::new();
            }
            return vec![("Authorization".to_string(), format!("Bearer {trimmed}"))];
        }
        self.locator.build_auth_headers()
    }

    /// Opens an HTTP/HTTPS response via pure-Rust `reqwest::blocking` (pure in-memory headers and
    /// JSON body, TLS 1.2+, native root certificates, `.no_proxy()` for loopback), or via
    /// `#[cfg(unix)]` `UnixStream` for `unix://` / `http+unix://` test endpoints.
    #[allow(clippy::too_many_arguments)]
    fn open_response(
        &self,
        endpoint: &NormalizedEndpoint,
        full_url: &str,
        method_str: &str,
        accept: &str,
        body_json: Option<&str>,
        request_timeout: Option<Duration>,
        check_cancel: bool,
    ) -> Result<HttpStreamResponse, TransportError> {
        if check_cancel && crate::is_turn_cancel_requested() {
            return Err(TransportError::Cancelled);
        }

        let auth_headers = self.resolve_auth_headers();

        match endpoint {
            #[cfg(unix)]
            NormalizedEndpoint::Unix { socket_path, .. } => unix_http::open_unix_response(
                socket_path,
                full_url,
                method_str,
                accept,
                &auth_headers,
                body_json,
                self.timeout,
                request_timeout,
                check_cancel,
            ),
            #[cfg(not(unix))]
            NormalizedEndpoint::Unix { .. } => Err(TransportError::InvalidUrl(
                "Unix domain sockets are only supported on Unix platforms".to_string(),
            )),
            NormalizedEndpoint::Http { is_loopback, .. } => {
                let client = get_reqwest_client(*is_loopback, self.timeout)?;
                let method = reqwest::Method::from_bytes(method_str.as_bytes()).map_err(|e| {
                    TransportError::Message(format!("Invalid HTTP method '{method_str}': {e}"))
                })?;

                let mut rb = client
                    .request(method, full_url)
                    .header(reqwest::header::ACCEPT, accept);

                for (k, v) in auth_headers {
                    if k.bytes().any(|b| b == b'\r' || b == b'\n' || b == 0)
                        || v.bytes().any(|b| b == b'\r' || b == b'\n' || b == 0)
                    {
                        return Err(TransportError::Message(
                            "HTTP auth header contains forbidden CR/LF/NUL characters".to_string(),
                        ));
                    }
                    let name =
                        reqwest::header::HeaderName::from_bytes(k.as_bytes()).map_err(|e| {
                            TransportError::Message(format!(
                                "Invalid HTTP auth header name '{k}': {e}"
                            ))
                        })?;
                    let val = reqwest::header::HeaderValue::from_str(&v).map_err(|e| {
                        TransportError::Message(format!(
                            "Invalid HTTP auth header value for '{k}': {e}"
                        ))
                    })?;
                    rb = rb.header(name, val);
                }

                if let Some(payload) = body_json {
                    rb = rb
                        .header(reqwest::header::CONTENT_TYPE, "application/json")
                        .body(payload.to_string());
                }

                if let Some(dur) = request_timeout {
                    rb = rb.timeout(dur);
                }

                let req = rb.build().map_err(map_reqwest_error)?;

                let resp = if check_cancel {
                    let client_clone = client.clone();
                    let (tx, rx) = mpsc::sync_channel(1);
                    std::thread::Builder::new()
                        .name("spell-http-send".to_string())
                        .spawn(move || {
                            let _ = tx.send(client_clone.execute(req));
                        })
                        .map_err(|e| {
                            TransportError::Connection(format!(
                                "Failed to spawn HTTP request thread: {e}"
                            ))
                        })?;
                    loop {
                        if crate::is_turn_cancel_requested() {
                            return Err(TransportError::Cancelled);
                        }
                        match rx.recv_timeout(Duration::from_millis(50)) {
                            Ok(Ok(r)) => break r,
                            Ok(Err(e)) => return Err(map_reqwest_error(e)),
                            Err(mpsc::RecvTimeoutError::Timeout) => continue,
                            Err(mpsc::RecvTimeoutError::Disconnected) => {
                                return Err(TransportError::Connection(
                                    "HTTP request thread terminated unexpectedly".to_string(),
                                ));
                            }
                        }
                    }
                } else {
                    client.execute(req).map_err(map_reqwest_error)?
                };

                let status = resp.status().as_u16();
                let mut headers = HashMap::new();
                for (name, value) in resp.headers() {
                    if let Ok(val_str) = value.to_str() {
                        headers.insert(name.as_str().to_ascii_lowercase(), val_str.to_string());
                    }
                }

                Ok(HttpStreamResponse {
                    status,
                    headers,
                    body: Box::new(resp),
                    cancel_flag: Arc::new(AtomicBool::new(false)),
                })
            }
        }
    }

    fn execute_http_json(
        &self,
        endpoint: &NormalizedEndpoint,
        full_url: &str,
        method: &str,
        body_json: Option<&str>,
        request_timeout: Option<Duration>,
        check_cancel: bool,
    ) -> Result<(u16, HashMap<String, String>, String), TransportError> {
        let resp = self.open_response(
            endpoint,
            full_url,
            method,
            "application/json",
            body_json,
            request_timeout,
            check_cancel,
        )?;
        let body = read_body_to_string(resp.body)?;
        Ok((resp.status, resp.headers, body))
    }

    fn merge_tool_call_delta(accumulated: &mut Vec<WireToolCall>, delta_calls: &[WireToolCall]) {
        for tc in delta_calls {
            let target_pos = tc
                .index
                .and_then(|idx| {
                    accumulated
                        .iter()
                        .position(|existing| existing.index == Some(idx))
                })
                .or_else(|| {
                    if !tc.id.is_empty() {
                        accumulated.iter().position(|existing| existing.id == tc.id)
                    } else {
                        None
                    }
                });

            if let Some(pos) = target_pos {
                let existing = &mut accumulated[pos];
                if !tc.id.is_empty() && existing.id.is_empty() {
                    existing.id = tc.id.clone();
                }
                if !tc.function.name.is_empty() && existing.function.name.is_empty() {
                    existing.function.name = tc.function.name.clone();
                }
                if !tc.function.arguments.is_empty() {
                    existing.function.arguments.push_str(&tc.function.arguments);
                }
            } else {
                accumulated.push(tc.clone());
            }
        }
    }

    /// Builds a clean, standard OpenAI-compatible JSON payload for a subagent completion,
    /// sanitizing outbound messages via [`WireMessage::for_outbound_openai`] and stripping
    /// proprietary fields.
    pub fn build_subagent_wire_json(
        req: &SubagentCompletionRequest,
    ) -> Result<Value, TransportError> {
        validate_model_for_role(AgentExecutionRole::CodingSubagent, &req.model)
            .map_err(TransportError::ModelRoleViolation)?;

        let mut wire_req = req.clone();
        wire_req.messages = wire_req
            .messages
            .iter()
            .map(WireMessage::for_outbound_openai)
            .collect();
        if !wire_req.stream {
            wire_req.stream_options = None;
        } else if wire_req.stream_options.is_none() {
            wire_req.stream_options = Some(crate::contract::StreamOptions {
                include_usage: true,
            });
        }

        let mut wire_json = serde_json::to_value(&wire_req).map_err(|e| {
            TransportError::Message(format!("Failed to serialize SubagentCompletionRequest: {e}"))
        })?;
        if let Some(obj) = wire_json.as_object_mut() {
            match req.tools {
                Some(ref tools) if !tools.is_empty() => {
                    obj.insert("tools".to_string(), serde_json::json!(tools));
                    let tc = req
                        .tool_choice
                        .clone()
                        .unwrap_or_else(|| serde_json::json!("none"));
                    obj.insert("tool_choice".to_string(), tc);
                }
                _ => {
                    obj.remove("tools");
                    obj.remove("tool_choice");
                }
            }
        }
        Ok(wire_json)
    }

    fn apply_chat_chunk_to_summary(
        chunk: &ChatCompletionChunk,
        summary: &mut ChatTurnSummary,
        first_token_at: &mut Option<Instant>,
        delta_chunks: &mut u64,
    ) {
        if !chunk.id.is_empty() {
            summary.response_id = chunk.id.clone();
        }
        if !chunk.model.trim().is_empty() {
            summary.model = chunk.model.clone();
        }
        for choice in &chunk.choices {
            let mut emitted_token = false;
            if let Some(ref reasoning) = choice.delta.reasoning_content {
                if !reasoning.is_empty() {
                    summary.reasoning_content.push_str(reasoning);
                    emitted_token = true;
                }
            }
            if let Some(ref content) = choice.delta.content {
                if !content.is_empty() {
                    summary.content.push_str(content);
                    emitted_token = true;
                }
            }
            if let Some(ref tcs) = choice.delta.tool_calls {
                if !tcs.is_empty() {
                    Self::merge_tool_call_delta(&mut summary.tool_calls, tcs);
                    emitted_token = true;
                }
            }
            if emitted_token {
                if first_token_at.is_none() {
                    *first_token_at = Some(Instant::now());
                }
                *delta_chunks = delta_chunks.saturating_add(1);
            }
            if let Some(ref fr) = choice.finish_reason {
                summary.finish_reason = fr.clone();
            }
        }
        if let Some(ref usage) = chunk.usage {
            summary.usage = usage.clone();
        }
        if let Some(adv) = chunk.effective_context_advice() {
            summary.context_advice = adv.clone();
        }
    }

    fn parse_non_streaming_chat_json(
        resp_body: &str,
        fallback_model: &str,
    ) -> Result<(ChatTurnSummary, ChatCompletionChunk), TransportError> {
        let val: Value = serde_json::from_str(resp_body).map_err(|e| {
            TransportError::Message(format!(
                "Failed to parse ChatCompletion JSON response: {e}"
            ))
        })?;
        let mut summary = ChatTurnSummary {
            response_id: val
                .get("id")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string(),
            model: val
                .get("model")
                .and_then(Value::as_str)
                .filter(|s| !s.trim().is_empty())
                .unwrap_or(fallback_model)
                .to_string(),
            finish_reason: "stop".to_string(),
            ..Default::default()
        };

        if let Some(first) = val
            .get("choices")
            .and_then(Value::as_array)
            .and_then(|a| a.first())
        {
            if let Some(msg) = first.get("message") {
                if let Some(c) = msg.get("content").and_then(Value::as_str) {
                    summary.content = c.to_string();
                }
                if let Some(r) = msg
                    .get("reasoning_content")
                    .or_else(|| msg.get("reasoning"))
                    .and_then(Value::as_str)
                {
                    summary.reasoning_content = r.to_string();
                }
                if let Some(tcs_val) = msg.get("tool_calls") {
                    if let Ok(parsed) = serde_json::from_value::<Vec<WireToolCall>>(tcs_val.clone())
                    {
                        summary.tool_calls = parsed;
                    }
                }
            } else if let Some(text) = first.get("text").and_then(Value::as_str) {
                summary.content = text.to_string();
            }
            if let Some(fr) = first.get("finish_reason").and_then(Value::as_str) {
                summary.finish_reason = fr.to_string();
            }
        }

        if let Some(u_val) = val.get("usage") {
            if let Ok(u) = serde_json::from_value::<ChatCompletionUsage>(u_val.clone()) {
                summary.usage = u;
            }
        }

        let synthetic_chunk = ChatCompletionChunk {
            id: summary.response_id.clone(),
            model: summary.model.clone(),
            choices: vec![ChatCompletionChunkChoice {
                index: 0,
                delta: ChatCompletionDelta {
                    role: Some("assistant".to_string()),
                    content: if summary.content.is_empty() {
                        None
                    } else {
                        Some(summary.content.clone())
                    },
                    reasoning_content: if summary.reasoning_content.is_empty() {
                        None
                    } else {
                        Some(summary.reasoning_content.clone())
                    },
                    tool_calls: if summary.tool_calls.is_empty() {
                        None
                    } else {
                        Some(summary.tool_calls.clone())
                    },
                },
                finish_reason: Some(summary.finish_reason.clone()),
            }],
            usage: Some(summary.usage.clone()),
            engine_context_advice: None,
        };

        Ok((summary, synthetic_chunk))
    }

    pub(crate) fn parse_subagent_sse_line(
        raw_line: &str,
        out: &mut SubagentCompletionResponse,
        first_token_at: &mut Option<Instant>,
        delta_chunks: &mut u64,
    ) -> Result<bool, TransportError> {
        if crate::is_turn_cancel_requested() {
            return Err(TransportError::Cancelled);
        }
        let trimmed = raw_line.trim();
        let Some(data_payload) = trimmed.strip_prefix("data:") else {
            return Ok(false);
        };
        let payload = data_payload.trim();
        if payload.is_empty() {
            return Ok(false);
        }
        if payload == "[DONE]" {
            return Ok(true);
        }

        let chunk: ChatCompletionChunk = serde_json::from_str(payload).map_err(|e| {
            TransportError::Message(format!(
                "Failed parsing subagent SSE chunk `{payload}`: {e}"
            ))
        })?;
        let raw_chunk_val: Option<Value> = serde_json::from_str(payload).ok();

        if !chunk.id.is_empty() {
            out.response_id = chunk.id.clone();
        }
        if !chunk.model.trim().is_empty() {
            out.model = chunk.model.clone();
        }

        for ch in &chunk.choices {
            let mut emitted = false;
            if let Some(ref r) = ch.delta.reasoning_content {
                if !r.is_empty() {
                    out.reasoning_content.push_str(r);
                    emitted = true;
                }
            }
            if let Some(ref c) = ch.delta.content {
                if !c.is_empty() {
                    out.content.push_str(c);
                    emitted = true;
                }
            }
            if let Some(ref tcs) = ch.delta.tool_calls {
                if !tcs.is_empty() {
                    Self::merge_tool_call_delta(&mut out.tool_calls, tcs);
                    emitted = true;
                }
            }
            if emitted {
                if first_token_at.is_none() {
                    *first_token_at = Some(Instant::now());
                }
                *delta_chunks = delta_chunks.saturating_add(1);
            }
            if let Some(ref fr) = ch.finish_reason {
                out.finish_reason = Some(fr.clone());
            }
        }

        if let Some(ref u) = chunk.usage {
            out.prompt_tokens = u.prompt_tokens as u32;
            out.completion_tokens = u.completion_tokens as u32;
            let details_cached = u
                .prompt_tokens_details
                .as_ref()
                .map(|d| d.cached_tokens as u32)
                .unwrap_or(0);
            if details_cached > 0 {
                out.cached_tokens = details_cached;
                out.prefix_cache_reused = true;
            } else if let Some(raw_cached) = raw_chunk_val
                .as_ref()
                .and_then(|v| v.get("usage"))
                .and_then(|u_val| u_val.get("cached_tokens"))
                .and_then(|v| v.as_u64())
            {
                out.cached_tokens = raw_cached as u32;
                if raw_cached > 0 {
                    out.prefix_cache_reused = true;
                }
            }
            if let Some(tps) = u.generation_tokens_per_second.filter(|&v| v > 0.0) {
                out.decode_tok_s = tps;
            }
        }

        Ok(false)
    }

    fn finalize_decode_tps(
        provider_tps: Option<f64>,
        completion_tokens: u64,
        delta_chunks: u64,
        req_start: Instant,
        first_token_at: Option<Instant>,
    ) -> f64 {
        if let Some(tps) = provider_tps.filter(|&v| v.is_finite() && v > 0.0) {
            return tps;
        }
        let gen_secs = first_token_at
            .unwrap_or(req_start)
            .elapsed()
            .as_secs_f64()
            .max(0.001);
        if completion_tokens > 0 {
            (completion_tokens as f64) / gen_secs
        } else if delta_chunks > 0 {
            (delta_chunks as f64) / gen_secs
        } else {
            0.0
        }
    }

    fn process_chat_sse_line(
        line_str: &str,
        summary: &mut ChatTurnSummary,
        first_token_at: &mut Option<Instant>,
        delta_chunks: &mut u64,
        on_chunk: &mut dyn FnMut(&ChatCompletionChunk),
    ) -> Result<bool, TransportError> {
        let trimmed = line_str.trim();
        if trimmed.is_empty() || trimmed.starts_with(':') {
            return Ok(false);
        }
        let Some(data_payload) = trimmed.strip_prefix("data:") else {
            return Ok(false);
        };
        let payload = data_payload.trim();
        if payload.is_empty() {
            return Ok(false);
        }
        summary.raw_sse_frames.push(format!("data: {payload}\n\n"));
        if payload == "[DONE]" {
            return Ok(true);
        }
        let chunk: ChatCompletionChunk = serde_json::from_str(payload).map_err(|e| {
            TransportError::Message(format!(
                "Failed to parse SSE ChatCompletionChunk `{payload}`: {e}"
            ))
        })?;
        Self::apply_chat_chunk_to_summary(&chunk, summary, first_token_at, delta_chunks);
        on_chunk(&chunk);
        Ok(false)
    }

    fn finalize_chat_summary(
        mut summary: ChatTurnSummary,
        delta_chunks: u64,
        req_start: Instant,
        first_token_at: Option<Instant>,
    ) -> ChatTurnSummary {
        let tps = Self::finalize_decode_tps(
            if summary.context_advice.decode_tok_s > 0.0 {
                Some(summary.context_advice.decode_tok_s)
            } else {
                summary.usage.generation_tokens_per_second
            },
            summary.usage.completion_tokens,
            delta_chunks,
            req_start,
            first_token_at,
        );
        if tps > 0.0 {
            summary.context_advice.decode_tok_s = tps;
            if summary.usage.generation_tokens_per_second.is_none() {
                summary.usage.generation_tokens_per_second = Some(tps);
            }
        }
        summary
    }

    fn detect_sse_or_json(
        content_type: &str,
        stream: &LineStreamHandle,
    ) -> Result<(bool, Option<String>), TransportError> {
        if content_type.contains("text/event-stream") {
            return Ok((true, None));
        }
        if content_type.contains("application/json") {
            return Ok((false, None));
        }
        let mut leading = String::new();
        while let Some(line) = stream.next_line(true)? {
            let t = line.trim_start();
            if t.is_empty() {
                leading.push_str(&line);
                continue;
            }
            if t.starts_with("data:") || t.starts_with(':') || t.starts_with("event:") {
                return Ok((true, Some(line)));
            }
            leading.push_str(&line);
            return Ok((false, Some(leading)));
        }
        Ok((false, Some(leading)))
    }

    fn stream_chat_once(
        &self,
        endpoint: &NormalizedEndpoint,
        full_url: &str,
        wire_req: &ChatCompletionRequest,
        on_chunk: &mut dyn FnMut(&ChatCompletionChunk),
    ) -> Result<ChatTurnSummary, TransportError> {
        if crate::is_turn_cancel_requested() {
            return Err(TransportError::Cancelled);
        }
        let req_start = Instant::now();
        let body_json = serde_json::to_string(wire_req).map_err(|e| {
            TransportError::Message(format!("Failed to serialize ChatCompletionRequest: {e}"))
        })?;

        let resp = self.open_response(
            endpoint,
            full_url,
            "POST",
            "text/event-stream, application/json",
            Some(&body_json),
            None,
            true,
        )?;

        if !(200..300).contains(&resp.status) {
            let err_body = read_body_to_string(resp.body).unwrap_or_default();
            return Err(TransportError::HttpStatus {
                status: resp.status,
                body: err_body,
            });
        }

        let content_type = resp
            .headers
            .get("content-type")
            .map(|s| s.to_ascii_lowercase())
            .unwrap_or_default();
        let stream = spawn_line_reader(resp.body, resp.cancel_flag)?;
        let (is_sse, first_line_opt) = Self::detect_sse_or_json(&content_type, &stream)?;

        if !is_sse {
            let resp_body = stream.collect_all(first_line_opt.unwrap_or_default(), true)?;
            let (mut summary, synthetic) =
                Self::parse_non_streaming_chat_json(&resp_body, &wire_req.model)?;
            let tps = Self::finalize_decode_tps(
                summary.usage.generation_tokens_per_second,
                summary.usage.completion_tokens,
                if summary.content.is_empty() && summary.reasoning_content.is_empty() {
                    0
                } else {
                    1
                },
                req_start,
                None,
            );
            if tps > 0.0 {
                summary.context_advice.decode_tok_s = tps;
                if summary.usage.generation_tokens_per_second.is_none() {
                    summary.usage.generation_tokens_per_second = Some(tps);
                }
            }
            on_chunk(&synthetic);
            return Ok(summary);
        }

        let mut summary = ChatTurnSummary {
            response_id: String::new(),
            model: wire_req.model.clone(),
            reasoning_content: String::new(),
            content: String::new(),
            tool_calls: Vec::new(),
            finish_reason: "stop".to_string(),
            usage: ChatCompletionUsage::default(),
            context_advice: EngineContextAdvice::default(),
            engine_injected_alarms: Vec::new(),
            raw_sse_frames: Vec::new(),
        };

        let mut first_token_at: Option<Instant> = None;
        let mut delta_chunks: u64 = 0;

        if let Some(first_line) = first_line_opt {
            if Self::process_chat_sse_line(
                &first_line,
                &mut summary,
                &mut first_token_at,
                &mut delta_chunks,
                on_chunk,
            )? {
                return Ok(Self::finalize_chat_summary(
                    summary,
                    delta_chunks,
                    req_start,
                    first_token_at,
                ));
            }
            if crate::is_turn_cancel_requested() {
                return Err(TransportError::Cancelled);
            }
        }

        while let Some(line) = stream.next_line(true)? {
            if Self::process_chat_sse_line(
                &line,
                &mut summary,
                &mut first_token_at,
                &mut delta_chunks,
                on_chunk,
            )? {
                break;
            }
            if crate::is_turn_cancel_requested() {
                return Err(TransportError::Cancelled);
            }
        }

        Ok(Self::finalize_chat_summary(
            summary,
            delta_chunks,
            req_start,
            first_token_at,
        ))
    }

    fn run_subagent_once(
        &self,
        endpoint: &NormalizedEndpoint,
        full_url: &str,
        wire_json: &Value,
        fallback_model: &str,
    ) -> Result<SubagentCompletionResponse, TransportError> {
        if crate::is_turn_cancel_requested() {
            return Err(TransportError::Cancelled);
        }
        let req_start = Instant::now();
        let body_json = serde_json::to_string(wire_json).map_err(|e| {
            TransportError::Message(format!("Failed to serialize SubagentCompletionRequest: {e}"))
        })?;
        let is_stream_req = wire_json
            .get("stream")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        let accept = if is_stream_req {
            "text/event-stream, application/json"
        } else {
            "application/json, text/event-stream"
        };
        let request_timeout = if is_stream_req {
            None
        } else {
            Some(Duration::from_secs(300))
        };

        let resp = self.open_response(
            endpoint,
            full_url,
            "POST",
            accept,
            Some(&body_json),
            request_timeout,
            true,
        )?;

        if !(200..300).contains(&resp.status) {
            let err_body = read_body_to_string(resp.body).unwrap_or_default();
            return Err(TransportError::HttpStatus {
                status: resp.status,
                body: err_body,
            });
        }

        let content_type = resp
            .headers
            .get("content-type")
            .map(|s| s.to_ascii_lowercase())
            .unwrap_or_default();
        let stream = spawn_line_reader(resp.body, resp.cancel_flag)?;
        let (is_sse, first_line_opt) = Self::detect_sse_or_json(&content_type, &stream)?;

        let mut out = SubagentCompletionResponse {
            model: fallback_model.to_string(),
            ..Default::default()
        };
        let mut first_token_at: Option<Instant> = None;
        let mut delta_chunks: u64 = 0;

        if is_sse {
            let mut saw_done = false;
            if let Some(first_line) = first_line_opt {
                if Self::parse_subagent_sse_line(
                    &first_line,
                    &mut out,
                    &mut first_token_at,
                    &mut delta_chunks,
                )? {
                    saw_done = true;
                }
            }
            if !saw_done {
                while let Some(line) = stream.next_line(true)? {
                    if Self::parse_subagent_sse_line(
                        &line,
                        &mut out,
                        &mut first_token_at,
                        &mut delta_chunks,
                    )? {
                        break;
                    }
                }
            }

            out.decode_tok_s = Self::finalize_decode_tps(
                if out.decode_tok_s > 0.0 {
                    Some(out.decode_tok_s)
                } else {
                    None
                },
                out.completion_tokens as u64,
                delta_chunks,
                req_start,
                first_token_at,
            );
            return Ok(out);
        }

        let resp_body = stream.collect_all(first_line_opt.unwrap_or_default(), true)?;

        if resp_body.trim_start().starts_with("data:") {
            for raw_line in resp_body.lines() {
                if Self::parse_subagent_sse_line(
                    raw_line,
                    &mut out,
                    &mut first_token_at,
                    &mut delta_chunks,
                )? {
                    break;
                }
            }
            out.decode_tok_s = Self::finalize_decode_tps(
                if out.decode_tok_s > 0.0 {
                    Some(out.decode_tok_s)
                } else {
                    None
                },
                out.completion_tokens as u64,
                delta_chunks,
                req_start,
                first_token_at,
            );
            return Ok(out);
        }

        let val: Value = serde_json::from_str(&resp_body).map_err(|e| {
            TransportError::Message(format!(
                "Failed to parse SubagentCompletionResponse JSON `{resp_body}`: {e}"
            ))
        })?;
        if let Some(m) = val.get("model").and_then(Value::as_str).filter(|s| !s.trim().is_empty()) {
            out.model = m.trim().to_string();
        }
        out.response_id = val
            .get("id")
            .or_else(|| val.get("response_id"))
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string();

        if let Some(first) = val
            .get("choices")
            .and_then(Value::as_array)
            .and_then(|a| a.first())
        {
            if let Some(msg) = first.get("message") {
                out.content = msg
                    .get("content")
                    .and_then(Value::as_str)
                    .unwrap_or("")
                    .to_string();
                out.reasoning_content = msg
                    .get("reasoning_content")
                    .or_else(|| msg.get("reasoning"))
                    .and_then(Value::as_str)
                    .unwrap_or("")
                    .to_string();
                if let Some(tc_val) = msg.get("tool_calls") {
                    if let Ok(parsed_tcs) =
                        serde_json::from_value::<Vec<WireToolCall>>(tc_val.clone())
                    {
                        out.tool_calls = parsed_tcs;
                    }
                }
            } else if let Some(c) = first.get("text").and_then(Value::as_str) {
                out.content = c.to_string();
            }
            out.finish_reason = first
                .get("finish_reason")
                .and_then(Value::as_str)
                .map(ToString::to_string);
        } else if let Some(direct_content) = val.get("content").and_then(Value::as_str) {
            out.content = direct_content.to_string();
        }

        if let Some(usage) = val.get("usage") {
            out.prompt_tokens = usage
                .get("prompt_tokens")
                .and_then(Value::as_u64)
                .unwrap_or(0) as u32;
            out.completion_tokens = usage
                .get("completion_tokens")
                .and_then(Value::as_u64)
                .unwrap_or(0) as u32;
            out.cached_tokens = usage
                .get("cached_tokens")
                .or_else(|| {
                    usage
                        .get("prompt_tokens_details")
                        .and_then(|d| d.get("cached_tokens"))
                })
                .and_then(Value::as_u64)
                .unwrap_or(0) as u32;
            if out.cached_tokens > 0 {
                out.prefix_cache_reused = true;
            }
            if let Some(tps) = usage
                .get("decode_tok_s")
                .or_else(|| usage.get("generation_tokens_per_second"))
                .and_then(Value::as_f64)
            {
                out.decode_tok_s = tps;
            }
        }
        out.decode_tok_s = Self::finalize_decode_tps(
            if out.decode_tok_s > 0.0 {
                Some(out.decode_tok_s)
            } else {
                None
            },
            out.completion_tokens as u64,
            if out.content.is_empty() { 0 } else { 1 },
            req_start,
            None,
        );

        Ok(out)
    }
}

impl EngineTransport for HttpTransport {
    fn transport_name(&self) -> &str {
        "HttpTransport (OpenAI-Compatible)"
    }

    fn list_models(&self) -> Result<Vec<ModelDescriptor>, TransportError> {
        let raw_base = self.base_url();
        let endpoint =
            normalize_and_parse_endpoint(&raw_base).map_err(TransportError::InvalidUrl)?;
        let models_url =
            resolve_openai_endpoint_url(&raw_base, "models").map_err(TransportError::InvalidUrl)?;

        let (code, _headers, body) = self.execute_http_json(
            &endpoint,
            &models_url,
            "GET",
            None,
            Some(self.timeout),
            false,
        )?;

        if code == 404 {
            if let NormalizedEndpoint::Http {
                ref base_url,
                has_path: false,
                ..
            } = endpoint
            {
                // Fallback for older Ollama servers serving `/api/tags`
                let ollama_tags_url = format!("{base_url}/api/tags");
                if let Ok((200..=299, _, tags_body)) = self.execute_http_json(
                    &endpoint,
                    &ollama_tags_url,
                    "GET",
                    None,
                    Some(self.timeout),
                    false,
                ) {
                    return ModelDescriptor::parse_models_response(&tags_body)
                        .map_err(TransportError::Message);
                }
            }
        }

        if code == 401 || code == 403 {
            return Err(TransportError::HttpStatus {
                status: code,
                body: format!("Unauthorized on GET {models_url}: {body}"),
            });
        }
        if !(200..300).contains(&code) {
            return Err(TransportError::HttpStatus { status: code, body });
        }

        ModelDescriptor::parse_models_response(&body).map_err(TransportError::Message)
    }

    fn stream_chat_completion(
        &self,
        req: &mut ChatCompletionRequest,
        on_chunk: &mut dyn FnMut(&ChatCompletionChunk),
    ) -> Result<ChatTurnSummary, String> {
        validate_model_for_role(AgentExecutionRole::MainOrchestrator, &req.model)?;
        if crate::is_turn_cancel_requested() {
            return Err(TransportError::Cancelled.to_string());
        }

        let raw_base = self.base_url();
        let endpoint = normalize_and_parse_endpoint(&raw_base)?;
        let full_url = resolve_openai_endpoint_url(&raw_base, "chat/completions")?;

        req.stream = true;
        let mut wire_req = req.sanitized_for_openai();

        match self.stream_chat_once(&endpoint, &full_url, &wire_req, on_chunk) {
            Ok(summary) => Ok(summary),
            Err(TransportError::HttpStatus { status: 400, body })
                if wire_req.stream_options.is_some()
                    && body.to_ascii_lowercase().contains("stream_options") =>
            {
                wire_req.stream_options = None;
                self.stream_chat_once(&endpoint, &full_url, &wire_req, on_chunk)
                    .map_err(|e| e.to_string())
            }
            Err(e) => Err(e.to_string()),
        }
    }

    fn run_subagent_completion(
        &self,
        req: &SubagentCompletionRequest,
    ) -> Result<SubagentCompletionResponse, TransportError> {
        if crate::is_turn_cancel_requested() {
            return Err(TransportError::Cancelled);
        }
        let mut wire_json = Self::build_subagent_wire_json(req)?;
        let raw_base = self.base_url();
        let endpoint =
            normalize_and_parse_endpoint(&raw_base).map_err(TransportError::InvalidUrl)?;
        let full_url = resolve_openai_endpoint_url(&raw_base, "chat/completions")
            .map_err(TransportError::InvalidUrl)?;

        match self.run_subagent_once(&endpoint, &full_url, &wire_json, &req.model) {
            Ok(resp) => Ok(resp),
            Err(TransportError::HttpStatus { status: 400, body })
                if wire_json.get("stream_options").is_some()
                    && body.to_ascii_lowercase().contains("stream_options") =>
            {
                if let Some(obj) = wire_json.as_object_mut() {
                    obj.remove("stream_options");
                }
                self.run_subagent_once(&endpoint, &full_url, &wire_json, &req.model)
            }
            Err(e) => Err(e),
        }
    }

    fn get_telemetry(&self) -> Result<TelemetryResponse, String> {
        let models = self.list_models().map_err(|e| e.to_string())?;
        Ok(TelemetryResponse {
            engine_online: true,
            engine_version: models
                .first()
                .map(|m| m.id.clone())
                .unwrap_or_else(|| "openai-compatible".to_string()),
            api_schema_version: "openai.v1".to_string(),
            transport_mode: format!("OpenAI HTTP {}", self.base_url()),
            ..Default::default()
        })
    }
}

// ============================================================================
// Unit Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use crate::contract::{WireToolFunction, TIMER_ALARM_MARKER};
    use std::io::{Read, Write};
    use std::net::TcpListener;

    #[test]
    fn test_url_normalization_and_openai_resource_resolution() {
        // Bare host without path -> appends /v1/{resource}
        assert_eq!(
            resolve_openai_endpoint_url("https://api.openai.com", "models").unwrap(),
            "https://api.openai.com/v1/models"
        );
        assert_eq!(
            resolve_openai_endpoint_url("https://api.openai.com/", "chat/completions").unwrap(),
            "https://api.openai.com/v1/chat/completions"
        );
        assert_eq!(
            resolve_openai_endpoint_url("http://localhost:11434", "chat/completions").unwrap(),
            "http://localhost:11434/v1/chat/completions"
        );
        assert_eq!(
            resolve_openai_endpoint_url("http://127.0.0.1:8000", "models").unwrap(),
            "http://127.0.0.1:8000/v1/models"
        );
        assert_eq!(
            resolve_openai_endpoint_url("http://192.168.1.50:8080", "chat/completions").unwrap(),
            "http://192.168.1.50:8080/v1/chat/completions"
        );

        // Already has a path component -> appends /{resource} directly
        assert_eq!(
            resolve_openai_endpoint_url("https://api.openai.com/v1", "models").unwrap(),
            "https://api.openai.com/v1/models"
        );
        assert_eq!(
            resolve_openai_endpoint_url("https://openrouter.ai/api/v1", "chat/completions")
                .unwrap(),
            "https://openrouter.ai/api/v1/chat/completions"
        );
        assert_eq!(
            resolve_openai_endpoint_url(
                "https://generativelanguage.googleapis.com/v1beta/openai",
                "models"
            )
            .unwrap(),
            "https://generativelanguage.googleapis.com/v1beta/openai/models"
        );
        assert_eq!(
            resolve_openai_endpoint_url("http://localhost:11434/v1/", "models").unwrap(),
            "http://localhost:11434/v1/models"
        );

        // Strips accidental trailing /chat/completions or /models
        assert_eq!(
            resolve_openai_endpoint_url(
                "https://api.openai.com/v1/chat/completions",
                "models"
            )
            .unwrap(),
            "https://api.openai.com/v1/models"
        );
        assert_eq!(
            resolve_openai_endpoint_url(
                "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions/",
                "chat/completions"
            )
            .unwrap(),
            "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions"
        );
        assert_eq!(
            resolve_openai_endpoint_url("https://api.openai.com/models", "chat/completions")
                .unwrap(),
            "https://api.openai.com/v1/chat/completions"
        );

        // Rejects bad schemes, control chars, internal whitespace, empty URLs
        for bad in [
            "",
            "   ",
            "ftp://example.com",
            "file:///etc/passwd",
            "api.openai.com",
            "https://api.openai.com\r\nX-Inject: 1",
            "https://api.openai.com/v1 path",
            "https://user:pass@api.openai.com/v1",
        ] {
            assert!(
                normalize_and_parse_endpoint(bad).is_err(),
                "Expected error for invalid endpoint '{bad}'"
            );
        }
    }

    #[test]
    fn test_http_transport_models_sse_null_deltas_reasoning_alias_and_sanitization() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();

        let captured_post_body = std::sync::Arc::new(parking_lot::Mutex::new(String::new()));
        let captured_auth = std::sync::Arc::new(parking_lot::Mutex::new(String::new()));
        let captured_ua = std::sync::Arc::new(parking_lot::Mutex::new(String::new()));
        let body_sink = captured_post_body.clone();
        let auth_sink = captured_auth.clone();
        let ua_sink = captured_ua.clone();

        let server = std::thread::spawn(move || {
            // Serve 3 requests:
            // 1. GET /v1/models
            // 2. POST /v1/chat/completions -> 400 complaining about stream_options (tests auto-retry!)
            // 3. POST /v1/chat/completions -> 200 SSE with null fields & "reasoning" alias
            for req_idx in 0..3 {
                let (mut stream, _) = listener.accept().unwrap();
                let mut req_buf = Vec::new();
                let mut tmp = [0u8; 4096];
                let mut content_len = 0usize;
                let mut header_end = None;
                loop {
                    let n = stream.read(&mut tmp).unwrap();
                    if n == 0 {
                        break;
                    }
                    req_buf.extend_from_slice(&tmp[..n]);
                    if header_end.is_none() {
                        if let Some((he, bs)) = find_header_delimiter(&req_buf) {
                            header_end = Some(bs);
                            let hdr_text = String::from_utf8_lossy(&req_buf[..he]);
                            for line in hdr_text.lines() {
                                if let Some((k, v)) = line.split_once(':') {
                                    if k.trim().eq_ignore_ascii_case("content-length") {
                                        content_len = v.trim().parse().unwrap_or(0);
                                    }
                                    if k.trim().eq_ignore_ascii_case("authorization") {
                                        *auth_sink.lock() = v.trim().to_string();
                                    }
                                    if k.trim().eq_ignore_ascii_case("user-agent") {
                                        *ua_sink.lock() = v.trim().to_string();
                                    }
                                }
                            }
                        }
                    }
                    if let Some(bs) = header_end {
                        if req_buf.len() >= bs + content_len {
                            break;
                        }
                    }
                }
                let bs = header_end.unwrap_or(req_buf.len());
                let req_head = String::from_utf8_lossy(&req_buf[..bs]).into_owned();
                let req_body = String::from_utf8_lossy(&req_buf[bs..]).into_owned();

                if req_idx == 0 {
                    assert!(req_head.starts_with("GET /v1/models "), "{req_head}");
                    let json = r#"{"object":"list","data":[{"id":"custom/model-alpha","owned_by":"openrouter","context_length":131072}]}"#;
                    let resp = format!(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                        json.len(),
                        json
                    );
                    stream.write_all(resp.as_bytes()).unwrap();
                } else if req_idx == 1 {
                    assert!(req_head.starts_with("POST /v1/chat/completions "), "{req_head}");
                    let err_json = r#"{"error":{"message":"Unknown parameter: stream_options"}}"#;
                    let resp = format!(
                        "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                        err_json.len(),
                        err_json
                    );
                    stream.write_all(resp.as_bytes()).unwrap();
                } else {
                    *body_sink.lock() = req_body;
                    let sse = concat!(
                        "data: {\"id\":null,\"model\":null,\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":null,\"reasoning\":\"Step 1: inspect.\"},\"finish_reason\":null}]}\n\n",
                        "data: {\"id\":\"cmpl-1\",\"model\":\"custom/model-alpha\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello \",\"tool_calls\":[{\"index\":0,\"id\":null,\"type\":null,\"function\":{\"name\":null,\"arguments\":\"\"}}]},\"finish_reason\":null}]}\n\n",
                        "data: {\"id\":\"cmpl-1\",\"model\":\"custom/model-alpha\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Spell!\"},\"finish_reason\":\"stop\"}]}\n\n",
                        "data: {\"id\":\"cmpl-1\",\"model\":\"custom/model-alpha\",\"choices\":[],\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":8,\"total_tokens\":50,\"prompt_tokens_details\":{\"cached_tokens\":null}}}\n\n",
                        "data: [DONE]\n\n"
                    );
                    let resp = format!(
                        "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                        sse.len(),
                        sse
                    );
                    stream.write_all(resp.as_bytes()).unwrap();
                }
                let _ = stream.flush();
            }
        });

        let client = HttpTransport::with_api_key(format!("http://{addr}"), "sk-test-secret-key");

        // 1. GET /v1/models
        let models = client.list_models().unwrap();
        assert_eq!(models.len(), 1);
        assert_eq!(models[0].id, "custom/model-alpha");
        assert_eq!(models[0].context_window, Some(131_072));
        assert_eq!(*captured_auth.lock(), "Bearer sk-test-secret-key");
        assert_eq!(*captured_ua.lock(), "SpellIDE/1.0");

        // 2. Streaming SSE with prior assistant message containing reasoning_content + tool_call.index
        //    (must be sanitized outbound) + auto-retry on HTTP 400 stream_options + 100 Continue drain
        let mut req = ChatCompletionRequest {
            model: "custom/model-alpha".to_string(),
            messages: vec![
                WireMessage::system("System prompt"),
                WireMessage::user("First turn"),
                WireMessage::assistant(
                    Some(String::new()),
                    Some("Private reasoning retained in SQLite".to_string()),
                    Some(vec![WireToolCall {
                        id: "call_prev_1".to_string(),
                        r#type: "function".to_string(),
                        function: WireToolFunction {
                            name: "read_file".to_string(),
                            arguments: "{\"path\":\"README.md\"}".to_string(),
                        },
                        index: Some(0),
                    }]),
                ),
                WireMessage::tool("call_prev_1", Some("read_file".to_string()), "readme body"),
            ],
            ..Default::default()
        };

        let mut chunk_count = 0usize;
        let summary = client
            .stream_chat_completion(&mut req, &mut |_c| {
                chunk_count += 1;
            })
            .unwrap();
        server.join().unwrap();

        assert!(chunk_count >= 3);
        assert_eq!(summary.model, "custom/model-alpha");
        assert_eq!(summary.reasoning_content, "Step 1: inspect.");
        assert_eq!(summary.content, "Hello Spell!");
        assert_eq!(summary.usage.prompt_tokens, 42);
        assert_eq!(summary.usage.completion_tokens, 8);
        assert!(summary.context_advice.decode_tok_s > 0.0);

        // Verify local req.messages kept reasoning_content intact, while outbound wire JSON stripped it!
        assert_eq!(
            req.messages[2].reasoning_content.as_deref(),
            Some("Private reasoning retained in SQLite")
        );
        let sent_val: Value = serde_json::from_str(&captured_post_body.lock()).unwrap();
        assert!(sent_val.get("stream_options").is_none(), "retry must drop stream_options");
        assert!(sent_val["messages"][2].get("reasoning_content").is_none());
        assert!(sent_val["messages"][2]["tool_calls"][0].get("index").is_none());
    }

    #[test]
    fn test_http_transport_ollama_tags_fallback_without_auth_and_non_streaming_json() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();

        let server = std::thread::spawn(move || {
            for req_idx in 0..3 {
                let (mut stream, _) = listener.accept().unwrap();
                let mut req_buf = Vec::new();
                let mut tmp = [0u8; 4096];
                let mut content_len = 0usize;
                let mut header_end = None;
                let mut saw_auth = false;
                loop {
                    let n = stream.read(&mut tmp).unwrap();
                    if n == 0 {
                        break;
                    }
                    req_buf.extend_from_slice(&tmp[..n]);
                    if header_end.is_none() {
                        if let Some((he, bs)) = find_header_delimiter(&req_buf) {
                            header_end = Some(bs);
                            let hdr_text = String::from_utf8_lossy(&req_buf[..he]);
                            for line in hdr_text.lines() {
                                if let Some((k, v)) = line.split_once(':') {
                                    if k.trim().eq_ignore_ascii_case("content-length") {
                                        content_len = v.trim().parse().unwrap_or(0);
                                    }
                                    if k.trim().eq_ignore_ascii_case("authorization") {
                                        saw_auth = true;
                                    }
                                }
                            }
                        }
                    }
                    if let Some(bs) = header_end {
                        if req_buf.len() >= bs + content_len {
                            break;
                        }
                    }
                }
                assert!(
                    !saw_auth,
                    "without_auth() must not send an Authorization header"
                );
                let bs = header_end.unwrap_or(req_buf.len());
                let req_head = String::from_utf8_lossy(&req_buf[..bs]).into_owned();

                if req_idx == 0 {
                    assert!(req_head.starts_with("GET /v1/models "), "{req_head}");
                    let resp = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
                    stream.write_all(resp.as_bytes()).unwrap();
                } else if req_idx == 1 {
                    assert!(req_head.starts_with("GET /api/tags "), "{req_head}");
                    let body = r#"{"models":[{"name":"qwen2.5-coder:7b"}]}"#;
                    let resp = format!(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                        body.len(),
                        body
                    );
                    stream.write_all(resp.as_bytes()).unwrap();
                } else {
                    assert!(
                        req_head.starts_with("POST /v1/chat/completions "),
                        "{req_head}"
                    );
                    let body = r#"{"id":"chatcmpl-nonstream","model":"qwen2.5-coder:7b","choices":[{"index":0,"message":{"role":"assistant","content":"Non-streaming reply"},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":4,"total_tokens":14}}"#;
                    let resp = format!(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                        body.len(),
                        body
                    );
                    stream.write_all(resp.as_bytes()).unwrap();
                }
                let _ = stream.flush();
            }
        });

        let client = HttpTransport::without_auth(format!("http://{addr}"));
        let models = client.list_models().unwrap();
        assert_eq!(models.len(), 1);
        assert_eq!(models[0].id, "qwen2.5-coder:7b");

        let mut req = ChatCompletionRequest::new(
            "qwen2.5-coder:7b",
            vec![WireMessage::user("ping")],
        );
        let mut emitted = 0usize;
        let summary = client
            .stream_chat_completion(&mut req, &mut |_| emitted += 1)
            .unwrap();
        server.join().unwrap();

        assert_eq!(emitted, 1);
        assert_eq!(summary.content, "Non-streaming reply");
        assert_eq!(summary.usage.completion_tokens, 4);
        assert!(summary.context_advice.decode_tok_s > 0.0);
    }

    #[test]
    fn test_line_stream_handle_fast_cancellation() {
        struct StallingReader {
            sent_first: bool,
            unblock_rx: mpsc::Receiver<()>,
        }

        impl Read for StallingReader {
            fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
                if !self.sent_first {
                    self.sent_first = true;
                    let first = b"data: {\"chunk\":1}\n";
                    let n = first.len().min(buf.len());
                    buf[..n].copy_from_slice(&first[..n]);
                    return Ok(n);
                }
                let _ = self.unblock_rx.recv_timeout(Duration::from_secs(5));
                Ok(0)
            }
        }

        let (unblock_tx, unblock_rx) = mpsc::channel();
        let reader = StallingReader {
            sent_first: false,
            unblock_rx,
        };
        let cancel_flag = Arc::new(AtomicBool::new(false));
        let stream = spawn_line_reader(Box::new(reader), cancel_flag).unwrap();

        let local_cancel = Arc::new(AtomicBool::new(false));
        let first = stream
            .next_line_with(|| local_cancel.load(Ordering::SeqCst))
            .unwrap();
        assert_eq!(first.as_deref(), Some("data: {\"chunk\":1}\n"));

        let trigger = Arc::clone(&local_cancel);
        let canceller = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(20));
            trigger.store(true, Ordering::SeqCst);
        });

        let started = Instant::now();
        let err = stream
            .next_line_with(|| local_cancel.load(Ordering::SeqCst))
            .unwrap_err();
        let elapsed = started.elapsed();

        let _ = unblock_tx.send(());
        canceller.join().unwrap();

        assert_eq!(err, TransportError::Cancelled);
        assert!(
            elapsed < Duration::from_millis(400),
            "Streaming cancellation took too long: {elapsed:?}"
        );
    }

    #[cfg(unix)]
    #[test]
    fn test_unix_socket_http11_chunked_models_and_sse() {
        use std::os::unix::net::UnixListener;

        let sock_path = PathBuf::from(format!("/tmp/bt_tr_u_{}.sock", std::process::id()));
        let _ = std::fs::remove_file(&sock_path);
        let listener = UnixListener::bind(&sock_path).unwrap();

        let server = std::thread::spawn(move || {
            for req_idx in 0..2 {
                let (mut stream, _) = listener.accept().unwrap();
                let mut req_buf = Vec::new();
                let mut tmp = [0u8; 4096];
                let mut content_len = 0usize;
                let mut header_end = None;
                loop {
                    let n = stream.read(&mut tmp).unwrap();
                    if n == 0 {
                        break;
                    }
                    req_buf.extend_from_slice(&tmp[..n]);
                    if header_end.is_none() {
                        if let Some((he, bs)) = find_header_delimiter(&req_buf) {
                            header_end = Some(bs);
                            let hdr_text = String::from_utf8_lossy(&req_buf[..he]);
                            for line in hdr_text.lines() {
                                if let Some((k, v)) = line.split_once(':') {
                                    if k.trim().eq_ignore_ascii_case("content-length") {
                                        content_len = v.trim().parse().unwrap_or(0);
                                    }
                                }
                            }
                        }
                    }
                    if let Some(bs) = header_end {
                        if req_buf.len() >= bs + content_len {
                            break;
                        }
                    }
                }
                if req_idx == 0 {
                    let c1 = r#"{"object":"list","data":[{"id":"#;
                    let c2 = r#""unix-local-model"}]}"#;
                    let resp = format!(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n{:x}\r\n{}\r\n{:x}\r\n{}\r\n0\r\n\r\n",
                        c1.len(),
                        c1,
                        c2.len(),
                        c2
                    );
                    stream.write_all(resp.as_bytes()).unwrap();
                } else {
                    let f1 = "data: {\"id\":\"u-1\",\"model\":\"unix-local-model\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hi \"},\"finish_reason\":null}]}\n\n";
                    let f2 = "data: {\"id\":\"u-1\",\"model\":\"unix-local-model\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Unix!\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
                    let resp = format!(
                        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n{:x}\r\n{}\r\n{:x}\r\n{}\r\n0\r\n\r\n",
                        f1.len(),
                        f1,
                        f2.len(),
                        f2
                    );
                    stream.write_all(resp.as_bytes()).unwrap();
                }
                let _ = stream.flush();
            }
        });

        let client = HttpTransport::with_api_key(
            format!("unix://{}", sock_path.display()),
            "sk-unix-test",
        );
        let models = client.list_models().unwrap();
        assert_eq!(models.len(), 1);
        assert_eq!(models[0].id, "unix-local-model");

        let mut req = ChatCompletionRequest::new(
            "unix-local-model",
            vec![WireMessage::user("hello unix")],
        );
        let summary = client.stream_chat_completion(&mut req, &mut |_| {}).unwrap();
        server.join().unwrap();
        let _ = std::fs::remove_file(&sock_path);

        assert_eq!(summary.content, "Hi Unix!");
        assert_eq!(summary.model, "unix-local-model");
    }

    #[test]
    fn test_alarm_header_placement_golden_fixture() {
        let fixture_raw = include_str!("../tests/fixtures/alarms_header_v1.json");
        let fixture_val: Value =
            serde_json::from_str(fixture_raw).expect("alarms_header_v1.json must be valid JSON");

        let initial_messages: Vec<WireMessage> =
            serde_json::from_value(fixture_val["initial_messages"].clone()).unwrap();
        let expected_patched: Vec<WireMessage> =
            serde_json::from_value(fixture_val["expected_patched_messages"].clone()).unwrap();
        let banners: Vec<String> =
            serde_json::from_value(fixture_val["expected_alarms_json"].clone()).unwrap();

        let mut msgs = initial_messages.clone();
        apply_engine_alarm_banners_to_messages(&mut msgs, &banners);
        assert_eq!(msgs, expected_patched);
        assert_eq!(msgs[4], initial_messages[4]);
        assert!(msgs[3].content.as_deref().unwrap_or("").contains(TIMER_ALARM_MARKER));

        // Idempotent on re-apply
        apply_engine_alarm_banners_to_messages(&mut msgs, &banners);
        assert_eq!(msgs, expected_patched);
    }
}
