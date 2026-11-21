//! `online_fetch` — Gated Online Research, Lossless Paper Cascade, Clean HTML-to-Markdown,
//! 2-Tier Federated Search, and Sparse GitHub/GitLab/Codeberg/HuggingFace Fetcher.
//!
//! Designed for Spell IDE:
//! - Preserves `"fetch"` (`{"url": string}`) cleanly across turns.
//! - Stages full cleaned documents & repo subtrees into read-only `<workspace>/.spell/research_cache/`
//!   and returns compact `~600–950`-token TOC/Manifest envelopes wrapped in per-session random-nonce
//!   `<untrusted_external_content>` provenance tags.
//! - Enforces `Offline` (Air-Gapped) fail-closed default (`0` DNS/TCP sockets), socket-level DNS-pinning
//!   anti-SSRF (`SafePublicDnsResolver`), cross-origin credential stripping, CAPTCHA/anti-bot hard-stop
//!   (`ChallengeOrRateLimited` — never solved or bypassed), and Gatekeeper Critic isolation.

pub mod archive_tar;
pub mod html_clean;
pub mod net_guard;
pub mod paper_extract;
pub mod repo_fetch;
pub mod search;
pub mod staging;

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use parking_lot::RwLock;
use once_cell::sync::Lazy;
use serde::{Deserialize, Serialize};

/// Explicit three-mode network posture (`Offline` fresh-install default & 1-click kill-switch).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum OnlineResearchMode {
    /// Zero outbound DNS or TCP sockets; fails closed before DNS resolution.
    Offline,
    /// Only vetted academic, package, docs, and code-host domains (`arxiv.org`, `github.com`, etc.).
    CuratedAllowlist,
    /// Vetted allowlist auto-allowed; unknown public domains require per-domain user approval.
    OpenWebAskPerDomain,
}

impl Default for OnlineResearchMode {
    fn default() -> Self {
        Self::Offline
    }
}

impl OnlineResearchMode {
    pub fn from_wire(raw: &str) -> Self {
        match raw.trim().to_ascii_lowercase().as_str() {
            "curated" | "curated_allowlist" | "allowlist" => Self::CuratedAllowlist,
            "open" | "open_web" | "open_web_ask_per_domain" | "ask_per_domain" => {
                Self::OpenWebAskPerDomain
            }
            _ => Self::Offline,
        }
    }

    pub fn as_wire(&self) -> &'static str {
        match self {
            Self::Offline => "offline",
            Self::CuratedAllowlist => "curated_allowlist",
            Self::OpenWebAskPerDomain => "open_web_ask_per_domain",
        }
    }
}

/// Optional BYOK general web search provider stored in macOS Keychain.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SearchProviderKind {
    Brave,
    Exa,
    Tavily,
    Kagi,
}

impl SearchProviderKind {
    pub fn from_wire(raw: &str) -> Option<Self> {
        match raw.trim().to_ascii_lowercase().as_str() {
            "brave" => Some(Self::Brave),
            "exa" => Some(Self::Exa),
            "tavily" => Some(Self::Tavily),
            "kagi" => Some(Self::Kagi),
            _ => None,
        }
    }
}

/// In-memory session configuration & Keychain-injected credentials (never persisted to SQLite/logs).
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct OnlineResearchConfig {
    pub mode: OnlineResearchMode,
    /// User-approved permanent custom domains (lowercase ASCII FQDN, e.g. `"pytorch.org"`).
    pub custom_allowed_domains: BTreeSet<String>,
    /// Session-scoped ephemeral domain approvals (`Allow Session`).
    pub session_allowed_domains: BTreeSet<String>,
    /// Single-shot domain approvals (`Allow Once`).
    pub once_allowed_domains: BTreeSet<String>,
    /// Optional GitHub/GitLab/HF personal access token (never logged; stripped on cross-host redirects).
    #[serde(skip_serializing)]
    pub github_token: Option<String>,
    #[serde(skip_serializing)]
    pub huggingface_token: Option<String>,
    /// Optional OpenAlex free API key (`10x` daily budget upgrade).
    #[serde(skip_serializing)]
    pub openalex_api_key: Option<String>,
    /// Optional BYOK search provider & API key.
    pub search_provider: Option<SearchProviderKind>,
    #[serde(skip_serializing)]
    pub search_api_key: Option<String>,
    /// Optional polite `mailto:` contact for Crossref Polite Pool.
    pub crossref_mailto: Option<String>,
    /// Optional workspace root & fallback cache root for `.spell/research_cache/` staging.
    pub workspace_root: Option<PathBuf>,
    pub workspace_read_only: bool,
    pub fallback_data_dir: Option<PathBuf>,
    /// Per-session random 8-hex provenance nonce for `<untrusted_external_content nonce="...">`.
    pub session_nonce: String,
    /// Optional maximum single-file download bytes (`default = 10 MiB`).
    pub max_single_file_bytes: usize,
    /// Optional maximum sparse tree download bytes (`default = 50 MiB`).
    pub max_tree_bytes: usize,
}

impl OnlineResearchConfig {
    pub fn with_defaults() -> Self {
        Self {
            mode: OnlineResearchMode::Offline,
            custom_allowed_domains: BTreeSet::new(),
            session_allowed_domains: BTreeSet::new(),
            once_allowed_domains: BTreeSet::new(),
            github_token: None,
            huggingface_token: None,
            openalex_api_key: None,
            search_provider: None,
            search_api_key: None,
            crossref_mailto: None,
            workspace_root: None,
            workspace_read_only: false,
            fallback_data_dir: None,
            session_nonce: generate_default_nonce(),
            max_single_file_bytes: 10 * 1024 * 1024,
            max_tree_bytes: 50 * 1024 * 1024,
        }
    }
}

pub fn generate_default_nonce() -> String {
    use sha2::{Digest, Sha256};
    let now_ns = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let pid = std::process::id();
    let mut h = Sha256::new();
    h.update(now_ns.to_le_bytes());
    h.update(pid.to_le_bytes());
    let hex = format!("{:x}", h.finalize());
    hex[..8].to_string()
}

/// Structured HTTP request sent through [`HttpsFetcher`].
#[derive(Debug, Clone)]
pub struct OutboundHttpRequest {
    pub url: String,
    pub method: String,
    pub headers: Vec<(String, String)>,
    pub body: Option<Vec<u8>>,
    pub max_bytes: usize,
    pub timeout_secs: u64,
    /// When `false`, return `3xx` response headers immediately without following `Location`
    /// (used for HuggingFace `/resolve/` hop-1 `X-Linked-Size` inspection & `ar5iv` `/abs/` detection).
    pub follow_redirects: bool,
}

impl OutboundHttpRequest {
    pub fn get(url: impl Into<String>) -> Self {
        Self {
            url: url.into(),
            method: "GET".to_string(),
            headers: Vec::new(),
            body: None,
            max_bytes: 12 * 1024 * 1024,
            timeout_secs: 20,
            follow_redirects: true,
        }
    }
}

/// Structured HTTP response returned by [`HttpsFetcher`].
#[derive(Debug, Clone)]
pub struct OutboundHttpResponse {
    pub status: u16,
    pub final_url: String,
    /// Lowercase header names -> values.
    pub headers: BTreeMap<String, String>,
    pub body: Vec<u8>,
}

impl OutboundHttpResponse {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .get(&name.to_ascii_lowercase())
            .map(|s| s.as_str())
    }
}

/// Pluggable HTTPS transport abstraction (production DNS-pinned `curl` + offline unit-test mocks).
pub trait HttpsFetcher: Send + Sync {
    fn execute(
        &self,
        req: &OutboundHttpRequest,
        cfg: &OnlineResearchConfig,
    ) -> Result<OutboundHttpResponse, OnlineFetchError>;

    /// Streaming fetch passing chunks to `on_chunk`; returning `Ok(false)` from `on_chunk` aborts
    /// the stream early cleanly (used by Strategy B `codeload` sparse subdirectory tarballs!).
    fn execute_streaming(
        &self,
        req: &OutboundHttpRequest,
        cfg: &OnlineResearchConfig,
        on_chunk: &mut dyn FnMut(&[u8]) -> Result<bool, OnlineFetchError>,
    ) -> Result<(u16, BTreeMap<String, String>), OnlineFetchError> {
        let resp = self.execute(req, cfg)?;
        let _ = on_chunk(&resp.body)?;
        Ok((resp.status, resp.headers))
    }
}

/// Optional CPU-only PDF text extractor hook (registered by host UI or pure-Rust fallback).
pub type PdfExtractorHook = Arc<dyn Fn(&[u8]) -> Result<String, String> + Send + Sync>;

static PDF_EXTRACTOR_HOOK: Lazy<RwLock<Option<PdfExtractorHook>>> = Lazy::new(|| RwLock::new(None));

pub fn register_pdf_extractor_hook(hook: PdfExtractorHook) {
    *PDF_EXTRACTOR_HOOK.write() = Some(hook);
}

pub fn current_pdf_extractor_hook() -> Option<PdfExtractorHook> {
    PDF_EXTRACTOR_HOOK.read().clone()
}

/// Hierarchical section index entry written to `manifest.json` and summarized in the compact envelope.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SectionTocEntry {
    pub heading: String,
    pub level: u8,
    pub start_line: usize,
    pub end_line: usize,
    pub byte_offset: usize,
    pub byte_len: usize,
    pub est_tokens: usize,
    pub cached_rel_path: Option<String>,
}

/// Rich citation metadata surfaced to UI `ToolCallCard` Citation Header Strip.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CitationMetadata {
    pub title: String,
    pub canonical_url: String,
    pub domain: String,
    /// E.g. `"arXiv HTML5 · Lossless Math"`, `"arXiv e-print TeX"`, `"GitHub Tree · 14 files"`, `"Federated Search"`.
    pub tier_badge: String,
    /// Relative or absolute path in `.spell/research_cache/...` for 1-click editor open.
    pub cached_primary_path: Option<String>,
    pub cached_file_count: usize,
    pub est_total_tokens: usize,
    pub authors: Vec<String>,
    pub published: Option<String>,
}

/// Full outcome of an online research operation before rendering into tool text.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct OnlineFetchOutcome {
    /// Compact `~500–950`-token provenance-wrapped envelope returned directly to the main agent.
    pub envelope_markdown: String,
    pub citation: CitationMetadata,
    pub toc: Vec<SectionTocEntry>,
    pub staged_root: Option<PathBuf>,
    pub staged_files: Vec<PathBuf>,
}

/// Typed security, policy, rate-limit, and extraction errors.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum OnlineFetchError {
    /// `OnlineResearchMode::Offline` active (`0` DNS/TCP sockets created).
    OfflineAirGapped(String),
    /// SSRF, loopback (`:8000`), RFC1918, non-HTTPS, non-443, IP literal, or URL exfil blocked.
    SecurityBlocked(String),
    /// Unknown domain in `CuratedAllowlist` mode (hard deny) or `OpenWebAskPerDomain` (needs user approval).
    DomainApprovalRequired { domain: String, url: String },
    /// Session Taint Guard (`web_tainted && workspace_read` contacting unreferenced repo/host).
    TaintConfirmationRequired { domain: String, url: String, reason: String },
    /// CAPTCHA, Turnstile, reCAPTCHA, hCaptcha, or `403`/`429` anti-bot challenge — NEVER bypassed!
    ChallengeOrRateLimited { domain: String, status: u16, detail: String },
    /// File or tree exceeds configured byte budget (e.g. HuggingFace `X-Linked-Size > 10 MiB` or `.safetensors`).
    SizeOrBinaryBlocked { url: String, bytes: u64, reason: String },
    /// Network, DNS, TLS, or upstream HTTP failure.
    TransportError(String),
    /// Malformed archive, invalid input, or extraction error.
    ExtractionError(String),
}

impl std::fmt::Display for OnlineFetchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::OfflineAirGapped(msg) => write!(f, "Online research is disabled (Air-Gapped mode): {msg}"),
            Self::SecurityBlocked(msg) => write!(f, "Outbound security guard blocked URL: {msg}"),
            Self::DomainApprovalRequired { domain, url } => {
                write!(f, "Domain approval required for '{domain}' ({url})")
            }
            Self::TaintConfirmationRequired { domain, url, reason } => {
                write!(f, "Session taint confirmation required for '{domain}' ({url}): {reason}")
            }
            Self::ChallengeOrRateLimited { domain, status, detail } => write!(
                f,
                "Stopped at upstream challenge or rate limit on '{domain}' (HTTP {status}): {detail}"
            ),
            Self::SizeOrBinaryBlocked { url, bytes, reason } => {
                write!(f, "Blocked large or binary download ({bytes} bytes) from '{url}': {reason}")
            }
            Self::TransportError(msg) => write!(f, "Online fetch transport error: {msg}"),
            Self::ExtractionError(msg) => write!(f, "Online fetch extraction error: {msg}"),
        }
    }
}

impl std::error::Error for OnlineFetchError {}

/// Top-level router dispatching `arXiv`/DOI/papers, `search://`, GitHub/GitLab/Codeberg/HF repos, and general HTML docs.
pub fn execute_online_fetch_with_fetcher(
    raw_input: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let trimmed = raw_input.trim();
    if trimmed.is_empty() {
        return Err(OnlineFetchError::ExtractionError(
            "fetch requires a non-empty `url`".to_string(),
        ));
    }
    if cfg.mode == OnlineResearchMode::Offline {
        return Err(OnlineFetchError::OfflineAirGapped(
            "Enable 'Curated Research Allowlist' or 'Open Web' in BeatrixLeslie IDE Settings -> Online Research.".to_string(),
        ));
    }

    // 1. Federated search queries (`search://...` or `search: ...`)
    if search::is_search_request(trimmed) {
        return search::execute_federated_search(trimmed, cfg, fetcher);
    }

    // 2. Academic papers (`arXiv`, `ar5iv`, `DOI`, `OpenReview`, `ACL Anthology`, `PMLR`)
    if paper_extract::is_academic_paper_url(trimmed) {
        return paper_extract::fetch_and_extract_paper(trimmed, cfg, fetcher);
    }

    // 3. Code repositories & model hubs (`GitHub`, `GitLab`, `Codeberg`, `HuggingFace`)
    if repo_fetch::is_repo_or_hub_url(trimmed) {
        return repo_fetch::fetch_repo_or_hub_resource(trimmed, cfg, fetcher);
    }

    // 4. Smart documentation auto-upgraders (`Apple Swift-DocC JSON`, `docs.rs`) + 3-stage Clean HTML
    if let Some(doc_outcome) = search::try_fetch_structured_doc_url(trimmed, cfg, fetcher)? {
        return Ok(doc_outcome);
    }

    let req = OutboundHttpRequest::get(trimmed);
    let resp = fetcher.execute(&req, cfg)?;
    if resp.status >= 400 {
        return Err(OnlineFetchError::TransportError(format!(
            "HTTP {} fetching {}",
            resp.status, resp.final_url
        )));
    }
    let cleaned = html_clean::extract_clean_markdown_and_toc(&resp.body, &resp.final_url)?;
    staging::stage_single_document_and_build_envelope(
        &cleaned.title,
        &resp.final_url,
        "Clean Web Markdown",
        &cleaned.markdown,
        cleaned.toc,
        None,
        cfg,
    )
}

/// Convenience entry point using the production DNS-pinning HTTPS fetcher (`net_guard::CurlPinnedHttpsFetcher`).
pub fn execute_online_fetch(
    raw_input: &str,
    cfg: &OnlineResearchConfig,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let fetcher = net_guard::CurlPinnedHttpsFetcher::new();
    execute_online_fetch_with_fetcher(raw_input, cfg, &fetcher)
}

/// Helper check for staging path write-protection (matched component-wise case-insensitively
/// so both `.spell/research_cache/...` and legacy `.beatrix/research_cache/...` are caught on
/// case-insensitive APFS even before the directory exists).
pub fn is_research_cache_path(path: &Path) -> bool {
    let matches_cache_pair = |parent: &str, child: &str| {
        (parent.eq_ignore_ascii_case(".spell") || parent.eq_ignore_ascii_case(".beatrix"))
            && child.eq_ignore_ascii_case("research_cache")
    };

    let normal_comps: Vec<std::borrow::Cow<'_, str>> = path
        .components()
        .filter_map(|c| match c {
            std::path::Component::Normal(os) => Some(os.to_string_lossy()),
            _ => None,
        })
        .collect();
    if normal_comps
        .windows(2)
        .any(|w| matches_cache_pair(&w[0], &w[1]))
    {
        return true;
    }

    let lossy = path.to_string_lossy();
    let raw_segs: Vec<&str> = lossy
        .split(['/', '\\'])
        .filter(|s| !s.is_empty() && *s != ".")
        .collect();
    if raw_segs
        .windows(2)
        .any(|w| matches_cache_pair(w[0], w[1]))
    {
        return true;
    }

    let mut collapsed: Vec<&str> = Vec::with_capacity(raw_segs.len());
    for seg in raw_segs {
        if seg == ".." {
            collapsed.pop();
        } else {
            collapsed.push(seg);
        }
    }
    collapsed
        .windows(2)
        .any(|w| matches_cache_pair(w[0], w[1]))
}
