# Security Policy

## Supported Versions

| Version | Supported |
| :-- | :-- |
| `1.0.x` | Yes |
| `< 1.0` | No |

## Security Boundaries

- **Credential storage.** Endpoint API keys are stored exclusively in the operating system credential vault (macOS Keychain, Linux Secret Service, Windows Credential Manager). Keys are never written to SQLite, settings files, or diagnostic logs. Automated tests run with `SPELL_EPHEMERAL_KEYRING=1` to isolate process-local memory storage.
- **Zero unprompted network traffic.** BeatrixLeslie IDE contains no telemetry, analytics, crash uploaders, or background update checks. Until an endpoint is explicitly saved, the application makes zero outbound network requests.
- **SSRF and DNS-pinning guards.** Online research (`web_search` and `fetch_webpage`) rejects loopback, RFC 1918 private, link-local, CGNAT, multicast, broadcast, and cloud metadata IP ranges (`169.254.169.254`, IPv6 unique-local/link-local). Resolved public socket addresses are pinned across HTTP redirects to prevent DNS rebinding.
- **Web-tainted session isolation.** When untrusted external web content enters a conversation thread, the session is marked web-tainted. File edits in web-tainted threads are held in Stage-Write for explicit human review, and sensitive paths (`.git/`, `.github/workflows/`, `.env*`, SSH/credential files, shell startup profiles, and build scripts) are blocked from agent modification.
- **Workspace path confinement.** Agent file, diff, and search tools canonicalize target paths and reject directory traversal (`..`) and symlink escapes outside the active project root.
- **Terminal command approval.** Agent-proposed shell commands require explicit user approval before execution unless matched by a user-configured allowlist rule.

## Out of Scope

- Prompt-injection behavior originating from a model endpoint explicitly configured and trusted by the user when operating within approved tool permissions.
- Vulnerabilities in third-party inference servers (Ollama, vLLM, llama.cpp, LM Studio, or remote `/v1` providers).

## Reporting a Vulnerability

Do not open public issues or pull requests for undisclosed security vulnerabilities.

Report vulnerabilities privately via GitHub Private Vulnerability Reporting:

https://github.com/spell-ide/BeatrixLeslie/security/advisories/new

Include the affected version or commit hash, operating system, minimal reproduction steps, and security impact. Reports are acknowledged within 7 days.
