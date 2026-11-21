# BeatrixLeslie IDE

**BeatrixLeslie IDE** (Standalone Arbitrary-Endpoint Edition) is a fast, native, cross-platform desktop IDE built with **C++20 / Qt 6 / QScintilla2** and **Rust (`spell_core`)** for coding with any OpenAI-compatible HTTP/HTTPS inference endpoint.

It ships with **zero bundled model weights** and **zero proprietary local server dependencies**—connect directly to local engines (**Ollama**, **LM Studio**, **vLLM**, **`llama.cpp` / `llama-server`**) or cloud gateways (**OpenAI**, **OpenRouter**, or self-hosted proxies) over standard OpenAI `/v1/chat/completions` and `/v1/models` endpoints.

---

## Key Capabilities

- **Monochrome Three-Pane Native Workspace**: Crisp **Dark** and **White** monochrome themes across a collapsible Project Explorer, multi-tab **QScintilla2** Code Editor (with custom Rust lexer, inline symbol/citation navigation, and modified-buffer tracking), and an integrated Agent Chat + PTY Terminal dock.
- **Arbitrary OpenAI-Compatible Endpoints**: Point **Settings → Engine** at any HTTP or HTTPS `/v1` base URL—works out of the box with **OpenAI**, **OpenRouter**, **Ollama**, **LM Studio**, **vLLM**, and **`llama.cpp`**.
- **Ask vs. Write Conversation Modes**:
  - **Ask Mode**: Strictly read-only workspace exploration, architecture analysis, search, and cited explanations without mutating files on disk.
  - **Write Mode**: Full agentic code authoring, file creation/patching, and workspace tool execution.
- **Stage-Write Diff Review & Gatekeeper Critic**: Review proposed multi-file unified diffs inline before accepting or rejecting edits, backed by automated **Gatekeeper Critic** verification passes.
- **Coding Subagents**: Delegate parallel subtasks to specialized coding subagents with live progress and status cards inside the chat timeline.
- **Local SQLite Timers**: Persistent one-shot and recurring scheduled prompts stored locally in embedded SQLite via `spell_core`.
- **`#spell` Message Terminal CLI Handoff**: Use the `#spell` trigger to hand off tasks and context cleanly between the Agent Chat composer and CLI sessions inside the integrated PTY terminal (`forkpty` on macOS/Linux, ConPTY on Windows).
- **OS Keychain Credential Storage**: Endpoint API keys are stored natively in **macOS Keychain**, **Linux Secret Service (`libsecret`)**, or **Windows Credential Manager**—never written in plaintext to workspace files (with `SPELL_EPHEMERAL_KEYRING=1` in-memory isolation for automated tests and CI).

---

## Repository Architecture

| Path | Language | License | Role |
| :--- | :--- | :--- | :--- |
| `cpp/` | C++20 / Qt 6 / QScintilla2 | `GPL-3.0-only` | Native desktop UI, three-pane shell, QScintilla2 editor, PTY terminal emulator, settings dialog, FFI client (`SpellCoreClient`), and `SpellQtTests`. |
| `rust/spell_core/` | Rust (2021) | `MIT OR Apache-2.0` | `cdylib` (`libspell_core.dylib` / `libspell_core.so` / `spell_core.dll`) + C header (`rust/spell_core/include/spell_core.h`) implementing streaming OpenAI HTTP/SSE client, agent tool loop, Stage-Write diff staging, Gatekeeper Critic, subagents, SQLite persistence, timers, online research, and OS keyring storage. |
| `Resources/` | Assets | `GPL-3.0-only` | Application icons, rounded branding assets (`AppLogo.jpg`), `Credits.rtf`, and generated `THIRD_PARTY_LICENSES.txt`. |
| `scripts/` | Bash / PowerShell / Python 3 | `GPL-3.0-only` | Cross-platform bootstrap, test, dynamic-loader verification, and third-party license generation scripts. |

### Application Targets & Coexistence

- **macOS**: `BeatrixLeslieIDE.app` (bundle identifier `com.spell.BeatrixLeslieIDE`, portable `@rpath/libspell_core.dylib` inside `Contents/Frameworks/`, ad-hoc signed for Apple Silicon)
- **Linux**: `BeatrixLeslieIDE` (portable `$ORIGIN;$ORIGIN/../lib` ELF `RUNPATH` loading `libspell_core.so`)
- **Windows**: `BeatrixLeslieIDE.exe` (loads `spell_core.dll` alongside the executable)

---

## Prerequisites & Building from Source

`SPELL_BUILD_RUST` is enabled (`ON`) by default in `CMakeLists.txt`, so a clean `git clone` followed by CMake configure + build automatically compiles `rust/spell_core` (`cargo build --release --lib`), links `BeatrixLeslieIDE` and `SpellQtTests`, stages runtime resources, and configures OS-portable dynamic loader paths.

### 1. macOS (Apple Silicon & Intel)

#### Prerequisites
1. Xcode Command Line Tools:
   ```bash
   xcode-select --install
   ```
2. Rust toolchain (`cargo` / `rustc` 1.78+):
   ```bash
   curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
   ```
3. CMake, Ninja (optional), Qt 6 (`qtbase`, `qtsvg`), and QScintilla2 via Homebrew:
   ```bash
   brew install cmake ninja qtbase qtsvg qscintilla2
   ```

#### One-Command Build, Test, Loader Verification & Launch
```bash
./scripts/build_and_run.sh --test --verify-loader --run
```

#### Raw CMake Commands
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
SPELL_EPHEMERAL_KEYRING=1 cargo test --manifest-path rust/spell_core/Cargo.toml
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist
open dist/BeatrixLeslieIDE.app
```

---

### 2. Linux (Ubuntu / Debian, Fedora, Arch Linux)

#### Prerequisites
1. Install system C++20 compiler, CMake, Qt 6 (`Core`, `Gui`, `Widgets`, `Svg`, `Test`), QScintilla2 for Qt 6, and `libsecret` headers (used by OS keyring integration):

   - **Ubuntu 24.04+ / Debian 12+**:
     ```bash
     sudo apt update
     sudo apt install -y build-essential cmake ninja-build pkg-config \
       qt6-base-dev qt6-svg-dev libqscintilla2-qt6-dev libsecret-1-dev
     ```
   - **Fedora 40+**:
     ```bash
     sudo dnf install -y gcc-c++ cmake ninja-build pkgconf-pkg-config \
       qt6-qtbase-devel qt6-qtsvg-devel qscintilla-qt6-devel libsecret-devel
     ```
   - **Arch Linux**:
     ```bash
     sudo pacman -S --needed base-devel cmake ninja pkgconf \
       qt6-base qt6-svg qscintilla-qt6 libsecret
     ```

2. Install Rust via `rustup` if not already installed:
   ```bash
   curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
   ```

#### One-Command Build, Test, Loader Verification & Launch
```bash
./scripts/build_and_run.sh --test --verify-loader --run
```

#### Raw CMake Commands
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
SPELL_EPHEMERAL_KEYRING=1 cargo test --manifest-path rust/spell_core/Cargo.toml
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist
./dist/bin/BeatrixLeslieIDE
```

---

### 3. Windows (10 / 11 x64)

#### Prerequisites
1. **Visual Studio 2022** (or Build Tools for Visual Studio 2022) with the **Desktop development with C++** workload and CMake tools.
2. **Rust (`x86_64-pc-windows-msvc`)** via [rustup.rs](https://rustup.rs/).
3. **Qt 6** (`qtbase`, `qtsvg`) and **QScintilla2** via `vcpkg` (or official Qt Online Installer + QScintilla2):
   ```powershell
   vcpkg install qtbase:x64-windows qtsvg:x64-windows qscintilla:x64-windows
   ```

#### One-Command Build, Test, Loader Verification & Launch (PowerShell)
```powershell
.\scripts\build_and_run.ps1 -Test -VerifyLoader -DeployQt -Run
```

#### Raw CMake Commands (PowerShell)
```powershell
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
$env:SPELL_EPHEMERAL_KEYRING = "1"
cargo test --manifest-path rust\spell_core\Cargo.toml
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist
.\dist\bin\BeatrixLeslieIDE.exe
```

---

## Connecting an Inference Endpoint

Launch **BeatrixLeslie IDE** and open **Settings** (`Cmd+,` on macOS / `Ctrl+,` on Linux & Windows) → **Engine**, then configure any OpenAI-compatible base URL, model identifier, and optional API key:

| Provider | Example Base URL | API Key |
| :--- | :--- | :--- |
| **Ollama** (local) | `http://127.0.0.1:11434/v1` | Optional / leave blank |
| **LM Studio** (local) | `http://127.0.0.1:1234/v1` | Optional / leave blank |
| **`llama.cpp` (`llama-server`) / vLLM** | `http://127.0.0.1:8080/v1` | Optional or server token |
| **OpenAI** | `https://api.openai.com/v1` | Stored in OS Keychain |
| **OpenRouter** | `https://openrouter.ai/api/v1` | Stored in OS Keychain |

---

## Regenerating Third-Party Notices

When updating dependencies in `rust/spell_core/Cargo.toml`, regenerate the bundled attribution notice file from Cargo metadata:

```bash
python3 scripts/generate_third_party_licenses.py
# Verify up-to-date status in CI:
python3 scripts/generate_third_party_licenses.py --check
```

---

## License

- **Application & Desktop UI (`BeatrixLeslieIDE`, `cpp/`, combined distributions)**: Licensed under the **GNU General Public License v3.0 only (`GPL-3.0-only`)** in compliance with QScintilla2 open-source licensing. See [`LICENSE`](LICENSE) for the full license text.
- **Rust Core Library (`rust/spell_core`)**: Standalone source code under `rust/spell_core` is dual-licensed under either the **MIT License** or **Apache License, Version 2.0** (`MIT OR Apache-2.0`), at your option.
- **Third-Party Software**: See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and `Resources/THIRD_PARTY_LICENSES.txt` for full third-party attributions and license texts.
