## Summary

## Verification

- [ ] `cargo test` (`SPELL_EPHEMERAL_KEYRING=1`) passes
- [ ] `SpellQtTests` (`ctest`) passes
- [ ] `--verify-loader` / `-VerifyLoader` passes
- [ ] `python3 scripts/generate_third_party_licenses.py --check` passes
- [ ] Zero new C++ or Rust compiler warnings
- [ ] macOS, Linux, and Windows CI green
- [ ] No plaintext secrets, telemetry, or vendor-specific SDKs introduced
