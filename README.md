# BeatrixLeslie IDE

A native agentic IDE. C++20, Qt 6, QScintilla2, Rust. No Electron. No telemetry. No bundled models. No vendor lock-in.

BeatrixLeslie IDE speaks the OpenAI wire protocol and nothing else. Point it at any `/v1` endpoint — local or remote — and it works. Your keys live in the OS keychain. Your code never leaves your machine unless you send it.

- **Three panes.** Projects and conversations. Editor. Agent.
- **Ask or Write.** Ask is read-only. Write edits, runs, and verifies.
- **Stage-Write.** Every change is a diff. You approve it or it does not land.
- **Gatekeeper Critic.** A second pass rejects bad edits before you see them.
- **Subagents.** Bounded parallel coding work, committed or escalated.
- **Timers.** Local, persistent, SQLite-backed.

---

## `#spell`

`#spell` hands the terminal to the agent.

Work in the integrated Message Terminal. When something breaks, type:

```
#spell why did this fail?
```

The terminal closes. The working directory and recent output are attached. The agent picks it up from there. No copy. No paste. No context lost.

---

## Install

### Requirements

| Platform | Toolchain |
| :-- | :-- |
| macOS 14+ | Xcode CLT, Rust, `brew install cmake qtbase qtsvg qscintilla2` |
| Ubuntu / Debian | `build-essential cmake qt6-base-dev qt6-svg-dev libqscintilla2-qt6-dev libsecret-1-dev`, Rust |
| Fedora | `gcc-c++ cmake qt6-qtbase-devel qt6-qtsvg-devel qscintilla-qt6-devel libsecret-devel`, Rust |
| Arch | `base-devel cmake qt6-base qt6-svg qscintilla-qt6 libsecret`, Rust |
| Windows 10/11 | Visual Studio 2022 (C++), Rust (MSVC), `vcpkg install qtbase qtsvg qscintilla` |

Rust: https://rustup.rs

### Build

```bash
git clone https://github.com/spell-ide/BeatrixLeslie.git
cd BeatrixLeslie
./scripts/build_and_run.sh --test --verify-loader --run
```

Windows:

```powershell
.\scripts\build_and_run.ps1 -Test -VerifyLoader -DeployQt -Run
```

Manual:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist
```

The Rust core builds automatically.

---

## Setup

Open **Settings → Engine**. Enter a base URL. Fetch models. Pick one. Save.

| Endpoint | Base URL |
| :-- | :-- |
| Ollama | `http://127.0.0.1:11434/v1` |
| LM Studio | `http://127.0.0.1:1234/v1` |
| llama.cpp / vLLM | `http://127.0.0.1:8080/v1` |
| OpenAI | `https://api.openai.com/v1` |
| OpenRouter | `https://openrouter.ai/api/v1` |

API keys go in the OS keychain. Nothing is written to disk in plaintext. Until an endpoint is saved, the IDE makes zero network requests.

---

## License

Application: `GPL-3.0-only`. See [`LICENSE`](LICENSE).
`rust/spell_core`: `MIT OR Apache-2.0`.
Third-party: [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
