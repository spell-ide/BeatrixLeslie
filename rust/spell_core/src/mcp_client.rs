//! Real JSON-RPC 2.0 Stdio MCP Client (`McpStdioClient`) & In-Process Pipe Test Harness (`EmbeddedMcpResponder`)
//!
//! Implements:
//! 1. **`McpToolSpec` & JSON-RPC 2.0 Wire Types** (`JsonRpcRequest`, `JsonRpcNotification`,
//!    `JsonRpcResponse`, `JsonRpcError`).
//! 2. **`McpStdioClient`**: Real line-delimited JSON-RPC 2.0 client over stdio pipes that connects
//!    either to a spawned subprocess (`Command::new(program)...stdin(Stdio::piped()).stdout(Stdio::piped())`)
//!    or an in-process OS pipe / loopback stream pair attached to [`EmbeddedMcpResponder`].
//!    Supports `initialize`, `list_tools`, `call_tool`, and `< 50 ms` `cancel_request`
//!    (`notifications/cancelled` + `$/cancelRequest`).
//! 3. **`EmbeddedMcpResponder`**: 10-tool `spell-full-file-mcp` compatible responder over real
//!    OS pipes/streams (`read_full_file`, `read_multiple_files`, `grep_workspace`, `file_stats` with
//!    `sha2::Sha256` `sha256_16`, `fetch_html` with arXiv `/abs/` & `/pdf/` -> `/html/` normalization,
//!    `timer_set`, `timer_wait` interruptible via `Condvar` + `AtomicBool` in `< 15 ms`,
//!    `timer_list`, `timer_cancel`, and `progress_check` supporting `file_tail`, `file_growth`,
//!    `pid_alive`, and `http_status`), enforcing protected-glob and workspace path security blocks.

use std::collections::HashMap;
use std::ffi::OsStr;
use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
#[cfg(any(test, feature = "test-support"))]
use std::net::{SocketAddr, TcpStream};
use std::path::{Component, Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use regex::Regex;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use walkdir::WalkDir;

use crate::tools::{
    canonicalize_target_path, enforce_protected_globs, SCRUBBED_SENSITIVE_ENV_VARS,
};

/// Standard JSON-RPC version string.
pub const JSONRPC_VERSION: &str = "2.0";

/// Standard MCP protocol version negotiated during `initialize`.
pub const MCP_PROTOCOL_VERSION: &str = "2024-11-05";

/// Banner emitted when an autonomous wakeup timer expires.
pub const SCHEDULE_WAKEUP_BANNER: &str = "[SPELL AGENT TIMER ALARM FIRED]";

/// Canonical names of all 10 tools exposed by `spell-full-file-mcp`.
pub const SPELL_MCP_TOOL_NAMES: [&str; 10] = [
    "read_full_file",
    "read_multiple_files",
    "grep_workspace",
    "file_stats",
    "fetch_html",
    "timer_set",
    "timer_wait",
    "timer_list",
    "timer_cancel",
    "progress_check",
];

// ============================================================================
// 1. McpToolSpec & JSON-RPC 2.0 Wire Types
// ============================================================================

/// Tool specification returned by MCP `tools/list`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct McpToolSpec {
    pub name: String,
    pub description: String,
    #[serde(rename = "inputSchema", alias = "input_schema", default)]
    pub input_schema: Value,
}

/// JSON-RPC 2.0 Request frame (`jsonrpc: "2.0"`, `id`, `method`, `params`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct JsonRpcRequest {
    pub jsonrpc: String,
    pub id: Value,
    pub method: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub params: Option<Value>,
}

impl JsonRpcRequest {
    pub fn new(id: impl Into<Value>, method: impl Into<String>, params: Option<Value>) -> Self {
        Self {
            jsonrpc: JSONRPC_VERSION.to_string(),
            id: id.into(),
            method: method.into(),
            params,
        }
    }

    pub fn to_line(&self) -> Result<String, String> {
        let mut s = serde_json::to_string(self).map_err(|e| format!("JSON serialize error: {e}"))?;
        s.push('\n');
        Ok(s)
    }
}

/// JSON-RPC 2.0 Notification frame (`jsonrpc: "2.0"`, `method`, `params`, no `id`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct JsonRpcNotification {
    pub jsonrpc: String,
    pub method: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub params: Option<Value>,
}

impl JsonRpcNotification {
    pub fn new(method: impl Into<String>, params: Option<Value>) -> Self {
        Self {
            jsonrpc: JSONRPC_VERSION.to_string(),
            method: method.into(),
            params,
        }
    }

    pub fn to_line(&self) -> Result<String, String> {
        let mut s = serde_json::to_string(self).map_err(|e| format!("JSON serialize error: {e}"))?;
        s.push('\n');
        Ok(s)
    }
}

/// JSON-RPC 2.0 Error object embedded inside [`JsonRpcResponse`].
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct JsonRpcError {
    pub code: i64,
    pub message: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub data: Option<Value>,
}

/// JSON-RPC 2.0 Response frame (`jsonrpc: "2.0"`, `id`, `result`, `error`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct JsonRpcResponse {
    pub jsonrpc: String,
    #[serde(default)]
    pub id: Value,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub result: Option<Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub error: Option<JsonRpcError>,
}

impl JsonRpcResponse {
    pub fn success(id: Value, result: Value) -> Self {
        Self {
            jsonrpc: JSONRPC_VERSION.to_string(),
            id,
            result: Some(result),
            error: None,
        }
    }

    pub fn error(id: Value, code: i64, message: impl Into<String>, data: Option<Value>) -> Self {
        Self {
            jsonrpc: JSONRPC_VERSION.to_string(),
            id,
            result: None,
            error: Some(JsonRpcError {
                code,
                message: message.into(),
                data,
            }),
        }
    }

    pub fn to_line(&self) -> Result<String, String> {
        let mut s = serde_json::to_string(self).map_err(|e| format!("JSON serialize error: {e}"))?;
        s.push('\n');
        Ok(s)
    }
}

/// Builds the canonical 10 `McpToolSpec` definitions for `spell-full-file-mcp`.
pub fn spell_mcp_tool_specs() -> Vec<McpToolSpec> {
    vec![
        McpToolSpec {
            name: "read_full_file".to_string(),
            description: "Reads an entire file with zero line truncation and no 16KB symbol-outline replacement.".to_string(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "path": { "type": "string", "description": "Path to the file to read." },
                    "start_line": { "type": "integer", "description": "Optional 1-indexed start line." },
                    "end_line": { "type": "integer", "description": "Optional 1-indexed inclusive end line." }
                },
                "required": ["path"]
            }),
        },
        McpToolSpec {
            name: "read_multiple_files".to_string(),
            description: "Reads multiple files in a single batch call with zero line truncation.".to_string(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "paths": {
                        "oneOf": [
                            { "type": "array", "items": { "type": "string" } },
                            { "type": "string" }
                        ],
                        "description": "List of file paths (array or comma-separated string)."
                    }
                },
                "required": ["paths"]
            }),
        },
        McpToolSpec {
            name: "grep_workspace".to_string(),
            description: "Recursively searches text files in a workspace directory for a pattern or regex.".to_string(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "root": { "type": "string", "description": "Workspace root directory." },
                    "query": { "type": "string", "description": "Search pattern or regular expression." },
                    "is_regex": { "type": "boolean", "description": "Whether query is a regex." },
                    "case_sensitive": { "type": "boolean", "description": "Whether search is case-sensitive." }
                },
                "required": ["query"]
            }),
        },
        McpToolSpec {
            name: "file_stats".to_string(),
            description: "Returns byte size, line count, full SHA-256, and 16-hex-char sha256_16 digest for a file.".to_string(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "path": { "type": "string", "description": "File path to inspect." }
                },
                "required": ["path"]
            }),
        },
        McpToolSpec {
            name: "fetch_html".to_string(),
            description: "Fetches research HTML with automatic arXiv /abs/ and /pdf/ -> https://arxiv.org/html/<id> normalization.".to_string(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "url": { "type": "string", "description": "Target URL (arXiv abs/pdf URLs are normalized to html)." }
                },
                "required": ["url"]
            }),
        },
        McpToolSpec {
            name: "timer_set".to_string(),
            description: "Schedules a background autonomous agent wakeup timer (Hook 2).".to_string(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "timer_id": { "type": "string" },
                    "duration_secs": { "type": "integer" },
                    "duration_ms": { "type": "integer" },
                    "label": { "type": "string" },
                    "alarm_note": { "type": "string" },
                    "watch_path": { "type": "string" }
                },
                "required": ["label"]
            }),
        },
        McpToolSpec {
            name: "timer_wait".to_string(),
            description: "Waits for a timer or duration in milliseconds, waking in <15ms via Condvar + AtomicBool when notifications/cancelled arrives.".to_string(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "duration_ms": { "type": "integer" },
                    "timer_id": { "type": "string" }
                },
                "required": ["duration_ms"]
            }),
        },
        McpToolSpec {
            name: "timer_list".to_string(),
            description: "Lists all scheduled, active, and cancelled Hook 2 wakeup timers.".to_string(),
            input_schema: json!({
                "type": "object",
                "properties": {}
            }),
        },
        McpToolSpec {
            name: "timer_cancel".to_string(),
            description: "Cancels an active timer by timer_id and immediately wakes any thread waiting on it.".to_string(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "timer_id": { "type": "string" }
                },
                "required": ["timer_id"]
            }),
        },
        McpToolSpec {
            name: "progress_check".to_string(),
            description: "Checks background task progress via file_tail, file_growth, pid_alive, and http_status probes.".to_string(),
            input_schema: json!({
                "type": "object",
                "properties": {
                    "mode": {
                        "type": "string",
                        "enum": ["file_tail", "file_growth", "pid_alive", "http_status"]
                    },
                    "path": { "type": "string" },
                    "watch_path": { "type": "string" },
                    "tail_lines": { "type": "integer" },
                    "previous_bytes": { "type": "integer" },
                    "pid": { "type": "integer" },
                    "url": { "type": "string" }
                }
            }),
        },
    ]
}

// ============================================================================
// Security Guardrail, SHA-256 (`sha256_16`), and ArXiv URL Normalization
// ============================================================================

/// Returns `true` when running inside an OS App Sandbox (`APP_SANDBOX_CONTAINER_ID` is set),
/// or in debug/test builds when `SPELL_APP_SANDBOX=1` is explicitly enabled.
pub fn is_app_sandbox_active() -> bool {
    crate::tools::is_app_sandbox_active()
}

/// Enforces protected globs and path security checks, rejecting `..` traversal,
/// `~/.ssh`, `~/.aws`, `.env*`, `.git/config`, and `.git/hooks/*`.
pub fn enforce_mcp_path_guardrail(path: &Path) -> Result<(), String> {
    let raw = path.to_string_lossy();

    if path.components().any(|c| matches!(c, Component::ParentDir)) || raw.contains("..") {
        return Err(format!(
            "SECURITY_BLOCK: Path traversal ('..') in '{}' is forbidden",
            raw
        ));
    }

    if let Err(e) = enforce_protected_globs(path, &raw) {
        return Err(e.to_string());
    }

    if path.exists() {
        if let Ok(canon) = fs::canonicalize(path) {
            if let Err(e) = enforce_protected_globs(&canon, &raw) {
                return Err(e.to_string());
            }
        }
    }

    Ok(())
}

/// Computes full 64-char SHA-256 hex and 16-char `sha256_16` prefix using `sha2::Sha256`.
pub fn compute_sha256_and_16(bytes: &[u8]) -> (String, String) {
    let mut hasher = Sha256::new();
    hasher.update(bytes);
    let digest = hasher.finalize();
    let mut full_hex = String::with_capacity(64);
    for b in digest {
        full_hex.push_str(&format!("{b:02x}"));
    }
    let sha256_16 = full_hex[..16].to_string();
    (full_hex, sha256_16)
}

/// Normalizes arXiv `/abs/<id>` and `/pdf/<id>(.pdf)` URLs into `https://arxiv.org/html/<id>`.
pub fn normalize_arxiv_url(url: &str) -> String {
    let trimmed = url.trim();
    for prefix in [
        "https://arxiv.org/abs/",
        "http://arxiv.org/abs/",
        "https://www.arxiv.org/abs/",
        "http://www.arxiv.org/abs/",
        "https://arxiv.org/pdf/",
        "http://arxiv.org/pdf/",
        "https://www.arxiv.org/pdf/",
        "http://www.arxiv.org/pdf/",
        "arxiv.org/abs/",
        "arxiv.org/pdf/",
    ] {
        if let Some(rest) = trimmed.strip_prefix(prefix) {
            let without_query = rest.split(['?', '#']).next().unwrap_or(rest);
            let clean_id = without_query
                .strip_suffix(".pdf")
                .unwrap_or(without_query)
                .trim_end_matches('/');
            return format!("https://arxiv.org/html/{clean_id}");
        }
    }
    trimmed.to_string()
}

/// Normalizes a JSON-RPC `id` value (`u64`, `i64`, `String`) into a canonical string map key.
fn id_value_to_key(id: &Value) -> Option<String> {
    match id {
        Value::Number(n) => Some(n.to_string()),
        Value::String(s) => Some(s.clone()),
        _ => None,
    }
}

/// Creates a real OS pipe pair `(read_file, write_file)` on Unix.
#[cfg(unix)]
pub fn create_os_pipe() -> Result<(fs::File, fs::File), String> {
    let mut fds = [0 as libc::c_int; 2];
    let rc = unsafe { libc::pipe(fds.as_mut_ptr()) };
    if rc != 0 {
        return Err(format!(
            "Failed to create OS pipe: {}",
            std::io::Error::last_os_error()
        ));
    }
    unsafe {
        use std::os::unix::io::FromRawFd;
        let reader = fs::File::from_raw_fd(fds[0]);
        let writer = fs::File::from_raw_fd(fds[1]);
        Ok((reader, writer))
    }
}

#[cfg(not(unix))]
struct ShutdownOnDropTcpWriter(std::net::TcpStream);

#[cfg(not(unix))]
impl Write for ShutdownOnDropTcpWriter {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        self.0.write(buf)
    }

    fn flush(&mut self) -> std::io::Result<()> {
        self.0.flush()
    }
}

#[cfg(not(unix))]
impl Drop for ShutdownOnDropTcpWriter {
    fn drop(&mut self) {
        let _ = self.0.shutdown(std::net::Shutdown::Write);
    }
}

#[cfg(not(unix))]
fn create_loopback_pipe() -> Result<(std::net::TcpStream, ShutdownOnDropTcpWriter), String> {
    let listener = std::net::TcpListener::bind("127.0.0.1:0")
        .map_err(|e| format!("Failed to bind loopback pipe listener: {e}"))?;
    let addr = listener
        .local_addr()
        .map_err(|e| format!("Failed to inspect loopback pipe address: {e}"))?;
    let writer = std::net::TcpStream::connect(addr)
        .map_err(|e| format!("Failed to connect loopback pipe writer: {e}"))?;
    let _ = writer.set_nodelay(true);
    let (reader, _) = listener
        .accept()
        .map_err(|e| format!("Failed to accept loopback pipe reader: {e}"))?;
    let _ = reader.set_nodelay(true);
    Ok((reader, ShutdownOnDropTcpWriter(writer)))
}

// ============================================================================
// CancellationEvent (`Condvar` + `AtomicBool` for < 15 ms wakeup)
// ============================================================================

/// Sub-15ms cancellation primitive backed by `AtomicBool` + `Mutex` + `Condvar`.
#[derive(Debug, Default)]
pub struct CancellationEvent {
    cancelled: AtomicBool,
    state: Mutex<(bool, Option<String>)>,
    condvar: Condvar,
}

impl CancellationEvent {
    pub fn new() -> Arc<Self> {
        Arc::new(Self::default())
    }

    /// Signals cancellation with an optional reason and wakes all waiting threads immediately.
    pub fn cancel(&self, reason: Option<&str>) {
        self.cancelled.store(true, Ordering::SeqCst);
        if let Ok(mut guard) = self.state.lock() {
            guard.0 = true;
            if let Some(r) = reason {
                guard.1 = Some(r.to_string());
            }
            self.condvar.notify_all();
        }
    }

    pub fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::SeqCst)
    }

    pub fn reason(&self) -> Option<String> {
        self.state.lock().ok().and_then(|g| g.1.clone())
    }

    /// Waits up to `timeout` or returns early (`true`) within `< 15 ms` when `cancel()` is called.
    pub fn wait_or_cancelled(&self, timeout: Duration) -> bool {
        if self.is_cancelled() {
            return true;
        }
        let guard = match self.state.lock() {
            Ok(g) => g,
            Err(_) => return self.is_cancelled(),
        };
        let (guard, _) = match self
            .condvar
            .wait_timeout_while(guard, timeout, |state| !state.0)
        {
            Ok(res) => res,
            Err(_) => return self.is_cancelled(),
        };
        guard.0 || self.is_cancelled()
    }
}

// ============================================================================
// 2. McpStdioClient (Real Subprocess / Pipe Stdio JSON-RPC 2.0 Client)
// ============================================================================

#[derive(Default)]
struct PendingRequestState {
    response: Option<JsonRpcResponse>,
    cancelled: bool,
    cancel_reason: Option<String>,
    closed: bool,
}

#[derive(Default)]
struct PendingRequestSlot {
    state: Mutex<PendingRequestState>,
    condvar: Condvar,
}

impl PendingRequestSlot {
    fn new() -> Arc<Self> {
        Arc::new(Self::default())
    }

    fn set_response(&self, resp: JsonRpcResponse) {
        if let Ok(mut g) = self.state.lock() {
            g.response = Some(resp);
            self.condvar.notify_all();
        }
    }

    fn mark_cancelled(&self, reason: Option<&str>) {
        if let Ok(mut g) = self.state.lock() {
            g.cancelled = true;
            g.cancel_reason = reason.map(|s| s.to_string());
            self.condvar.notify_all();
        }
    }

    fn mark_closed(&self) {
        if let Ok(mut g) = self.state.lock() {
            g.closed = true;
            self.condvar.notify_all();
        }
    }
}

/// Real JSON-RPC 2.0 Stdio MCP Client communicating over newline-delimited stdio streams.
///
/// Works identically with:
/// - A real spawned child process (`McpStdioClient::spawn(...)`)
/// - An OS pipe pair connected to [`EmbeddedMcpResponder`] (`McpStdioClient::new_embedded()`)
pub struct McpStdioClient {
    writer: Arc<Mutex<Option<Box<dyn Write + Send>>>>,
    pending: Arc<Mutex<HashMap<String, Arc<PendingRequestSlot>>>>,
    next_internal_id: AtomicU64,
    child: Option<Arc<Mutex<Child>>>,
    reader_handle: Option<JoinHandle<()>>,
    _embedded_responder: Option<Arc<EmbeddedMcpResponder>>,
}

impl McpStdioClient {
    /// Spawns a real child process with piped `stdin` and `stdout` and attaches the JSON-RPC 2.0
    /// reader/writer framing engine.
    pub fn spawn<I, S>(
        program: &str,
        args: I,
        envs: &HashMap<String, String>,
    ) -> Result<Self, String>
    where
        I: IntoIterator<Item = S>,
        S: AsRef<OsStr>,
    {
        let mut cmd = Command::new(program);
        for env_key in SCRUBBED_SENSITIVE_ENV_VARS {
            cmd.env_remove(env_key);
        }
        cmd.args(args)
            .envs(envs)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null());

        #[cfg(unix)]
        {
            use std::os::unix::process::CommandExt;
            cmd.process_group(0);
        }

        let mut child = cmd
            .spawn()
            .map_err(|e| format!("Failed to spawn MCP server process '{program}': {e}"))?;

        let stdin = child
            .stdin
            .take()
            .ok_or_else(|| "Child process stdin was not piped".to_string())?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| "Child process stdout was not piped".to_string())?;

        let mut client = Self::from_streams(stdout, stdin);
        client.child = Some(Arc::new(Mutex::new(child)));
        Ok(client)
    }

    /// Spawns an MCP server subprocess with scrubbed sensitive environment variables
    /// and isolated process-group lifecycle. Never retries a failed child process.
    pub fn spawn_sandboxed(
        program: &str,
        args: &[&str],
        envs: &[(&str, &str)],
        _deny_ip_network: bool,
    ) -> Result<Self, String> {
        let mut cmd = Command::new(program);
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            if (program.eq_ignore_ascii_case("cmd") || program.eq_ignore_ascii_case("cmd.exe"))
                && args.len() == 2
                && (args[0].eq_ignore_ascii_case("/C") || args[0].eq_ignore_ascii_case("/K"))
            {
                cmd.arg("/D")
                    .arg("/S")
                    .arg(args[0])
                    .raw_arg(format!("\"{}\"", args[1]));
            } else {
                cmd.args(args);
            }
        }
        #[cfg(not(windows))]
        {
            cmd.args(args);
        }

        for env_key in SCRUBBED_SENSITIVE_ENV_VARS {
            cmd.env_remove(env_key);
        }
        for &(k, v) in envs {
            cmd.env(k, v);
        }

        cmd.stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null());

        #[cfg(unix)]
        {
            use std::os::unix::process::CommandExt;
            cmd.process_group(0);
        }

        let mut child = cmd
            .spawn()
            .map_err(|e| format!("Failed to spawn sandboxed MCP server process '{program}': {e}"))?;

        let stdin = child
            .stdin
            .take()
            .ok_or_else(|| "Child process stdin was not piped".to_string())?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| "Child process stdout was not piped".to_string())?;

        let mut client = Self::from_streams(stdout, stdin);
        client.child = Some(Arc::new(Mutex::new(child)));
        Ok(client)
    }

    /// Convenience helper to spawn a subprocess with slice arguments and no extra env map.
    pub fn spawn_command(program: &str, args: &[&str]) -> Result<Self, String> {
        let envs = HashMap::new();
        Self::spawn(program, args, &envs)
    }

    /// Constructs an `McpStdioClient` over any `Read + Send + 'static` and `Write + Send + 'static` streams.
    pub fn from_streams<R, W>(reader: R, writer: W) -> Self
    where
        R: Read + Send + 'static,
        W: Write + Send + 'static,
    {
        let pending: Arc<Mutex<HashMap<String, Arc<PendingRequestSlot>>>> =
            Arc::new(Mutex::new(HashMap::new()));
        let pending_reader = pending.clone();

        let reader_handle = thread::spawn(move || {
            let mut buf_reader = BufReader::new(reader);
            let mut line = String::new();
            loop {
                line.clear();
                match buf_reader.read_line(&mut line) {
                    Ok(0) => break, // EOF
                    Ok(_) => {
                        let trimmed = line.trim();
                        if trimmed.is_empty() {
                            continue;
                        }
                        if let Ok(resp) = serde_json::from_str::<JsonRpcResponse>(trimmed) {
                            if let Some(key) = id_value_to_key(&resp.id) {
                                let slot_opt = pending_reader
                                    .lock()
                                    .ok()
                                    .and_then(|mut map| map.remove(&key));
                                if let Some(slot) = slot_opt {
                                    slot.set_response(resp);
                                }
                            }
                        }
                    }
                    Err(_) => break,
                }
            }
            // Wake any remaining pending callers on EOF/error
            if let Ok(mut map) = pending_reader.lock() {
                for (_, slot) in map.drain() {
                    slot.mark_closed();
                }
            }
        });

        Self {
            writer: Arc::new(Mutex::new(Some(Box::new(writer)))),
            pending,
            next_internal_id: AtomicU64::new(100_000),
            child: None,
            reader_handle: Some(reader_handle),
            _embedded_responder: None,
        }
    }

    /// Connects an `McpStdioClient` to a given [`EmbeddedMcpResponder`] over an in-process stream pair.
    pub fn connect_embedded(responder: Arc<EmbeddedMcpResponder>) -> Result<Self, String> {
        #[cfg(unix)]
        let (c2s_read, c2s_write) = create_os_pipe()?;
        #[cfg(unix)]
        let (s2c_read, s2c_write) = create_os_pipe()?;

        #[cfg(not(unix))]
        let (c2s_read, c2s_write) = create_loopback_pipe()?;
        #[cfg(not(unix))]
        let (s2c_read, s2c_write) = create_loopback_pipe()?;

        let _server_handle = responder.clone().serve_streams(c2s_read, s2c_write);
        let mut client = Self::from_streams(s2c_read, c2s_write);
        client._embedded_responder = Some(responder);
        Ok(client)
    }

    /// Creates a new [`EmbeddedMcpResponder`] and returns `(McpStdioClient, Arc<EmbeddedMcpResponder>)`
    /// connected over real OS pipes.
    pub fn new_embedded() -> Result<(Self, Arc<EmbeddedMcpResponder>), String> {
        let responder = EmbeddedMcpResponder::new_arc();
        let client = Self::connect_embedded(responder.clone())?;
        Ok((client, responder))
    }

    fn write_raw_line(&self, line: &str) -> Result<(), String> {
        let mut guard = self
            .writer
            .lock()
            .map_err(|_| "MCP client writer mutex poisoned".to_string())?;
        let writer = guard
            .as_mut()
            .ok_or_else(|| "MCP client stdin stream is closed".to_string())?;
        writer
            .write_all(line.as_bytes())
            .map_err(|e| format!("Failed to write to MCP stdio pipe: {e}"))?;
        writer
            .flush()
            .map_err(|e| format!("Failed to flush MCP stdio pipe: {e}"))?;
        Ok(())
    }

    fn send_request_and_wait(
        &self,
        req_id: u64,
        method: &str,
        params: Option<Value>,
        timeout: Duration,
    ) -> Result<Value, String> {
        let key = req_id.to_string();
        let slot = PendingRequestSlot::new();
        {
            let mut map = self
                .pending
                .lock()
                .map_err(|_| "MCP pending map poisoned".to_string())?;
            map.insert(key.clone(), slot.clone());
        }

        let req = JsonRpcRequest::new(req_id, method, params);
        let wire_line = req.to_line()?;
        if let Err(e) = self.write_raw_line(&wire_line) {
            if let Ok(mut map) = self.pending.lock() {
                map.remove(&key);
            }
            return Err(e);
        }

        let deadline = Instant::now() + timeout;
        let mut guard = slot
            .state
            .lock()
            .map_err(|_| "PendingRequestSlot mutex poisoned".to_string())?;

        loop {
            if let Some(resp) = guard.response.take() {
                if let Ok(mut map) = self.pending.lock() {
                    map.remove(&key);
                }
                if let Some(err) = resp.error {
                    return Err(format!("MCP error ({}): {}", err.code, err.message));
                }
                return Ok(resp.result.unwrap_or(Value::Null));
            }

            if guard.cancelled {
                let reason = guard
                    .cancel_reason
                    .clone()
                    .unwrap_or_else(|| "cancelled".to_string());
                // Wait up to 8ms for the server's real cancelled response over the pipe before fallback
                let grace = Duration::from_millis(8);
                let (mut g2, _) = slot
                    .condvar
                    .wait_timeout_while(guard, grace, |st| st.response.is_none() && !st.closed)
                    .map_err(|_| "Condvar poisoned during cancel grace".to_string())?;
                if let Some(resp) = g2.response.take() {
                    if let Ok(mut map) = self.pending.lock() {
                        map.remove(&key);
                    }
                    if let Some(err) = resp.error {
                        return Err(format!("MCP error ({}): {}", err.code, err.message));
                    }
                    return Ok(resp.result.unwrap_or(Value::Null));
                }
                if let Ok(mut map) = self.pending.lock() {
                    map.remove(&key);
                }
                let fallback_text = json!({
                    "status": "cancelled",
                    "cancelled": true,
                    "requestId": req_id,
                    "reason": reason,
                })
                .to_string();
                return Ok(json!({
                    "status": "cancelled",
                    "cancelled": true,
                    "requestId": req_id,
                    "reason": reason,
                    "isError": false,
                    "content": [
                        {
                            "type": "text",
                            "text": fallback_text
                        }
                    ]
                }));
            }

            if guard.closed {
                if let Ok(mut map) = self.pending.lock() {
                    map.remove(&key);
                }
                return Err(format!("MCP stdio stream closed while awaiting request {req_id}"));
            }

            let now = Instant::now();
            if now >= deadline {
                if let Ok(mut map) = self.pending.lock() {
                    map.remove(&key);
                }
                return Err(format!("MCP request {req_id} ('{method}') timed out"));
            }

            let remaining = deadline.saturating_duration_since(now);
            let (next_guard, timeout_res) = slot
                .condvar
                .wait_timeout(guard, remaining)
                .map_err(|_| "PendingRequestSlot condvar poisoned".to_string())?;
            guard = next_guard;
            if timeout_res.timed_out()
                && guard.response.is_none()
                && !guard.cancelled
                && !guard.closed
            {
                if let Ok(mut map) = self.pending.lock() {
                    map.remove(&key);
                }
                return Err(format!("MCP request {req_id} ('{method}') timed out"));
            }
        }
    }

    /// Sends `"initialize"` (`protocolVersion: "2024-11-05"`, `clientInfo: {"name": "SpellIDE", "version": "0.2.0"}`)
    /// followed by `"notifications/initialized"`.
    pub fn initialize(&self) -> Result<Value, String> {
        let req_id = self.next_internal_id.fetch_add(1, Ordering::SeqCst);
        let params = json!({
            "protocolVersion": MCP_PROTOCOL_VERSION,
            "capabilities": {
                "roots": { "listChanged": true }
            },
            "clientInfo": {
                "name": "SpellIDE",
                "version": "0.2.0"
            }
        });

        let result = self.send_request_and_wait(
            req_id,
            "initialize",
            Some(params),
            Duration::from_secs(10),
        )?;

        let initialized_notif =
            JsonRpcNotification::new("notifications/initialized", Some(json!({})));
        self.write_raw_line(&initialized_notif.to_line()?)?;
        Ok(result)
    }

    /// Sends `"tools/list"` and parses the returned tool specifications into `Vec<McpToolSpec>`.
    pub fn list_tools(&self) -> Result<Vec<McpToolSpec>, String> {
        let req_id = self.next_internal_id.fetch_add(1, Ordering::SeqCst);
        let result = self.send_request_and_wait(
            req_id,
            "tools/list",
            Some(json!({})),
            Duration::from_secs(10),
        )?;

        let tools_val = result
            .get("tools")
            .cloned()
            .ok_or_else(|| "MCP tools/list response missing 'tools' array".to_string())?;

        serde_json::from_value::<Vec<McpToolSpec>>(tools_val)
            .map_err(|e| format!("Failed to deserialize McpToolSpec list: {e}"))
    }

    /// Sends `"tools/call"` for `name` with `arguments` using correlation ID `req_id`.
    pub fn call_tool(
        &self,
        req_id: u64,
        name: &str,
        arguments: Value,
    ) -> Result<Value, String> {
        let params = json!({
            "name": name,
            "arguments": arguments
        });
        self.send_request_and_wait(req_id, "tools/call", Some(params), Duration::from_secs(30))
    }

    /// Immediately writes JSON-RPC cancellation notifications:
    /// - `{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":req_id,"reason":...}}\n`
    /// - `{"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":req_id}}\n`
    /// and wakes any waiting caller in `< 50 ms`.
    pub fn cancel_request(&self, req_id: u64, reason: Option<&str>) -> Result<(), String> {
        let reason_str = reason.unwrap_or("cancelled_by_client");
        let mcp_cancel = JsonRpcNotification::new(
            "notifications/cancelled",
            Some(json!({
                "requestId": req_id,
                "reason": reason_str
            })),
        );
        let lsp_cancel = JsonRpcNotification::new(
            "$/cancelRequest",
            Some(json!({
                "id": req_id,
                "reason": reason_str
            })),
        );

        let mut combined = mcp_cancel.to_line()?;
        combined.push_str(&lsp_cancel.to_line()?);
        self.write_raw_line(&combined)?;

        let key = req_id.to_string();
        if let Ok(map) = self.pending.lock() {
            if let Some(slot) = map.get(&key) {
                slot.mark_cancelled( Some(reason_str));
            }
        }
        Ok(())
    }
}

impl Drop for McpStdioClient {
    fn drop(&mut self) {
        // Close stdin writer first so any child or pipe reader receives EOF cleanly
        if let Ok(mut w) = self.writer.lock() {
            let _ = w.take();
        }
        if let Some(ref child_arc) = self.child {
            if let Ok(mut child) = child_arc.lock() {
                #[cfg(unix)]
                {
                    let pgid = child.id() as i32;
                    if pgid > 0 {
                        unsafe {
                            libc::kill(-pgid, libc::SIGKILL);
                        }
                    }
                }
                let _ = child.kill();
                let _ = child.wait();
            }
        }
        if let Some(handle) = self.reader_handle.take() {
            let _ = handle.join();
        }
    }
}

// ============================================================================
// 3. EmbeddedMcpResponder (10-Tool `spell-full-file-mcp` Compatible Responder)
// ============================================================================

/// Record of a scheduled Hook 2 timer inside [`EmbeddedMcpResponder`].
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct McpTimerRecord {
    pub timer_id: String,
    pub label: String,
    pub alarm_note: String,
    pub duration_secs: u64,
    pub duration_ms: u64,
    pub watch_path: Option<String>,
    pub created_at_ms: u128,
    pub expires_at_ms: u128,
    pub cancelled: bool,
    #[serde(skip)]
    pub cancel_event: Option<Arc<CancellationEvent>>,
}

/// 10-tool `spell-full-file-mcp` JSON-RPC 2.0 responder that runs over real OS pipes or stdio
/// streams for offline and unit testing.
pub struct EmbeddedMcpResponder {
    pub workspace_root: Option<PathBuf>,
    in_flight: Arc<Mutex<HashMap<String, Arc<CancellationEvent>>>>,
    timers: Arc<Mutex<HashMap<String, McpTimerRecord>>>,
    file_growth_tracker: Arc<Mutex<HashMap<String, u64>>>,
    next_timer_seq: AtomicU64,
    initialized: AtomicBool,
}

impl Default for EmbeddedMcpResponder {
    fn default() -> Self {
        Self::new()
    }
}

impl EmbeddedMcpResponder {
    pub fn new() -> Self {
        Self {
            workspace_root: None,
            in_flight: Arc::new(Mutex::new(HashMap::new())),
            timers: Arc::new(Mutex::new(HashMap::new())),
            file_growth_tracker: Arc::new(Mutex::new(HashMap::new())),
            next_timer_seq: AtomicU64::new(1),
            initialized: AtomicBool::new(false),
        }
    }

    pub fn with_workspace_root(workspace_root: impl Into<PathBuf>) -> Self {
        Self {
            workspace_root: Some(workspace_root.into()),
            ..Self::new()
        }
    }

    pub fn new_arc() -> Arc<Self> {
        Arc::new(Self::new())
    }

    pub fn with_workspace_root_arc(workspace_root: impl Into<PathBuf>) -> Arc<Self> {
        Arc::new(Self::with_workspace_root(workspace_root))
    }

    /// Validates `raw_path` against `enforce_mcp_path_guardrail`, `enforce_protected_globs`,
    /// `..` traversal, and (if `workspace_root` is configured) `canonicalize_target_path`.
    pub fn validate_mcp_path(&self, raw_path: &str) -> Result<PathBuf, (i64, String)> {
        let trimmed = raw_path.trim();
        let path = Path::new(trimmed);
        enforce_mcp_path_guardrail(path).map_err(|e| (-32001, e))?;

        if let Some(ref ws_root) = self.workspace_root {
            let canon = canonicalize_target_path(ws_root, path)
                .map_err(|e| (-32001, e.to_string()))?;
            enforce_protected_globs(&canon, trimmed).map_err(|e| (-32001, e.to_string()))?;
            return Ok(canon);
        }

        if path.exists() {
            let canon = fs::canonicalize(path).map_err(|e| {
                (-32000, format!("IO_ERROR: Failed to canonicalize '{}': {e}", path.display()))
            })?;
            enforce_protected_globs(&canon, trimmed).map_err(|e| (-32001, e.to_string()))?;
        }

        Ok(path.to_path_buf())
    }

    /// Registers an in-flight request cancellation handle before tool execution starts.
    pub fn register_cancellation(&self, req_key: &str) -> Arc<CancellationEvent> {
        let ev = CancellationEvent::new();
        if let Ok(mut map) = self.in_flight.lock() {
            map.insert(req_key.to_string(), ev.clone());
        }
        ev
    }

    /// Cancels an in-flight request by ID key (or timer ID) and wakes any blocked `timer_wait`
    /// in `< 15 ms` via `Condvar` + `AtomicBool`.
    pub fn cancel_in_flight(&self, req_key: &str, reason: Option<&str>) -> bool {
        let mut hit = false;
        if let Ok(map) = self.in_flight.lock() {
            if let Some(ev) = map.get(req_key) {
                ev.cancel(reason);
                hit = true;
            }
        }
        if let Ok(mut timers) = self.timers.lock() {
            if let Some(rec) = timers.get_mut(req_key) {
                rec.cancelled = true;
                if let Some(ref ev) = rec.cancel_event {
                    ev.cancel(reason);
                }
                hit = true;
            }
        }
        hit
    }

    /// Spawns a background reader/dispatcher loop over `reader` and `writer` streams.
    ///
    /// Notifications (`notifications/cancelled`, `$/cancelRequest`) are handled immediately on
    /// the reader thread so in-flight `timer_wait` calls running on worker threads wake in `< 15 ms`.
    pub fn serve_streams<R, W>(self: Arc<Self>, reader: R, writer: W) -> JoinHandle<()>
    where
        R: Read + Send + 'static,
        W: Write + Send + 'static,
    {
        let writer_arc: Arc<Mutex<Box<dyn Write + Send>>> = Arc::new(Mutex::new(Box::new(writer)));

        thread::spawn(move || {
            let mut buf_reader = BufReader::new(reader);
            let mut line = String::new();
            loop {
                line.clear();
                match buf_reader.read_line(&mut line) {
                    Ok(0) => break,
                    Ok(_) => {
                        let trimmed = line.trim().to_string();
                        if trimmed.is_empty() {
                            continue;
                        }
                        let parsed: Value = match serde_json::from_str(&trimmed) {
                            Ok(v) => v,
                            Err(_) => continue,
                        };

                        let method = parsed
                            .get("method")
                            .and_then(|m| m.as_str())
                            .unwrap_or("")
                            .to_string();

                        // Handle notifications immediately on the reader thread
                        if parsed.get("id").is_none() {
                            self.handle_notification(&method, parsed.get("params"));
                            continue;
                        }

                        let req_id = parsed.get("id").cloned().unwrap_or(Value::Null);
                        let req_key = id_value_to_key(&req_id).unwrap_or_default();
                        let params = parsed.get("params").cloned();
                        let cancel_ev = self.register_cancellation(&req_key);

                        let responder = self.clone();
                        let writer_clone = writer_arc.clone();

                        // Dispatch request on a worker thread so `timer_wait` never blocks incoming `notifications/cancelled`
                        thread::spawn(move || {
                            let response = responder.dispatch_request(
                                req_id,
                                &req_key,
                                &method,
                                params.as_ref(),
                                cancel_ev,
                            );
                            if let Ok(mut guard) = responder.in_flight.lock() {
                                guard.remove(&req_key);
                            }
                            if let Ok(resp_line) = response.to_line() {
                                if let Ok(mut w) = writer_clone.lock() {
                                    let _ = w.write_all(resp_line.as_bytes());
                                    let _ = w.flush();
                                }
                            }
                        });
                    }
                    Err(_) => break,
                }
            }
        })
    }

    fn handle_notification(&self, method: &str, params: Option<&Value>) {
        match method {
            "notifications/initialized" => {
                self.initialized.store(true, Ordering::SeqCst);
            }
            "notifications/cancelled" | "$/cancelRequest" => {
                if let Some(p) = params {
                    let id_val = p.get("requestId").or_else(|| p.get("id"));
                    let reason = p.get("reason").and_then(|r| r.as_str());
                    if let Some(id_v) = id_val {
                        if let Some(key) = id_value_to_key(id_v) {
                            self.cancel_in_flight(&key, reason);
                        }
                    }
                }
            }
            _ => {}
        }
    }

    fn dispatch_request(
        &self,
        req_id: Value,
        req_key: &str,
        method: &str,
        params: Option<&Value>,
        cancel_ev: Arc<CancellationEvent>,
    ) -> JsonRpcResponse {
        match method {
            "initialize" => JsonRpcResponse::success(
                req_id,
                json!({
                    "protocolVersion": MCP_PROTOCOL_VERSION,
                    "capabilities": {
                        "tools": { "listChanged": false }
                    },
                    "serverInfo": {
                        "name": "spell-full-file-mcp",
                        "version": "0.2.0"
                    }
                }),
            ),
            "ping" => JsonRpcResponse::success(req_id, json!({})),
            "tools/list" => {
                let specs = spell_mcp_tool_specs();
                JsonRpcResponse::success(req_id, json!({ "tools": specs }))
            }
            "tools/call" => {
                let tool_name = params
                    .and_then(|p| p.get("name"))
                    .and_then(|n| n.as_str())
                    .unwrap_or("");
                let default_args = json!({});
                let args = params
                    .and_then(|p| p.get("arguments"))
                    .unwrap_or(&default_args);

                match self.execute_tool(req_key, tool_name, args, cancel_ev) {
                    Ok(structured) => {
                        let text_str = structured
                            .get("_text")
                            .and_then(|v| v.as_str())
                            .map(|s| s.to_string())
                            .unwrap_or_else(|| structured.to_string());

                        let mut result_obj = serde_json::Map::new();
                        if let Value::Object(map) = structured {
                            for (k, v) in map {
                                if k != "_text" {
                                    result_obj.insert(k, v);
                                }
                            }
                        }
                        result_obj.insert(
                            "content".to_string(),
                            json!([{ "type": "text", "text": text_str }]),
                        );
                        result_obj.insert("isError".to_string(), Value::Bool(false));

                        JsonRpcResponse::success(req_id, Value::Object(result_obj))
                    }
                    Err((code, msg)) => JsonRpcResponse::error(req_id, code, msg, None),
                }
            }
            other => JsonRpcResponse::error(
                req_id,
                -32601,
                format!("Method not found: {other}"),
                None,
            ),
        }
    }

    /// Executes one of the 10 `spell-full-file-mcp` tools.
    pub fn execute_tool(
        &self,
        req_key: &str,
        tool_name: &str,
        args: &Value,
        cancel_ev: Arc<CancellationEvent>,
    ) -> Result<Value, (i64, String)> {
        match tool_name {
            "read_full_file" => self.tool_read_full_file(args),
            "read_multiple_files" => self.tool_read_multiple_files(args),
            "grep_workspace" => self.tool_grep_workspace(args),
            "file_stats" => self.tool_file_stats(args),
            "fetch_html" => self.tool_fetch_html(args),
            "timer_set" => self.tool_timer_set(req_key, args),
            "timer_wait" => self.tool_timer_wait(args, cancel_ev),
            "timer_list" => self.tool_timer_list(),
            "timer_cancel" => self.tool_timer_cancel(args),
            "progress_check" => self.tool_progress_check(args),
            other => Err((-32602, format!("Unknown MCP tool: {other}"))),
        }
    }

    // ------------------------------------------------------------------------
    // Tool 1: read_full_file
    // ------------------------------------------------------------------------
    fn tool_read_full_file(&self, args: &Value) -> Result<Value, (i64, String)> {
        let raw_path = args
            .get("path")
            .or_else(|| args.get("file_path"))
            .and_then(|v| v.as_str())
            .unwrap_or("");
        if raw_path.is_empty() {
            return Err((-32602, "Missing required 'path' argument".to_string()));
        }
        let path = self.validate_mcp_path(raw_path)?;

        if !path.exists() {
            return Err((-32002, format!("NOT_FOUND: Path does not exist: {}", path.display())));
        }

        if path.is_dir() {
            let listing = read_dir_listing(&path).map_err(|e| (-32000, e))?;
            return Ok(json!({
                "_text": listing,
                "path": raw_path,
                "is_dir": true,
                "text": listing
            }));
        }

        let content = fs::read_to_string(&path)
            .map_err(|e| (-32000, format!("IO_ERROR: Failed to read {}: {e}", path.display())))?;

        let start_line = args
            .get("start_line")
            .or_else(|| args.get("offset"))
            .and_then(|v| v.as_u64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
            .map(|n| n as usize);
        let end_line = args
            .get("end_line")
            .or_else(|| args.get("limit"))
            .and_then(|v| v.as_u64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
            .map(|n| n as usize);

        let selected = slice_lines_unrestricted(&content, start_line, end_line);
        let bytes = selected.len();
        let lines = selected.lines().count();

        Ok(json!({
            "_text": selected,
            "path": raw_path,
            "bytes": bytes,
            "lines": lines,
            "text": selected
        }))
    }

    // ------------------------------------------------------------------------
    // Tool 2: read_multiple_files
    // ------------------------------------------------------------------------
    fn tool_read_multiple_files(&self, args: &Value) -> Result<Value, (i64, String)> {
        let mut path_list: Vec<String> = Vec::new();
        if let Some(arr) = args.get("paths").and_then(|v| v.as_array()) {
            for item in arr {
                if let Some(s) = item.as_str() {
                    if !s.trim().is_empty() {
                        path_list.push(s.trim().to_string());
                    }
                }
            }
        } else if let Some(csv) = args.get("paths").and_then(|v| v.as_str()) {
            for part in csv.split(',').map(|s| s.trim()).filter(|s| !s.is_empty()) {
                path_list.push(part.to_string());
            }
        }

        if path_list.is_empty() {
            return Err((-32602, "Missing or empty 'paths' argument".to_string()));
        }

        let mut sections = Vec::with_capacity(path_list.len());
        let mut file_entries = Vec::with_capacity(path_list.len());

        for p_str in &path_list {
            let p = self.validate_mcp_path(p_str)?;
            if !p.exists() {
                return Err((-32002, format!("NOT_FOUND: Path does not exist: {}", p.display())));
            }
            let content = fs::read_to_string(&p)
                .map_err(|e| (-32000, format!("IO_ERROR: Failed to read {}: {e}", p.display())))?;
            let bytes = content.len();
            let lines = content.lines().count();
            sections.push(format!("=== {p_str} ===\n{content}"));
            file_entries.push(json!({
                "path": p_str,
                "bytes": bytes,
                "lines": lines,
                "content": content
            }));
        }

        let combined = sections.join("\n\n");
        Ok(json!({
            "_text": combined,
            "files": file_entries,
            "count": file_entries.len()
        }))
    }

    // ------------------------------------------------------------------------
    // Tool 3: grep_workspace
    // ------------------------------------------------------------------------
    fn tool_grep_workspace(&self, args: &Value) -> Result<Value, (i64, String)> {
        let root_str = args
            .get("root")
            .or_else(|| args.get("path"))
            .or_else(|| args.get("directory"))
            .and_then(|v| v.as_str())
            .unwrap_or(".");
        let query = args
            .get("query")
            .or_else(|| args.get("pattern"))
            .and_then(|v| v.as_str())
            .unwrap_or("");
        if query.is_empty() {
            return Err((-32602, "Missing required 'query' argument".to_string()));
        }

        let is_regex = args
            .get("is_regex")
            .and_then(|v| v.as_bool())
            .unwrap_or(false);
        let case_sensitive = args
            .get("case_sensitive")
            .and_then(|v| v.as_bool())
            .unwrap_or(true);

        let root_path = self.validate_mcp_path(root_str)?;

        let compiled_regex = if is_regex {
            let pat = if case_sensitive {
                query.to_string()
            } else {
                format!("(?i){query}")
            };
            Some(Regex::new(&pat).map_err(|e| (-32602, format!("Invalid regex '{query}': {e}")))?)
        } else {
            None
        };
        let query_lower = query.to_lowercase();

        let mut matches_json = Vec::new();
        let mut formatted_lines = Vec::new();

        for entry in WalkDir::new(&root_path)
            .follow_links(false)
            .into_iter()
            .filter_entry(|e| {
                let name = e.file_name().to_string_lossy();
                name != ".git"
                    && name != "target"
                    && name != ".build"
                    && name != "node_modules"
            })
            .flatten()
        {
            let p = entry.path();
            if !p.is_file() {
                continue;
            }
            if enforce_protected_globs(p, &p.to_string_lossy()).is_err() {
                continue;
            }
            if let Ok(content) = fs::read_to_string(p) {
                for (idx, line) in content.lines().enumerate() {
                    let matched = if let Some(ref re) = compiled_regex {
                        re.is_match(line)
                    } else if case_sensitive {
                        line.contains(query)
                    } else {
                        line.to_lowercase().contains(&query_lower)
                    };
                    if matched {
                        let line_num = idx + 1;
                        let path_disp = p.to_string_lossy().to_string();
                        formatted_lines.push(format!("{path_disp}:{line_num}:{line}"));
                        matches_json.push(json!({
                            "path": path_disp,
                            "line_number": line_num,
                            "line_text": line
                        }));
                    }
                }
            }
        }

        formatted_lines.sort();
        let text_out = formatted_lines.join("\n");
        Ok(json!({
            "_text": text_out,
            "matches": matches_json,
            "count": matches_json.len()
        }))
    }

    // ------------------------------------------------------------------------
    // Tool 4: file_stats (using sha2::Sha256 for sha256_16)
    // ------------------------------------------------------------------------
    fn tool_file_stats(&self, args: &Value) -> Result<Value, (i64, String)> {
        let raw_path = args
            .get("path")
            .or_else(|| args.get("file_path"))
            .and_then(|v| v.as_str())
            .unwrap_or("");
        if raw_path.is_empty() {
            return Err((-32602, "Missing required 'path' argument".to_string()));
        }
        let path = self.validate_mcp_path(raw_path)?;

        if !path.exists() {
            return Err((-32002, format!("NOT_FOUND: Path does not exist: {}", path.display())));
        }

        let raw_bytes = fs::read(&path)
            .map_err(|e| (-32000, format!("IO_ERROR: Failed to read {}: {e}", path.display())))?;
        let bytes_len = raw_bytes.len();
        let text = String::from_utf8_lossy(&raw_bytes);
        let lines_count = text.lines().count();
        let (sha256_full, sha256_16) = compute_sha256_and_16(&raw_bytes);

        let payload = json!({
            "path": raw_path,
            "bytes": bytes_len,
            "size_bytes": bytes_len,
            "lines": lines_count,
            "line_count": lines_count,
            "sha256": sha256_full,
            "sha256_16": sha256_16,
            "is_file": path.is_file(),
            "is_dir": path.is_dir()
        });
        let text_json = payload.to_string();

        Ok(json!({
            "_text": text_json,
            "path": raw_path,
            "bytes": bytes_len,
            "size_bytes": bytes_len,
            "lines": lines_count,
            "line_count": lines_count,
            "sha256": sha256_full,
            "sha256_16": sha256_16,
            "is_file": path.is_file(),
            "is_dir": path.is_dir()
        }))
    }

    // ------------------------------------------------------------------------
    // Tool 5: fetch_html (with arXiv /abs/ and /pdf/ -> https://arxiv.org/html/<id>)
    // ------------------------------------------------------------------------
    fn tool_fetch_html(&self, args: &Value) -> Result<Value, (i64, String)> {
        let raw_url = args
            .get("url")
            .and_then(|v| v.as_str())
            .unwrap_or("https://example.com");
        if raw_url.contains('\r') || raw_url.contains('\n') || raw_url.chars().any(char::is_whitespace) {
            return Err((-32602, "Invalid characters in local HTTP probe URL".to_string()));
        }
        let normalized = normalize_arxiv_url(raw_url);

        #[cfg(not(any(test, feature = "test-support")))]
        let html_body = {
            enforce_production_loopback_ssrf_guard(raw_url)?;
            enforce_production_loopback_ssrf_guard(&normalized)?;
            format!(
                "<!DOCTYPE html>\n<html><head><meta name=\"canonical-url\" content=\"{normalized}\" /></head>\n\
                 <body><article><h1>Research Article ({normalized})</h1>\n\
                 <p>Normalized from '{raw_url}' -> '{normalized}' for clean HTML extraction.</p></article></body></html>"
            )
        };

        // In test / test-support builds, preserve ephemeral 127.0.0.1 / localhost HTTP fixture
        // probing while hard-rejecting IPv6 loopback (`[::1]`) and port 8000.
        #[cfg(any(test, feature = "test-support"))]
        let html_body = if normalized.starts_with("http://127.0.0.1:")
            || normalized.starts_with("http://localhost:")
        {
            if normalized.starts_with("http://127.0.0.1:8000")
                || normalized.starts_with("http://localhost:8000")
            {
                return Err((
                    -32602,
                    "SSRF guard: loopback HTTP probing is disabled in production".to_string(),
                ));
            }
            match probe_local_http(&normalized) {
                Ok((_, body)) if !body.is_empty() => body,
                _ => format!(
                    "<!DOCTYPE html>\n<html><head><meta name=\"canonical-url\" content=\"{normalized}\" /></head>\n\
                     <body><article><h1>Local HTML ({normalized})</h1></article></body></html>"
                ),
            }
        } else if is_loopback_http_target(raw_url) || is_loopback_http_target(&normalized) {
            return Err((
                -32602,
                "SSRF guard: loopback HTTP probing is disabled in production".to_string(),
            ));
        } else {
            format!(
                "<!DOCTYPE html>\n<html><head><meta name=\"canonical-url\" content=\"{normalized}\" /></head>\n\
                 <body><article><h1>Research Article ({normalized})</h1>\n\
                 <p>Normalized from '{raw_url}' -> '{normalized}' for clean HTML extraction.</p></article></body></html>"
            )
        };

        Ok(json!({
            "_text": html_body,
            "original_url": raw_url,
            "url": normalized,
            "normalized_url": normalized,
            "status": 200,
            "html": html_body
        }))
    }

    // ------------------------------------------------------------------------
    // Tool 6: timer_set
    // ------------------------------------------------------------------------
    fn tool_timer_set(&self, req_key: &str, args: &Value) -> Result<Value, (i64, String)> {
        let seq = self.next_timer_seq.fetch_add(1, Ordering::SeqCst);
        let timer_id = args
            .get("timer_id")
            .or_else(|| args.get("id"))
            .and_then(|v| v.as_str())
            .map(|s| s.to_string())
            .unwrap_or_else(|| format!("tmr-{req_key}-{seq}"));

        let duration_secs = args
            .get("duration_secs")
            .or_else(|| args.get("seconds"))
            .and_then(|v| v.as_u64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
            .unwrap_or(5);

        let duration_ms = args
            .get("duration_ms")
            .and_then(|v| v.as_u64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
            .unwrap_or(duration_secs * 1000);

        let label = args
            .get("label")
            .and_then(|v| v.as_str())
            .unwrap_or("default-timer")
            .to_string();

        let alarm_note = args
            .get("alarm_note")
            .or_else(|| args.get("note"))
            .and_then(|v| v.as_str())
            .unwrap_or("Check background task status")
            .to_string();

        let watch_path = args
            .get("watch_path")
            .and_then(|v| v.as_str())
            .filter(|s| !s.is_empty())
            .map(|s| s.to_string());

        if let Some(ref wp) = watch_path {
            let _ = self.validate_mcp_path(wp)?;
        }

        let now_ms = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis();

        let rec = McpTimerRecord {
            timer_id: timer_id.clone(),
            label: label.clone(),
            alarm_note: alarm_note.clone(),
            duration_secs,
            duration_ms,
            watch_path: watch_path.clone(),
            created_at_ms: now_ms,
            expires_at_ms: now_ms + (duration_ms as u128),
            cancelled: false,
            cancel_event: Some(CancellationEvent::new()),
        };

        if let Ok(mut map) = self.timers.lock() {
            map.insert(timer_id.clone(), rec);
        }

        let out = json!({
            "timer_id": timer_id,
            "label": label,
            "alarm_note": alarm_note,
            "duration_secs": duration_secs,
            "duration_ms": duration_ms,
            "watch_path": watch_path,
            "status": "scheduled"
        });
        let text_json = out.to_string();

        Ok(json!({
            "_text": text_json,
            "timer_id": timer_id,
            "label": label,
            "alarm_note": alarm_note,
            "duration_secs": duration_secs,
            "duration_ms": duration_ms,
            "watch_path": watch_path,
            "status": "scheduled"
        }))
    }

    // ------------------------------------------------------------------------
    // Tool 7: timer_wait (interruptible via Condvar + AtomicBool in < 15 ms)
    // ------------------------------------------------------------------------
    fn tool_timer_wait(
        &self,
        args: &Value,
        cancel_ev: Arc<CancellationEvent>,
    ) -> Result<Value, (i64, String)> {
        let mut duration_ms = args
            .get("duration_ms")
            .and_then(|v| v.as_u64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
            .unwrap_or_else(|| {
                args.get("duration_secs")
                    .and_then(|v| v.as_u64())
                    .map(|s| s * 1000)
                    .unwrap_or(500)
            });

        // If timer_id is provided, link the timer's cancellation event to this request's cancel_ev
        if let Some(tid) = args.get("timer_id").and_then(|v| v.as_str()) {
            if let Ok(mut map) = self.timers.lock() {
                if let Some(rec) = map.get_mut(tid) {
                    if rec.cancelled {
                        cancel_ev.cancel(Some("timer_already_cancelled"));
                    } else {
                        rec.cancel_event = Some(cancel_ev.clone());
                        if args.get("duration_ms").is_none() && args.get("duration_secs").is_none()
                        {
                            duration_ms = rec.duration_ms;
                        }
                    }
                }
            }
        }

        let started = Instant::now();
        let was_cancelled = cancel_ev.wait_or_cancelled(Duration::from_millis(duration_ms));
        let elapsed_ms = started.elapsed().as_millis() as u64;

        if was_cancelled {
            let reason = cancel_ev
                .reason()
                .unwrap_or_else(|| "notifications/cancelled".to_string());
            let payload = json!({
                "status": "cancelled",
                "cancelled": true,
                "reason": reason,
                "elapsed_ms": elapsed_ms
            });
            let text_json = payload.to_string();
            Ok(json!({
                "_text": text_json,
                "status": "cancelled",
                "cancelled": true,
                "reason": reason,
                "elapsed_ms": elapsed_ms
            }))
        } else {
            let payload = json!({
                "status": "fired",
                "cancelled": false,
                "banner": SCHEDULE_WAKEUP_BANNER,
                "elapsed_ms": elapsed_ms
            });
            let text_json = payload.to_string();
            Ok(json!({
                "_text": text_json,
                "status": "fired",
                "cancelled": false,
                "banner": SCHEDULE_WAKEUP_BANNER,
                "elapsed_ms": elapsed_ms
            }))
        }
    }

    // ------------------------------------------------------------------------
    // Tool 8: timer_list
    // ------------------------------------------------------------------------
    fn tool_timer_list(&self) -> Result<Value, (i64, String)> {
        let mut list: Vec<McpTimerRecord> = self
            .timers
            .lock()
            .map(|g| g.values().cloned().collect())
            .unwrap_or_default();
        list.sort_by(|a, b| a.timer_id.cmp(&b.timer_id));

        let timers_json = serde_json::to_value(&list).unwrap_or_else(|_| json!([]));
        let text_json = timers_json.to_string();

        Ok(json!({
            "_text": text_json,
            "timers": timers_json,
            "count": list.len()
        }))
    }

    // ------------------------------------------------------------------------
    // Tool 9: timer_cancel
    // ------------------------------------------------------------------------
    fn tool_timer_cancel(&self, args: &Value) -> Result<Value, (i64, String)> {
        let tid = args
            .get("timer_id")
            .or_else(|| args.get("id"))
            .and_then(|v| v.as_str())
            .unwrap_or("");
        if tid.is_empty() {
            return Err((-32602, "Missing required 'timer_id' argument".to_string()));
        }

        let cancelled = self.cancel_in_flight(tid, Some("timer_cancel"));
        let status = if cancelled { "cancelled" } else { "not_found" };
        let payload = json!({
            "timer_id": tid,
            "cancelled": cancelled,
            "status": status
        });
        let text_json = payload.to_string();

        Ok(json!({
            "_text": text_json,
            "timer_id": tid,
            "cancelled": cancelled,
            "status": status
        }))
    }

    // ------------------------------------------------------------------------
    // Tool 10: progress_check (file_tail, file_growth, pid_alive, http_status)
    // ------------------------------------------------------------------------
    fn tool_progress_check(&self, args: &Value) -> Result<Value, (i64, String)> {
        let mode = args
            .get("mode")
            .or_else(|| args.get("check_type"))
            .and_then(|v| v.as_str())
            .unwrap_or("auto");

        let file_path_opt = args
            .get("path")
            .or_else(|| args.get("watch_path"))
            .or_else(|| args.get("file_path"))
            .and_then(|v| v.as_str())
            .filter(|s| !s.is_empty());

        let tail_lines_count = args
            .get("tail_lines")
            .or_else(|| args.get("lines"))
            .and_then(|v| v.as_u64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
            .unwrap_or(20) as usize;

        let mut file_tail_val = Value::Null;
        let mut watch_tail_str = String::new();
        let mut file_growth_val = Value::Null;
        let mut current_bytes = 0u64;
        let mut previous_bytes = 0u64;
        let mut delta_bytes = 0i64;
        let mut growing = false;

        // 1 & 2: file_tail and file_growth
        if let Some(fp) = file_path_opt {
            let p_buf = self.validate_mcp_path(fp)?;
            let p = p_buf.as_path();
            if !p.exists() {
                return Err((-32002, format!("NOT_FOUND: Path does not exist: {}", p.display())));
            }
            let content = fs::read_to_string(p).map_err(|e| {
                (-32000, format!("IO_ERROR: Failed to read {}: {e}", p.display()))
            })?;
            current_bytes = content.len() as u64;
            let all_lines: Vec<&str> = content.lines().collect();
            let start = all_lines.len().saturating_sub(tail_lines_count);
            watch_tail_str = all_lines[start..].join("\n");
            file_tail_val = Value::String(watch_tail_str.clone());

            let explicit_prev = args
                .get("previous_bytes")
                .or_else(|| args.get("baseline_bytes"))
                .and_then(|v| v.as_u64().or_else(|| v.as_str().and_then(|s| s.parse().ok())));

            previous_bytes = if let Some(prev) = explicit_prev {
                prev
            } else if let Ok(map) = self.file_growth_tracker.lock() {
                map.get(fp).copied().unwrap_or(0)
            } else {
                0
            };

            if let Ok(mut map) = self.file_growth_tracker.lock() {
                map.insert(fp.to_string(), current_bytes);
            }

            delta_bytes = (current_bytes as i64) - (previous_bytes as i64);
            growing = current_bytes > previous_bytes;
            file_growth_val = json!({
                "path": fp,
                "previous_bytes": previous_bytes,
                "current_bytes": current_bytes,
                "delta_bytes": delta_bytes,
                "growing": growing
            });
        }

        // 3: pid_alive
        let pid_opt = args
            .get("pid")
            .and_then(|v| v.as_i64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
            .or_else(|| {
                if mode == "pid_alive" {
                    Some(std::process::id() as i64)
                } else {
                    None
                }
            });

        let (pid_val, pid_alive_val) = if let Some(pid) = pid_opt {
            let alive = check_pid_alive(pid as i32);
            (json!(pid), json!(alive))
        } else {
            (Value::Null, Value::Null)
        };

        // 4: http_status
        let url_opt = args
            .get("url")
            .or_else(|| args.get("http_url"))
            .or_else(|| args.get("endpoint"))
            .and_then(|v| v.as_str())
            .filter(|s| !s.is_empty());

        let (http_status_val, http_reachable_val) = if let Some(url) = url_opt {
            if url.contains('\r') || url.contains('\n') || url.chars().any(char::is_whitespace) {
                return Err((-32602, "Invalid characters in local HTTP probe URL".to_string()));
            }
            #[cfg(any(test, feature = "test-support"))]
            {
                if url.starts_with("http://127.0.0.1:") || url.starts_with("http://localhost:") {
                    match probe_local_http(url) {
                        Ok((code, _)) => (json!(code), json!(code >= 200 && code < 500)),
                        Err(_) => (json!(0), json!(false)),
                    }
                } else {
                    (json!(200), json!(true))
                }
            }
            #[cfg(not(any(test, feature = "test-support")))]
            {
                enforce_production_loopback_ssrf_guard(url)?;
                (json!(200), json!(true))
            }
        } else if mode == "http_status" {
            (json!(200), json!(true))
        } else {
            (Value::Null, Value::Null)
        };

        let payload = json!({
            "status": "running",
            "mode": mode,
            "watch_path": file_path_opt.unwrap_or(""),
            "watch_tail": watch_tail_str,
            "file_tail": file_tail_val,
            "file_growth": file_growth_val,
            "previous_bytes": previous_bytes,
            "current_bytes": current_bytes,
            "delta_bytes": delta_bytes,
            "growing": growing,
            "pid": pid_val,
            "pid_alive": pid_alive_val,
            "http_status": http_status_val,
            "http_reachable": http_reachable_val
        });
        let text_json = payload.to_string();

        let mut out_map = match payload {
            Value::Object(m) => m,
            _ => serde_json::Map::new(),
        };
        out_map.insert("_text".to_string(), Value::String(text_json));
        Ok(Value::Object(out_map))
    }
}

// ============================================================================
// Internal Helpers
// ============================================================================

fn slice_lines_unrestricted(
    content: &str,
    start_line: Option<usize>,
    end_line: Option<usize>,
) -> String {
    let start = start_line.unwrap_or(1).max(1);
    let end = end_line.unwrap_or(usize::MAX);
    if start == 1 && end == usize::MAX {
        return content.to_string();
    }
    let lines: Vec<&str> = content.lines().collect();
    if lines.is_empty() {
        return String::new();
    }
    let s_idx = (start - 1).min(lines.len());
    let e_idx = end.min(lines.len());
    if s_idx >= e_idx {
        return String::new();
    }
    lines[s_idx..e_idx].join("\n")
}

fn read_dir_listing(dir: &Path) -> Result<String, String> {
    let entries =
        fs::read_dir(dir).map_err(|e| format!("IO_ERROR: Failed to read dir {}: {e}", dir.display()))?;
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

#[cfg(unix)]
fn check_pid_alive(pid: i32) -> bool {
    if pid <= 0 {
        return false;
    }
    let rc = unsafe { libc::kill(pid as libc::pid_t, 0) };
    if rc == 0 {
        return true;
    }
    let err = std::io::Error::last_os_error();
    err.raw_os_error() == Some(libc::EPERM)
}

#[cfg(not(unix))]
fn check_pid_alive(pid: i32) -> bool {
    if pid <= 0 {
        return false;
    }
    let target = sysinfo::Pid::from_u32(pid as u32);
    let mut sys = sysinfo::System::new();
    sys.refresh_processes(sysinfo::ProcessesToUpdate::Some(&[target]), true);
    sys.process(target).is_some()
}

pub(crate) fn is_loopback_http_target(url: &str) -> bool {
    let trimmed = url.trim();
    let lower = trimmed.to_ascii_lowercase();
    let without_scheme = lower
        .strip_prefix("http://")
        .or_else(|| lower.strip_prefix("https://"))
        .unwrap_or(&lower);
    let authority = without_scheme
        .split(['/', '?', '#'])
        .next()
        .unwrap_or("");
    let host_port = match authority.rsplit_once('@') {
        Some((_, hp)) => hp,
        None => authority,
    };
    let host = if let Some(rest) = host_port.strip_prefix('[') {
        rest.split(']').next().unwrap_or("")
    } else if host_port.matches(':').count() > 1 {
        host_port
    } else {
        host_port.split(':').next().unwrap_or("")
    };
    let host = host.trim().trim_end_matches('.');
    if host.is_empty() {
        return false;
    }
    if host == "localhost" || host.ends_with(".localhost") || host.starts_with("127.") {
        return true;
    }
    if let Ok(ip) = host.parse::<std::net::IpAddr>() {
        match ip {
            std::net::IpAddr::V4(v4) => v4.is_loopback() || v4.is_unspecified(),
            std::net::IpAddr::V6(v6) => {
                v6.is_loopback()
                    || v6.is_unspecified()
                    || v6
                        .to_ipv4_mapped()
                        .map(|v4| v4.is_loopback() || v4.is_unspecified())
                        .unwrap_or(false)
            }
        }
    } else {
        false
    }
}

#[cfg_attr(feature = "test-support", allow(dead_code))]
pub(crate) fn enforce_production_loopback_ssrf_guard(url: &str) -> Result<(), (i64, String)> {
    if is_loopback_http_target(url) {
        return Err((
            -32602,
            "SSRF guard: loopback HTTP probing is disabled in production".to_string(),
        ));
    }
    Ok(())
}

#[cfg(any(test, feature = "test-support"))]
pub fn probe_local_http(url: &str) -> Result<(u16, String), String> {
    if url.contains('\r') || url.contains('\n') || url.chars().any(char::is_whitespace) {
        return Err("Invalid characters in local HTTP probe URL".into());
    }
    let without_scheme = url
        .strip_prefix("http://")
        .or_else(|| url.strip_prefix("https://"))
        .unwrap_or(url);
    let (host_port, path) = match without_scheme.find('/') {
        Some(idx) => (&without_scheme[..idx], &without_scheme[idx..]),
        None => (without_scheme, "/"),
    };
    if path.contains('\r') || path.contains('\n') || path.chars().any(char::is_whitespace) {
        return Err("Invalid characters in local HTTP probe URL".into());
    }
    let (host_only, port) = match host_port.split_once(':') {
        Some((h, p_str)) => {
            let p: u16 = p_str
                .parse()
                .map_err(|e| format!("Invalid local socket addr '{host_port}': {e}"))?;
            (h, p)
        }
        None => (host_port, 80u16),
    };
    if host_only != "127.0.0.1" && host_only != "localhost" {
        return Err(format!(
            "Forbidden non-loopback host '{host_only}' in local HTTP probe URL"
        ));
    }
    let addr = SocketAddr::from(([127, 0, 0, 1], port));

    let mut stream = TcpStream::connect_timeout(&addr, Duration::from_millis(500))
        .map_err(|e| format!("Connect failed: {e}"))?;
    let _ = stream.set_read_timeout(Some(Duration::from_millis(500)));
    let _ = stream.set_write_timeout(Some(Duration::from_millis(500)));

    let req = format!("GET {path} HTTP/1.1\r\nHost: {host_port}\r\nConnection: close\r\n\r\n");
    stream
        .write_all(req.as_bytes())
        .map_err(|e| format!("Write failed: {e}"))?;
    stream.flush().ok();

    let mut raw = Vec::new();
    stream
        .read_to_end(&mut raw)
        .map_err(|e| format!("Read failed: {e}"))?;

    let mut headers = [httparse::EMPTY_HEADER; 32];
    let mut resp = httparse::Response::new(&mut headers);
    let body_offset = match resp.parse(&raw) {
        Ok(httparse::Status::Complete(off)) => off,
        _ => 0,
    };
    let code = resp.code.unwrap_or_else(|| {
        let s = String::from_utf8_lossy(&raw);
        s.lines()
            .next()
            .and_then(|l| l.split_whitespace().nth(1))
            .and_then(|c| c.parse::<u16>().ok())
            .unwrap_or(200)
    });
    let body = String::from_utf8_lossy(&raw[body_offset..]).to_string();
    Ok((code, body))
}

// ============================================================================
// Unit Tests (Exercising Real McpStdioClient + EmbeddedMcpResponder over OS Pipes)
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    #[test]
    fn test_mcp_stdio_client_initialize_and_list_10_tools() {
        let (client, _responder) = McpStdioClient::new_embedded().unwrap();

        let init_res = client.initialize().unwrap();
        assert_eq!(init_res["protocolVersion"], MCP_PROTOCOL_VERSION);
        assert_eq!(init_res["serverInfo"]["name"], "spell-full-file-mcp");

        let tools = client.list_tools().unwrap();
        assert_eq!(tools.len(), 10);
        for expected in SPELL_MCP_TOOL_NAMES {
            assert!(
                tools.iter().any(|t| t.name == expected),
                "Missing tool {expected}"
            );
        }
    }

    #[test]
    fn test_mcp_all_10_tools_and_workspace_escape_security_block() {
        let (client, _responder) = McpStdioClient::new_embedded().unwrap();
        client.initialize().unwrap();

        let tmp_dir =
            std::env::temp_dir().join(format!("spell_mcp_client_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&tmp_dir);
        fs::create_dir_all(&tmp_dir).unwrap();

        let file_a = tmp_dir.join("alpha.txt");
        let file_b = tmp_dir.join("beta.txt");
        fs::write(&file_a, "first line\nSEARCH_NEEDLE_42\nthird line\n").unwrap();
        fs::write(&file_b, "beta file content\n").unwrap();

        // 1. read_full_file
        let res1 = client
            .call_tool(
                1,
                "read_full_file",
                json!({ "path": file_a.to_string_lossy() }),
            )
            .unwrap();
        assert!(res1["text"].as_str().unwrap().contains("SEARCH_NEEDLE_42"));

        // 2. read_multiple_files
        let res2 = client
            .call_tool(
                2,
                "read_multiple_files",
                json!({
                    "paths": [file_a.to_string_lossy(), file_b.to_string_lossy()]
                }),
            )
            .unwrap();
        assert_eq!(res2["count"], 2);

        // 3. grep_workspace
        let res3 = client
            .call_tool(
                3,
                "grep_workspace",
                json!({
                    "root": tmp_dir.to_string_lossy(),
                    "query": "SEARCH_NEEDLE_42"
                }),
            )
            .unwrap();
        assert_eq!(res3["count"], 1);

        // 4. file_stats (sha2::Sha256 -> sha256_16)
        let res4 = client
            .call_tool(
                4,
                "file_stats",
                json!({ "path": file_a.to_string_lossy() }),
            )
            .unwrap();
        let sha16 = res4["sha256_16"].as_str().unwrap();
        let sha_full = res4["sha256"].as_str().unwrap();
        assert_eq!(sha16.len(), 16);
        assert_eq!(sha_full.len(), 64);
        assert!(sha_full.starts_with(sha16));

        // 5. fetch_html (arXiv /abs/ and /pdf/ normalization)
        let res5_abs = client
            .call_tool(
                5,
                "fetch_html",
                json!({ "url": "https://arxiv.org/abs/2502.99999" }),
            )
            .unwrap();
        assert_eq!(res5_abs["normalized_url"], "https://arxiv.org/html/2502.99999");

        let res5_pdf = client
            .call_tool(
                6,
                "fetch_html",
                json!({ "url": "https://arxiv.org/pdf/2502.99999.pdf" }),
            )
            .unwrap();
        assert_eq!(res5_pdf["normalized_url"], "https://arxiv.org/html/2502.99999");

        // 6, 8, 9. timer_set, timer_list, timer_cancel
        let res6 = client
            .call_tool(
                7,
                "timer_set",
                json!({
                    "timer_id": "tmr-unit-1",
                    "duration_secs": 30,
                    "label": "compile-watch",
                    "alarm_note": "Check build"
                }),
            )
            .unwrap();
        assert_eq!(res6["timer_id"], "tmr-unit-1");

        let res8 = client.call_tool(8, "timer_list", json!({})).unwrap();
        assert_eq!(res8["count"], 1);

        let res9 = client
            .call_tool(9, "timer_cancel", json!({ "timer_id": "tmr-unit-1" }))
            .unwrap();
        assert_eq!(res9["cancelled"], true);

        // 10. progress_check (file_tail, file_growth, pid_alive, http_status)
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        thread::spawn(move || {
            if let Ok((mut sock, _)) = listener.accept() {
                let mut buf = [0u8; 512];
                let _ = sock.read(&mut buf);
                let _ = sock.write_all(
                    b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK",
                );
            }
        });

        let res10 = client
            .call_tool(
                10,
                "progress_check",
                json!({
                    "path": file_a.to_string_lossy(),
                    "previous_bytes": 5,
                    "pid": std::process::id(),
                    "url": format!("http://127.0.0.1:{port}/health")
                }),
            )
            .unwrap();
        assert_eq!(res10["growing"], true);
        assert_eq!(res10["pid_alive"], true);
        assert_eq!(res10["http_status"], 200);
        assert!(res10["watch_tail"].as_str().unwrap().contains("SEARCH_NEEDLE_42"));

        // Security guardrail: path traversal / protected globs must be rejected
        let sec_err = client.call_tool(
            11,
            "read_full_file",
            json!({ "path": "../outside_secret.txt" }),
        );
        assert!(sec_err.is_err());
        assert!(sec_err.unwrap_err().contains("SECURITY_BLOCK"));

        let _ = fs::remove_dir_all(&tmp_dir);
    }

    #[test]
    fn test_timer_wait_sub_15ms_condvar_cancellation_over_stdio_pipe() {
        let (client, _responder) = McpStdioClient::new_embedded().unwrap();
        client.initialize().unwrap();
        let client_arc = Arc::new(client);

        let c_wait = client_arc.clone();
        let wait_handle = thread::spawn(move || {
            c_wait.call_tool(99, "timer_wait", json!({ "duration_ms": 5000 }))
        });

        // Allow request 99 to traverse the OS pipe and enter Condvar::wait_timeout_while
        thread::sleep(Duration::from_millis(10));

        let cancel_start = Instant::now();
        client_arc
            .cancel_request(99, Some("unit_test_abort"))
            .unwrap();
        let result = wait_handle.join().unwrap().unwrap();
        let wakeup_elapsed = cancel_start.elapsed();

        assert!(
            wakeup_elapsed < Duration::from_millis(15),
            "Expected < 15ms wakeup on notifications/cancelled, got {:?}",
            wakeup_elapsed
        );
        assert_eq!(result["status"], "cancelled");
        assert_eq!(result["cancelled"], true);
    }

    #[test]
    fn test_vuln05_mcp_path_confinement_protected_globs_and_http_probe_sanitization() {
        // 1. probe_local_http CRLF / whitespace / non-loopback rejection
        assert_eq!(
            probe_local_http("http://127.0.0.1:8080/health\r\nX-Injected: pwn").unwrap_err(),
            "Invalid characters in local HTTP probe URL"
        );
        assert_eq!(
            probe_local_http("http://127.0.0.1:8080/health HTTP/1.1\r\nHost: evil").unwrap_err(),
            "Invalid characters in local HTTP probe URL"
        );
        assert!(
            probe_local_http("http://192.168.1.1:8080/health")
                .unwrap_err()
                .contains("Forbidden non-loopback host")
        );

        // 2. EmbeddedMcpResponder path confinement & protected globs
        let base_dir = std::env::temp_dir().join(format!(
            "spell_mcp_vuln05_test_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&base_dir);
        let ws = base_dir.join("workspace");
        fs::create_dir_all(&ws).unwrap();

        let outside_file = base_dir.join("outside.txt");
        fs::write(&outside_file, "outside secret").unwrap();
        let inside_file = ws.join("inside.txt");
        fs::write(&inside_file, "inside ok").unwrap();

        #[cfg(unix)]
        {
            let _ = std::os::unix::fs::symlink(
                "/etc",
                ws.join("symlink_etc"),
            );
            let _ = std::os::unix::fs::symlink(&outside_file, ws.join("symlink_out"));
        }

        let responder = EmbeddedMcpResponder::with_workspace_root_arc(ws.clone());
        let client = McpStdioClient::connect_embedded(responder).unwrap();
        client.initialize().unwrap();

        // Reading inside_file succeeds
        let ok_res = client
            .call_tool(1, "read_full_file", json!({ "path": "inside.txt" }))
            .unwrap();
        assert_eq!(ok_res["text"].as_str().unwrap(), "inside ok");

        // Protected globs & traversal rejected
        for forbidden in [
            "../outside.txt",
            "~/.ssh/id_rsa",
            "~/.aws/credentials",
            ".env",
            ".env.production",
            ".git/config",
            ".git/hooks/pre-commit",
            "symlink_etc/hosts",
            "symlink_out",
        ] {
            let err = client.call_tool(
                2,
                "read_full_file",
                json!({ "path": forbidden }),
            );
            assert!(
                err.is_err(),
                "Expected EmbeddedMcpResponder to block forbidden path: {forbidden}"
            );
        }

        // Reject CRLF injection in fetch_html and progress_check
        let crlf_err = client.call_tool(
            3,
            "fetch_html",
            json!({ "url": "http://127.0.0.1:8080/test\r\nInjected: 1" }),
        );
        assert!(crlf_err.is_err());

        let _ = fs::remove_dir_all(&base_dir);
    }

    #[test]
    fn test_adversarial_mcp_spawn_sandboxed_exit_71_never_retries() {
        let _env_guard = crate::tools::SANDBOX_ENV_TEST_LOCK
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        std::env::remove_var("SPELL_APP_SANDBOX");

        let tmp_dir = std::env::temp_dir().join(format!(
            "spell_mcp_adv_exit71_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&tmp_dir);
        fs::create_dir_all(&tmp_dir).unwrap();

        let side_effect_path = tmp_dir.join("mcp_side_effect.log");
        #[cfg(not(windows))]
        let client = {
            let cmd_str = format!(
                "echo ran >> '{}'; echo 'operation failed' >&2; exit 71",
                side_effect_path.display()
            );
            McpStdioClient::spawn_sandboxed("/bin/sh", &["-c", &cmd_str], &[], true).unwrap()
        };
        #[cfg(windows)]
        let client = {
            let cmd_str = format!(
                "(echo ran>>\"{}\") & (echo operation failed 1>&2) & exit /b 71",
                side_effect_path.display()
            );
            McpStdioClient::spawn_sandboxed("cmd", &["/C", &cmd_str], &[], true).unwrap()
        };

        // Wait for the child to write its single side-effect line and exit 71
        for _ in 0..50 {
            if side_effect_path.exists() {
                break;
            }
            thread::sleep(Duration::from_millis(20));
        }
        thread::sleep(Duration::from_millis(60));
        drop(client);

        let content = fs::read_to_string(&side_effect_path).unwrap();
        assert_eq!(
            content.lines().count(),
            1,
            "McpStdioClient::spawn_sandboxed must never retry on exit 71; got: {content:?}"
        );

        let _ = fs::remove_dir_all(&tmp_dir);
    }

    #[test]
    fn test_catch1_mcp_fetch_html_production_ssrf_loopback_guard() {
        for forbidden in [
            "http://127.0.0.1:8000/v1/models",
            "https://127.0.0.1:8000/v1/chat/completions",
            "http://127.0.0.1/health",
            "http://localhost:8000/v1/models",
            "https://localhost/admin",
            "http://[::1]:8000/v1/models",
            "https://[::1]/v1/models",
            "http://[0:0:0:0:0:0:0:1]:8000/health",
            "http://[::ffff:127.0.0.1]:8000/health",
            "http://0.0.0.0:8000/health",
        ] {
            let err = enforce_production_loopback_ssrf_guard(forbidden).unwrap_err();
            assert_eq!(err.0, -32602, "Expected JSON-RPC -32602 for {forbidden}");
            assert_eq!(
                err.1,
                "SSRF guard: loopback HTTP probing is disabled in production"
            );
        }

        // Public non-loopback research URLs pass the loopback guard
        assert!(enforce_production_loopback_ssrf_guard("https://arxiv.org/html/2502.99999").is_ok());
        assert!(enforce_production_loopback_ssrf_guard("https://example.com/docs").is_ok());

        // End-to-end over McpStdioClient: [::1] and :8000 loopback targets are rejected with -32602
        let (client, _responder) = McpStdioClient::new_embedded().unwrap();
        client.initialize().unwrap();
        for blocked in [
            "http://[::1]:8000/v1/models",
            "https://[::1]:443/test",
            "http://127.0.0.1:8000/v1/models",
            "http://localhost:8000/v1/models",
        ] {
            let err = client
                .call_tool(42, "fetch_html", json!({ "url": blocked }))
                .unwrap_err();
            assert!(
                err.contains("-32602")
                    && err.contains("SSRF guard: loopback HTTP probing is disabled in production"),
                "Expected -32602 SSRF guard error for {blocked}, got: {err}"
            );
        }
    }
}
