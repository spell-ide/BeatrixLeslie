# BeatrixLeslie IDE

A native agentic IDE. C++20, Qt 6, QScintilla2, Rust. No Electron. No telemetry. No bundled models. No vendor lock-in.

![BeatrixLeslie IDE](docs/screenshot.png)

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

The working directory and recent output are attached. The agent picks it up from there. No copy. No paste. No context lost.

---

## Install

### Downloads

Prebuilt archives and `SHA256SUMS.txt` are published on [GitHub Releases](https://github.com/spell-ide/BeatrixLeslie/releases):

| Platform | Artifact |
| :-- | :-- |
| macOS 14+ (Apple Silicon) | `BeatrixLeslieIDE-macos-arm64.zip` |
| macOS 14+ (Intel) | `BeatrixLeslieIDE-macos-x86_64.zip` |
| Linux x86_64 (AppImage) | `BeatrixLeslieIDE-linux-x86_64.AppImage` |
| Linux x86_64 (Portable archive) | `BeatrixLeslieIDE-linux-x86_64.tar.gz` |
| Windows 10/11 x86_64 | `BeatrixLeslieIDE-windows-x86_64.zip` |

Verify. Then run.

```bash
shasum -a 256 -c SHA256SUMS.txt --ignore-missing
```

- **macOS:** Signed and notarized. Unzip. Drag to Applications. Open.
- **Linux:** `chmod +x BeatrixLeslieIDE-linux-x86_64.AppImage && ./BeatrixLeslieIDE-linux-x86_64.AppImage`
- **Windows:** Unsigned. SmartScreen → **More info** → **Run anyway**.

### Requirements

| Platform | Toolchain |
| :-- | :-- |
| macOS 14+ | Xcode CLT, Rust, `brew install cmake ninja qtbase qtsvg qscintilla2` |
| Ubuntu / Debian | `build-essential cmake ninja-build pkg-config patchelf file qt6-base-dev qt6-base-dev-tools qt6-svg-dev qt6-wayland libqscintilla2-qt6-dev libdbus-1-dev libsecret-1-dev libgl1-mesa-dev libxkbcommon-dev libxcb-cursor0`, Rust |
| Fedora | `gcc-c++ cmake ninja-build pkgconf-pkg-config patchelf file qt6-qtbase-devel qt6-qtsvg-devel qt6-qtwayland-devel qscintilla-qt6-devel dbus-devel libsecret-devel`, Rust |
| Arch | `base-devel cmake ninja pkgconf patchelf file qt6-base qt6-svg qt6-wayland qscintilla-qt6 dbus libsecret`, Rust |
| Windows 10/11 | Visual Studio 2022 (C++), Rust (MSVC), Qt 6.7+ (`msvc2019_64` / `msvc2022_64` on `PATH`), `.\scripts\install_qscintilla_windows.ps1` |

Rust: https://rustup.rs

### Build

```bash
git clone https://github.com/spell-ide/BeatrixLeslie.git
cd BeatrixLeslie
./scripts/build_and_run.sh --test --verify-loader --run
```

macOS self-contained `.app`:

```bash
./scripts/build_and_run.sh --install dist --deploy-qt --test --verify-loader
```

Sign and notarize your own build. Developer ID plus an App Store Connect API key. No passwords.

```bash
./scripts/build_and_run.sh --install dist --deploy-qt \
  --codesign-identity "Developer ID Application: <Name> (<TEAMID>)" \
  --notary-key AuthKey_<KEYID>.p8 --notary-key-id <KEYID> --notary-issuer <ISSUER-UUID>
```

Linux self-contained bundle and AppImage:

```bash
./scripts/build_and_run.sh --install dist --deploy-qt --test --verify-loader --package-appimage BeatrixLeslieIDE-linux-x86_64.AppImage
```

Windows (from Visual Studio 2022 Developer PowerShell with Qt 6 `bin` on `PATH`):

```powershell
.\scripts\install_qscintilla_windows.ps1
.\scripts\build_and_run.ps1 -InstallPrefix dist -DeployQt -Test -VerifyLoader -Run
```

Manual:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
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

## Contributing & Security

See [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`SECURITY.md`](SECURITY.md).

---

## License

Application: `GPL-3.0-only`. See [`LICENSE`](LICENSE).
`rust/spell_core`: `MIT OR Apache-2.0`.
Third-party: [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
