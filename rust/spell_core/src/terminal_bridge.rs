//! Agent <-> user PTY terminal bridge.
//!
//! The user's interactive shell (a real PTY owned by the host UI) publishes its rendered
//! scrollback here, and drains agent keystrokes that the user has explicitly approved.
//!
//! This store is deliberately **independent of the `SpellCoreSession` lock**: an agent turn runs
//! inside `spell_session_dispatch_json` while holding the session lock, and during that turn the agent
//! must be able to read fresh terminal output and have approved input delivered. The host PTY IO
//! queue therefore talks to this store through its own short-lived mutex (`spell_terminal_*` FFI),
//! never through the session.
//!
//! Safety model:
//! - `read_terminal` is read-only.
//! - `type_in_terminal` is **always** `Confirm` (user approval per call), regardless of the
//!   persisted tool-permission map, and is denied in the `Ask` profile / read-only projects
//!   (see `SpellCoreSession::effective_tool_permission`). Text is length-capped and only
//!   enqueued after approval; nothing is written to the PTY from Rust directly.

use std::collections::VecDeque;
use std::time::{Duration, Instant};

use once_cell::sync::Lazy;
use parking_lot::{Condvar, Mutex};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

/// Maximum scrollback bytes retained from the host PTY side (tail-truncated on UTF-8 boundaries).
pub const MAX_SCROLLBACK_BYTES: usize = 256 * 1024;
/// Maximum characters an agent may type in one approved `type_in_terminal` call.
pub const MAX_AGENT_INPUT_CHARS: usize = 4096;
/// Maximum queued, not-yet-delivered approved inputs.
pub const MAX_PENDING_INPUTS: usize = 32;
/// Default / maximum lines returned by `read_terminal`.
pub const DEFAULT_READ_LINES: usize = 120;
pub const MAX_READ_LINES: usize = 2000;

/// One approved agent input waiting to be written into the user's PTY by the host UI.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct ApprovedTerminalInput {
    pub seq: u64,
    pub call_id: String,
    /// Exact bytes to write (UTF-8). `"\r"` is appended when `submit` was requested.
    pub data: String,
}

#[derive(Debug, Default)]
struct BridgeState {
    attached: bool,
    cwd: String,
    title: String,
    scrollback: String,
    /// Incremented on every publish; lets waiters detect fresh output.
    revision: u64,
    pending: VecDeque<ApprovedTerminalInput>,
    next_seq: u64,
    /// Highest `seq` the host UI has written into the PTY.
    delivered_seq: u64,
}

/// Process-global bridge (one user terminal per app process).
pub struct TerminalBridge {
    state: Mutex<BridgeState>,
    changed: Condvar,
}

pub static TERMINAL_BRIDGE: Lazy<TerminalBridge> = Lazy::new(TerminalBridge::new);

/// Snapshot of what the user terminal currently shows.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TerminalView {
    pub attached: bool,
    pub cwd: String,
    pub title: String,
    pub scrollback: String,
    pub revision: u64,
}

fn tail_utf8(s: &str, max_bytes: usize) -> &str {
    if s.len() <= max_bytes {
        return s;
    }
    let mut start = s.len() - max_bytes;
    while start < s.len() && !s.is_char_boundary(start) {
        start += 1;
    }
    &s[start..]
}

/// Returns the last `n` lines of `text` (without a trailing empty line from a final `\n`).
pub fn last_lines(text: &str, n: usize) -> String {
    let trimmed = text.trim_end_matches(['\n', '\r']);
    let lines: Vec<&str> = trimmed.lines().collect();
    let start = lines.len().saturating_sub(n);
    lines[start..].join("\n")
}

impl Default for TerminalBridge {
    fn default() -> Self {
        Self::new()
    }
}

impl TerminalBridge {
    pub fn new() -> Self {
        Self {
            state: Mutex::new(BridgeState::default()),
            changed: Condvar::new(),
        }
    }

    /// Called by the host PTY session whenever its rendered text changes (or it attaches/detaches).
    pub fn publish(&self, attached: bool, cwd: &str, title: &str, scrollback: &str) {
        let mut st = self.state.lock();
        st.attached = attached;
        st.cwd = cwd.to_string();
        st.title = title.to_string();
        st.scrollback = tail_utf8(scrollback, MAX_SCROLLBACK_BYTES).to_string();
        st.revision = st.revision.wrapping_add(1);
        if !attached {
            // A closed shell must never receive stale approved input later.
            st.pending.clear();
        }
        drop(st);
        self.changed.notify_all();
    }

    pub fn view(&self) -> TerminalView {
        let st = self.state.lock();
        TerminalView {
            attached: st.attached,
            cwd: st.cwd.clone(),
            title: st.title.clone(),
            scrollback: st.scrollback.clone(),
            revision: st.revision,
        }
    }

    /// Enqueues an **already approved** input. Returns the assigned sequence number.
    pub fn enqueue_approved(&self, call_id: &str, text: &str, submit: bool) -> Result<u64, String> {
        let mut st = self.state.lock();
        if !st.attached {
            return Err(
                "TERMINAL_NOT_ATTACHED: the user's terminal is not open. Ask the user to open it (or call open_message_terminal) first."
                    .to_string(),
            );
        }
        if st.pending.len() >= MAX_PENDING_INPUTS {
            return Err("TERMINAL_BUSY: too many undelivered terminal inputs are queued.".to_string());
        }
        st.next_seq += 1;
        let seq = st.next_seq;
        let mut data = text.to_string();
        if submit {
            data.push('\r');
        }
        st.pending.push_back(ApprovedTerminalInput {
            seq,
            call_id: call_id.to_string(),
            data,
        });
        drop(st);
        self.changed.notify_all();
        Ok(seq)
    }

    /// Drains approved inputs for the host UI to write into the PTY, marking them delivered.
    pub fn take_approved(&self) -> Vec<ApprovedTerminalInput> {
        let mut st = self.state.lock();
        let drained: Vec<ApprovedTerminalInput> = st.pending.drain(..).collect();
        if let Some(last) = drained.last() {
            st.delivered_seq = st.delivered_seq.max(last.seq);
        }
        drop(st);
        if !drained.is_empty() {
            self.changed.notify_all();
        }
        drained
    }

    /// Waits until input `seq` was delivered, then until output settles (no new publish for
    /// `quiet`), bounded by `timeout`. Returns the resulting view and whether delivery happened.
    pub fn wait_for_delivery_and_output(
        &self,
        seq: u64,
        quiet: Duration,
        timeout: Duration,
    ) -> (TerminalView, bool) {
        let deadline = Instant::now() + timeout;
        let mut st = self.state.lock();
        while st.delivered_seq < seq && st.attached {
            if self.changed.wait_until(&mut st, deadline).timed_out() {
                break;
            }
        }
        let delivered = st.delivered_seq >= seq;
        if delivered {
            let rev_at_delivery = st.revision;
            let mut last_rev = st.revision;
            let mut last_change = Instant::now();
            loop {
                let now = Instant::now();
                if now >= deadline {
                    break;
                }
                // Require at least one publish after delivery before declaring the output settled.
                if last_rev != rev_at_delivery && now.duration_since(last_change) >= quiet {
                    break;
                }
                let wake = (now + quiet).min(deadline);
                let _ = self.changed.wait_until(&mut st, wake);
                if st.revision != last_rev {
                    last_rev = st.revision;
                    last_change = Instant::now();
                }
            }
        }
        let view = TerminalView {
            attached: st.attached,
            cwd: st.cwd.clone(),
            title: st.title.clone(),
            scrollback: st.scrollback.clone(),
            revision: st.revision,
        };
        (view, delivered)
    }

    /// Test-only reset.
    pub fn reset_for_tests(&self) {
        *self.state.lock() = BridgeState::default();
    }
}

/// Renders the `read_terminal` tool output.
pub fn read_terminal_tool_output(args: &Value) -> (String, String) {
    let lines = args
        .get("lines")
        .and_then(Value::as_u64)
        .map(|n| (n as usize).clamp(1, MAX_READ_LINES))
        .unwrap_or(DEFAULT_READ_LINES);
    let view = TERMINAL_BRIDGE.view();
    if !view.attached {
        return (
            "TERMINAL_NOT_ATTACHED: the user's terminal is not open.".to_string(),
            "failed".to_string(),
        );
    }
    let body = last_lines(&view.scrollback, lines);
    (
        format!(
            "[user terminal • cwd: {} • last {} lines]\n{}",
            if view.cwd.is_empty() { "?" } else { &view.cwd },
            lines,
            body
        ),
        "completed".to_string(),
    )
}

/// Executes an **approved** `type_in_terminal` call: validates, enqueues, waits for the host UI
/// to deliver it and for output to settle, then returns the visible tail.
pub fn type_in_terminal_tool_output(call_id: &str, args: &Value) -> (String, String) {
    let text = args
        .get("text")
        .or_else(|| args.get("input"))
        .or_else(|| args.get("command"))
        .and_then(Value::as_str)
        .unwrap_or("");
    let submit = args.get("submit").and_then(Value::as_bool).unwrap_or(true);
    if text.is_empty() && !submit {
        return ("INVALID_ARGUMENT: `text` is empty.".to_string(), "failed".to_string());
    }
    if text.chars().count() > MAX_AGENT_INPUT_CHARS {
        return (
            format!("INVALID_ARGUMENT: `text` exceeds {MAX_AGENT_INPUT_CHARS} characters."),
            "failed".to_string(),
        );
    }
    let seq = match TERMINAL_BRIDGE.enqueue_approved(call_id, text, submit) {
        Ok(s) => s,
        Err(e) => return (e, "failed".to_string()),
    };
    let (view, delivered) = TERMINAL_BRIDGE.wait_for_delivery_and_output(
        seq,
        Duration::from_millis(350),
        Duration::from_secs(8),
    );
    if !delivered {
        return (
            "TERMINAL_DELIVERY_TIMEOUT: the approved input was not delivered to the terminal within 8s."
                .to_string(),
            "failed".to_string(),
        );
    }
    (
        format!(
            "Typed into the user's terminal (approved). cwd: {}\n[last 60 lines]\n{}",
            if view.cwd.is_empty() { "?" } else { &view.cwd },
            last_lines(&view.scrollback, 60)
        ),
        "completed".to_string(),
    )
}

/// OpenAI function schemas for the terminal tools exposed to the model.
pub fn terminal_tool_schemas(include_type_tool: bool) -> Vec<Value> {
    let mut out = vec![
        json!({
            "type": "function",
            "function": {
                "name": "open_message_terminal",
                "description": "Opens the user's interactive terminal window in a workspace directory. The user sees it immediately.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": { "type": "string", "description": "Workspace-relative or absolute directory." },
                        "reason": { "type": "string", "description": "Short reason shown to the user." }
                    }
                }
            }
        }),
        json!({
            "type": "function",
            "function": {
                "name": "read_terminal",
                "description": "Reads the visible text and recent scrollback of the user's interactive terminal (read-only).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "lines": { "type": "integer", "description": "Number of trailing lines to return (default 120, max 2000)." }
                    }
                }
            }
        }),
    ];
    if include_type_tool {
        out.push(json!({
            "type": "function",
            "function": {
                "name": "type_in_terminal",
                "description": "Types text into the user's interactive shell. Every call requires the user's explicit approval. Returns the terminal tail after the output settles.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "text": { "type": "string", "description": "Exact text to type." },
                        "submit": { "type": "boolean", "description": "Press Return after typing (default true)." }
                    },
                    "required": ["text"]
                }
            }
        }));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use std::thread;

    #[test]
    fn tail_and_last_lines_are_utf8_safe() {
        let s = "é".repeat(10);
        let t = tail_utf8(&s, 5);
        assert!(t.len() <= 5 && t.chars().all(|c| c == 'é'));
        assert_eq!(last_lines("a\nb\nc\n", 2), "b\nc");
        assert_eq!(last_lines("", 3), "");
    }

    #[test]
    fn enqueue_requires_attached_and_detach_clears_pending() {
        let b = TerminalBridge::new();
        assert!(b.enqueue_approved("c1", "ls", true).is_err());
        b.publish(true, "/tmp", "zsh", "$ ");
        let seq = b.enqueue_approved("c1", "ls", true).unwrap();
        assert_eq!(seq, 1);
        b.publish(false, "/tmp", "zsh", "$ ");
        assert!(b.take_approved().is_empty(), "detaching must drop undelivered input");
    }

    #[test]
    fn delivery_and_output_settle_across_threads() {
        let b = Arc::new(TerminalBridge::new());
        b.publish(true, "/tmp", "zsh", "$ ");
        let seq = b.enqueue_approved("c1", "echo hi", true).unwrap();
        let pty = Arc::clone(&b);
        let h = thread::spawn(move || {
            thread::sleep(Duration::from_millis(30));
            let inputs = pty.take_approved();
            assert_eq!(inputs.len(), 1);
            assert_eq!(inputs[0].data, "echo hi\r");
            thread::sleep(Duration::from_millis(20));
            pty.publish(true, "/tmp", "zsh", "$ echo hi\nhi\n$ ");
        });
        let (view, delivered) =
            b.wait_for_delivery_and_output(seq, Duration::from_millis(80), Duration::from_secs(3));
        h.join().unwrap();
        assert!(delivered);
        assert!(view.scrollback.contains("\nhi\n"));
    }

    #[test]
    fn undelivered_input_times_out() {
        let b = TerminalBridge::new();
        b.publish(true, "/tmp", "zsh", "$ ");
        let seq = b.enqueue_approved("c1", "x", false).unwrap();
        let (_, delivered) =
            b.wait_for_delivery_and_output(seq, Duration::from_millis(10), Duration::from_millis(60));
        assert!(!delivered);
    }

    #[test]
    fn pending_queue_is_bounded() {
        let b = TerminalBridge::new();
        b.publish(true, "/", "zsh", "");
        for i in 0..MAX_PENDING_INPUTS {
            b.enqueue_approved(&format!("c{i}"), "x", false).unwrap();
        }
        assert!(b.enqueue_approved("overflow", "x", false).is_err());
    }
}
