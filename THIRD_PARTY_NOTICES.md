# Third-Party Software Notices & Attribution

This distribution of **BeatrixLeslie IDE (Standalone)** (`com.spell.BeatrixLeslieIDE`) is an open-source, cross-platform (macOS, Linux, Windows) desktop IDE built with **C++20 / Qt 6 / QScintilla2** (licensed under `GPL-3.0-only`; see `LICENSE`) and the **Rust** core library **`spell_core`** (`rust/spell_core`, dual-licensed under `MIT OR Apache-2.0`). It connects to user-configured OpenAI-compatible HTTP/HTTPS inference endpoints and ships zero bundled model weights or proprietary local inference server.

## Desktop UI & Code Editor Libraries

- **Qt 6** (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`, `Qt6Svg`) — © The Qt Company Ltd., dynamically linked under `LGPL-3.0-only` / `GPL-3.0-only` (see https://www.qt.io/licensing).
- **QScintilla2** (`qscintilla2_qt6`, Qt 6 port of Scintilla) — © Riverbank Computing Limited & Neil Hodgson, linked under `GPL-3.0-only` and the Historical Permission Notice and Disclaimer (HPND) for Scintilla.

## Open-Source Rust & C Crates (`rust/spell_core`)

`spell_core` (`libspell_core.dylib` on macOS, `libspell_core.so` on Linux, `spell_core.dll` on Windows) links open-source Rust crates (`reqwest`, `rustls`, `serde`, `serde_json`, `directories`, `keyring`, `sysinfo`, `flate2`, `lopdf`, `walkdir`, `regex`, `parking_lot`, etc.) and bundled SQLite 3 (`libsqlite3-sys`, Public Domain) resolved via Cargo. Run `python3 scripts/generate_third_party_licenses.py` to regenerate `Resources/THIRD_PARTY_LICENSES.txt` from the active `Cargo.lock` dependency graph.
