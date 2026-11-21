# Contributing

## Scope

- **Stack.** C++20, Qt 6, QScintilla2 (`cpp/`), and Rust (`rust/spell_core`) bridged over C ABI.
- **Wire protocol.** OpenAI `/v1` compatible HTTP/SSE endpoints only. No vendor-specific SDKs.
- **Boundaries.** No Electron. No webviews. No telemetry, analytics, or unprompted network calls.
- **Secrets.** Credentials stay in the OS keychain (`SPELL_EPHEMERAL_KEYRING=1` in tests). Never write API keys to disk or logs.

## Verification

Every pull request must pass before review:

```bash
SPELL_EPHEMERAL_KEYRING=1 cargo test --manifest-path rust/spell_core/Cargo.toml
python3 scripts/generate_third_party_licenses.py --check
./scripts/build_and_run.sh --install dist --test --verify-loader
```

On macOS, also verify self-contained bundle deployment:

```bash
./scripts/build_and_run.sh --install dist --deploy-qt --test --verify-loader
```

On Windows:

```powershell
.\scripts\build_and_run.ps1 -InstallPrefix dist -DeployQt -Test -VerifyLoader
```

Builds must complete with zero compiler warnings across C++ and Rust. If Cargo dependencies change, regenerate third-party notices with `python3 scripts/generate_third_party_licenses.py`.

## Licensing

Contributions are accepted under the repository license layout:

- `cpp/` and the combined application: `GPL-3.0-only` ([`LICENSE`](LICENSE)).
- `rust/spell_core`: dual-licensed `MIT OR Apache-2.0`.
