//! SQLite3 Persistence (`SpellStorage`) via bundled SQLite3 (`libsqlite3-sys`, cross-platform macOS/Linux/Windows).
//!
//! Implements:
//! - Minimal, zero-hazard C FFI calls via bundled `libsqlite3-sys` (`sqlite3_open`,
//!   `sqlite3_close`, `sqlite3_exec`, and `sqlite3_free`).
//! - Relational and KV document tables (`kv_documents`, `projects`, `threads`, `messages`,
//!   `tags`, `tool_permissions`).
//! - **Exact Wire History Preservation**: Stores exact OpenAI `WireMessage` JSON
//!   (including `reasoning_content`, `tool_calls`, and `tool_call_id`) alongside rich
//!   presentation `UiChatEvent` items so resending multi-turn history never invalidates
//!   prefix caches.

use std::collections::HashMap;
use std::ffi::{c_void, CStr, CString};
use std::fs;
use std::os::raw::{c_char, c_int};
use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

use libsqlite3_sys::{
    sqlite3, sqlite3_busy_timeout, sqlite3_close, sqlite3_exec, sqlite3_free, sqlite3_open,
    SQLITE_OK,
};
use parking_lot::Mutex;
use serde::{Deserialize, Serialize};

use crate::contract::{EngineContextAdvice, TimerRecord, WireMessage, COMPACTION_SUMMARY_MARKER};
use crate::tags::ConversationTag;
use crate::tools::ToolPermissionPolicy;

static EVENT_SEQ_COUNTER: AtomicU64 = AtomicU64::new(1);

/// Returns the current UNIX timestamp in seconds.
pub fn now_unix_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

/// Returns the current UNIX timestamp in milliseconds.
pub fn now_unix_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as i64
}

/// Generates a unique monotonic ID with the given prefix.
pub fn next_storage_id(prefix: &str) -> String {
    let seq = EVENT_SEQ_COUNTER.fetch_add(1, Ordering::SeqCst);
    let ts = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis();
    format!("{prefix}-{ts:x}-{seq:04x}")
}

/// Escapes a string for safe embedding in a single-quoted SQLite string literal.
pub fn sql_escape(input: &str) -> String {
    let mut out = String::with_capacity(input.len() + 8);
    for ch in input.chars() {
        if ch == '\'' {
            out.push_str("''");
        } else if ch != '\0' {
            out.push(ch);
        }
    }
    out
}

/// Single row returned by `sqlite3_exec`: list of `(column_name, column_value)` pairs.
pub type SqlRow = Vec<(String, String)>;

extern "C" fn sqlite_collect_rows_callback(
    arg: *mut c_void,
    argc: c_int,
    argv: *mut *mut c_char,
    col_names: *mut *mut c_char,
) -> c_int {
    if arg.is_null() || argc <= 0 {
        return 0;
    }
    // SAFETY: `arg` points to the caller's stack-allocated `Vec<SqlRow>` passed to `sqlite3_exec`,
    // and SQLite guarantees `argv` and `col_names` hold `argc` valid or null C-string pointers for
    // the duration of the callback.
    unsafe {
        let rows = &mut *(arg as *mut Vec<SqlRow>);
        let count = argc as usize;
        let mut row = Vec::with_capacity(count);
        for i in 0..count {
            let name_ptr = if col_names.is_null() {
                std::ptr::null_mut()
            } else {
                *col_names.add(i)
            };
            let val_ptr = if argv.is_null() {
                std::ptr::null_mut()
            } else {
                *argv.add(i)
            };
            let col_name = if name_ptr.is_null() {
                String::new()
            } else {
                CStr::from_ptr(name_ptr).to_string_lossy().into_owned()
            };
            let col_val = if val_ptr.is_null() {
                String::new()
            } else {
                CStr::from_ptr(val_ptr).to_string_lossy().into_owned()
            };
            row.push((col_name, col_val));
        }
        rows.push(row);
    }
    0
}

struct SqliteConnection {
    raw: *mut sqlite3,
}

// SAFETY: Access to the underlying `sqlite3*` handle is serialized through a `Mutex`, and bundled
// SQLite is compiled in serialized/multi-thread safe mode so moving the handle across threads is sound.
unsafe impl Send for SqliteConnection {}

impl Drop for SqliteConnection {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            // SAFETY: `self.raw` is a valid open `sqlite3*` handle owned uniquely by `self`, and
            // setting it to null prevents double-close.
            unsafe {
                let _ = sqlite3_close(self.raw);
            }
            self.raw = std::ptr::null_mut();
        }
    }
}

// ============================================================================
// Persisted Domain Models (Exact Wire History + UI Presentation Events)
// ============================================================================

/// Persisted workspace project metadata (Pane 1).
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct StoredProject {
    pub id: String,
    pub name: String,
    #[serde(alias = "rootPath")]
    pub root_path: String,
    #[serde(default, alias = "isReadOnly")]
    pub is_read_only: bool,
    #[serde(default = "now_unix_secs", alias = "createdAt")]
    pub created_at: u64,
}

impl StoredProject {
    pub fn new(
        id: impl Into<String>,
        name: impl Into<String>,
        root_path: impl Into<String>,
        is_read_only: bool,
    ) -> Self {
        Self {
            id: id.into(),
            name: name.into(),
            root_path: root_path.into(),
            is_read_only,
            created_at: now_unix_secs(),
        }
    }

    /// Returns the effective [`crate::tools::FolderAccessTier`] for this project.
    pub fn effective_access_tier(&self) -> crate::tools::FolderAccessTier {
        if self.is_read_only {
            crate::tools::FolderAccessTier::ReadOnly
        } else {
            crate::tools::FolderAccessTier::DirectWrite
        }
    }
}

/// Tool call card metadata rendered inside `UiChatEvent` (Pane 3).
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct UiToolCallItem {
    pub id: String,
    pub name: String,
    #[serde(alias = "argumentsJSON")]
    pub arguments_json: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output: Option<String>,
    /// `"running" | "completed" | "awaiting_confirmation" | "denied" | "failed"`
    pub status: String,
    #[serde(default, alias = "isMCP")]
    pub is_mcp: bool,
    /// Error category for failed/denied calls (see `tools::classify_tool_error`).
    #[serde(default, alias = "errorKind", skip_serializing_if = "Option::is_none")]
    pub error_kind: Option<String>,
}

impl UiToolCallItem {
    pub fn new(
        id: impl Into<String>,
        name: impl Into<String>,
        arguments_json: impl Into<String>,
        output: Option<String>,
        status: impl Into<String>,
        is_mcp: bool,
    ) -> Self {
        let status: String = status.into();
        let error_kind = crate::tools::classify_tool_error(output.as_deref().unwrap_or(""), &status)
            .map(str::to_string);
        Self {
            id: id.into(),
            name: name.into(),
            arguments_json: arguments_json.into(),
            output,
            status,
            is_mcp,
            error_kind,
        }
    }
}

/// Rich UI presentation event stored alongside `wire_messages` in `StoredThread`.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct UiChatEvent {
    pub id: String,
    /// `"user" | "assistant" | "tool_call" | "engine_compaction_notice" | "engine_timer_alarm"`
    pub kind: String,
    pub role: String,
    pub content: String,
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        alias = "reasoningContent"
    )]
    pub reasoning_content: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none", alias = "toolCall")]
    pub tool_call: Option<UiToolCallItem>,
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        alias = "contextAdvice"
    )]
    pub context_advice: Option<EngineContextAdvice>,
    #[serde(default = "now_unix_secs")]
    pub timestamp: u64,
    /// Tokens the engine generated for this response (`usage.completion_tokens`, summed over the
    /// tool-call steps that led to it).
    #[serde(default, skip_serializing_if = "Option::is_none", alias = "completionTokens")]
    pub completion_tokens: Option<u64>,
}

impl UiChatEvent {
    /// Creates a user prompt presentation event.
    pub fn user_message(content: impl Into<String>) -> Self {
        Self {
            id: next_storage_id("ev-user"),
            kind: "user".to_string(),
            role: "user".to_string(),
            content: content.into(),
            reasoning_content: None,
            tool_call: None,
            context_advice: None,
            timestamp: now_unix_secs(),
            completion_tokens: None,
        }
    }

    /// Creates an assistant response presentation event with optional `reasoning_content` and `EngineContextAdvice`.
    pub fn assistant_message(
        content: impl Into<String>,
        reasoning_content: Option<String>,
        context_advice: Option<EngineContextAdvice>,
    ) -> Self {
        Self {
            id: next_storage_id("ev-asst"),
            kind: "assistant".to_string(),
            role: "assistant".to_string(),
            content: content.into(),
            reasoning_content: reasoning_content.filter(|s| !s.is_empty()),
            tool_call: None,
            context_advice,
            timestamp: now_unix_secs(),
            completion_tokens: None,
        }
    }

    /// Creates a tool execution or confirmation card presentation event.
    pub fn tool_call_card(tool_call: UiToolCallItem) -> Self {
        let summary = match (&tool_call.status[..], &tool_call.output) {
            ("awaiting_confirmation", _) => {
                format!(
                    "Tool `{}` requires confirmation before execution.",
                    tool_call.name
                )
            }
            ("denied", _) => format!("Tool `{}` execution was denied by user.", tool_call.name),
            (_, Some(out)) => out.clone(),
            (_, None) => format!("Executed tool `{}`", tool_call.name),
        };
        Self {
            id: format!("ev-tool-{}", tool_call.id),
            kind: "tool_call".to_string(),
            role: "tool".to_string(),
            content: summary,
            reasoning_content: None,
            tool_call: Some(tool_call),
            context_advice: None,
            timestamp: now_unix_secs(),
            completion_tokens: None,
        }
    }

    /// Creates an informational notice when the context epoch is reset.
    pub fn engine_compaction_notice(advice: &EngineContextAdvice) -> Self {
        let text = format!(
            "{COMPACTION_SUMMARY_MARKER} Context Epoch #{} • Decode {:.1} tok/s (EWMA {:.1} tok/s) • Reason: {}",
            advice.compaction_epoch,
            advice.decode_tok_s,
            advice.decode_tok_s_ewma,
            advice.reason
        );
        Self {
            id: next_storage_id("ev-compact"),
            kind: "engine_compaction_notice".to_string(),
            role: "system".to_string(),
            content: text,
            reasoning_content: None,
            tool_call: None,
            context_advice: Some(advice.clone()),
            timestamp: now_unix_secs(),
            completion_tokens: None,
        }
    }

    /// Creates an engine timer wakeup alarm banner event.
    pub fn engine_timer_alarm(banner: impl Into<String>) -> Self {
        Self {
            id: next_storage_id("ev-alarm"),
            kind: "engine_timer_alarm".to_string(),
            role: "system".to_string(),
            content: banner.into(),
            reasoning_content: None,
            tool_call: None,
            context_advice: None,
            timestamp: now_unix_secs(),
            completion_tokens: None,
        }
    }

    #[allow(non_snake_case)]
    pub fn ToolCallCard(tool_call: UiToolCallItem) -> Self {
        Self::tool_call_card(tool_call)
    }

    #[allow(non_snake_case)]
    pub fn EngineCompactionNotice(advice: &EngineContextAdvice) -> Self {
        Self::engine_compaction_notice(advice)
    }

    #[allow(non_snake_case)]
    pub fn EngineTimerAlarm(banner: impl Into<String>) -> Self {
        Self::engine_timer_alarm(banner)
    }
}

/// Persisted multi-turn conversation thread (preserving exact wire history).
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct StoredThread {
    pub id: String,
    #[serde(alias = "projectId")]
    pub project_id: String,
    pub title: String,
    /// `"write"` or `"ask"`
    pub profile: String,
    #[serde(default, alias = "activeTags")]
    pub active_tags: Vec<String>,
    /// Exact OpenAI wire messages (`reasoning_content`, `tool_calls`, `tool_call_id`) preserved verbatim.
    #[serde(default, alias = "wireMessages")]
    pub wire_messages: Vec<WireMessage>,
    /// Rich presentation events for the SwiftUI 3-pane interface.
    #[serde(default, alias = "events", alias = "uiEvents")]
    pub ui_events: Vec<UiChatEvent>,
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        alias = "lastContextAdvice"
    )]
    pub last_context_advice: Option<EngineContextAdvice>,
    #[serde(default, alias = "engineCompactionEpoch")]
    pub engine_compaction_epoch: u64,
    #[serde(default, alias = "promptTokens")]
    pub prompt_tokens: u64,
    #[serde(default = "now_unix_secs", alias = "createdAt")]
    pub created_at: u64,
    #[serde(default = "now_unix_secs", alias = "updatedAt")]
    pub updated_at: u64,
}

impl StoredThread {
    pub fn new(
        id: impl Into<String>,
        project_id: impl Into<String>,
        title: impl Into<String>,
        profile: impl Into<String>,
        active_tags: Vec<String>,
    ) -> Self {
        let now = now_unix_secs();
        Self {
            id: id.into(),
            project_id: project_id.into(),
            title: title.into(),
            profile: profile.into(),
            active_tags,
            wire_messages: Vec::new(),
            ui_events: Vec::new(),
            last_context_advice: None,
            engine_compaction_epoch: 0,
            prompt_tokens: 0,
            created_at: now,
            updated_at: now,
        }
    }
}

// ============================================================================
// RawSqliteStore & SpellStorage — Real SQLite3 Engine Store
// ============================================================================

/// Schema-free, thread-safe SQLite3 store backed by bundled SQLite3 (`libsqlite3-sys`).
pub(crate) struct RawSqliteStore {
    conn: Mutex<SqliteConnection>,
    db_path: String,
}

impl std::fmt::Debug for RawSqliteStore {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RawSqliteStore")
            .field("db_path", &self.db_path)
            .finish()
    }
}

impl RawSqliteStore {
    /// Opens or creates a raw SQLite3 database at `db_path` (creating parent directories when
    /// `db_path != ":memory:"`) with an optional `busy_timeout_ms`, without running schema DDL.
    pub(crate) fn open(db_path: &Path, busy_timeout_ms: Option<i32>) -> Result<Self, String> {
        let path_str = db_path.to_string_lossy().into_owned();
        if path_str != ":memory:" {
            if let Some(parent) = db_path.parent() {
                if !parent.as_os_str().is_empty() {
                    fs::create_dir_all(parent).map_err(|e| {
                        format!(
                            "Failed to create parent directory {}: {e}",
                            parent.display()
                        )
                    })?;
                }
            }
        }
        Self::open_str(&path_str, busy_timeout_ms)
    }

    /// Opens or creates a raw SQLite3 database from a string path or `":memory:"` without running
    /// [`SpellStorage::init_schema`].
    pub(crate) fn open_str(db_path: &str, busy_timeout_ms: Option<i32>) -> Result<Self, String> {
        let target = if db_path.trim().is_empty() {
            ":memory:"
        } else {
            db_path.trim()
        };
        if target != ":memory:" {
            let p = Path::new(target);
            if let Some(parent) = p.parent() {
                if !parent.as_os_str().is_empty() {
                    let _ = fs::create_dir_all(parent);
                }
            }
        }

        let c_filename =
            CString::new(target).map_err(|e| format!("Invalid SQLite path '{target}': {e}"))?;
        let mut raw_db: *mut sqlite3 = std::ptr::null_mut();
        // SAFETY: `c_filename` is a valid NUL-terminated C string and `raw_db` is a valid out-pointer.
        let rc = unsafe { sqlite3_open(c_filename.as_ptr(), &mut raw_db) };
        if rc != SQLITE_OK || raw_db.is_null() {
            if !raw_db.is_null() {
                // SAFETY: `raw_db` was returned by `sqlite3_open` and is closed immediately on failure.
                unsafe {
                    let _ = sqlite3_close(raw_db);
                }
            }
            return Err(format!(
                "sqlite3_open failed for '{target}' with code {rc}"
            ));
        }

        if let Some(ms) = busy_timeout_ms {
            // SAFETY: `raw_db` is an open, exclusively owned `sqlite3*` handle.
            unsafe {
                let _ = sqlite3_busy_timeout(raw_db, ms);
            }
        }

        Ok(Self {
            conn: Mutex::new(SqliteConnection { raw: raw_db }),
            db_path: target.to_string(),
        })
    }

    pub(crate) fn db_path(&self) -> &str {
        &self.db_path
    }

    fn execute_sql_locked(raw_db: *mut sqlite3, sql: &str) -> Result<Vec<SqlRow>, String> {
        if raw_db.is_null() {
            return Err("SQLite connection is closed".to_string());
        }
        let cleaned = sql.replace('\0', "");
        let c_sql = CString::new(cleaned).map_err(|e| format!("Invalid SQL string: {e}"))?;
        let mut rows: Vec<SqlRow> = Vec::new();
        let mut errmsg_ptr: *mut c_char = std::ptr::null_mut();

        // SAFETY: `raw_db` is a valid open `sqlite3*` protected by our `Mutex`, `c_sql` is a valid
        // NUL-terminated SQL string, `rows` outlives `sqlite3_exec`, and `errmsg_ptr` is a valid out-pointer.
        let rc = unsafe {
            sqlite3_exec(
                raw_db,
                c_sql.as_ptr(),
                Some(sqlite_collect_rows_callback),
                (&mut rows as *mut Vec<SqlRow>) as *mut c_void,
                &mut errmsg_ptr,
            )
        };

        if rc != SQLITE_OK {
            let err_str = if !errmsg_ptr.is_null() {
                // SAFETY: Non-null `errmsg_ptr` from `sqlite3_exec` is a valid NUL-terminated C string
                // allocated by SQLite that must be released with `sqlite3_free`.
                let s = unsafe { CStr::from_ptr(errmsg_ptr).to_string_lossy().into_owned() };
                // SAFETY: `errmsg_ptr` was allocated by `sqlite3_exec` and is freed once here.
                unsafe {
                    sqlite3_free(errmsg_ptr as *mut c_void);
                }
                s
            } else {
                format!("SQLite error code {rc}")
            };
            return Err(err_str);
        }

        if !errmsg_ptr.is_null() {
            // SAFETY: Non-null `errmsg_ptr` from `sqlite3_exec` must be freed via `sqlite3_free`.
            unsafe {
                sqlite3_free(errmsg_ptr as *mut c_void);
            }
        }
        Ok(rows)
    }

    /// Executes one or more SQL statements and returns any result rows as `Vec<SqlRow>`.
    pub(crate) fn execute_sql(&self, sql: &str) -> Result<Vec<SqlRow>, String> {
        let guard = self.conn.lock();
        Self::execute_sql_locked(guard.raw, sql)
    }
}

/// Thread-safe SQLite3 persistence layer backed by bundled SQLite3 (`libsqlite3-sys`).
#[derive(Clone)]
pub struct SpellStorage {
    raw: Arc<RawSqliteStore>,
}

impl std::fmt::Debug for SpellStorage {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SpellStorage")
            .field("db_path", &self.raw.db_path())
            .finish()
    }
}

impl SpellStorage {
    /// Opens or creates a SQLite3 database at `db_path` (or `":memory:"` for in-memory storage).
    pub fn open(db_path: impl AsRef<Path>) -> Result<Self, String> {
        let raw = RawSqliteStore::open(db_path.as_ref(), None)?;
        let store = Self { raw: Arc::new(raw) };
        store.init_schema()?;
        Ok(store)
    }

    /// Opens a SQLite3 database from a string path or `":memory:"`.
    pub fn open_str(db_path: &str) -> Result<Self, String> {
        let raw = RawSqliteStore::open_str(db_path, None)?;
        let store = Self { raw: Arc::new(raw) };
        store.init_schema()?;
        Ok(store)
    }

    /// Opens an isolated in-memory SQLite3 database (`":memory:"`).
    pub fn new_in_memory() -> Result<Self, String> {
        Self::open_str(":memory:")
    }

    /// Returns the path or `":memory:"` identifier for this database.
    pub fn db_path(&self) -> &str {
        self.raw.db_path()
    }

    /// Executes one or more SQL statements and returns any result rows as `Vec<SqlRow>`.
    pub fn execute_sql(&self, sql: &str) -> Result<Vec<SqlRow>, String> {
        self.raw.execute_sql(sql)
    }

    /// Initializes the relational and document schema for `SpellStorage`.
    pub fn init_schema(&self) -> Result<(), String> {
        let ddl = "
            PRAGMA journal_mode = WAL;
            PRAGMA synchronous = NORMAL;

            CREATE TABLE IF NOT EXISTS kv_documents (
                collection TEXT NOT NULL,
                id TEXT NOT NULL,
                json_payload TEXT NOT NULL,
                updated_at INTEGER NOT NULL,
                PRIMARY KEY (collection, id)
            );

            CREATE TABLE IF NOT EXISTS projects (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                root_path TEXT NOT NULL,
                is_read_only INTEGER NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL,
                json_payload TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS threads (
                id TEXT PRIMARY KEY,
                project_id TEXT NOT NULL,
                title TEXT NOT NULL,
                profile TEXT NOT NULL,
                active_tags_json TEXT NOT NULL,
                wire_messages_json TEXT NOT NULL,
                ui_events_json TEXT NOT NULL,
                last_context_advice_json TEXT,
                engine_compaction_epoch INTEGER NOT NULL DEFAULT 0,
                prompt_tokens INTEGER NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL,
                json_payload TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS messages (
                thread_id TEXT NOT NULL,
                seq_idx INTEGER NOT NULL,
                role TEXT NOT NULL,
                wire_json TEXT NOT NULL,
                updated_at INTEGER NOT NULL,
                PRIMARY KEY (thread_id, seq_idx)
            );

            CREATE TABLE IF NOT EXISTS tags (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                category TEXT NOT NULL,
                description TEXT NOT NULL,
                rubric_prompt TEXT NOT NULL,
                is_builtin INTEGER NOT NULL DEFAULT 0,
                json_payload TEXT NOT NULL,
                updated_at INTEGER NOT NULL
            );

            CREATE TABLE IF NOT EXISTS tool_permissions (
                tool_name TEXT PRIMARY KEY,
                policy TEXT NOT NULL,
                updated_at INTEGER NOT NULL
            );

            CREATE TABLE IF NOT EXISTS timers (
                id TEXT PRIMARY KEY,
                thread_id TEXT NOT NULL,
                label TEXT NOT NULL,
                note TEXT NOT NULL,
                duration_s REAL NOT NULL,
                created_at_ms INTEGER NOT NULL,
                fire_at_ms INTEGER NOT NULL,
                status TEXT NOT NULL DEFAULT 'armed',
                delivered INTEGER NOT NULL DEFAULT 0,
                watch_target_json TEXT,
                updated_at INTEGER NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_timers_thread_status
                ON timers (thread_id, status, fire_at_ms);
        ";
        self.execute_sql(ddl).map(|_| ())
    }

    fn row_get<'a>(row: &'a SqlRow, col: &str) -> Option<&'a str> {
        row.iter()
            .find(|(k, _)| k == col)
            .map(|(_, v)| v.as_str())
    }

    // ========================================================================
    // Generic KV Document Operations (`kv_documents`)
    // ========================================================================

    pub fn put_kv_document(
        &self,
        collection: &str,
        id: &str,
        json_payload: &str,
    ) -> Result<(), String> {
        let now = now_unix_secs();
        let sql = format!(
            "INSERT OR REPLACE INTO kv_documents (collection, id, json_payload, updated_at) VALUES ('{}', '{}', '{}', {});",
            sql_escape(collection),
            sql_escape(id),
            sql_escape(json_payload),
            now
        );
        self.execute_sql(&sql).map(|_| ())
    }

    pub fn get_kv_document(&self, collection: &str, id: &str) -> Result<Option<String>, String> {
        let sql = format!(
            "SELECT json_payload FROM kv_documents WHERE collection = '{}' AND id = '{}' LIMIT 1;",
            sql_escape(collection),
            sql_escape(id)
        );
        let rows = self.execute_sql(&sql)?;
        Ok(rows
            .first()
            .and_then(|r| Self::row_get(r, "json_payload"))
            .map(str::to_string))
    }

    pub fn list_kv_documents(&self, collection: &str) -> Result<Vec<(String, String)>, String> {
        let sql = format!(
            "SELECT id, json_payload FROM kv_documents WHERE collection = '{}' ORDER BY updated_at ASC, id ASC;",
            sql_escape(collection)
        );
        let rows = self.execute_sql(&sql)?;
        let mut out = Vec::with_capacity(rows.len());
        for r in rows {
            if let (Some(id), Some(payload)) =
                (Self::row_get(&r, "id"), Self::row_get(&r, "json_payload"))
            {
                out.push((id.to_string(), payload.to_string()));
            }
        }
        Ok(out)
    }

    pub fn delete_kv_document(&self, collection: &str, id: &str) -> Result<bool, String> {
        let sql = format!(
            "DELETE FROM kv_documents WHERE collection = '{}' AND id = '{}';",
            sql_escape(collection),
            sql_escape(id)
        );
        self.execute_sql(&sql)?;
        Ok(true)
    }

    // ========================================================================
    // Projects (`projects`)
    // ========================================================================

    pub fn upsert_project(&self, project: &StoredProject) -> Result<(), String> {
        let payload =
            serde_json::to_string(project).map_err(|e| format!("Serialize project error: {e}"))?;
        let ro_flag = if project.is_read_only { 1 } else { 0 };
        let sql = format!(
            "INSERT OR REPLACE INTO projects (id, name, root_path, is_read_only, created_at, json_payload) \
             VALUES ('{}', '{}', '{}', {}, {}, '{}');",
            sql_escape(&project.id),
            sql_escape(&project.name),
            sql_escape(&project.root_path),
            ro_flag,
            project.created_at,
            sql_escape(&payload)
        );
        self.execute_sql(&sql)?;
        self.put_kv_document("projects", &project.id, &payload)
    }

    pub fn get_project(&self, project_id: &str) -> Result<Option<StoredProject>, String> {
        let sql = format!(
            "SELECT json_payload FROM projects WHERE id = '{}' LIMIT 1;",
            sql_escape(project_id)
        );
        let rows = self.execute_sql(&sql)?;
        if let Some(raw) = rows.first().and_then(|r| Self::row_get(r, "json_payload")) {
            let parsed = serde_json::from_str::<StoredProject>(raw)
                .map_err(|e| format!("Deserialize project error: {e}"))?;
            return Ok(Some(parsed));
        }
        Ok(None)
    }

    pub fn list_projects(&self) -> Result<Vec<StoredProject>, String> {
        let rows = self.execute_sql(
            "SELECT json_payload FROM projects ORDER BY created_at ASC, name ASC;",
        )?;
        let mut projects = Vec::with_capacity(rows.len());
        for r in rows {
            if let Some(raw) = Self::row_get(&r, "json_payload") {
                if let Ok(proj) = serde_json::from_str::<StoredProject>(raw) {
                    projects.push(proj);
                }
            }
        }
        Ok(projects)
    }

    pub fn delete_project(&self, project_id: &str) -> Result<bool, String> {
        let sql = format!(
            "DELETE FROM projects WHERE id = '{}';",
            sql_escape(project_id)
        );
        self.execute_sql(&sql)?;
        let _ = self.delete_kv_document("projects", project_id);
        Ok(true)
    }

    // ========================================================================
    // Threads & Exact Wire Messages (`threads` + `messages`)
    // ========================================================================

    /// Persists a `StoredThread`, including its exact OpenAI `wire_messages` (`reasoning_content`,
    /// `tool_calls`, `tool_call_id`), into `threads`, `messages`, and `kv_documents`.
    pub fn upsert_thread(&self, thread: &StoredThread) -> Result<(), String> {
        let full_json =
            serde_json::to_string(thread).map_err(|e| format!("Serialize thread error: {e}"))?;
        let active_tags_json = serde_json::to_string(&thread.active_tags)
            .map_err(|e| format!("Serialize active_tags error: {e}"))?;
        let wire_messages_json = serde_json::to_string(&thread.wire_messages)
            .map_err(|e| format!("Serialize wire_messages error: {e}"))?;
        let ui_events_json = serde_json::to_string(&thread.ui_events)
            .map_err(|e| format!("Serialize ui_events error: {e}"))?;
        let advice_json = match &thread.last_context_advice {
            Some(adv) => serde_json::to_string(adv).unwrap_or_default(),
            None => String::new(),
        };

        let mut batch_sql = String::with_capacity(full_json.len() * 2 + 512);
        batch_sql.push_str("BEGIN TRANSACTION;\n");
        batch_sql.push_str(&format!(
            "INSERT OR REPLACE INTO threads (
                id, project_id, title, profile, active_tags_json, wire_messages_json,
                ui_events_json, last_context_advice_json, engine_compaction_epoch,
                prompt_tokens, created_at, updated_at, json_payload
            ) VALUES (
                '{}', '{}', '{}', '{}', '{}', '{}',
                '{}', '{}', {},
                {}, {}, {}, '{}'
            );\n",
            sql_escape(&thread.id),
            sql_escape(&thread.project_id),
            sql_escape(&thread.title),
            sql_escape(&thread.profile),
            sql_escape(&active_tags_json),
            sql_escape(&wire_messages_json),
            sql_escape(&ui_events_json),
            sql_escape(&advice_json),
            thread.engine_compaction_epoch,
            thread.prompt_tokens,
            thread.created_at,
            thread.updated_at,
            sql_escape(&full_json)
        ));

        // Synchronize relational `messages` rows for exact per-turn wire inspection
        batch_sql.push_str(&format!(
            "DELETE FROM messages WHERE thread_id = '{}';\n",
            sql_escape(&thread.id)
        ));
        for (idx, msg) in thread.wire_messages.iter().enumerate() {
            let msg_json = serde_json::to_string(msg)
                .map_err(|e| format!("Serialize wire message error: {e}"))?;
            batch_sql.push_str(&format!(
                "INSERT INTO messages (thread_id, seq_idx, role, wire_json, updated_at) VALUES ('{}', {}, '{}', '{}', {});\n",
                sql_escape(&thread.id),
                idx,
                sql_escape(&msg.role),
                sql_escape(&msg_json),
                thread.updated_at
            ));
        }

        batch_sql.push_str(&format!(
            "INSERT OR REPLACE INTO kv_documents (collection, id, json_payload, updated_at) VALUES ('threads', '{}', '{}', {});\n",
            sql_escape(&thread.id),
            sql_escape(&full_json),
            thread.updated_at
        ));
        batch_sql.push_str("COMMIT;");

        self.execute_sql(&batch_sql).map(|_| ())
    }

    /// Loads a `StoredThread` by ID, reconstructing exact `wire_messages` from the relational
    /// `messages` table if needed so prefix-cache invariance is guaranteed.
    pub fn get_thread(&self, thread_id: &str) -> Result<Option<StoredThread>, String> {
        let sql = format!(
            "SELECT json_payload, wire_messages_json FROM threads WHERE id = '{}' LIMIT 1;",
            sql_escape(thread_id)
        );
        let rows = self.execute_sql(&sql)?;
        let Some(row) = rows.first() else {
            return Ok(None);
        };
        let Some(raw_payload) = Self::row_get(row, "json_payload") else {
            return Ok(None);
        };
        let mut thread = serde_json::from_str::<StoredThread>(raw_payload)
            .map_err(|e| format!("Deserialize thread error: {e}"))?;

        // Verify relational messages table consistency
        let relational_msgs = self.load_thread_wire_messages(thread_id)?;
        if !relational_msgs.is_empty() && relational_msgs.len() >= thread.wire_messages.len() {
            thread.wire_messages = relational_msgs;
        }
        Ok(Some(thread))
    }

    /// Loads the ordered `Vec<WireMessage>` for `thread_id` directly from the `messages` table.
    pub fn load_thread_wire_messages(&self, thread_id: &str) -> Result<Vec<WireMessage>, String> {
        let sql = format!(
            "SELECT wire_json FROM messages WHERE thread_id = '{}' ORDER BY seq_idx ASC;",
            sql_escape(thread_id)
        );
        let rows = self.execute_sql(&sql)?;
        let mut msgs = Vec::with_capacity(rows.len());
        for r in rows {
            if let Some(raw) = Self::row_get(&r, "wire_json") {
                let m = serde_json::from_str::<WireMessage>(raw)
                    .map_err(|e| format!("Deserialize WireMessage error: {e}"))?;
                msgs.push(m);
            }
        }
        Ok(msgs)
    }

    /// Lists persisted threads, optionally filtered by `project_id`, ordered by `updated_at DESC`.
    pub fn list_threads(&self, project_id: Option<&str>) -> Result<Vec<StoredThread>, String> {
        let sql = match project_id {
            Some(pid) if !pid.is_empty() => format!(
                "SELECT json_payload FROM threads WHERE project_id = '{}' ORDER BY updated_at DESC, created_at DESC;",
                sql_escape(pid)
            ),
            _ => "SELECT json_payload FROM threads ORDER BY updated_at DESC, created_at DESC;"
                .to_string(),
        };
        let rows = self.execute_sql(&sql)?;
        let mut out = Vec::with_capacity(rows.len());
        for r in rows {
            if let Some(raw) = Self::row_get(&r, "json_payload") {
                if let Ok(t) = serde_json::from_str::<StoredThread>(raw) {
                    out.push(t);
                }
            }
        }
        Ok(out)
    }

    pub fn delete_thread(&self, thread_id: &str) -> Result<bool, String> {
        let batch = format!(
            "BEGIN TRANSACTION;
             DELETE FROM messages WHERE thread_id = '{tid}';
             DELETE FROM timers WHERE thread_id = '{tid}';
             DELETE FROM threads WHERE id = '{tid}';
             DELETE FROM kv_documents WHERE collection = 'threads' AND id = '{tid}';
             COMMIT;",
            tid = sql_escape(thread_id)
        );
        self.execute_sql(&batch)?;
        Ok(true)
    }

    // ========================================================================
    // Conversation Rubric Tags (`tags`)
    // ========================================================================

    pub fn upsert_tag(&self, tag: &ConversationTag) -> Result<(), String> {
        let now = now_unix_secs();
        let payload =
            serde_json::to_string(tag).map_err(|e| format!("Serialize tag error: {e}"))?;
        let builtin_int = if tag.is_builtin { 1 } else { 0 };
        let sql = format!(
            "INSERT OR REPLACE INTO tags (id, name, category, description, rubric_prompt, is_builtin, json_payload, updated_at) \
             VALUES ('{}', '{}', '{}', '{}', '{}', {}, '{}', {});",
            sql_escape(&tag.id),
            sql_escape(&tag.name),
            sql_escape(&tag.category),
            sql_escape(&tag.description),
            sql_escape(&tag.rubric_prompt),
            builtin_int,
            sql_escape(&payload),
            now
        );
        self.execute_sql(&sql)?;
        self.put_kv_document("tags", &tag.id, &payload)
    }

    pub fn get_tag(&self, id_or_name: &str) -> Result<Option<ConversationTag>, String> {
        let esc = sql_escape(id_or_name);
        let sql = format!(
            "SELECT json_payload FROM tags WHERE id = '{esc}' OR name = '{esc}' LIMIT 1;"
        );
        let rows = self.execute_sql(&sql)?;
        if let Some(raw) = rows.first().and_then(|r| Self::row_get(r, "json_payload")) {
            let tag = serde_json::from_str::<ConversationTag>(raw)
                .map_err(|e| format!("Deserialize tag error: {e}"))?;
            return Ok(Some(tag));
        }
        Ok(None)
    }

    pub fn list_tags(&self) -> Result<Vec<ConversationTag>, String> {
        let rows = self
            .execute_sql("SELECT json_payload FROM tags ORDER BY is_builtin DESC, name ASC;")?;
        let mut tags = Vec::with_capacity(rows.len());
        for r in rows {
            if let Some(raw) = Self::row_get(&r, "json_payload") {
                if let Ok(tag) = serde_json::from_str::<ConversationTag>(raw) {
                    tags.push(tag);
                }
            }
        }
        Ok(tags)
    }

    pub fn delete_tag(&self, id_or_name: &str) -> Result<bool, String> {
        let esc = sql_escape(id_or_name);
        let sql = format!("DELETE FROM tags WHERE id = '{esc}' OR name = '{esc}';");
        self.execute_sql(&sql)?;
        let _ = self.delete_kv_document("tags", id_or_name);
        Ok(true)
    }

    // ========================================================================
    // Steering Directives (`kv_documents` collection = `'steering'`)
    // ========================================================================

    pub fn upsert_steering(&self, directive: &crate::tags::SteeringDirective) -> Result<(), String> {
        let payload = serde_json::to_string(directive)
            .map_err(|e| format!("Serialize steering directive error: {e}"))?;
        self.put_kv_document("steering", &directive.id, &payload)
    }

    pub fn list_steering(&self) -> Result<Vec<crate::tags::SteeringDirective>, String> {
        let docs = self.list_kv_documents("steering")?;
        let mut out = Vec::with_capacity(docs.len());
        for (_, raw) in docs {
            if let Ok(directive) = serde_json::from_str::<crate::tags::SteeringDirective>(&raw) {
                out.push(directive);
            }
        }
        Ok(out)
    }

    // ========================================================================
    // Tool Permissions (`tool_permissions`)
    // ========================================================================

    pub fn set_tool_permission(
        &self,
        tool_name: &str,
        policy: ToolPermissionPolicy,
    ) -> Result<(), String> {
        let policy_str = match policy {
            ToolPermissionPolicy::Allow => "allow",
            ToolPermissionPolicy::Confirm => "confirm",
            ToolPermissionPolicy::Deny => "deny",
        };
        let now = now_unix_secs();
        let sql = format!(
            "INSERT OR REPLACE INTO tool_permissions (tool_name, policy, updated_at) VALUES ('{}', '{}', {});",
            sql_escape(tool_name),
            sql_escape(policy_str),
            now
        );
        self.execute_sql(&sql)?;
        self.put_kv_document("tool_permissions", tool_name, policy_str)
    }

    pub fn get_tool_permission(
        &self,
        tool_name: &str,
    ) -> Result<Option<ToolPermissionPolicy>, String> {
        let sql = format!(
            "SELECT policy FROM tool_permissions WHERE tool_name = '{}' LIMIT 1;",
            sql_escape(tool_name)
        );
        let rows = self.execute_sql(&sql)?;
        if let Some(raw) = rows.first().and_then(|r| Self::row_get(r, "policy")) {
            let policy = match raw.trim().to_ascii_lowercase().as_str() {
                "confirm" => ToolPermissionPolicy::Confirm,
                "deny" => ToolPermissionPolicy::Deny,
                _ => ToolPermissionPolicy::Allow,
            };
            return Ok(Some(policy));
        }
        Ok(None)
    }

    pub fn list_tool_permissions(&self) -> Result<HashMap<String, ToolPermissionPolicy>, String> {
        let rows =
            self.execute_sql("SELECT tool_name, policy FROM tool_permissions ORDER BY tool_name;")?;
        let mut map = HashMap::with_capacity(rows.len());
        for r in rows {
            if let (Some(name), Some(raw_policy)) =
                (Self::row_get(&r, "tool_name"), Self::row_get(&r, "policy"))
            {
                let policy = match raw_policy.trim().to_ascii_lowercase().as_str() {
                    "confirm" => ToolPermissionPolicy::Confirm,
                    "deny" => ToolPermissionPolicy::Deny,
                    _ => ToolPermissionPolicy::Allow,
                };
                map.insert(name.to_string(), policy);
            }
        }
        Ok(map)
    }

    // ========================================================================
    // Local SQLite Wall-Clock Timers (`timers`)
    // ========================================================================

    fn timer_from_row(row: &SqlRow, now_ms: i64) -> Option<TimerRecord> {
        let id = Self::row_get(row, "id")?.to_string();
        let thread_id = Self::row_get(row, "thread_id").unwrap_or("").to_string();
        let label = Self::row_get(row, "label").unwrap_or("").to_string();
        let note = Self::row_get(row, "note").unwrap_or("").to_string();
        let duration_s = Self::row_get(row, "duration_s")
            .and_then(|s| s.parse::<f64>().ok())
            .unwrap_or(0.0);
        let fire_at_ms = Self::row_get(row, "fire_at_ms")
            .and_then(|s| s.parse::<i64>().ok())
            .unwrap_or(0);
        let raw_status = Self::row_get(row, "status").unwrap_or("armed");
        let delivered = Self::row_get(row, "delivered")
            .and_then(|s| s.parse::<i64>().ok())
            .map(|n| n != 0)
            .unwrap_or(false);
        let watch_target = Self::row_get(row, "watch_target_json")
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .and_then(|raw| serde_json::from_str::<serde_json::Value>(raw).ok());

        let remaining_s = if delivered || raw_status == "cancelled" || raw_status == "fired" {
            0.0
        } else {
            ((fire_at_ms - now_ms) as f64 / 1000.0).max(0.0)
        };
        let status = if raw_status == "armed" && !delivered && fire_at_ms <= now_ms {
            "due".to_string()
        } else {
            raw_status.to_string()
        };

        Some(TimerRecord {
            id,
            thread_id,
            label,
            note,
            duration_s,
            remaining_s,
            status,
            delivered,
            watch_target,
        })
    }

    /// Creates and persists a local wall-clock timer in SQLite.
    pub fn create_timer(
        &self,
        thread_id: &str,
        duration_s: f64,
        label: &str,
        note: &str,
        watch_target: Option<&serde_json::Value>,
    ) -> Result<TimerRecord, String> {
        let clamped_s = if duration_s.is_finite() && duration_s >= 0.0 {
            duration_s
        } else {
            0.0
        };
        let id = next_storage_id("tmr");
        let created_at_ms = now_unix_ms();
        let fire_at_ms = created_at_ms.saturating_add((clamped_s * 1000.0).round() as i64);
        let updated_at = now_unix_secs();
        let watch_json_sql = match watch_target {
            Some(v) => {
                let raw = serde_json::to_string(v)
                    .map_err(|e| format!("Serialize watch_target error: {e}"))?;
                format!("'{}'", sql_escape(&raw))
            }
            None => "NULL".to_string(),
        };

        let sql = format!(
            "INSERT INTO timers (
                id, thread_id, label, note, duration_s, created_at_ms, fire_at_ms,
                status, delivered, watch_target_json, updated_at
            ) VALUES (
                '{}', '{}', '{}', '{}', {:.6}, {}, {},
                'armed', 0, {}, {}
            );",
            sql_escape(&id),
            sql_escape(thread_id.trim()),
            sql_escape(label),
            sql_escape(note),
            clamped_s,
            created_at_ms,
            fire_at_ms,
            watch_json_sql,
            updated_at
        );
        self.execute_sql(&sql)?;

        let status = if fire_at_ms <= created_at_ms {
            "due".to_string()
        } else {
            "armed".to_string()
        };
        Ok(TimerRecord {
            id,
            thread_id: thread_id.trim().to_string(),
            label: label.to_string(),
            note: note.to_string(),
            duration_s: clamped_s,
            remaining_s: clamped_s,
            status,
            delivered: false,
            watch_target: watch_target.cloned(),
        })
    }

    /// Lists local timers, computing live `remaining_s` and marking overdue armed timers `"due"`.
    pub fn list_timers(
        &self,
        thread_id: Option<&str>,
        include_delivered: bool,
    ) -> Result<Vec<TimerRecord>, String> {
        let mut clauses: Vec<String> = Vec::new();
        if let Some(tid) = thread_id.map(str::trim).filter(|s| !s.is_empty()) {
            clauses.push(format!("thread_id = '{}'", sql_escape(tid)));
        }
        if !include_delivered {
            clauses.push("delivered = 0 AND status != 'cancelled'".to_string());
        }
        let where_sql = if clauses.is_empty() {
            String::new()
        } else {
            format!("WHERE {}", clauses.join(" AND "))
        };
        let sql = format!(
            "SELECT id, thread_id, label, note, duration_s, created_at_ms, fire_at_ms, \
                    status, delivered, watch_target_json \
             FROM timers {where_sql} \
             ORDER BY fire_at_ms ASC, created_at_ms ASC;"
        );
        let rows = self.execute_sql(&sql)?;
        let now_ms = now_unix_ms();
        Ok(rows
            .iter()
            .filter_map(|r| Self::timer_from_row(r, now_ms))
            .collect())
    }

    /// Atomically pops all due armed timers in a `BEGIN IMMEDIATE` transaction, flipping them
    /// to `status = 'fired', delivered = 1` so each due timer fires at most once.
    pub fn pop_due_timers(&self, thread_id: Option<&str>) -> Result<Vec<TimerRecord>, String> {
        let now_ms = now_unix_ms();
        let now_secs = now_unix_secs();
        let thread_filter = match thread_id.map(str::trim).filter(|s| !s.is_empty()) {
            Some(tid) => format!(" AND thread_id = '{}'", sql_escape(tid)),
            None => String::new(),
        };

        let guard = self.raw.conn.lock();
        RawSqliteStore::execute_sql_locked(guard.raw, "BEGIN IMMEDIATE;")?;

        let select_sql = format!(
            "SELECT id, thread_id, label, note, duration_s, created_at_ms, fire_at_ms, \
                    status, delivered, watch_target_json \
             FROM timers \
             WHERE status = 'armed' AND delivered = 0 AND fire_at_ms <= {now_ms}{thread_filter} \
             ORDER BY fire_at_ms ASC, created_at_ms ASC;"
        );
        let rows = match RawSqliteStore::execute_sql_locked(guard.raw, &select_sql) {
            Ok(r) => r,
            Err(e) => {
                let _ = RawSqliteStore::execute_sql_locked(guard.raw, "ROLLBACK;");
                return Err(e);
            }
        };

        if rows.is_empty() {
            let _ = RawSqliteStore::execute_sql_locked(guard.raw, "COMMIT;");
            return Ok(Vec::new());
        }

        let mut popped: Vec<TimerRecord> = Vec::with_capacity(rows.len());
        let mut update_sql = String::new();
        for row in &rows {
            if let Some(mut rec) = Self::timer_from_row(row, now_ms) {
                rec.status = "fired".to_string();
                rec.delivered = true;
                rec.remaining_s = 0.0;
                update_sql.push_str(&format!(
                    "UPDATE timers SET status = 'fired', delivered = 1, updated_at = {now_secs} WHERE id = '{}';\n",
                    sql_escape(&rec.id)
                ));
                popped.push(rec);
            }
        }

        if !update_sql.is_empty() {
            if let Err(e) = RawSqliteStore::execute_sql_locked(guard.raw, &update_sql) {
                let _ = RawSqliteStore::execute_sql_locked(guard.raw, "ROLLBACK;");
                return Err(e);
            }
        }
        RawSqliteStore::execute_sql_locked(guard.raw, "COMMIT;")?;
        Ok(popped)
    }

    /// Cancels a timer by ID, returning `true` if an active timer row was updated.
    pub fn cancel_timer(&self, timer_id: &str) -> Result<bool, String> {
        let tid = timer_id.trim();
        if tid.is_empty() {
            return Ok(false);
        }
        let now_secs = now_unix_secs();
        let guard = self.raw.conn.lock();
        let sql = format!(
            "UPDATE timers SET status = 'cancelled', delivered = 1, updated_at = {now_secs} \
             WHERE id = '{}' AND status != 'cancelled' AND delivered = 0; \
             SELECT changes() AS changed;",
            sql_escape(tid)
        );
        let rows = RawSqliteStore::execute_sql_locked(guard.raw, &sql)?;
        let changed = rows
            .first()
            .and_then(|r| Self::row_get(r, "changed"))
            .and_then(|s| s.parse::<u64>().ok())
            .unwrap_or(0);
        Ok(changed > 0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::contract::{WireToolCall, WireToolFunction};

    #[test]
    fn test_sqlite_in_memory_exact_wire_history_roundtrip() {
        let storage = SpellStorage::new_in_memory().expect("open :memory: sqlite");

        // 1. Project persistence
        let proj = StoredProject::new("proj-1", "BeatrixLeslie_IDE", "/tmp/worktree", false);
        storage.upsert_project(&proj).unwrap();
        let loaded_proj = storage.get_project("proj-1").unwrap().unwrap();
        assert_eq!(loaded_proj, proj);

        // 2. Thread with exact WireMessage including reasoning_content, tool_calls, and tool_call_id
        let mut thread = StoredThread::new(
            "thread-wire-01",
            "proj-1",
            "Prefix Cache Verification",
            "write",
            vec!["systems-architecture".to_string()],
        );
        thread.wire_messages.push(WireMessage {
            role: "user".to_string(),
            content: Some("Inspect README.md and summarize.".to_string()),
            reasoning_content: None,
            tool_calls: None,
            tool_call_id: None,
            name: None,
        });
        thread.wire_messages.push(WireMessage {
            role: "assistant".to_string(),
            content: Some(String::new()),
            reasoning_content: Some(
                "Checking README.md with unbounded zero-truncation reader.".to_string(),
            ),
            tool_calls: Some(vec![WireToolCall {
                id: "call_read_01".to_string(),
                r#type: "function".to_string(),
                function: WireToolFunction {
                    name: "read_file".to_string(),
                    arguments: "{\"path\":\"README.md\"}".to_string(),
                },
                index: Some(0),
            }]),
            tool_call_id: None,
            name: None,
        });
        thread.wire_messages.push(WireMessage {
            role: "tool".to_string(),
            content: Some("# BeatrixLeslie_IDE\nClean-room AI IDE.".to_string()),
            reasoning_content: None,
            tool_calls: None,
            tool_call_id: Some("call_read_01".to_string()),
            name: Some("read_file".to_string()),
        });

        thread
            .ui_events
            .push(UiChatEvent::user_message("Inspect README.md and summarize."));
        thread.ui_events.push(UiChatEvent::tool_call_card(UiToolCallItem::new(
            "call_read_01",
            "read_file",
            "{\"path\":\"README.md\"}",
            Some("# BeatrixLeslie_IDE\nClean-room AI IDE.".to_string()),
            "completed",
            false,
        )));

        storage.upsert_thread(&thread).unwrap();

        let reloaded = storage.get_thread("thread-wire-01").unwrap().unwrap();
        assert_eq!(reloaded.wire_messages.len(), 3);
        assert_eq!(
            reloaded.wire_messages[1].reasoning_content.as_deref(),
            Some("Checking README.md with unbounded zero-truncation reader.")
        );
        assert_eq!(
            reloaded.wire_messages[1]
                .tool_calls
                .as_ref()
                .unwrap()[0]
                .id,
            "call_read_01"
        );
        assert_eq!(
            reloaded.wire_messages[2].tool_call_id.as_deref(),
            Some("call_read_01")
        );

        // 3. Tool permissions & Tags persistence
        storage
            .set_tool_permission("terminal", ToolPermissionPolicy::Confirm)
            .unwrap();
        assert_eq!(
            storage.get_tool_permission("terminal").unwrap(),
            Some(ToolPermissionPolicy::Confirm)
        );
    }

    #[test]
    fn test_sqlite_local_timers_create_list_due_pop_and_cancel() {
        let storage = SpellStorage::new_in_memory().expect("open :memory: sqlite");

        // 1. Future timer stays armed with positive remaining_s
        let future = storage
            .create_timer("thread-1", 60.0, "future-job", "Check later", None)
            .unwrap();
        assert_eq!(future.status, "armed");
        assert!(!future.delivered);
        let active = storage.list_timers(Some("thread-1"), false).unwrap();
        assert_eq!(active.len(), 1);
        assert_eq!(active[0].id, future.id);
        assert_eq!(active[0].status, "armed");
        assert!(active[0].remaining_s > 0.0);

        // 2. Immediate (0s) timer shows up as "due" and pops atomically once
        let due_t = storage
            .create_timer(
                "thread-1",
                0.0,
                "now-job",
                "Fire now",
                Some(&serde_json::json!({"kind": "file_tail", "path": "Cargo.toml"})),
            )
            .unwrap();
        let listed = storage.list_timers(Some("thread-1"), false).unwrap();
        let found_due = listed.iter().find(|t| t.id == due_t.id).unwrap();
        assert_eq!(found_due.status, "due");
        assert_eq!(found_due.remaining_s, 0.0);

        let popped = storage.pop_due_timers(Some("thread-1")).unwrap();
        assert_eq!(popped.len(), 1);
        assert_eq!(popped[0].id, due_t.id);
        assert_eq!(popped[0].status, "fired");
        assert!(popped[0].delivered);

        // Second pop must return empty (at-most-once atomic delivery)
        let popped_again = storage.pop_due_timers(Some("thread-1")).unwrap();
        assert!(popped_again.is_empty());

        // 3. Cancel future timer
        assert!(storage.cancel_timer(&future.id).unwrap());
        assert!(!storage.cancel_timer(&future.id).unwrap());
        assert!(storage.list_timers(Some("thread-1"), false).unwrap().is_empty());
        assert_eq!(storage.list_timers(Some("thread-1"), true).unwrap().len(), 2);
    }
}
