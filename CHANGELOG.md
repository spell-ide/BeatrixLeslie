# Changelog

All notable changes to BeatrixLeslie IDE are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-11-22

### Added

- **Native desktop shell (`cpp/`).** Three-pane C++20, Qt 6, and QScintilla2 IDE with project and conversation explorer, tabbed code editor, and agent timeline (`Dark` and `White` themes).
- **Standalone Rust engine (`rust/spell_core`).** C-ABI core library providing workspace indexing, headless tools, MCP stdio client integration, and persistent SQLite conversation and project state.
- **OpenAI `/v1` wire protocol client.** Streaming HTTP/SSE chat completions and model discovery compatible with local and remote OpenAI-compatible endpoints, backed by OS keychain credential storage.
- **`#spell` Message Terminal.** Integrated PTY terminal (`SpellPtyProcess` and `SpellTerminalEmulator`) attaching working directory and recent terminal output directly to agent turns via `#spell`.
- **Stage-Write and Gatekeeper Critic.** Diff-based edit staging with hunk review, automatic second-pass critic verification, and web-tainted session write holds.
- **Autonomous coding subagents.** Bounded parallel subagent execution with scoped workspace tools, commit verification, and escalation back to the primary thread.
- **Persistent timers.** Local SQLite-backed recurring and one-shot agent timers.
- **Self-contained cross-platform releases.** Portable builds and release archives for macOS (`arm64` and `x86_64` `.app` bundles), Linux (`x86_64` `AppImage` and `.tar.gz` bundles with `$ORIGIN` `RPATH` and `qt.conf`), and Windows (`x86_64` MSVC archive), accompanied by `SHA256SUMS.txt`.
