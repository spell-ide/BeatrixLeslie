# Contributing

## Scope

- **Stack.** C++20, Qt 6, QScintilla2 (`cpp/`), and Rust (`rust/spell_core`) bridged over C ABI.
- **Wire protocol.** OpenAI `/v1` compatible HTTP/SSE endpoints only. No vendor-specific SDKs.
- **Boundaries.** No Electron. No webviews. No telemetry, analytics, or unprompted network calls.
- **Secrets.** Credentials stay in the OS keychain (`SPELL_EPHEMERAL_KEYRING=1` in tests). Never write API keys to disk or logs.

## Verification

CI enforces zero warnings across C++ (`-Wall -Wextra -Werror` on Clang/GCC, `/W4 /WX` on MSVC) and Rust (`RUSTFLAGS="-D warnings"`), third-party notice freshness, and `cargo-deny` policy checks.

Every pull request must pass before review:

```bash
python3 scripts/generate_third_party_licenses.py --check
cargo deny --manifest-path rust/spell_core/Cargo.toml check advisories bans licenses sources
./scripts/build_and_run.sh --strict-warnings --install dist --deploy-qt --test --verify-loader
```

On Windows (from Visual Studio 2022 Developer PowerShell with Qt 6 `bin` on `PATH`):

```powershell
.\scripts\install_qscintilla_windows.ps1
.\scripts\build_and_run.ps1 -StrictWarnings -InstallPrefix dist -DeployQt -Test -VerifyLoader
```

Use `--no-werror` (or `-NoWerror` on Windows) only for local toolchain troubleshooting; pull requests must compile cleanly with `--strict-warnings` / `-StrictWarnings`. If Cargo dependencies change, regenerate third-party notices with `python3 scripts/generate_third_party_licenses.py`.

## Conduct & Security

Participation is governed by [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md). Report security vulnerabilities privately per [`SECURITY.md`](SECURITY.md).

## Licensing

Contributions are accepted under the repository license layout:

- `cpp/` and the combined application: `GPL-3.0-only` ([`LICENSE`](LICENSE)).
- `rust/spell_core`: dual-licensed `MIT OR Apache-2.0`.
