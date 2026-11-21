//! `net_guard` — Socket-Level Zero-TOCTOU Anti-SSRF (`SafePublicDnsResolver`),
//! 3-Mode Curated Host Policy, Exfiltration Guard, Per-Host Polite Rate Limiter,
//! Cross-Origin Credential-Stripping Redirect Engine, and Pure-Rust DNS-Pinned HTTPS Transport.
//!
//! Security Invariants:
//! 1. `OnlineResearchMode::Offline` fails closed BEFORE any DNS lookup or socket creation.
//! 2. `https://` ONLY and port `443` ONLY — raw IPv4/IPv6 literals, credentials (`user:pass@`),
//!    single-label hosts, `.local`/`.localhost`/`.internal`/`.home.arpa`/`.onion`/`.corp`/`.lan`,
//!    and `:8000` (local inference engine) are hard-blocked.
//! 3. `SafePublicDnsResolver` unwraps IPv4-mapped (`::ffff:a.b.c.d`), SIIT (`::ffff:0:a.b.c.d`),
//!    NAT64 (`64:ff9b::/96` & `64:ff9b:1::/48`), 6to4 (`2002::/16`), and Teredo (`2001:0000::/32`)
//!    IPv6 addresses and requires **every** resolved `A`/`AAAA` record to be global public unicast.
//! 4. Production HTTPS requests use pure-Rust `reqwest::blocking::Client` (`rustls` native roots,
//!    zero external subprocesses) with `.resolve(&host, SocketAddr::new(pinned_ip, 443))`,
//!    `.redirect(Policy::none())`, `.no_proxy()`, `.https_only(true)`, `.min_tls_version(TLS_1_2)`,
//!    sensitive headers marked in-memory, and strict `response.take(max_bytes + 1)` body caps.
//! 5. Redirects are validated hop-by-hop in Rust (`max_hops = 5`), stripping `Authorization`,
//!    `X-Api-Key`, and `X-Subscription-Token` whenever `target_host != origin_host`.
//! 6. CAPTCHA / Turnstile / reCAPTCHA / hCaptcha / `429` challenges fail closed immediately with
//!    `OnlineFetchError::ChallengeOrRateLimited` (never solved, waited out, or bypassed).

use std::collections::{BTreeMap, HashMap};
use std::io::Read;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, ToSocketAddrs};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use once_cell::sync::Lazy;
use parking_lot::Mutex;
use reqwest::header::{HeaderMap, HeaderName, HeaderValue, USER_AGENT};

use super::{
    HttpsFetcher, OnlineFetchError, OnlineResearchConfig, OnlineResearchMode, OutboundHttpRequest,
    OutboundHttpResponse,
};

/// Maximum allowed outbound HTTPS URL byte length (`search://` internal URIs are exempt).
pub const MAX_OUTBOUND_URL_BYTES: usize = 512;

/// Maximum redirects followed when `OutboundHttpRequest::follow_redirects == true`.
pub const MAX_REDIRECT_HOPS: usize = 5;

/// Default polite User-Agent used when caller headers do not specify one.
pub const DEFAULT_POLITE_USER_AGENT: &str =
    "SpellIDE-Research/1.0 (polite-academic-client)";

/// Exact vetted hosts in `OnlineResearchMode::CuratedAllowlist`.
/// Never includes user-content wildcard domains such as `*.github.io` or `*.hf.space`.
pub const CURATED_ALLOWLIST_DOMAINS: &[&str] = &[
    // Academic & preprint venues
    "arxiv.org",
    "www.arxiv.org",
    "export.arxiv.org",
    "ar5iv.labs.arxiv.org",
    "api.openalex.org",
    "openalex.org",
    "api.semanticscholar.org",
    "semanticscholar.org",
    "api.crossref.org",
    "doi.org",
    "dx.doi.org",
    "openreview.net",
    "api2.openreview.net",
    "aclanthology.org",
    "proceedings.mlr.press",
    // Code forges & raw content CDNs
    "github.com",
    "api.github.com",
    "raw.githubusercontent.com",
    "codeload.github.com",
    "objects.githubusercontent.com",
    "gitlab.com",
    "codeberg.org",
    // HuggingFace Hub & verified LFS/Xet bridges
    "huggingface.co",
    "cdn-lfs.hf.co",
    "cdn-lfs-us-1.hf.co",
    "cas-bridge.xethub.hf.co",
    // Package registries & official documentation
    "crates.io",
    "static.crates.io",
    "docs.rs",
    "pypi.org",
    "files.pythonhosted.org",
    "api.deps.dev",
    "registry.npmjs.org",
    "api.stackexchange.com",
    "stackoverflow.com",
    "en.wikipedia.org",
    "hn.algolia.com",
    "developer.apple.com",
    "doc.rust-lang.org",
    "docs.python.org",
    // Optional BYOK Tier-2 federated search APIs
    "api.search.brave.com",
    "api.exa.ai",
    "api.tavily.com",
    "kagi.com",
];

const FORBIDDEN_HOST_SUFFIXES: &[&str] = &[
    ".local",
    ".localhost",
    ".internal",
    ".home.arpa",
    ".onion",
    ".corp",
    ".lan",
];

const SENSITIVE_CROSS_HOST_HEADERS: &[&str] = &[
    "authorization",
    "proxy-authorization",
    "x-api-key",
    "x-subscription-token",
    "cookie",
];

/// Validated, normalized outbound `https://` URL on port `443`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ValidatedUrl {
    /// Canonical `https://{host}{path_and_query}` wire URL (fragment stripped).
    pub wire_url: String,
    /// Lowercase ASCII FQDN hostname.
    pub host: String,
    /// Validated port (always `443`).
    pub port: u16,
    /// Path + optional `?query` starting with `'/'`.
    pub path_and_query: String,
    /// Raw query component without leading `'?'`, if present.
    pub query: Option<String>,
}

/// Extracts lowercase hostname from an `https://` or `http://` URL for citation/display helpers.
pub fn extract_domain(url: &str) -> Option<String> {
    let trimmed = url.trim();
    let rest = if trimmed.len() >= 8 && trimmed[..8].eq_ignore_ascii_case("https://") {
        &trimmed[8..]
    } else if trimmed.len() >= 7 && trimmed[..7].eq_ignore_ascii_case("http://") {
        &trimmed[7..]
    } else {
        return None;
    };
    let authority = rest.split(['/', '?', '#', '\\']).next()?;
    let host_port = match authority.rsplit_once('@') {
        Some((_, hp)) => hp,
        None => authority,
    };
    let host = if host_port.starts_with('[') {
        let end = host_port.find(']')?;
        &host_port[..=end]
    } else {
        host_port.split(':').next()?
    };
    let low = host.trim().to_ascii_lowercase();
    if low.is_empty() {
        None
    } else {
        Some(low)
    }
}

/// Checks URL byte-length budget (`<= 512` bytes for outbound URLs; internal `search://` exempt).
pub fn validate_input_url_budget(raw_url: &str) -> Result<(), OnlineFetchError> {
    let trimmed = raw_url.trim();
    if trimmed.len() >= 9 && trimmed[..9].eq_ignore_ascii_case("search://") {
        return Ok(());
    }
    if trimmed.len() > MAX_OUTBOUND_URL_BYTES {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Outbound URL length ({} bytes) exceeds maximum allowed {} bytes",
            trimmed.len(),
            MAX_OUTBOUND_URL_BYTES
        )));
    }
    Ok(())
}

/// Returns `true` iff `domain` is an exact match in [`CURATED_ALLOWLIST_DOMAINS`].
pub fn is_curated_allowlist_domain(domain: &str) -> bool {
    let candidate = domain.trim().to_ascii_lowercase();
    if candidate.is_empty() {
        return false;
    }
    CURATED_ALLOWLIST_DOMAINS.contains(&candidate.as_str())
}

/// Computes byte-level Shannon entropy in bits/byte (`0.0 ..= 8.0`).
pub fn shannon_entropy_bits_per_byte(data: &[u8]) -> f64 {
    if data.is_empty() {
        return 0.0;
    }
    let mut counts = [0u32; 256];
    for &b in data {
        counts[b as usize] += 1;
    }
    let len = data.len() as f64;
    let mut entropy = 0.0f64;
    for &c in &counts {
        if c > 0 {
            let p = (c as f64) / len;
            entropy -= p * p.log2();
        }
    }
    entropy
}

fn looks_like_base64_or_hex_alphabet(s: &[u8]) -> bool {
    if s.is_empty() {
        return false;
    }
    let matching = s
        .iter()
        .filter(|&&b| {
            b.is_ascii_alphanumeric() || matches!(b, b'+' | b'/' | b'=' | b'-' | b'_' | b'%')
        })
        .count();
    matching * 10 >= s.len() * 9
}

fn decode_hex_even_prefix(s: &[u8]) -> Option<Vec<u8>> {
    if s.len() < 2 || !s.iter().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    let pairs = s.len() / 2;
    let mut out = Vec::with_capacity(pairs);
    for i in 0..pairs {
        let hi = (s[2 * i] as char).to_digit(16)? as u8;
        let lo = (s[2 * i + 1] as char).to_digit(16)? as u8;
        out.push((hi << 4) | lo);
    }
    Some(out)
}

fn is_high_entropy_exfil_blob(blob: &[u8]) -> bool {
    if blob.len() <= 256 {
        return false;
    }
    if looks_like_base64_or_hex_alphabet(blob) && shannon_entropy_bits_per_byte(blob) > 4.7 {
        return true;
    }
    // Pure hexadecimal encoding (`[0-9a-fA-F]`) has max raw ASCII symbol entropy <= log2(16) = 4.0
    // (or log2(22) = 4.46 mixed-case), so inspect the decoded byte entropy as well.
    if let Some(decoded_hex) = decode_hex_even_prefix(blob) {
        if shannon_entropy_bits_per_byte(&decoded_hex) > 4.7 {
            return true;
        }
    }
    false
}

/// Checks query-string parameters against high-entropy secret exfiltration on untrusted domains.
pub fn check_query_exfiltration(
    query: &str,
    is_trusted_domain: bool,
) -> Result<(), OnlineFetchError> {
    if is_trusted_domain || query.len() <= 256 {
        return Ok(());
    }
    for segment in query.split('&') {
        let val = match segment.split_once('=') {
            Some((_, v)) => v,
            None => segment,
        };
        if (segment.len() > 256 && is_high_entropy_exfil_blob(segment.as_bytes()))
            || (val.len() > 256 && is_high_entropy_exfil_blob(val.as_bytes()))
        {
            return Err(OnlineFetchError::SecurityBlocked(
                "Blocked high-entropy query parameter (>256B, >4.7 bits/byte) on untrusted domain"
                    .to_string(),
            ));
        }
    }
    if is_high_entropy_exfil_blob(query.as_bytes()) {
        return Err(OnlineFetchError::SecurityBlocked(
            "Blocked high-entropy query string (>256B, >4.7 bits/byte) on untrusted domain"
                .to_string(),
        ));
    }
    Ok(())
}

/// Validates outbound URL syntax, scheme (`https://`), port (`443`), FQDN host rules,
/// length (`<= 512`), and query exfiltration guard.
pub fn validate_outbound_url(raw_url: &str) -> Result<ValidatedUrl, OnlineFetchError> {
    let trimmed = raw_url.trim();
    if trimmed.is_empty() {
        return Err(OnlineFetchError::SecurityBlocked(
            "Outbound URL must not be empty".to_string(),
        ));
    }
    if trimmed.bytes().any(|b| b <= 0x20 || b == 0x7f) {
        return Err(OnlineFetchError::SecurityBlocked(
            "Outbound URL contains forbidden ASCII whitespace or control characters".to_string(),
        ));
    }
    if trimmed.len() > MAX_OUTBOUND_URL_BYTES {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Outbound URL length ({} bytes) exceeds maximum allowed {} bytes",
            trimmed.len(),
            MAX_OUTBOUND_URL_BYTES
        )));
    }

    if trimmed.len() < 8 || !trimmed[..8].eq_ignore_ascii_case("https://") {
        let scheme_hint = trimmed.split(':').next().unwrap_or("unknown");
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Outbound scheme '{scheme_hint}' is forbidden; only 'https://' is allowed"
        )));
    }

    let after_scheme = &trimmed[8..];
    let split_idx = after_scheme
        .find(['/', '?', '#'])
        .unwrap_or(after_scheme.len());
    let authority = &after_scheme[..split_idx];
    let remainder = &after_scheme[split_idx..];

    if authority.is_empty() {
        return Err(OnlineFetchError::SecurityBlocked(
            "Outbound URL is missing hostname authority".to_string(),
        ));
    }
    if authority.contains('\\') {
        return Err(OnlineFetchError::SecurityBlocked(
            "Outbound URL authority contains forbidden backslash character".to_string(),
        ));
    }
    if authority.contains('@') {
        return Err(OnlineFetchError::SecurityBlocked(
            "Userinfo credentials ('user:pass@') in outbound URLs are forbidden".to_string(),
        ));
    }
    if authority.starts_with('[') || authority.contains(']') {
        return Err(OnlineFetchError::SecurityBlocked(
            "Raw IPv6 literal hostnames are forbidden; domain FQDN required".to_string(),
        ));
    }

    let (host_raw, port) = match authority.split_once(':') {
        Some((h, port_str)) => {
            if port_str.contains(':') {
                return Err(OnlineFetchError::SecurityBlocked(
                    "Raw IPv6 literal hostnames are forbidden; domain FQDN required".to_string(),
                ));
            }
            if port_str != "443" {
                return Err(OnlineFetchError::SecurityBlocked(format!(
                    "Outbound port '{port_str}' is forbidden; only port 443 is allowed"
                )));
            }
            (h, 443u16)
        }
        None => (authority, 443u16),
    };

    let host = host_raw.to_ascii_lowercase();
    validate_fqdn_hostname(&host)?;

    // Strip #fragment for wire URL while preserving path + query.
    let without_fragment = match remainder.split_once('#') {
        Some((before_hash, _frag)) => before_hash,
        None => remainder,
    };

    let path_and_query = if without_fragment.is_empty() {
        "/".to_string()
    } else if without_fragment.starts_with('?') {
        format!("/{without_fragment}")
    } else {
        without_fragment.to_string()
    };

    let query = path_and_query
        .split_once('?')
        .map(|(_, q)| q.to_string())
        .filter(|q| !q.is_empty());

    let is_trusted = is_curated_allowlist_domain(&host);
    if let Some(ref q) = query {
        check_query_exfiltration(q, is_trusted)?;
    }

    let wire_url = format!("https://{host}{path_and_query}");
    Ok(ValidatedUrl {
        wire_url,
        host,
        port,
        path_and_query,
        query,
    })
}

fn validate_fqdn_hostname(host: &str) -> Result<(), OnlineFetchError> {
    if host.is_empty() || host.len() > 253 {
        return Err(OnlineFetchError::SecurityBlocked(
            "Invalid hostname length".to_string(),
        ));
    }
    if host.starts_with('.') || host.ends_with('.') || host.contains("..") {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Invalid leading, trailing, or empty DNS label in hostname '{host}'"
        )));
    }
    if host.parse::<IpAddr>().is_ok() {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Raw IP literal '{host}' is forbidden; domain FQDN required"
        )));
    }
    if host == "localhost" {
        return Err(OnlineFetchError::SecurityBlocked(
            "Localhost hostname is forbidden".to_string(),
        ));
    }
    for suffix in FORBIDDEN_HOST_SUFFIXES {
        if host.ends_with(suffix) {
            return Err(OnlineFetchError::SecurityBlocked(format!(
                "Internal/special-use domain suffix '{suffix}' in '{host}' is forbidden"
            )));
        }
    }

    let labels: Vec<&str> = host.split('.').collect();
    if labels.len() < 2 {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Single-label hostname '{host}' is forbidden; domain FQDN required"
        )));
    }
    for label in &labels {
        if label.is_empty() || label.len() > 63 {
            return Err(OnlineFetchError::SecurityBlocked(format!(
                "Invalid DNS label length in '{host}'"
            )));
        }
        if label.starts_with('-') || label.ends_with('-') {
            return Err(OnlineFetchError::SecurityBlocked(format!(
                "Invalid hyphen placement in DNS label of '{host}'"
            )));
        }
        if !label
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
        {
            return Err(OnlineFetchError::SecurityBlocked(format!(
                "Forbidden characters in hostname '{host}'"
            )));
        }
    }

    // Reject numeric/octal/hex/inet_aton IPv4 literals (RFC 3696: valid TLDs are never all-numeric,
    // and hex IPv4 components like `0x7f.0.0.1` have numeric/hex final labels).
    let tld = labels[labels.len() - 1];
    if tld.len() < 2 || tld.bytes().all(|b| b.is_ascii_digit()) {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Numeric TLD or shorthand IPv4 literal '{host}' is forbidden"
        )));
    }
    let all_numeric_or_hex = labels.iter().all(|l| {
        l.bytes().all(|b| b.is_ascii_digit())
            || (l.starts_with("0x") && l.len() > 2 && l[2..].bytes().all(|b| b.is_ascii_hexdigit()))
    });
    if all_numeric_or_hex {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Encoded numeric/hex IPv4 literal '{host}' is forbidden"
        )));
    }

    Ok(())
}

fn set_contains_domain_ci(set: &std::collections::BTreeSet<String>, domain: &str) -> bool {
    set.iter().any(|d| d.trim().eq_ignore_ascii_case(domain))
}

/// Enforces the 3-mode host policy (`Offline` / `CuratedAllowlist` / `OpenWebAskPerDomain`)
/// strictly BEFORE DNS resolution.
pub fn enforce_domain_policy(
    domain: &str,
    url: &str,
    cfg: &OnlineResearchConfig,
) -> Result<(), OnlineFetchError> {
    if cfg.mode == OnlineResearchMode::Offline {
        return Err(OnlineFetchError::OfflineAirGapped(
            "Outbound network requests are disabled in Offline (Air-Gapped) mode.".to_string(),
        ));
    }

    let norm = domain.trim().to_ascii_lowercase();
    let approved = is_curated_allowlist_domain(&norm)
        || set_contains_domain_ci(&cfg.custom_allowed_domains, &norm)
        || set_contains_domain_ci(&cfg.session_allowed_domains, &norm)
        || set_contains_domain_ci(&cfg.once_allowed_domains, &norm);

    match cfg.mode {
        OnlineResearchMode::Offline => Err(OnlineFetchError::OfflineAirGapped(
            "Outbound network requests are disabled in Offline (Air-Gapped) mode.".to_string(),
        )),
        OnlineResearchMode::CuratedAllowlist => {
            if approved {
                Ok(())
            } else {
                Err(OnlineFetchError::DomainApprovalRequired {
                    domain: norm,
                    url: url.to_string(),
                })
            }
        }
        // Open Web auto-approves every public domain (no per-domain prompt). SSRF/IP-literal,
        // non-HTTPS, loopback/RFC1918 and challenge guards still apply downstream.
        OnlineResearchMode::OpenWebAskPerDomain => Ok(()),
    }
}

/// Returns `true` iff `v4` is a globally routable public unicast IPv4 address.
pub fn is_global_public_ipv4(v4: Ipv4Addr) -> bool {
    let o = v4.octets();
    // 0.0.0.0/8 — Unspecified / "This network" (RFC 1122)
    if o[0] == 0 {
        return false;
    }
    // 10.0.0.0/8 — RFC 1918 Private-Use
    if o[0] == 10 {
        return false;
    }
    // 100.64.0.0/10 — RFC 6598 Shared Address Space (CGNAT)
    if o[0] == 100 && (o[1] & 0b1100_0000) == 64 {
        return false;
    }
    // 127.0.0.0/8 — Loopback (protects local inference engine :8000)
    if o[0] == 127 {
        return false;
    }
    // 169.254.0.0/16 — Link-Local & Cloud IMDS (169.254.169.254)
    if o[0] == 169 && o[1] == 254 {
        return false;
    }
    // 172.16.0.0/12 — RFC 1918 Private-Use
    if o[0] == 172 && (16..=31).contains(&o[1]) {
        return false;
    }
    // 192.0.0.0/24 — IETF Protocol Assignments (RFC 6890)
    if o[0] == 192 && o[1] == 0 && o[2] == 0 {
        return false;
    }
    // 192.0.2.0/24 — TEST-NET-1 Documentation (RFC 5737)
    if o[0] == 192 && o[1] == 0 && o[2] == 2 {
        return false;
    }
    // 192.88.99.0/24 — Deprecated 6to4 Relay Anycast (RFC 7526)
    if o[0] == 192 && o[1] == 88 && o[2] == 99 {
        return false;
    }
    // 192.168.0.0/16 — RFC 1918 Private-Use
    if o[0] == 192 && o[1] == 168 {
        return false;
    }
    // 198.18.0.0/15 — Network Interconnect Device Benchmarking (RFC 2544)
    if o[0] == 198 && (o[1] == 18 || o[1] == 19) {
        return false;
    }
    // 198.51.100.0/24 — TEST-NET-2 Documentation (RFC 5737)
    if o[0] == 198 && o[1] == 51 && o[2] == 100 {
        return false;
    }
    // 203.0.113.0/24 — TEST-NET-3 Documentation (RFC 5737)
    if o[0] == 203 && o[1] == 0 && o[2] == 113 {
        return false;
    }
    // 224.0.0.0/4 — Multicast (224..=239) & 240.0.0.0/4 — Class E Reserved / Broadcast (240..=255)
    if o[0] >= 224 {
        return false;
    }
    true
}

/// Returns `true` iff `ip` is a globally routable public unicast IPv4 or IPv6 address,
/// unwrapping IPv4-mapped (`::ffff:0:0/96`), SIIT (`::ffff:0:0:0/96`), NAT64 (`64:ff9b::/96`
/// and `64:ff9b:1::/48`), 6to4 (`2002::/16`), and Teredo (`2001:0000::/32`) transition addresses.
pub fn is_global_public_ip(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(v4) => is_global_public_ipv4(v4),
        IpAddr::V6(v6) => is_global_public_ipv6(v6),
    }
}

fn is_global_public_ipv6(v6: Ipv6Addr) -> bool {
    let seg = v6.segments();
    let oct = v6.octets();

    // Unspecified (`::`), Loopback (`::1`), and deprecated IPv4-compatible (`::/96`)
    if v6.is_unspecified() || v6.is_loopback() || seg[0..6] == [0, 0, 0, 0, 0, 0] {
        return false;
    }

    // IPv4-mapped (`::ffff:a.b.c.d`)
    if seg[0..5] == [0, 0, 0, 0, 0] && seg[5] == 0xffff {
        return is_global_public_ipv4(Ipv4Addr::new(oct[12], oct[13], oct[14], oct[15]));
    }

    // SIIT IPv4-translated (`::ffff:0:a.b.c.d`)
    if seg[0..4] == [0, 0, 0, 0] && seg[4] == 0xffff && seg[5] == 0 {
        return is_global_public_ipv4(Ipv4Addr::new(oct[12], oct[13], oct[14], oct[15]));
    }

    // NAT64 Well-Known (`64:ff9b::/96`, RFC 6052) & Local-Use (`64:ff9b:1::/48`, RFC 8215)
    if seg[0] == 0x0064 && seg[1] == 0xff9b {
        if seg[2..6] == [0, 0, 0, 0] {
            return is_global_public_ipv4(Ipv4Addr::new(oct[12], oct[13], oct[14], oct[15]));
        }
        if seg[2] == 0x0001 {
            if seg[3..6] == [0, 0, 0] {
                return is_global_public_ipv4(Ipv4Addr::new(oct[12], oct[13], oct[14], oct[15]));
            }
            if oct[8] == 0 && oct[11..16] == [0, 0, 0, 0, 0] {
                return is_global_public_ipv4(Ipv4Addr::new(oct[6], oct[7], oct[9], oct[10]));
            }
            return false;
        }
        return false;
    }

    // 6to4 (`2002::/16`, RFC 3056) — embedded IPv4 in bits 16..48 (`oct[2..6]`)
    if seg[0] == 0x2002 {
        return is_global_public_ipv4(Ipv4Addr::new(oct[2], oct[3], oct[4], oct[5]));
    }

    // Teredo (`2001:0000::/32`, RFC 4380) — server IPv4 in `oct[4..8]`, XOR-inverted client IPv4 in `oct[12..16]`
    if seg[0] == 0x2001 && seg[1] == 0x0000 {
        let server_v4 = Ipv4Addr::new(oct[4], oct[5], oct[6], oct[7]);
        let client_v4 = Ipv4Addr::new(!oct[12], !oct[13], !oct[14], !oct[15]);
        if !is_global_public_ipv4(server_v4) || !is_global_public_ipv4(client_v4) {
            return false;
        }
        if oct[8..12] == [0, 0, 0, 0] {
            let raw_tail_v4 = Ipv4Addr::new(oct[12], oct[13], oct[14], oct[15]);
            if !is_global_public_ipv4(raw_tail_v4) {
                return false;
            }
        }
        return true;
    }

    // Documentation (`2001:db8::/32`, RFC 3849)
    if seg[0] == 0x2001 && seg[1] == 0x0db8 {
        return false;
    }
    // Benchmarking (`2001:2::/48`, RFC 5180) and IETF Protocol Assignments (`2001::/23`)
    if seg[0] == 0x2001 && seg[1] < 0x0200 {
        return false;
    }
    // Discard-Only (`100::/64`, RFC 6666)
    if seg[0] == 0x0100 && seg[1..4] == [0, 0, 0] {
        return false;
    }
    // Unique Local (`fc00::/7`), Link-Local (`fe80::/10`), Site-Local (`fec0::/10`), Multicast (`ff00::/8`)
    if (seg[0] & 0xfe00) == 0xfc00
        || (seg[0] & 0xffc0) == 0xfe80
        || (seg[0] & 0xffc0) == 0xfec0
        || v6.is_multicast()
    {
        return false;
    }

    // Native Global Unicast Allocation (`2000::/3`, IANA Global Unicast)
    (seg[0] & 0xe000) == 0x2000
}

/// Validates a set of DNS-resolved IP records, failing closed if empty or if **any** `A`/`AAAA`
/// record is non-public (defeating multi-record DNS rebinding). Prefers IPv4 when available.
pub fn validate_resolved_ips(host: &str, ips: &[IpAddr]) -> Result<IpAddr, OnlineFetchError> {
    if ips.is_empty() {
        return Err(OnlineFetchError::TransportError(format!(
            "DNS resolution returned 0 records for '{host}'"
        )));
    }
    for &ip in ips {
        if !is_global_public_ip(ip) {
            return Err(OnlineFetchError::SecurityBlocked(format!(
                "SafePublicDnsResolver blocked '{host}': resolved record {ip} is not global public unicast"
            )));
        }
    }
    if let Some(&v4) = ips.iter().find(|ip| ip.is_ipv4()) {
        Ok(v4)
    } else {
        Ok(ips[0])
    }
}

/// Zero-TOCTOU DNS resolver pinning validated global public IPs for socket connection.
#[derive(Debug, Clone, Default)]
pub struct SafePublicDnsResolver;

impl SafePublicDnsResolver {
    pub fn resolve(host: &str) -> Result<IpAddr, OnlineFetchError> {
        resolve_pinned_public_ip(host)
    }
}

/// Resolves `(host, 443)` once via OS `getaddrinfo` and asserts **every** returned `A`/`AAAA`
/// record is global public unicast.
pub fn resolve_pinned_public_ip(host: &str) -> Result<IpAddr, OnlineFetchError> {
    validate_fqdn_hostname(&host.to_ascii_lowercase())?;
    let addrs: Vec<SocketAddr> = (host, 443u16)
        .to_socket_addrs()
        .map_err(|e| OnlineFetchError::TransportError(format!("DNS lookup failed for '{host}': {e}")))?
        .collect();
    let ips: Vec<IpAddr> = addrs.into_iter().map(|sa| sa.ip()).collect();
    validate_resolved_ips(host, &ips)
}

// ============================================================================
// Process-Wide Per-Host Polite Rate Limiter
// ============================================================================

static HOST_NEXT_SLOT: Lazy<Mutex<HashMap<String, Instant>>> =
    Lazy::new(|| Mutex::new(HashMap::new()));

/// Minimum polite inter-request spacing for a target hostname.
pub fn host_min_spacing(host: &str) -> Duration {
    match host.trim().to_ascii_lowercase().as_str() {
        "export.arxiv.org" => Duration::from_millis(3100),
        "crates.io" => Duration::from_millis(1000),
        _ => Duration::from_millis(120),
    }
}

/// Reserves the next polite request slot for `host` under lock in `O(1)` without holding
/// the mutex while sleeping, returning the required wait `Duration`.
pub fn reserve_host_rate_limit_wait(host: &str, now: Instant) -> Duration {
    let key = host.trim().to_ascii_lowercase();
    let spacing = host_min_spacing(&key);
    let mut map = HOST_NEXT_SLOT.lock();
    match map.get(&key).copied() {
        Some(next_allowed) if next_allowed > now => {
            let wait = next_allowed.duration_since(now);
            map.insert(key, next_allowed + spacing);
            wait
        }
        _ => {
            map.insert(key, now + spacing);
            Duration::ZERO
        }
    }
}

fn enforce_host_rate_limit(host: &str) {
    let wait = reserve_host_rate_limit_wait(host, Instant::now());
    if !wait.is_zero() {
        std::thread::sleep(wait);
    }
}

// ============================================================================
// Redirect Resolution, Cross-Host Credential Stripper & Anti-Bot Hard-Stop
// ============================================================================

/// Resolves an HTTP `3xx` `Location` header (absolute, protocol-relative, root-relative,
/// or path-relative) against `current`.
pub fn resolve_redirect_url(
    current: &ValidatedUrl,
    location: &str,
) -> Result<String, OnlineFetchError> {
    let loc = location.trim();
    if loc.is_empty() {
        return Err(OnlineFetchError::TransportError(
            "Upstream 3xx redirect missing non-empty Location header".to_string(),
        ));
    }
    if loc.contains("://") {
        return Ok(loc.to_string());
    }
    if let Some(rest) = loc.strip_prefix("//") {
        return Ok(format!("https://{rest}"));
    }
    if loc.starts_with('/') {
        return Ok(format!("https://{}{loc}", current.host));
    }
    let current_path = current
        .path_and_query
        .split('?')
        .next()
        .unwrap_or("/");
    let base_dir = match current_path.rfind('/') {
        Some(idx) => &current_path[..=idx],
        None => "/",
    };
    Ok(format!("https://{}{base_dir}{loc}", current.host))
}

/// Strips sensitive authentication headers (`Authorization`, `X-Api-Key`, `X-Subscription-Token`,
/// `Proxy-Authorization`, `Cookie`) whenever `target_host != origin_host`.
pub fn sanitize_headers_for_redirect(
    headers: &[(String, String)],
    origin_host: &str,
    target_host: &str,
) -> Vec<(String, String)> {
    if origin_host.eq_ignore_ascii_case(target_host) {
        return headers.to_vec();
    }
    headers
        .iter()
        .filter(|(k, _)| {
            let low = k.trim().to_ascii_lowercase();
            !SENSITIVE_CROSS_HOST_HEADERS.contains(&low.as_str())
        })
        .cloned()
        .collect()
}

fn is_redirect_status(status: u16) -> bool {
    matches!(status, 301 | 302 | 303 | 307 | 308)
}

/// Inspects status, headers, and body prefix for anti-bot / CAPTCHA / rate-limit walls.
/// Fails closed immediately with [`OnlineFetchError::ChallengeOrRateLimited`] — never solved or bypassed.
pub fn check_challenge_or_rate_limit(
    domain: &str,
    status: u16,
    headers: &BTreeMap<String, String>,
    body_sample: &[u8],
) -> Result<(), OnlineFetchError> {
    if status == 429 {
        return Err(OnlineFetchError::ChallengeOrRateLimited {
            domain: domain.to_string(),
            status,
            detail: "Upstream returned HTTP 429 rate limit".to_string(),
        });
    }

    if let Some(cf_mitigated) = headers.get("cf-mitigated") {
        if cf_mitigated.to_ascii_lowercase().contains("challenge") {
            return Err(OnlineFetchError::ChallengeOrRateLimited {
                domain: domain.to_string(),
                status,
                detail: "Cloudflare challenge mitigation active (cf-mitigated: challenge)"
                    .to_string(),
            });
        }
    }

    if status == 403 || status == 503 {
        let cap = body_sample.len().min(65_536);
        let lower_html = String::from_utf8_lossy(&body_sample[..cap]).to_ascii_lowercase();
        let markers: &[(&str, &str)] = &[
            ("cf-turnstile", "Cloudflare Turnstile CAPTCHA"),
            ("challenges.cloudflare.com/turnstile", "Cloudflare Turnstile CAPTCHA"),
            ("g-recaptcha", "Google reCAPTCHA challenge"),
            ("www.google.com/recaptcha", "Google reCAPTCHA challenge"),
            ("h-captcha", "hCaptcha challenge"),
            ("hcaptcha.com/1/api.js", "hCaptcha challenge"),
            ("just a moment...", "Cloudflare interstitial challenge"),
            ("_cf_chl_opt", "Cloudflare JS/managed challenge"),
            ("attention required! | cloudflare", "Cloudflare block/challenge page"),
            ("captcha-delivery.com", "DataDome CAPTCHA challenge"),
        ];
        for &(needle, label) in markers {
            if lower_html.contains(needle) {
                return Err(OnlineFetchError::ChallengeOrRateLimited {
                    domain: domain.to_string(),
                    status,
                    detail: format!("Detected {label} on HTTP {status}"),
                });
            }
        }
    }

    Ok(())
}

// ============================================================================
// `ReqwestPinnedHttpsFetcher` (Pure-Rust DNS-Pinned `reqwest::blocking::Client`)
// ============================================================================

/// Validated single-hop request & client configuration for [`ReqwestPinnedHttpsFetcher`].
/// Secrets and credentials stay strictly in-process in `headers` (never exposed to OS `argv`).
#[derive(Debug, Clone)]
pub struct PreparedPinnedRequest {
    pub url: String,
    pub host: String,
    pub pinned_addr: SocketAddr,
    pub method: reqwest::Method,
    pub headers: HeaderMap,
    pub body: Option<Vec<u8>>,
    pub connect_timeout: Duration,
    pub total_timeout: Duration,
    pub min_tls_version: reqwest::tls::Version,
    pub follow_redirects: bool,
    pub use_proxy: bool,
    pub https_only: bool,
}

/// Validates and builds the exact pure-Rust single-hop request and DNS-pinning client parameters.
pub fn build_pinned_request_config(
    validated: &ValidatedUrl,
    pinned_ip: IpAddr,
    method: &str,
    headers: &[(String, String)],
    body: Option<&[u8]>,
    timeout_secs: u64,
) -> Result<PreparedPinnedRequest, OnlineFetchError> {
    if !is_global_public_ip(pinned_ip) {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Refusing to pin '{}' to non-public IP {pinned_ip}",
            validated.host
        )));
    }

    let clean_method = method.trim().to_ascii_uppercase();
    if clean_method.is_empty()
        || !clean_method
            .bytes()
            .all(|b| b.is_ascii_uppercase() || b == b'-')
    {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Invalid HTTP method '{method}'"
        )));
    }
    let parsed_method = reqwest::Method::from_bytes(clean_method.as_bytes()).map_err(|_| {
        OnlineFetchError::SecurityBlocked(format!("Invalid HTTP method '{method}'"))
    })?;

    let mut header_map = HeaderMap::new();
    let mut has_user_agent = false;
    for (k, v) in headers {
        if k.bytes().any(|b| b == b'\r' || b == b'\n' || b == 0)
            || v.bytes().any(|b| b == b'\r' || b == b'\n' || b == 0)
        {
            return Err(OnlineFetchError::SecurityBlocked(
                "HTTP request header contains forbidden CR/LF/NUL characters".to_string(),
            ));
        }
        let trimmed_k = k.trim();
        let trimmed_v = v.trim();
        if trimmed_k.eq_ignore_ascii_case("user-agent") {
            has_user_agent = true;
        }
        let header_name = HeaderName::from_bytes(trimmed_k.as_bytes()).map_err(|_| {
            OnlineFetchError::SecurityBlocked(format!(
                "Invalid HTTP request header name '{trimmed_k}'"
            ))
        })?;
        let mut header_value = HeaderValue::from_str(trimmed_v).map_err(|_| {
            OnlineFetchError::SecurityBlocked(format!(
                "Invalid HTTP request header value for '{trimmed_k}'"
            ))
        })?;
        if SENSITIVE_CROSS_HOST_HEADERS.contains(&header_name.as_str()) {
            header_value.set_sensitive(true);
        }
        header_map.append(header_name, header_value);
    }

    if !has_user_agent {
        header_map.insert(USER_AGENT, HeaderValue::from_static(DEFAULT_POLITE_USER_AGENT));
    }

    let eff_timeout = timeout_secs.max(1);
    let conn_timeout = eff_timeout.min(10);

    Ok(PreparedPinnedRequest {
        url: validated.wire_url.clone(),
        host: validated.host.clone(),
        pinned_addr: SocketAddr::new(pinned_ip, validated.port),
        method: parsed_method,
        headers: header_map,
        body: body.map(|b| b.to_vec()),
        connect_timeout: Duration::from_secs(conn_timeout),
        total_timeout: Duration::from_secs(eff_timeout),
        min_tls_version: reqwest::tls::Version::TLS_1_2,
        follow_redirects: false,
        use_proxy: false,
        https_only: true,
    })
}

/// Builds a hardened single-hop `reqwest::blocking::Client` pinned strictly to `prepared.pinned_addr`.
pub fn build_pinned_client(
    prepared: &PreparedPinnedRequest,
) -> Result<reqwest::blocking::Client, OnlineFetchError> {
    if !is_global_public_ip(prepared.pinned_addr.ip()) {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Refusing to build pinned client for '{}' targeting non-public IP {}",
            prepared.host,
            prepared.pinned_addr.ip()
        )));
    }

    reqwest::blocking::Client::builder()
        .resolve(&prepared.host, prepared.pinned_addr)
        .redirect(reqwest::redirect::Policy::none())
        .no_proxy()
        .https_only(true)
        .referer(false)
        .use_rustls_tls()
        .min_tls_version(prepared.min_tls_version)
        .connect_timeout(prepared.connect_timeout)
        .timeout(prepared.total_timeout)
        .build()
        .map_err(|e| {
            OnlineFetchError::TransportError(format!(
                "Failed to build DNS-pinned HTTPS client for '{}': {e}",
                prepared.host
            ))
        })
}

fn extract_response_headers(header_map: &HeaderMap) -> BTreeMap<String, String> {
    let mut headers = BTreeMap::new();
    for (name, value) in header_map.iter() {
        let key = name.as_str().trim().to_ascii_lowercase();
        if key.is_empty() {
            continue;
        }
        let val = match value.to_str() {
            Ok(s) => s.trim().to_string(),
            Err(_) => String::from_utf8_lossy(value.as_bytes()).trim().to_string(),
        };
        headers.insert(key, val);
    }
    headers
}

fn stream_bounded_reader<R: Read>(
    reader: R,
    url: &str,
    max_bytes: usize,
    on_chunk: &mut dyn FnMut(&[u8]) -> Result<bool, OnlineFetchError>,
) -> Result<(), OnlineFetchError> {
    let limit = (max_bytes as u64).saturating_add(1);
    let mut limited = reader.take(limit);
    let mut total_bytes = 0usize;
    let mut chunk_buf = [0u8; 16 * 1024];

    loop {
        let n = match limited.read(&mut chunk_buf) {
            Ok(n) => n,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => {
                return Err(OnlineFetchError::TransportError(format!(
                    "Error reading HTTPS stream: {e}"
                )));
            }
        };
        if n == 0 {
            break;
        }
        total_bytes = total_bytes.saturating_add(n);
        if total_bytes > max_bytes {
            return Err(OnlineFetchError::SizeOrBinaryBlocked {
                url: url.to_string(),
                bytes: total_bytes as u64,
                reason: format!("Streamed body exceeded max_bytes ({max_bytes} bytes)"),
            });
        }
        if !on_chunk(&chunk_buf[..n])? {
            // Immediate early abort (e.g., sparse `codeload` tarball past target subdir).
            return Ok(());
        }
    }

    Ok(())
}

/// Production DNS-pinning HTTPS fetcher backed by pure-Rust `reqwest::blocking::Client`
/// (`rustls` with native OS root certificates; zero external subprocesses).
#[derive(Debug, Clone, Default)]
pub struct ReqwestPinnedHttpsFetcher;

/// Backwards-compatible alias retained for callers referencing `CurlPinnedHttpsFetcher`.
pub type CurlPinnedHttpsFetcher = ReqwestPinnedHttpsFetcher;

impl ReqwestPinnedHttpsFetcher {
    pub fn new() -> Self {
        Self
    }
}

impl HttpsFetcher for ReqwestPinnedHttpsFetcher {
    fn execute(
        &self,
        req: &OutboundHttpRequest,
        cfg: &OnlineResearchConfig,
    ) -> Result<OutboundHttpResponse, OnlineFetchError> {
        let mut collected = Vec::new();
        let mut final_url_out = String::new();
        let (status, headers) = self.execute_internal(
            req,
            cfg,
            true,
            &mut final_url_out,
            &mut |chunk| {
                collected.extend_from_slice(chunk);
                Ok(true)
            },
        )?;
        Ok(OutboundHttpResponse {
            status,
            final_url: final_url_out,
            headers,
            body: collected,
        })
    }

    fn execute_streaming(
        &self,
        req: &OutboundHttpRequest,
        cfg: &OnlineResearchConfig,
        on_chunk: &mut dyn FnMut(&[u8]) -> Result<bool, OnlineFetchError>,
    ) -> Result<(u16, BTreeMap<String, String>), OnlineFetchError> {
        let mut unused_final_url = String::new();
        self.execute_internal(req, cfg, false, &mut unused_final_url, on_chunk)
    }
}

impl ReqwestPinnedHttpsFetcher {
    fn execute_internal(
        &self,
        req: &OutboundHttpRequest,
        cfg: &OnlineResearchConfig,
        enforce_content_length_precheck: bool,
        final_url_out: &mut String,
        on_chunk: &mut dyn FnMut(&[u8]) -> Result<bool, OnlineFetchError>,
    ) -> Result<(u16, BTreeMap<String, String>), OnlineFetchError> {
        if cfg.mode == OnlineResearchMode::Offline {
            return Err(OnlineFetchError::OfflineAirGapped(
                "Outbound network requests are disabled in Offline (Air-Gapped) mode.".to_string(),
            ));
        }

        let initial_validated = validate_outbound_url(&req.url)?;
        let origin_host = initial_validated.host.clone();

        let mut current_url_str = initial_validated.wire_url.clone();
        let mut current_method = req.method.clone();
        let mut current_headers = req.headers.clone();
        let mut current_body = req.body.clone();
        let max_bytes = req.max_bytes.max(1);

        for hop in 0..=MAX_REDIRECT_HOPS {
            let validated = validate_outbound_url(&current_url_str)?;
            enforce_domain_policy(&validated.host, &validated.wire_url, cfg)?;
            if validated.host != origin_host {
                current_headers =
                    sanitize_headers_for_redirect(&current_headers, &origin_host, &validated.host);
            }

            let pinned_ip = resolve_pinned_public_ip(&validated.host)?;
            enforce_host_rate_limit(&validated.host);

            let prepared = build_pinned_request_config(
                &validated,
                pinned_ip,
                &current_method,
                &current_headers,
                current_body.as_deref(),
                req.timeout_secs,
            )?;
            let client = build_pinned_client(&prepared)?;

            let mut req_builder = client
                .request(prepared.method, &prepared.url)
                .headers(prepared.headers);
            if let Some(body_bytes) = prepared.body {
                req_builder = req_builder.body(body_bytes);
            }

            let response = req_builder.send().map_err(|e| {
                OnlineFetchError::TransportError(format!(
                    "HTTPS request failed for '{}': {e}",
                    validated.wire_url
                ))
            })?;

            let status = response.status().as_u16();
            let headers = extract_response_headers(response.headers());
            *final_url_out = validated.wire_url.clone();

            // Handle 3xx redirects hop-by-hop in Rust
            if is_redirect_status(status) {
                if !req.follow_redirects {
                    return Ok((status, headers));
                }
                if let Some(loc) = headers.get("location").cloned() {
                    drop(response);
                    if hop >= MAX_REDIRECT_HOPS {
                        return Err(OnlineFetchError::TransportError(format!(
                            "Exceeded maximum HTTP redirect hops ({MAX_REDIRECT_HOPS}) starting from '{}'",
                            req.url
                        )));
                    }
                    let next_raw = resolve_redirect_url(&validated, &loc)?;
                    let next_validated = validate_outbound_url(&next_raw)?;
                    current_headers = sanitize_headers_for_redirect(
                        &current_headers,
                        &validated.host,
                        &next_validated.host,
                    );
                    current_headers = sanitize_headers_for_redirect(
                        &current_headers,
                        &origin_host,
                        &next_validated.host,
                    );
                    if matches!(status, 301 | 302 | 303)
                        && !current_method.eq_ignore_ascii_case("GET")
                        && !current_method.eq_ignore_ascii_case("HEAD")
                    {
                        current_method = "GET".to_string();
                        current_body = None;
                    }
                    current_url_str = next_validated.wire_url;
                    continue;
                }
            }

            // Immediate header-level anti-bot / 429 check
            check_challenge_or_rate_limit(&validated.host, status, &headers, &[])?;

            // For 403/503, buffer up to 32 KiB to detect HTML CAPTCHA pages before streaming chunks
            if status == 403 || status == 503 {
                let mut err_body = Vec::new();
                response
                    .take(32 * 1024)
                    .read_to_end(&mut err_body)
                    .map_err(|e| {
                        OnlineFetchError::TransportError(format!("Read error on HTTP {status}: {e}"))
                    })?;
                check_challenge_or_rate_limit(&validated.host, status, &headers, &err_body)?;
                if err_body.len() > max_bytes {
                    return Err(OnlineFetchError::SizeOrBinaryBlocked {
                        url: validated.wire_url,
                        bytes: err_body.len() as u64,
                        reason: format!("Response exceeded max_bytes ({max_bytes})"),
                    });
                }
                if !err_body.is_empty() {
                    let _ = on_chunk(&err_body)?;
                }
                return Ok((status, headers));
            }

            // Pre-check Content-Length on full-body `execute` calls
            if enforce_content_length_precheck && !current_method.eq_ignore_ascii_case("HEAD") {
                if let Some(cl_str) = headers.get("content-length") {
                    if let Ok(cl) = cl_str.parse::<u64>() {
                        if cl > max_bytes as u64 {
                            return Err(OnlineFetchError::SizeOrBinaryBlocked {
                                url: validated.wire_url,
                                bytes: cl,
                                reason: format!(
                                    "Content-Length ({cl} bytes) exceeds max_bytes budget ({max_bytes} bytes)"
                                ),
                            });
                        }
                    }
                }
            }

            stream_bounded_reader(response, &validated.wire_url, max_bytes, on_chunk)?;
            return Ok((status, headers));
        }

        Err(OnlineFetchError::TransportError(format!(
            "Exceeded maximum HTTP redirect hops ({MAX_REDIRECT_HOPS}) starting from '{}'",
            req.url
        )))
    }
}

// ============================================================================
// Offline Deterministic `MockHttpsFetcher` for Submodule Unit Tests
// ============================================================================

pub type MockHttpsHandler =
    Arc<dyn Fn(&OutboundHttpRequest) -> Result<OutboundHttpResponse, OnlineFetchError> + Send + Sync>;

/// Offline mock transport executing the exact same URL validation, 3-mode domain policy,
/// hop-by-hop redirect credential stripping, and CAPTCHA hard-stop guards without DNS or sockets.
#[derive(Clone)]
pub struct MockHttpsFetcher {
    handler: MockHttpsHandler,
    recorded: Arc<Mutex<Vec<OutboundHttpRequest>>>,
    stream_chunk_size: usize,
    aborted_streams: Arc<AtomicUsize>,
}

impl std::fmt::Debug for MockHttpsFetcher {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MockHttpsFetcher")
            .field("recorded_len", &self.recorded.lock().len())
            .field("stream_chunk_size", &self.stream_chunk_size)
            .field("aborted_streams", &self.aborted_streams.load(Ordering::Relaxed))
            .finish()
    }
}

impl MockHttpsFetcher {
    pub fn with_handler<F>(handler: F) -> Self
    where
        F: Fn(&OutboundHttpRequest) -> Result<OutboundHttpResponse, OnlineFetchError>
            + Send
            + Sync
            + 'static,
    {
        Self {
            handler: Arc::new(handler),
            recorded: Arc::new(Mutex::new(Vec::new())),
            stream_chunk_size: 4096,
            aborted_streams: Arc::new(AtomicUsize::new(0)),
        }
    }

    pub fn with_stream_chunk_size(mut self, chunk_size: usize) -> Self {
        self.stream_chunk_size = chunk_size.max(1);
        self
    }

    pub fn recorded_requests(&self) -> Vec<OutboundHttpRequest> {
        self.recorded.lock().clone()
    }

    pub fn stream_abort_count(&self) -> usize {
        self.aborted_streams.load(Ordering::SeqCst)
    }

    fn execute_guarded(
        &self,
        req: &OutboundHttpRequest,
        cfg: &OnlineResearchConfig,
        precheck_max_bytes: bool,
    ) -> Result<OutboundHttpResponse, OnlineFetchError> {
        if cfg.mode == OnlineResearchMode::Offline {
            return Err(OnlineFetchError::OfflineAirGapped(
                "Outbound network requests are disabled in Offline (Air-Gapped) mode.".to_string(),
            ));
        }

        let initial_validated = validate_outbound_url(&req.url)?;
        let origin_host = initial_validated.host.clone();

        let mut current_url = initial_validated.wire_url;
        let mut current_method = req.method.clone();
        let mut current_headers = req.headers.clone();
        let mut current_body = req.body.clone();
        let max_bytes = req.max_bytes.max(1);

        for hop in 0..=MAX_REDIRECT_HOPS {
            let validated = validate_outbound_url(&current_url)?;
            enforce_domain_policy(&validated.host, &validated.wire_url, cfg)?;
            if validated.host != origin_host {
                current_headers =
                    sanitize_headers_for_redirect(&current_headers, &origin_host, &validated.host);
            }

            let hop_req = OutboundHttpRequest {
                url: validated.wire_url.clone(),
                method: current_method.clone(),
                headers: current_headers.clone(),
                body: current_body.clone(),
                max_bytes: req.max_bytes,
                timeout_secs: req.timeout_secs,
                follow_redirects: req.follow_redirects,
            };
            self.recorded.lock().push(hop_req.clone());

            let mut resp = (self.handler)(&hop_req)?;
            // Normalize response header keys to lowercase ASCII.
            resp.headers = resp
                .headers
                .into_iter()
                .map(|(k, v)| (k.to_ascii_lowercase(), v))
                .collect();
            if resp.final_url.is_empty() {
                resp.final_url = validated.wire_url.clone();
            }

            if is_redirect_status(resp.status) {
                if !req.follow_redirects {
                    return Ok(resp);
                }
                if let Some(loc) = resp.headers.get("location").cloned() {
                    if hop >= MAX_REDIRECT_HOPS {
                        return Err(OnlineFetchError::TransportError(format!(
                            "Exceeded maximum HTTP redirect hops ({MAX_REDIRECT_HOPS}) starting from '{}'",
                            req.url
                        )));
                    }
                    let next_raw = resolve_redirect_url(&validated, &loc)?;
                    let next_validated = validate_outbound_url(&next_raw)?;
                    current_headers = sanitize_headers_for_redirect(
                        &current_headers,
                        &validated.host,
                        &next_validated.host,
                    );
                    current_headers = sanitize_headers_for_redirect(
                        &current_headers,
                        &origin_host,
                        &next_validated.host,
                    );
                    if matches!(resp.status, 301 | 302 | 303)
                        && !current_method.eq_ignore_ascii_case("GET")
                        && !current_method.eq_ignore_ascii_case("HEAD")
                    {
                        current_method = "GET".to_string();
                        current_body = None;
                    }
                    current_url = next_validated.wire_url;
                    continue;
                }
            }

            check_challenge_or_rate_limit(&validated.host, resp.status, &resp.headers, &resp.body)?;

            if precheck_max_bytes && resp.body.len() > max_bytes {
                return Err(OnlineFetchError::SizeOrBinaryBlocked {
                    url: validated.wire_url,
                    bytes: resp.body.len() as u64,
                    reason: format!("Response body ({} bytes) exceeds max_bytes ({max_bytes})", resp.body.len()),
                });
            }

            return Ok(resp);
        }

        Err(OnlineFetchError::TransportError(format!(
            "Exceeded maximum HTTP redirect hops ({MAX_REDIRECT_HOPS}) starting from '{}'",
            req.url
        )))
    }
}

impl HttpsFetcher for MockHttpsFetcher {
    fn execute(
        &self,
        req: &OutboundHttpRequest,
        cfg: &OnlineResearchConfig,
    ) -> Result<OutboundHttpResponse, OnlineFetchError> {
        self.execute_guarded(req, cfg, true)
    }

    fn execute_streaming(
        &self,
        req: &OutboundHttpRequest,
        cfg: &OnlineResearchConfig,
        on_chunk: &mut dyn FnMut(&[u8]) -> Result<bool, OnlineFetchError>,
    ) -> Result<(u16, BTreeMap<String, String>), OnlineFetchError> {
        let resp = self.execute_guarded(req, cfg, false)?;
        let max_bytes = req.max_bytes.max(1);
        let mut streamed = 0usize;
        for chunk in resp.body.chunks(self.stream_chunk_size) {
            streamed = streamed.saturating_add(chunk.len());
            if streamed > max_bytes {
                self.aborted_streams.fetch_add(1, Ordering::SeqCst);
                return Err(OnlineFetchError::SizeOrBinaryBlocked {
                    url: resp.final_url,
                    bytes: streamed as u64,
                    reason: format!("Streamed body exceeded max_bytes ({max_bytes})"),
                });
            }
            if !on_chunk(chunk)? {
                self.aborted_streams.fetch_add(1, Ordering::SeqCst);
                break;
            }
        }
        Ok((resp.status, resp.headers))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn curated_cfg() -> OnlineResearchConfig {
        let mut cfg = OnlineResearchConfig::with_defaults();
        cfg.mode = OnlineResearchMode::CuratedAllowlist;
        cfg
    }

    #[test]
    fn test_ssrf_loopback_and_port_8000_rejected() {
        let blocked = [
            "https://127.0.0.1:8000/v1/chat/completions",
            "http://127.0.0.1:8000/v1/models",
            "https://localhost:8000/health",
            "https://arxiv.org:8000/abs/2401.00001",
            "https://arxiv.org:8443/abs/2401.00001",
            "https://arxiv.org:80/abs/2401.00001",
            "https://arxiv.org:/abs/2401.00001",
        ];
        for url in blocked {
            let err = validate_outbound_url(url).expect_err(url);
            assert!(
                matches!(err, OnlineFetchError::SecurityBlocked(_)),
                "expected SecurityBlocked for {url}, got {err:?}"
            );
        }

        let ok_default = validate_outbound_url("https://ArXiv.org/abs/2401.00001#sec1").unwrap();
        assert_eq!(ok_default.host, "arxiv.org");
        assert_eq!(ok_default.port, 443);
        assert_eq!(ok_default.wire_url, "https://arxiv.org/abs/2401.00001");

        let ok_explicit = validate_outbound_url("https://arxiv.org:443/html/2401.00001v2").unwrap();
        assert_eq!(ok_explicit.host, "arxiv.org");
        assert_eq!(ok_explicit.port, 443);
    }

    #[test]
    fn test_ssrf_non_https_schemes_and_credentials_rejected() {
        let blocked = [
            "http://arxiv.org/abs/2401.00001",
            "file:///etc/passwd",
            "ftp://export.arxiv.org/e-print/2401.00001",
            "gopher://127.0.0.1:8000/_",
            "https://user:pass@github.com/org/repo",
            "https://token@api.github.com/repos/org/repo",
            "https://evil.example.com\\@arxiv.org/abs/2401.00001",
            "https://arxiv.org/abs/2401.00001\r\nX-Injected: 1",
        ];
        for url in blocked {
            assert!(
                matches!(
                    validate_outbound_url(url),
                    Err(OnlineFetchError::SecurityBlocked(_))
                ),
                "expected SecurityBlocked for {url}"
            );
        }
    }

    #[test]
    fn test_ssrf_raw_ip_literals_and_internal_tlds_rejected() {
        let blocked = [
            "https://127.0.0.1/",
            "https://169.254.169.254/latest/meta-data/",
            "https://2130706433/",
            "https://0x7f.0.0.1/",
            "https://0177.0.0.1/",
            "https://127.1/",
            "https://[::1]/",
            "https://[::ffff:127.0.0.1]/",
            "https://[2606:4700:4700::1111]/",
            "https://localhost/",
            "https://intranet/",
            "https://engine.localhost/v1/models",
            "https://bonjoursrv.local/api",
            "https://metadata.google.internal/computeMetadata/v1/",
            "https://router.home.arpa/status",
            "https://hiddenservice.onion/",
            "https://gitlab.corp/team/repo",
            "https://nas.lan/files",
        ];
        for url in blocked {
            assert!(
                matches!(
                    validate_outbound_url(url),
                    Err(OnlineFetchError::SecurityBlocked(_))
                ),
                "expected SecurityBlocked for {url}"
            );
        }
    }

    #[test]
    fn test_is_global_public_ip_ipv4_exhaustive_ranges() {
        let non_public = [
            "0.0.0.0",
            "0.10.20.30",
            "127.0.0.1",
            "127.0.0.53",
            "127.255.255.254",
            "10.0.0.1",
            "10.255.255.255",
            "172.16.0.1",
            "172.31.255.254",
            "192.168.0.1",
            "192.168.255.254",
            "100.64.0.1",
            "100.127.255.254",
            "169.254.169.254",
            "169.254.0.1",
            "192.0.0.1",
            "192.0.2.10",
            "198.51.100.20",
            "203.0.113.30",
            "198.18.0.1",
            "198.19.255.254",
            "224.0.0.1",
            "239.255.255.250",
            "240.0.0.1",
            "255.255.255.255",
        ];
        for s in non_public {
            let ip: IpAddr = s.parse().unwrap();
            assert!(!is_global_public_ip(ip), "{s} MUST NOT be global public");
        }

        let public = [
            "1.1.1.1",
            "8.8.8.8",
            "140.82.112.4",
            "128.84.21.199",
            "100.63.255.254",
            "100.128.0.1",
            "172.15.255.254",
            "172.32.0.1",
        ];
        for s in public {
            let ip: IpAddr = s.parse().unwrap();
            assert!(is_global_public_ip(ip), "{s} SHOULD be global public");
        }
    }

    #[test]
    fn test_is_global_public_ip_ipv6_mapped_nat64_6to4_teredo_ula() {
        let non_public_v6 = [
            "::",
            "::1",
            "::127.0.0.1",
            "::ffff:127.0.0.1",
            "::ffff:10.0.0.1",
            "::ffff:169.254.169.254",
            "::ffff:100.64.1.1",
            "::ffff:0:127.0.0.1",
            // NAT64 well-known (64:ff9b::/96) & local-use (64:ff9b:1::/48)
            "64:ff9b::7f00:1",
            "64:ff9b::a9fe:a9fe",
            "64:ff9b::c0a8:101",
            "64:ff9b:1::7f00:1",
            "64:ff9b:1:7f00:0100::",
            // 6to4 (2002::/16) wrapping 127.0.0.1 & 192.168.1.1 & 169.254.169.254
            "2002:7f00:0001::1",
            "2002:c0a8:0101::1",
            "2002:a9fe:a9fe::1",
            // Teredo (2001:0000::/32) with XOR-obfuscated 127.0.0.1 (`!0x7f000001 == 0x80fffffe`) and server 127.0.0.1
            "2001:0000:0808:0808:8000:0000:80ff:fffe",
            "2001:0000:7f00:0001:8000:0000:f7f7:f7f7",
            "2001:0000::7f00:1",
            // Link-local, ULA, Documentation, Multicast
            "fe80::1",
            "fc00::1",
            "fd12:3456:789a::1",
            "2001:db8::1",
            "ff02::1",
        ];
        for s in non_public_v6 {
            let ip: IpAddr = s.parse().unwrap();
            assert!(!is_global_public_ip(ip), "{s} MUST NOT be global public");
        }

        let public_v6 = [
            "2606:4700:4700::1111",
            "2001:4860:4860::8888",
            "::ffff:8.8.8.8",
            "64:ff9b::0808:0808",
            "2002:0808:0808::1",
        ];
        for s in public_v6 {
            let ip: IpAddr = s.parse().unwrap();
            assert!(is_global_public_ip(ip), "{s} SHOULD be global public");
        }
    }

    #[test]
    fn test_validate_resolved_ips_blocks_split_dns_rebinding() {
        let pub_v4: IpAddr = "140.82.112.4".parse().unwrap();
        let pub_v6: IpAddr = "2606:4700:4700::1111".parse().unwrap();
        let lo_v4: IpAddr = "127.0.0.1".parse().unwrap();
        let nat64_imds: IpAddr = "64:ff9b::a9fe:a9fe".parse().unwrap();

        assert!(validate_resolved_ips("github.com", &[]).is_err());
        assert!(matches!(
            validate_resolved_ips("rebind.example.com", &[pub_v4, lo_v4]),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));
        assert!(matches!(
            validate_resolved_ips("rebind.example.com", &[pub_v6, nat64_imds]),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));
        assert_eq!(
            validate_resolved_ips("github.com", &[pub_v6, pub_v4]).unwrap(),
            pub_v4
        );
    }

    #[test]
    fn test_url_length_and_high_entropy_exfiltration_guard() {
        let long_search = format!("search://{}", "sparse mixture of experts ".repeat(30));
        assert!(long_search.len() > 512);
        assert!(validate_input_url_budget(&long_search).is_ok());

        let too_long_https = format!("https://arxiv.org/search?q={}", "a".repeat(505));
        assert!(matches!(
            validate_outbound_url(&too_long_https),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));

        // High-entropy ~300B Base64-like secret payload on untrusted domain -> blocked
        let alphabet = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        let mut high_entropy_b64 = String::with_capacity(300);
        let mut state: u64 = 0x9e37_79b9_7f4a_7c15;
        for _ in 0..300 {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            high_entropy_b64.push(alphabet[(state as usize) & 63] as char);
        }
        let exfil_b64_url = format!("https://untrusted.example.org/p?d={high_entropy_b64}");
        assert!(exfil_b64_url.len() <= 512);
        assert!(matches!(
            validate_outbound_url(&exfil_b64_url),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));

        // High-entropy 280B hex-encoded random bytes on untrusted domain -> blocked
        let mut hex_secret = String::with_capacity(280);
        for _ in 0..140 {
            state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
            let byte = (state >> 56) as u8;
            hex_secret.push_str(&format!("{byte:02x}"));
        }
        let exfil_hex_url = format!("https://untrusted.example.org/p?h={hex_secret}");
        assert!(exfil_hex_url.len() <= 512);
        assert!(matches!(
            validate_outbound_url(&exfil_hex_url),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));

        // Benign >256B natural-language query on untrusted domain -> allowed
        let benign_prose = "attention_mechanisms_in_sparse_mixture_of_experts_for_long_context_apple_silicon_inference_"
            .repeat(3);
        assert!(benign_prose.len() > 256);
        let benign_url = format!("https://untrusted.example.org/q?s={benign_prose}");
        assert!(benign_url.len() <= 512);
        assert!(validate_outbound_url(&benign_url).is_ok());
    }

    #[test]
    fn test_curated_allowlist_and_three_mode_policy() {
        assert!(is_curated_allowlist_domain("arxiv.org"));
        assert!(is_curated_allowlist_domain("export.arxiv.org"));
        assert!(is_curated_allowlist_domain("ar5iv.labs.arxiv.org"));
        assert!(is_curated_allowlist_domain("codeload.github.com"));
        assert!(is_curated_allowlist_domain("cdn-lfs.hf.co"));
        assert!(is_curated_allowlist_domain("developer.apple.com"));
        assert!(is_curated_allowlist_domain("api.search.brave.com"));

        // NEVER wildcard user-hosted subdomains!
        assert!(!is_curated_allowlist_domain("attacker.github.io"));
        assert!(!is_curated_allowlist_domain("github.io"));
        assert!(!is_curated_allowlist_domain("demo.hf.space"));
        assert!(!is_curated_allowlist_domain("evil-arxiv.org"));
        assert!(!is_curated_allowlist_domain("sub.github.com"));

        // 1. Offline mode fails closed before DNS even on arxiv.org
        let offline_cfg = OnlineResearchConfig::with_defaults();
        assert!(matches!(
            enforce_domain_policy("arxiv.org", "https://arxiv.org/abs/2401.00001", &offline_cfg),
            Err(OnlineFetchError::OfflineAirGapped(_))
        ));

        // 2. CuratedAllowlist mode
        let mut curated = curated_cfg();
        assert!(enforce_domain_policy("arxiv.org", "https://arxiv.org/", &curated).is_ok());
        assert_eq!(
            enforce_domain_policy("pytorch.org", "https://pytorch.org/docs", &curated),
            Err(OnlineFetchError::DomainApprovalRequired {
                domain: "pytorch.org".to_string(),
                url: "https://pytorch.org/docs".to_string(),
            })
        );
        curated.session_allowed_domains.insert("pytorch.org".to_string());
        assert!(enforce_domain_policy("pytorch.org", "https://pytorch.org/docs", &curated).is_ok());

        // 3. Open Web mode auto-approves any public domain (no per-domain prompt).
        let open_web = {
            let mut c = OnlineResearchConfig::with_defaults();
            c.mode = OnlineResearchMode::OpenWebAskPerDomain;
            c
        };
        assert!(enforce_domain_policy("docs.rs", "https://docs.rs/serde", &open_web).is_ok());
        assert!(enforce_domain_policy("ziglang.org", "https://ziglang.org/documentation/master/", &open_web).is_ok());
        assert!(enforce_domain_policy("www.wiz.io", "https://www.wiz.io/blog/x", &open_web).is_ok());
    }

    #[test]
    fn test_cross_host_redirect_strips_credentials_and_blocks_ssrf_hop() {
        let cfg = curated_cfg();

        // Cross-host redirect: huggingface.co -> cdn-lfs.hf.co MUST strip Authorization, X-Api-Key, X-Subscription-Token
        let mock = MockHttpsFetcher::with_handler(|req| {
            if req.url == "https://huggingface.co/org/model/resolve/main/config.json" {
                let mut h = BTreeMap::new();
                h.insert(
                    "location".to_string(),
                    "https://cdn-lfs.hf.co/repos/aa/bb/config.json".to_string(),
                );
                Ok(OutboundHttpResponse {
                    status: 307,
                    final_url: req.url.clone(),
                    headers: h,
                    body: Vec::new(),
                })
            } else {
                Ok(OutboundHttpResponse {
                    status: 200,
                    final_url: req.url.clone(),
                    headers: BTreeMap::new(),
                    body: b"{\"model_type\":\"qwen3_moe\"}".to_vec(),
                })
            }
        });

        let mut req =
            OutboundHttpRequest::get("https://huggingface.co/org/model/resolve/main/config.json");
        req.headers = vec![
            ("Authorization".to_string(), "Bearer hf_secret_token".to_string()),
            ("X-Api-Key".to_string(), "exa_secret".to_string()),
            ("X-Subscription-Token".to_string(), "brave_secret".to_string()),
            ("Accept".to_string(), "application/json".to_string()),
        ];

        let resp = mock.execute(&req, &cfg).unwrap();
        assert_eq!(resp.status, 200);
        let hops = mock.recorded_requests();
        assert_eq!(hops.len(), 2);
        assert!(hops[0].headers.iter().any(|(k, _)| k == "Authorization"));
        assert!(!hops[1]
            .headers
            .iter()
            .any(|(k, _)| k.eq_ignore_ascii_case("Authorization")));
        assert!(!hops[1]
            .headers
            .iter()
            .any(|(k, _)| k.eq_ignore_ascii_case("X-Api-Key")));
        assert!(!hops[1]
            .headers
            .iter()
            .any(|(k, _)| k.eq_ignore_ascii_case("X-Subscription-Token")));
        assert!(hops[1].headers.iter().any(|(k, v)| k == "Accept" && v == "application/json"));

        // Redirect to 127.0.0.1:8000 on Hop 2 MUST fail closed with SecurityBlocked!
        let ssrf_redirect = MockHttpsFetcher::with_handler(|_| {
            let mut h = BTreeMap::new();
            h.insert(
                "location".to_string(),
                "http://127.0.0.1:8000/v1/models".to_string(),
            );
            Ok(OutboundHttpResponse {
                status: 302,
                final_url: String::new(),
                headers: h,
                body: Vec::new(),
            })
        });
        assert!(matches!(
            ssrf_redirect.execute(&OutboundHttpRequest::get("https://github.com/org/repo"), &cfg),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));

        // `follow_redirects = false` returns 307 + X-Linked-Size immediately with 0 redirect hops
        let hf_probe = MockHttpsFetcher::with_handler(|req| {
            let mut h = BTreeMap::new();
            h.insert("location".to_string(), "https://cdn-lfs.hf.co/huge.safetensors".to_string());
            h.insert("x-linked-size".to_string(), "10737418240".to_string());
            Ok(OutboundHttpResponse {
                status: 307,
                final_url: req.url.clone(),
                headers: h,
                body: Vec::new(),
            })
        });
        let mut no_redir_req =
            OutboundHttpRequest::get("https://huggingface.co/org/m/resolve/main/model.safetensors");
        no_redir_req.follow_redirects = false;
        let hop0 = hf_probe.execute(&no_redir_req, &cfg).unwrap();
        assert_eq!(hop0.status, 307);
        assert_eq!(hop0.header("x-linked-size"), Some("10737418240"));
        assert_eq!(hf_probe.recorded_requests().len(), 1);
    }

    #[test]
    fn test_captcha_and_rate_limit_hard_stop() {
        let cfg = curated_cfg();

        let cases: Vec<(u16, Vec<(&str, &str)>, &[u8])> = vec![
            (429, vec![], b"Too Many Requests"),
            (403, vec![("cf-mitigated", "challenge")], b"<html>blocked</html>"),
            (
                403,
                vec![],
                b"<html><head><title>Just a moment...</title></head><body><div class=\"cf-turnstile\"></div></body></html>",
            ),
            (
                503,
                vec![],
                b"<html><body><div class=\"g-recaptcha\" data-sitekey=\"abc\"></div></body></html>",
            ),
            (
                403,
                vec![],
                b"<html><body><div class=\"h-captcha\"></div></body></html>",
            ),
        ];

        for (status, hdrs, body) in cases {
            let hdr_map: BTreeMap<String, String> = hdrs
                .into_iter()
                .map(|(k, v)| (k.to_string(), v.to_string()))
                .collect();
            let body_vec = body.to_vec();
            let mock = MockHttpsFetcher::with_handler(move |req| {
                Ok(OutboundHttpResponse {
                    status,
                    final_url: req.url.clone(),
                    headers: hdr_map.clone(),
                    body: body_vec.clone(),
                })
            });
            let res = mock.execute(&OutboundHttpRequest::get("https://arxiv.org/abs/2401.00001"), &cfg);
            assert!(
                matches!(res, Err(OnlineFetchError::ChallengeOrRateLimited { .. })),
                "expected ChallengeOrRateLimited for status {status}, got {res:?}"
            );
        }

        // 200 OK article mentioning Turnstile in prose MUST NOT false-positive
        let ok_mock = MockHttpsFetcher::with_handler(|req| {
            Ok(OutboundHttpResponse {
                status: 200,
                final_url: req.url.clone(),
                headers: BTreeMap::new(),
                body: b"<h1>Migrating from g-recaptcha to cf-turnstile</h1>".to_vec(),
            })
        });
        assert!(ok_mock
            .execute(&OutboundHttpRequest::get("https://stackoverflow.com/questions/1"), &cfg)
            .is_ok());
    }

    #[test]
    fn test_pinned_request_config_pins_dns_disables_redirects_and_hides_sensitive_headers() {
        let validated = validate_outbound_url("https://api.github.com/repos/vllm-project/vllm").unwrap();
        let pinned: IpAddr = "140.82.112.6".parse().unwrap();
        let secret = "ghp_SUPER_SECRET_KEYCHAIN_PAT_999999";
        let headers = vec![
            ("Authorization".to_string(), format!("Bearer {secret}")),
            ("Accept".to_string(), "application/vnd.github+json".to_string()),
        ];

        let prepared =
            build_pinned_request_config(&validated, pinned, "GET", &headers, None, 15).unwrap();
        assert_eq!(prepared.url, "https://api.github.com/repos/vllm-project/vllm");
        assert_eq!(prepared.host, "api.github.com");
        assert_eq!(prepared.pinned_addr, SocketAddr::new(pinned, 443));
        assert_eq!(prepared.method, reqwest::Method::GET);
        assert!(!prepared.follow_redirects);
        assert!(!prepared.use_proxy);
        assert!(prepared.https_only);
        assert_eq!(prepared.min_tls_version, reqwest::tls::Version::TLS_1_2);
        assert_eq!(prepared.connect_timeout, Duration::from_secs(10));
        assert_eq!(prepared.total_timeout, Duration::from_secs(15));
        assert_eq!(
            prepared.headers.get(USER_AGENT).and_then(|v| v.to_str().ok()),
            Some(DEFAULT_POLITE_USER_AGENT)
        );
        assert_eq!(
            prepared.headers.get("accept").and_then(|v| v.to_str().ok()),
            Some("application/vnd.github+json")
        );
        let auth = prepared.headers.get("authorization").expect("authorization header");
        assert!(auth.is_sensitive(), "Authorization header must be marked sensitive");
        assert_eq!(auth.to_str().unwrap(), format!("Bearer {secret}"));
        let debug_repr = format!("{prepared:?}");
        assert!(
            !debug_repr.contains(secret),
            "Sensitive tokens MUST NOT appear in Debug formatting"
        );
        assert!(build_pinned_client(&prepared).is_ok());

        // Custom User-Agent is preserved instead of overwritten by default
        let custom_ua = vec![("User-Agent".to_string(), "CustomResearchAgent/2.0".to_string())];
        let prepared_custom =
            build_pinned_request_config(&validated, pinned, "GET", &custom_ua, None, 5).unwrap();
        assert_eq!(
            prepared_custom.headers.get(USER_AGENT).and_then(|v| v.to_str().ok()),
            Some("CustomResearchAgent/2.0")
        );
        assert_eq!(prepared_custom.connect_timeout, Duration::from_secs(5));
        assert_eq!(prepared_custom.total_timeout, Duration::from_secs(5));

        // Defense-in-depth: non-public pinned IP, CRLF header injection, and invalid method rejected
        let loopback: IpAddr = "127.0.0.1".parse().unwrap();
        assert!(matches!(
            build_pinned_request_config(&validated, loopback, "GET", &[], None, 10),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));
        let bad_hdr = vec![("X-Test".to_string(), "ok\r\nX-Injected: 1".to_string())];
        assert!(matches!(
            build_pinned_request_config(&validated, pinned, "GET", &bad_hdr, None, 10),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));
        assert!(matches!(
            build_pinned_request_config(&validated, pinned, "GET HTTP/1.1", &[], None, 10),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));
    }

    #[test]
    fn test_rate_limiter_spacing_and_streaming_early_abort() {
        assert!(host_min_spacing("export.arxiv.org") >= Duration::from_millis(3100));
        assert!(host_min_spacing("crates.io") >= Duration::from_millis(1000));
        assert!(host_min_spacing("github.com") >= Duration::from_millis(120));

        let t0 = Instant::now();
        let w1 = reserve_host_rate_limit_wait("export.arxiv.org", t0);
        let w2 = reserve_host_rate_limit_wait("export.arxiv.org", t0 + Duration::from_millis(100));
        assert_eq!(w1, Duration::ZERO);
        assert!(w2 >= Duration::from_millis(3000));

        // Streaming early abort (`Ok(false)` from `on_chunk`)
        let cfg = curated_cfg();
        let big_payload = vec![b'x'; 64 * 1024];
        let mock = MockHttpsFetcher::with_handler(move |req| {
            Ok(OutboundHttpResponse {
                status: 200,
                final_url: req.url.clone(),
                headers: BTreeMap::new(),
                body: big_payload.clone(),
            })
        })
        .with_stream_chunk_size(1024);

        let mut received = 0usize;
        let (status, _) = mock
            .execute_streaming(
                &OutboundHttpRequest::get("https://codeload.github.com/org/repo/tar.gz/deadbeef"),
                &cfg,
                &mut |chunk| {
                    received += chunk.len();
                    Ok(received < 4096)
                },
            )
            .unwrap();
        assert_eq!(status, 200);
        assert_eq!(received, 4096);
        assert_eq!(mock.stream_abort_count(), 1);
    }

    #[test]
    fn test_response_header_extraction_and_bounded_stream_max_bytes() {
        let mut raw_headers = HeaderMap::new();
        raw_headers.insert(
            reqwest::header::CONTENT_TYPE,
            HeaderValue::from_static(" text/html; charset=utf-8 "),
        );
        raw_headers.insert(
            reqwest::header::LOCATION,
            HeaderValue::from_static("https://cdn-lfs.hf.co/repos/aa/bb/file.bin"),
        );
        raw_headers.insert(
            HeaderName::from_static("x-linked-size"),
            HeaderValue::from_static("4096"),
        );
        let headers = extract_response_headers(&raw_headers);
        assert_eq!(
            headers.get("content-type").map(String::as_str),
            Some("text/html; charset=utf-8")
        );
        assert_eq!(
            headers.get("location").map(String::as_str),
            Some("https://cdn-lfs.hf.co/repos/aa/bb/file.bin")
        );
        assert_eq!(headers.get("x-linked-size").map(String::as_str), Some("4096"));

        struct CountingReader<'a> {
            data: &'a [u8],
            pos: usize,
            bytes_read: Arc<AtomicUsize>,
        }
        impl<'a> Read for CountingReader<'a> {
            fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
                if self.pos >= self.data.len() {
                    return Ok(0);
                }
                let n = buf.len().min(self.data.len() - self.pos);
                buf[..n].copy_from_slice(&self.data[self.pos..self.pos + n]);
                self.pos += n;
                self.bytes_read.fetch_add(n, Ordering::SeqCst);
                Ok(n)
            }
        }

        // Exact max_bytes boundary succeeds cleanly
        let exact_payload = vec![b'a'; 1024];
        let mut exact_out = Vec::new();
        stream_bounded_reader(
            exact_payload.as_slice(),
            "https://arxiv.org/abs/2401.00001",
            1024,
            &mut |chunk| {
                exact_out.extend_from_slice(chunk);
                Ok(true)
            },
        )
        .unwrap();
        assert_eq!(exact_out.len(), 1024);

        // Oversized stream is rejected via `take(max_bytes + 1)` without reading past 1025 bytes
        let oversized = vec![b'z'; 64 * 1024];
        let read_counter = Arc::new(AtomicUsize::new(0));
        let counting_reader = CountingReader {
            data: &oversized,
            pos: 0,
            bytes_read: read_counter.clone(),
        };
        let err = stream_bounded_reader(
            counting_reader,
            "https://arxiv.org/abs/2401.00001",
            1024,
            &mut |_| Ok(true),
        )
        .expect_err("oversized stream must fail closed");
        assert!(matches!(
            err,
            OnlineFetchError::SizeOrBinaryBlocked { bytes: 1025, .. }
        ));
        assert_eq!(
            read_counter.load(Ordering::SeqCst),
            1025,
            "Bounded reader must never pull more than max_bytes + 1 bytes"
        );

        // Early abort stops reading immediately
        let abort_counter = Arc::new(AtomicUsize::new(0));
        let abort_reader = CountingReader {
            data: &oversized,
            pos: 0,
            bytes_read: abort_counter.clone(),
        };
        let mut received = 0usize;
        stream_bounded_reader(
            abort_reader,
            "https://codeload.github.com/org/repo/tar.gz/deadbeef",
            64 * 1024,
            &mut |chunk| {
                received += chunk.len();
                Ok(false)
            },
        )
        .unwrap();
        assert_eq!(received, 16 * 1024);
        assert_eq!(abort_counter.load(Ordering::SeqCst), 16 * 1024);
    }

    #[test]
    fn test_relative_redirect_resolution_and_max_hops_cap() {
        let base = validate_outbound_url("https://ar5iv.labs.arxiv.org/html/2401.12345").unwrap();
        assert_eq!(
            resolve_redirect_url(&base, "/abs/2401.12345").unwrap(),
            "https://ar5iv.labs.arxiv.org/abs/2401.12345"
        );
        assert_eq!(
            resolve_redirect_url(&base, "//arxiv.org/abs/2401.12345").unwrap(),
            "https://arxiv.org/abs/2401.12345"
        );
        assert_eq!(
            resolve_redirect_url(&base, "2401.12345v2").unwrap(),
            "https://ar5iv.labs.arxiv.org/html/2401.12345v2"
        );

        let cfg = curated_cfg();
        let loop_mock = MockHttpsFetcher::with_handler(|req| {
            let mut h = BTreeMap::new();
            h.insert("location".to_string(), req.url.clone());
            Ok(OutboundHttpResponse {
                status: 302,
                final_url: req.url.clone(),
                headers: h,
                body: Vec::new(),
            })
        });
        assert!(matches!(
            loop_mock.execute(&OutboundHttpRequest::get("https://arxiv.org/abs/2401.00001"), &cfg),
            Err(OnlineFetchError::TransportError(_))
        ));
        assert_eq!(loop_mock.recorded_requests().len(), MAX_REDIRECT_HOPS + 1);
    }

    #[test]
    fn test_ipv6_pinned_reqwest_client_and_head_method() {
        let validated = validate_outbound_url("https://docs.rs/serde/latest/serde/").unwrap();
        let v6_pin: IpAddr = "2606:4700:4700::1111".parse().unwrap();
        let prepared =
            build_pinned_request_config(&validated, v6_pin, "HEAD", &[], None, 10).unwrap();
        assert_eq!(prepared.pinned_addr, SocketAddr::new(v6_pin, 443));
        assert!(prepared.pinned_addr.is_ipv6());
        assert_eq!(prepared.method, reqwest::Method::HEAD);
        assert_eq!(prepared.connect_timeout, Duration::from_secs(10));
        assert_eq!(prepared.total_timeout, Duration::from_secs(10));
        assert!(build_pinned_client(&prepared).is_ok());

        // Both primary struct and backwards-compatible alias implement HttpsFetcher & fail closed offline
        let offline_cfg = OnlineResearchConfig::with_defaults();
        let req = OutboundHttpRequest::get("https://arxiv.org/abs/2401.00001");
        let prod_fetcher = ReqwestPinnedHttpsFetcher::new();
        let alias_fetcher = CurlPinnedHttpsFetcher::new();
        assert!(matches!(
            prod_fetcher.execute(&req, &offline_cfg),
            Err(OnlineFetchError::OfflineAirGapped(_))
        ));
        assert!(matches!(
            alias_fetcher.execute(&req, &offline_cfg),
            Err(OnlineFetchError::OfflineAirGapped(_))
        ));
    }
}
