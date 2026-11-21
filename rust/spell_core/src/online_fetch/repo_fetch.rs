//! Selective `GitHub`, `GitLab`, `Codeberg` & `HuggingFace Hub` fetcher.
//!
//! Co-Design & Security Architecture:
//! 1. **Zero-Quota Git Protocol v2 `ls-refs` (`resolve_git_v2_ref_and_subpath`):**
//!    Sends a deterministic ~150-byte pkt-line `ls-refs` POST to `/{owner}/{repo}.git/git-upload-pack`
//!    (`Git-Protocol: version=2`) to resolve exact 40-char commit SHAs and longest-prefix slash
//!    branches/tags (e.g. `releases/v0.6.0/csrc/attn.cu#L10-L50`) at `0` REST API quota cost.
//! 2. **Authenticated vs Zero-Key Single-File Fetch (`blob` / `raw`):**
//!    Uses `api.github.com/repos/{owner}/{repo}/contents/{path}?ref={sha}` with
//!    `Accept: application/vnd.github.raw+json` + `Bearer {pat}` when `cfg.github_token` is set,
//!    and `raw.githubusercontent.com` (`/-/raw/` on GitLab, `/raw/commit/` on Codeberg) for public
//!    zero-key fetches. Rejects NUL/Mach-O/ELF binaries and stages full files (`0o444`) while
//!    returning focused `#Lstart-Lend` slices in the compact envelope.
//! 3. **Adaptive Sparse Subdirectory & Repo Root Router:**
//!    - **Strategy A (`<= 64` files & `<= 25 MiB`):** Segment-by-segment GitHub `/git/trees/{sha}`
//!      walk to `{subdir}`'s `tree_sha` (avoiding 100k-entry root `?recursive=1` truncation),
//!      followed by `?recursive=1` on `{subdir}` and `raw.githubusercontent.com` blob pulls.
//!    - **Strategy B (`> 64` files or REST `403/429` quota exhausted, plus GitLab/Codeberg):**
//!      Streams `codeload.github.com/{owner}/{repo}/tar.gz/{sha}` via `execute_streaming`,
//!      exploiting `git archive`'s strict unsigned-byte lexicographical path order to return
//!      `Ok(false)` from `on_chunk` the millisecond the first entry header `>` `{top_prefix}/{subdir}/`
//!      arrives, then extracts matching entries via `super::archive_tar`.
//! 4. **Pull Requests, Issues & Commits (`/pull/{n}`, `/issues/{n}`, `/commit/{sha}`):**
//!    Fetches PR metadata + comments + unified diff (`Accept: application/vnd.github.diff`), with
//!    automatic `406 Not Acceptable` (`too_large`) fallback to `/pulls/{n}/files?per_page=100`
//!    skipping lockfile noise (`Cargo.lock`, `package-lock.json`, `pnpm-lock.yaml`, `uv.lock`, `poetry.lock`).
//! 5. **HuggingFace Hub Pre-Redirect Multi-GB Weight Guard:**
//!    Denylists `.safetensors`, `.gguf`, `.bin`, `.pt`, `.pth`, `.ckpt`, `.onnx`, `.parquet`, `.tar`,
//!    `.zip` at `0` bytes downloaded, rewrites `/blob/` & `/raw/` to `/resolve/` with
//!    `follow_redirects = false` on Hop 1, inspects `x-linked-size` & `content-length` on `302`/`307`
//!    BEFORE dialing CDN/Xet, strips `Authorization` on cross-host redirects, and stages safe
//!    `config.json` / `tokenizer_config.json` / `chat_template.jinja` / `modeling_*.py` / `README.md`.

use serde_json::Value;

use super::archive_tar::{self, ExtractedArchiveFile};
use super::staging::{self, RepoStagingRequest, StagedRepoFileInput};
use super::{
    HttpsFetcher, OnlineFetchError, OnlineFetchOutcome, OnlineResearchConfig, OutboundHttpRequest,
    OutboundHttpResponse,
};

const DEFAULT_MAX_SINGLE_FILE_BYTES: usize = 10 * 1024 * 1024;
const DEFAULT_MAX_TREE_BYTES: usize = 50 * 1024 * 1024;
const STRATEGY_A_MAX_FILES: usize = 64;
const STRATEGY_A_MAX_BYTES: usize = 25 * 1024 * 1024;
const ROOT_TARBALL_MAX_FILES: usize = 96;
const USER_AGENT: &str = "SpellIDE/1.0";

/// Returns `true` when `raw` targets a supported Git forge or HuggingFace Hub URL.
pub fn is_repo_or_hub_url(raw: &str) -> bool {
    let low = raw.trim().to_ascii_lowercase();
    low.starts_with("https://github.com/")
        || low.starts_with("https://raw.githubusercontent.com/")
        || low.starts_with("https://gitlab.com/")
        || low.starts_with("https://codeberg.org/")
        || low.starts_with("https://huggingface.co/")
        || low.starts_with("https://hf.co/")
}

fn effective_max_single_file_bytes(cfg: &OnlineResearchConfig) -> usize {
    if cfg.max_single_file_bytes == 0 {
        DEFAULT_MAX_SINGLE_FILE_BYTES
    } else {
        cfg.max_single_file_bytes
    }
}

fn effective_max_tree_bytes(cfg: &OnlineResearchConfig) -> usize {
    if cfg.max_tree_bytes == 0 {
        DEFAULT_MAX_TREE_BYTES
    } else {
        cfg.max_tree_bytes
    }
}

fn is_hex_sha40(s: &str) -> bool {
    s.len() == 40 && s.bytes().all(|b| b.is_ascii_hexdigit())
}

fn percent_decode(input: &str) -> String {
    let bytes = input.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0usize;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let (Some(hi), Some(lo)) = (hex_val(bytes[i + 1]), hex_val(bytes[i + 2])) {
                out.push((hi << 4) | lo);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn hex_val(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}

fn url_encode_component(raw: &str) -> String {
    let mut out = String::with_capacity(raw.len());
    for &b in raw.as_bytes() {
        if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.' | b'~') {
            out.push(b as char);
        } else {
            out.push_str(&format!("%{:02X}", b));
        }
    }
    out
}

fn url_encode_repo_path(path: &str) -> String {
    path.split('/')
        .filter(|s| !s.is_empty())
        .map(url_encode_component)
        .collect::<Vec<_>>()
        .join("/")
}

fn base64_encode(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] =
        b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity((bytes.len() + 2) / 3 * 4);
    for chunk in bytes.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = if chunk.len() > 1 { chunk[1] as u32 } else { 0 };
        let b2 = if chunk.len() > 2 { chunk[2] as u32 } else { 0 };
        let n = (b0 << 16) | (b1 << 8) | b2;
        out.push(ALPHABET[((n >> 18) & 0x3F) as usize] as char);
        out.push(ALPHABET[((n >> 12) & 0x3F) as usize] as char);
        if chunk.len() > 1 {
            out.push(ALPHABET[((n >> 6) & 0x3F) as usize] as char);
        } else {
            out.push('=');
        }
        if chunk.len() > 2 {
            out.push(ALPHABET[(n & 0x3F) as usize] as char);
        } else {
            out.push('=');
        }
    }
    out
}

/// Parses `#L10`, `#L10-L50`, `#L10-50`, or `#L10C2-L50C8` line fragments.
pub fn parse_line_range_fragment(fragment: Option<&str>) -> Option<(usize, usize)> {
    let frag = fragment?.trim().trim_start_matches('#');
    if frag.is_empty() {
        return None;
    }
    let parse_line_num = |part: &str| -> Option<usize> {
        let p = part.trim().strip_prefix('L').or_else(|| part.trim().strip_prefix('l')).unwrap_or(part.trim());
        let digits: String = p.chars().take_while(|c| c.is_ascii_digit()).collect();
        if digits.is_empty() {
            None
        } else {
            digits.parse::<usize>().ok().filter(|&n| n >= 1)
        }
    };
    if let Some((lhs, rhs)) = frag.split_once('-') {
        let start = parse_line_num(lhs)?;
        let end = parse_line_num(rhs).unwrap_or(start);
        Some((start.min(end), start.max(end)))
    } else if frag.starts_with('L') || frag.starts_with('l') {
        let line = parse_line_num(frag)?;
        Some((line, line))
    } else {
        None
    }
}

fn split_url_parts(raw_url: &str) -> Result<(String, Vec<String>, Option<String>, Option<String>), OnlineFetchError> {
    let trimmed = raw_url.trim();
    let rest = trimmed.strip_prefix("https://").ok_or_else(|| {
        OnlineFetchError::SecurityBlocked(format!("Only https:// repository URLs are permitted: {trimmed}"))
    })?;
    let (before_frag, frag) = match rest.split_once('#') {
        Some((a, b)) => (a, Some(b.to_string())),
        None => (rest, None),
    };
    let (host_and_path, query) = match before_frag.split_once('?') {
        Some((a, b)) => (a, Some(b.to_string())),
        None => (before_frag, None),
    };
    let (host_raw, path_raw) = match host_and_path.split_once('/') {
        Some((h, p)) => (h, p),
        None => (host_and_path, ""),
    };
    let host = host_raw.trim().to_ascii_lowercase();
    if host.is_empty() || host.contains(':') || host.contains('@') {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Invalid repository host in URL: {trimmed}"
        )));
    }
    let segments: Vec<String> = path_raw
        .split('/')
        .filter(|s| !s.is_empty())
        .map(percent_decode)
        .collect();
    Ok((host, segments, query, frag))
}

// ============================================================================
// 1. Git Protocol v2 `ls-refs` Zero-Quota Resolver
// ============================================================================

/// Parsed reference advertised by a Git Protocol v2 `ls-refs` response.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitV2RefEntry {
    pub oid: String,
    pub peeled_oid: Option<String>,
    pub refname: String,
    pub symref_target: Option<String>,
}

impl GitV2RefEntry {
    pub fn commit_oid(&self) -> &str {
        self.peeled_oid.as_deref().unwrap_or(&self.oid)
    }
}

/// Disambiguated Git reference, pinned commit SHA, relative path, and `#L` line range.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ResolvedGitRef {
    pub ref_name: String,
    pub commit_sha: Option<String>,
    pub subpath: String,
    pub line_range: Option<(usize, usize)>,
    pub is_tag: bool,
}

fn encode_pkt_line(payload: &str) -> String {
    format!("{:04x}{payload}", payload.len() + 4)
}

/// Builds a deterministic Git Protocol v2 `ls-refs` pkt-line request body.
pub fn build_git_v2_ls_refs_request_body(seg0_hint: Option<&str>) -> Vec<u8> {
    let mut pkt = String::with_capacity(192);
    pkt.push_str(&encode_pkt_line("command=ls-refs\n"));
    pkt.push_str(&encode_pkt_line("agent=spell/1.0\n"));
    pkt.push_str("0001");
    pkt.push_str(&encode_pkt_line("peel\n"));
    pkt.push_str(&encode_pkt_line("symrefs\n"));
    match seg0_hint.map(str::trim).filter(|s| !s.is_empty()) {
        Some(seg0) => {
            pkt.push_str(&encode_pkt_line(&format!("ref-prefix refs/heads/{seg0}\n")));
            pkt.push_str(&encode_pkt_line(&format!("ref-prefix refs/tags/{seg0}\n")));
        }
        None => {
            pkt.push_str(&encode_pkt_line("ref-prefix HEAD\n"));
        }
    }
    pkt.push_str("0000");
    pkt.into_bytes()
}

/// Parses a Git Protocol v2 `ls-refs` pkt-line response (`4-hex` length prefixes).
pub fn parse_git_v2_ls_refs_response(body: &[u8]) -> Vec<GitV2RefEntry> {
    let mut entries: Vec<GitV2RefEntry> = Vec::new();
    let mut cursor = 0usize;
    while cursor + 4 <= body.len() {
        let Ok(len_str) = std::str::from_utf8(&body[cursor..cursor + 4]) else {
            break;
        };
        let Ok(pkt_len) = usize::from_str_radix(len_str, 16) else {
            break;
        };
        if pkt_len <= 2 {
            // `0000` flush-pkt, `0001` delim-pkt, `0002` response-end-pkt
            cursor += 4;
            continue;
        }
        if pkt_len < 4 || cursor + pkt_len > body.len() {
            break;
        }
        let payload_bytes = &body[cursor + 4..cursor + pkt_len];
        cursor += pkt_len;

        let Ok(line_raw) = std::str::from_utf8(payload_bytes) else {
            continue;
        };
        let line = line_raw.trim_end_matches(['\r', '\n']);
        if let Some(peeled_rest) = line.strip_prefix('^') {
            if is_hex_sha40(peeled_rest) {
                if let Some(last) = entries.last_mut() {
                    last.peeled_oid = Some(peeled_rest.to_ascii_lowercase());
                }
            }
            continue;
        }

        let mut parts = line.split_whitespace();
        let Some(oid_raw) = parts.next() else {
            continue;
        };
        let Some(refname) = parts.next() else {
            continue;
        };
        if !is_hex_sha40(oid_raw) {
            continue;
        }
        let mut peeled_oid = None;
        let mut symref_target = None;
        for attr in parts {
            if let Some(target) = attr.strip_prefix("symref-target:") {
                symref_target = Some(target.to_string());
            } else if let Some(peeled) = attr.strip_prefix("peeled:") {
                if is_hex_sha40(peeled) {
                    peeled_oid = Some(peeled.to_ascii_lowercase());
                }
            }
        }
        entries.push(GitV2RefEntry {
            oid: oid_raw.to_ascii_lowercase(),
            peeled_oid,
            refname: refname.to_string(),
            symref_target,
        });
    }
    entries
}

/// Resolves exact 40-char commit SHAs and longest-prefix slash-branch/tag boundaries via Git
/// Protocol v2 `ls-refs` (`0` REST API quota). Skips `ls-refs` entirely when the leading segment
/// is already a 40-hex commit SHA, and gracefully falls back to first-segment ref on `404`/`403`.
pub fn resolve_git_v2_ref_and_subpath(
    git_host: &str,
    repo_full_path: &str,
    raw_ref_and_path_segments: &[String],
    require_non_empty_subpath: bool,
    line_range: Option<(usize, usize)>,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<ResolvedGitRef, OnlineFetchError> {
    // Handle modern explicit `refs/heads/<branch>/...` or `refs/tags/<tag>/...` URL prefixes.
    let mut explicit_tag_hint = false;
    let segments: &[String] = if raw_ref_and_path_segments.len() >= 3
        && raw_ref_and_path_segments[0] == "refs"
        && (raw_ref_and_path_segments[1] == "heads" || raw_ref_and_path_segments[1] == "tags")
    {
        explicit_tag_hint = raw_ref_and_path_segments[1] == "tags";
        &raw_ref_and_path_segments[2..]
    } else {
        raw_ref_and_path_segments
    };

    if let Some(first) = segments.first() {
        if is_hex_sha40(first) {
            let sha = first.to_ascii_lowercase();
            return Ok(ResolvedGitRef {
                ref_name: sha.clone(),
                commit_sha: Some(sha),
                subpath: segments[1..].join("/"),
                line_range,
                is_tag: false,
            });
        }
    }

    let seg0_hint = segments.first().map(|s| s.as_str());
    let repo_trimmed = repo_full_path.trim_matches('/').trim_end_matches(".git");
    let upload_pack_url = format!("https://{git_host}/{repo_trimmed}.git/git-upload-pack");

    let mut headers = vec![
        ("Git-Protocol".to_string(), "version=2".to_string()),
        (
            "Content-Type".to_string(),
            "application/x-git-upload-pack-request".to_string(),
        ),
        (
            "Accept".to_string(),
            "application/x-git-upload-pack-result".to_string(),
        ),
        ("User-Agent".to_string(), USER_AGENT.to_string()),
    ];
    if git_host == "github.com" {
        if let Some(pat) = cfg
            .github_token
            .as_deref()
            .map(str::trim)
            .filter(|s| !s.is_empty())
        {
            let basic = base64_encode(format!("x-access-token:{pat}").as_bytes());
            headers.push(("Authorization".to_string(), format!("Basic {basic}")));
        }
    }

    let req = OutboundHttpRequest {
        url: upload_pack_url,
        method: "POST".to_string(),
        headers,
        body: Some(build_git_v2_ls_refs_request_body(seg0_hint)),
        max_bytes: 2 * 1024 * 1024,
        timeout_secs: 12,
        follow_redirects: true,
    };

    let fallback_resolution = || -> ResolvedGitRef {
        if segments.is_empty() {
            ResolvedGitRef {
                ref_name: "HEAD".to_string(),
                commit_sha: None,
                subpath: String::new(),
                line_range,
                is_tag: explicit_tag_hint,
            }
        } else {
            ResolvedGitRef {
                ref_name: segments[0].clone(),
                commit_sha: None,
                subpath: segments[1..].join("/"),
                line_range,
                is_tag: explicit_tag_hint,
            }
        }
    };

    let resp = match fetcher.execute(&req, cfg) {
        Ok(r) => r,
        Err(
            e @ (OnlineFetchError::OfflineAirGapped(_)
            | OnlineFetchError::SecurityBlocked(_)
            | OnlineFetchError::DomainApprovalRequired { .. }
            | OnlineFetchError::TaintConfirmationRequired { .. }
            | OnlineFetchError::ChallengeOrRateLimited { .. }),
        ) => return Err(e),
        Err(OnlineFetchError::TransportError(_)) => return Ok(fallback_resolution()),
        Err(e) => return Err(e),
    };

    if resp.status != 200 {
        return Ok(fallback_resolution());
    }

    let refs = parse_git_v2_ls_refs_response(&resp.body);
    if refs.is_empty() {
        return Ok(fallback_resolution());
    }

    if segments.is_empty() {
        if let Some(head) = refs.iter().find(|r| r.refname == "HEAD") {
            let branch = head
                .symref_target
                .as_deref()
                .and_then(|t| t.strip_prefix("refs/heads/"))
                .unwrap_or("HEAD")
                .to_string();
            return Ok(ResolvedGitRef {
                ref_name: branch,
                commit_sha: Some(head.commit_oid().to_string()),
                subpath: String::new(),
                line_range,
                is_tag: false,
            });
        }
        return Ok(fallback_resolution());
    }

    let max_k = if require_non_empty_subpath {
        segments.len().saturating_sub(1)
    } else {
        segments.len()
    };

    for k in (1..=max_k).rev() {
        let candidate = segments[..k].join("/");
        let head_ref = format!("refs/heads/{candidate}");
        if let Some(matched) = refs.iter().find(|r| r.refname == head_ref) {
            return Ok(ResolvedGitRef {
                ref_name: candidate,
                commit_sha: Some(matched.commit_oid().to_string()),
                subpath: segments[k..].join("/"),
                line_range,
                is_tag: false,
            });
        }
        let tag_ref = format!("refs/tags/{candidate}");
        if let Some(matched) = refs.iter().find(|r| r.refname == tag_ref) {
            return Ok(ResolvedGitRef {
                ref_name: candidate,
                commit_sha: Some(matched.commit_oid().to_string()),
                subpath: segments[k..].join("/"),
                line_range,
                is_tag: true,
            });
        }
    }

    Ok(fallback_resolution())
}

// ============================================================================
// 2. Binary / Size / Noise Guards & Line-Slice Formatter
// ============================================================================

fn is_lockfile_path(path: &str) -> bool {
    let fname = path.rsplit('/').next().unwrap_or(path);
    matches!(
        fname,
        "Cargo.lock"
            | "package-lock.json"
            | "pnpm-lock.yaml"
            | "yarn.lock"
            | "uv.lock"
            | "poetry.lock"
            | "Gemfile.lock"
            | "composer.lock"
            | "go.sum"
            | "Podfile.lock"
            | "flake.lock"
            | "bun.lock"
            | "bun.lockb"
    )
}

fn is_hf_denylisted_weight_or_archive(path: &str) -> bool {
    let low = path.trim().to_ascii_lowercase();
    let multi_exts = [".tar.gz", ".tar.zst", ".tar.xz", ".tar.bz2"];
    if multi_exts.iter().any(|s| low.ends_with(s)) {
        return true;
    }
    let Some((_, ext)) = low.rsplit_once('.') else {
        return false;
    };
    matches!(
        ext,
        "safetensors"
            | "gguf"
            | "ggml"
            | "bin"
            | "pt"
            | "pth"
            | "ckpt"
            | "onnx"
            | "parquet"
            | "arrow"
            | "feather"
            | "h5"
            | "hdf5"
            | "tflite"
            | "ot"
            | "msgpack"
            | "tar"
            | "tgz"
            | "zip"
            | "7z"
            | "gz"
            | "zst"
            | "xz"
    )
}

fn is_allowed_repo_source_path(rel_path: &str) -> bool {
    let Some(clean) = staging::normalize_and_validate_repo_rel_path(rel_path) else {
        return false;
    };
    if is_lockfile_path(&clean) || is_hf_denylisted_weight_or_archive(&clean) {
        return false;
    }
    let low = clean.to_ascii_lowercase();
    for seg in low.split('/') {
        if matches!(
            seg,
            "node_modules" | "__pycache__" | ".pytest_cache" | ".mypy_cache" | "dist" | ".next"
        ) {
            return false;
        }
    }
    if low.ends_with(".min.js") || low.ends_with(".min.css") || low.ends_with(".map") {
        return false;
    }
    if let Some((_, ext)) = low.rsplit_once('.') {
        if matches!(
            ext,
            "png"
                | "jpg"
                | "jpeg"
                | "gif"
                | "webp"
                | "ico"
                | "bmp"
                | "pdf"
                | "mp3"
                | "mp4"
                | "mov"
                | "wav"
                | "woff"
                | "woff2"
                | "ttf"
                | "otf"
                | "so"
                | "dylib"
                | "dll"
                | "exe"
                | "o"
                | "a"
                | "rlib"
                | "pyc"
                | "pyo"
                | "class"
                | "wasm"
                | "jar"
                | "whl"
        ) {
            return false;
        }
    }
    true
}

fn has_binary_magic_or_nul(bytes: &[u8]) -> Option<&'static str> {
    if bytes.len() >= 4 {
        let m4 = &bytes[..4];
        if m4 == b"\x7fELF" {
            return Some("ELF executable/shared-object binary");
        }
        if matches!(
            m4,
            [0xFE, 0xED, 0xFA, 0xCE]
                | [0xCE, 0xFA, 0xED, 0xFE]
                | [0xFE, 0xED, 0xFA, 0xCF]
                | [0xCF, 0xFA, 0xED, 0xFE]
                | [0xCA, 0xFE, 0xBA, 0xBE]
                | [0xBE, 0xBA, 0xFE, 0xCA]
                | [0xCA, 0xFE, 0xBA, 0xBF]
                | [0xBF, 0xBA, 0xFE, 0xCA]
        ) {
            return Some("Mach-O / Universal binary");
        }
        if m4 == b"\0asm" {
            return Some("WebAssembly binary");
        }
        if m4 == b"PK\x03\x04" {
            return Some("ZIP archive binary");
        }
        if m4 == b"%PDF" {
            return Some("PDF binary");
        }
    }
    if bytes.contains(&0u8) {
        return Some("NUL-byte binary payload");
    }
    None
}

fn validate_text_blob(
    bytes: &[u8],
    max_bytes: usize,
    source_url: &str,
) -> Result<String, OnlineFetchError> {
    if bytes.len() > max_bytes {
        return Err(OnlineFetchError::SizeOrBinaryBlocked {
            url: source_url.to_string(),
            bytes: bytes.len() as u64,
            reason: format!(
                "Payload size ({} bytes) exceeds single-file cap ({max_bytes} bytes)",
                bytes.len()
            ),
        });
    }
    if let Some(kind) = has_binary_magic_or_nul(bytes) {
        return Err(OnlineFetchError::SizeOrBinaryBlocked {
            url: source_url.to_string(),
            bytes: bytes.len() as u64,
            reason: format!("Binary file rejected ({kind})"),
        });
    }
    std::str::from_utf8(bytes)
        .map(|s| s.to_string())
        .map_err(|_| OnlineFetchError::SizeOrBinaryBlocked {
            url: source_url.to_string(),
            bytes: bytes.len() as u64,
            reason: "Non-UTF-8 binary payload rejected".to_string(),
        })
}

fn format_line_numbered_focus_slice(
    rel_path: &str,
    content: &str,
    start_line: usize,
    end_line: usize,
) -> String {
    let lines: Vec<&str> = content.lines().collect();
    if lines.is_empty() {
        return String::new();
    }
    let total = lines.len();
    let s = start_line.clamp(1, total);
    let e = end_line.clamp(s, total);
    let width = e.to_string().len().max(3);

    let ext = rel_path.rsplit('.').next().unwrap_or("").to_ascii_lowercase();
    let lang = match ext.as_str() {
        "rs" => "rust",
        "py" => "python",
        "cu" | "cuh" => "cuda",
        "c" | "h" => "c",
        "cc" | "cpp" | "hpp" => "cpp",
        "swift" => "swift",
        "metal" => "metal",
        "json" => "json",
        "toml" => "toml",
        "yaml" | "yml" => "yaml",
        _ => "",
    };

    let mut slice_body = String::new();
    let mut truncated_at: Option<usize> = None;
    for line_no in s..=e {
        let formatted = format!("{:>width$} | {}\n", line_no, lines[line_no - 1], width = width);
        if slice_body.len() + formatted.len() > 2_400 {
            truncated_at = Some(line_no);
            break;
        }
        slice_body.push_str(&formatted);
    }
    let fence = if slice_body.contains("```") {
        "````"
    } else {
        "```"
    };
    let mut out = format!(
        "## Focused Lines L{s}–L{e} of `{rel_path}` (of {total} total lines)\n\
         {fence}{lang}\n\
         {slice_body}{fence}"
    );
    if let Some(stopped) = truncated_at {
        out.push_str(&format!(
            "\n*(Inline focus preview capped at L{}; remaining lines up to L{e} are staged in the local cached file.)*",
            stopped.saturating_sub(1)
        ));
    }
    out
}

fn parse_header_u64(resp: &OutboundHttpResponse, name: &str) -> Option<u64> {
    resp.header(name)?.trim().parse::<u64>().ok()
}

fn is_captcha_or_antibot_response(resp: &OutboundHttpResponse) -> bool {
    if resp.header("cf-mitigated").is_some() {
        return true;
    }
    let sample_len = resp.body.len().min(4096);
    let low = String::from_utf8_lossy(&resp.body[..sample_len]).to_ascii_lowercase();
    low.contains("cf-turnstile")
        || low.contains("challenges.cloudflare.com")
        || low.contains("g-recaptcha")
        || low.contains("hcaptcha")
        || low.contains("verify you are human")
}

// ============================================================================
// 3. Lexicographical Tar Early-Abort Scanner (via `super::archive_tar`)
// ============================================================================

fn parse_tar_size(field: &[u8]) -> Result<usize, OnlineFetchError> {
    if field.is_empty() {
        return Ok(0);
    }
    if (field[0] & 0x80) != 0 {
        // GNU base-256 binary size encoding
        let mut acc: u64 = (field[0] & 0x7F) as u64;
        for &b in &field[1..] {
            acc = acc.checked_shl(8).and_then(|v| v.checked_add(b as u64)).ok_or_else(|| {
                OnlineFetchError::ExtractionError("Tar entry size overflow".to_string())
            })?;
        }
        return usize::try_from(acc)
            .map_err(|_| OnlineFetchError::ExtractionError("Tar entry size overflow".to_string()));
    }
    let nul_end = field.iter().position(|&b| b == 0).unwrap_or(field.len());
    let s = std::str::from_utf8(&field[..nul_end])
        .unwrap_or("")
        .trim_matches(|c: char| c.is_ascii_whitespace() || c == '\0');
    if s.is_empty() {
        return Ok(0);
    }
    usize::from_str_radix(s, 8)
        .map_err(|_| OnlineFetchError::ExtractionError(format!("Invalid octal tar size: '{s}'")))
}

fn parse_ustar_header_path(header: &[u8; 512]) -> String {
    fn read_cstr(slice: &[u8]) -> &str {
        let end = slice.iter().position(|&b| b == 0).unwrap_or(slice.len());
        std::str::from_utf8(&slice[..end]).unwrap_or("")
    }
    let name = read_cstr(&header[0..100]);
    let prefix = if archive_tar::is_tar_ustar_header(header) {
        read_cstr(&header[345..500])
    } else {
        ""
    };
    if prefix.is_empty() {
        name.to_string()
    } else {
        format!("{prefix}/{name}")
    }
}

fn parse_pax_records(buf: &[u8]) -> (Option<String>, Option<usize>, Option<String>) {
    let mut path_override = None;
    let mut size_override = None;
    let mut commit_comment = None;
    let text = String::from_utf8_lossy(buf);
    let mut rest = text.as_ref();
    while !rest.is_empty() {
        let Some((len_str, _after_space)) = rest.split_once(' ') else {
            break;
        };
        let Ok(rec_len) = len_str.trim().parse::<usize>() else {
            break;
        };
        if rec_len <= len_str.len() + 1 || rec_len > rest.len() {
            break;
        }
        let record_body = &rest[len_str.len() + 1..rec_len];
        let record_trimmed = record_body.trim_end_matches('\n');
        if let Some((k, v)) = record_trimmed.split_once('=') {
            match k {
                "path" => path_override = Some(v.to_string()),
                "size" => size_override = v.parse::<usize>().ok(),
                "comment" if is_hex_sha40(v.trim()) => {
                    commit_comment = Some(v.trim().to_ascii_lowercase());
                }
                _ => {}
            }
        }
        rest = &rest[rec_len..];
    }
    (path_override, size_override, commit_comment)
}

fn build_clean_ustar_header(rel_path: &str, size: usize) -> Result<[u8; 512], OnlineFetchError> {
    let mut hdr = [0u8; 512];
    let bytes = rel_path.as_bytes();
    if bytes.len() <= 100 {
        hdr[..bytes.len()].copy_from_slice(bytes);
    } else if let Some(split_pos) = rel_path.rfind('/') {
        let (prefix, rest) = (&bytes[..split_pos], &bytes[split_pos + 1..]);
        if prefix.len() <= 155 && rest.len() <= 100 {
            hdr[..rest.len()].copy_from_slice(rest);
            hdr[345..345 + prefix.len()].copy_from_slice(prefix);
        } else {
            return Err(OnlineFetchError::ExtractionError(format!(
                "Tar entry path exceeds POSIX ustar 255-byte limit: {rel_path}"
            )));
        }
    } else {
        return Err(OnlineFetchError::ExtractionError(format!(
            "Tar entry filename exceeds 100 bytes: {rel_path}"
        )));
    }

    hdr[100..108].copy_from_slice(b"0000444\0");
    hdr[108..116].copy_from_slice(b"0000000\0");
    hdr[116..124].copy_from_slice(b"0000000\0");
    let size_octal = format!("{:011o}\0", size);
    hdr[124..136].copy_from_slice(size_octal.as_bytes());
    hdr[136..148].copy_from_slice(b"00000000000\0");
    hdr[148..156].fill(b' ');
    hdr[156] = b'0';
    hdr[257..263].copy_from_slice(b"ustar\0");
    hdr[263..265].copy_from_slice(b"00");

    let checksum: u32 = hdr.iter().map(|&b| b as u32).sum();
    let chk_octal = format!("{:06o}\0 ", checksum);
    hdr[148..156].copy_from_slice(chk_octal.as_bytes());
    Ok(hdr)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum TarChunkMode {
    Header,
    SkipPayload(usize),
    KeepPayload { remaining_data: usize, pad_after: usize },
    ReadMeta { kind: u8, remaining_data: usize, pad_after: usize },
    Done,
}

struct StreamingTarSubdirScanner {
    subdir: Option<String>,
    source_url: String,
    max_single_file_bytes: usize,
    max_tree_bytes: usize,
    header_buf: [u8; 512],
    header_fill: usize,
    mode: TarChunkMode,
    meta_buf: Vec<u8>,
    pax_path: Option<String>,
    pax_size: Option<usize>,
    gnu_long_path: Option<String>,
    pub detected_commit_sha: Option<String>,
    pub top_prefix: Option<String>,
    pub seen_subdir_entry: bool,
    pub aborted_early: bool,
    pub matched_files: usize,
    pub matched_bytes: usize,
    pub kept_tar_bytes: Vec<u8>,
}

impl StreamingTarSubdirScanner {
    fn new(
        subdir: Option<&str>,
        source_url: &str,
        max_single_file_bytes: usize,
        max_tree_bytes: usize,
    ) -> Self {
        let clean_sub = subdir
            .map(|s| s.trim_matches('/').to_string())
            .filter(|s| !s.is_empty());
        Self {
            subdir: clean_sub,
            source_url: source_url.to_string(),
            max_single_file_bytes,
            max_tree_bytes,
            header_buf: [0u8; 512],
            header_fill: 0,
            mode: TarChunkMode::Header,
            meta_buf: Vec::new(),
            pax_path: None,
            pax_size: None,
            gnu_long_path: None,
            detected_commit_sha: None,
            top_prefix: None,
            seen_subdir_entry: false,
            aborted_early: false,
            matched_files: 0,
            matched_bytes: 0,
            kept_tar_bytes: Vec::new(),
        }
    }

    fn feed_tar_bytes(&mut self, mut slice: &[u8]) -> Result<bool, OnlineFetchError> {
        while !slice.is_empty() {
            match self.mode {
                TarChunkMode::Done => return Ok(true),
                TarChunkMode::SkipPayload(rem) => {
                    let take = rem.min(slice.len());
                    slice = &slice[take..];
                    let left = rem - take;
                    self.mode = if left == 0 {
                        TarChunkMode::Header
                    } else {
                        TarChunkMode::SkipPayload(left)
                    };
                }
                TarChunkMode::KeepPayload {
                    remaining_data,
                    pad_after,
                } => {
                    if remaining_data > 0 {
                        let take = remaining_data.min(slice.len());
                        self.kept_tar_bytes.extend_from_slice(&slice[..take]);
                        slice = &slice[take..];
                        let left_data = remaining_data - take;
                        if left_data == 0 {
                            if pad_after > 0 {
                                let start_len = self.kept_tar_bytes.len();
                                self.kept_tar_bytes.resize(start_len + pad_after, 0u8);
                                self.mode = TarChunkMode::SkipPayload(pad_after);
                            } else {
                                self.mode = TarChunkMode::Header;
                            }
                        } else {
                            self.mode = TarChunkMode::KeepPayload {
                                remaining_data: left_data,
                                pad_after,
                            };
                        }
                    } else if pad_after > 0 {
                        self.mode = TarChunkMode::SkipPayload(pad_after);
                    } else {
                        self.mode = TarChunkMode::Header;
                    }
                }
                TarChunkMode::ReadMeta {
                    kind,
                    remaining_data,
                    pad_after,
                } => {
                    if remaining_data > 0 {
                        let take = remaining_data.min(slice.len());
                        if self.meta_buf.len() + take <= 64 * 1024 {
                            self.meta_buf.extend_from_slice(&slice[..take]);
                        }
                        slice = &slice[take..];
                        let left = remaining_data - take;
                        if left == 0 {
                            self.finish_meta_block(kind);
                            self.mode = if pad_after > 0 {
                                TarChunkMode::SkipPayload(pad_after)
                            } else {
                                TarChunkMode::Header
                            };
                        } else {
                            self.mode = TarChunkMode::ReadMeta {
                                kind,
                                remaining_data: left,
                                pad_after,
                            };
                        }
                    } else {
                        self.finish_meta_block(kind);
                        self.mode = if pad_after > 0 {
                            TarChunkMode::SkipPayload(pad_after)
                        } else {
                            TarChunkMode::Header
                        };
                    }
                }
                TarChunkMode::Header => {
                    let needed = 512 - self.header_fill;
                    let take = needed.min(slice.len());
                    self.header_buf[self.header_fill..self.header_fill + take]
                        .copy_from_slice(&slice[..take]);
                    self.header_fill += take;
                    slice = &slice[take..];
                    if self.header_fill == 512 {
                        self.header_fill = 0;
                        if !self.inspect_completed_header()? {
                            return Ok(false);
                        }
                    }
                }
            }
        }
        Ok(true)
    }

    fn finish_meta_block(&mut self, kind: u8) {
        match kind {
            b'g' => {
                let (_, _, comment) = parse_pax_records(&self.meta_buf);
                if self.detected_commit_sha.is_none() {
                    self.detected_commit_sha = comment;
                }
            }
            b'x' => {
                let (p, s, _) = parse_pax_records(&self.meta_buf);
                if p.is_some() {
                    self.pax_path = p;
                }
                if s.is_some() {
                    self.pax_size = s;
                }
            }
            b'L' => {
                let end = self
                    .meta_buf
                    .iter()
                    .position(|&b| b == 0)
                    .unwrap_or(self.meta_buf.len());
                if let Ok(s) = std::str::from_utf8(&self.meta_buf[..end]) {
                    self.gnu_long_path = Some(s.to_string());
                }
            }
            _ => {}
        }
        self.meta_buf.clear();
    }

    fn inspect_completed_header(&mut self) -> Result<bool, OnlineFetchError> {
        if self.header_buf.iter().all(|&b| b == 0) {
            self.mode = TarChunkMode::Done;
            return Ok(true);
        }

        let header_size = parse_tar_size(&self.header_buf[124..136])?;
        let typeflag = self.header_buf[156];
        let pad_for = |sz: usize| -> usize { (512 - (sz % 512)) % 512 };

        if matches!(typeflag, b'g' | b'x' | b'L') {
            self.meta_buf.clear();
            let pad = pad_for(header_size);
            if header_size == 0 {
                self.mode = TarChunkMode::Header;
            } else {
                self.mode = TarChunkMode::ReadMeta {
                    kind: typeflag,
                    remaining_data: header_size,
                    pad_after: pad,
                };
            }
            return Ok(true);
        }

        let raw_path = self
            .pax_path
            .take()
            .or_else(|| self.gnu_long_path.take())
            .unwrap_or_else(|| parse_ustar_header_path(&self.header_buf));
        let entry_size = self.pax_size.take().unwrap_or(header_size);
        let pad = pad_for(entry_size);
        let total_padded = entry_size.saturating_add(pad);

        // Reject symlinks/hardlinks/FIFOs/devices inside matching path if encountered; skip outside.
        let is_regular = typeflag == 0 || typeflag == b'0';
        let is_dir = typeflag == b'5';

        let clean_entry = raw_path.trim_start_matches("./").trim_start_matches('/');
        if clean_entry.is_empty() {
            self.mode = TarChunkMode::SkipPayload(total_padded);
            return Ok(true);
        }

        if self.top_prefix.is_none() && (clean_entry.contains('/') || is_dir) {
            if let Some(first_seg) = clean_entry.split('/').next().filter(|s| !s.is_empty()) {
                self.top_prefix = Some(first_seg.to_string());
            }
        }

        let tp = self.top_prefix.as_deref().unwrap_or("");

        if let Some(sub) = self.subdir.clone() {
            let target_no_slash = if tp.is_empty() {
                sub.clone()
            } else {
                format!("{tp}/{sub}")
            };
            let target_prefix = format!("{target_no_slash}/");

            let entry_no_trailing = clean_entry.trim_end_matches('/');
            let inside_subdir =
                entry_no_trailing == target_no_slash || clean_entry.starts_with(&target_prefix);

            if inside_subdir {
                self.seen_subdir_entry = true;
            } else {
                // `git archive` emits paths in strict unsigned-byte order. Once `{top}/{subdir}/`
                // has been entered and an entry lexicographically `>` `{top}/{subdir}/` arrives,
                // zero further entries under `{subdir}` can exist — abort stream immediately!
                if self.seen_subdir_entry && clean_entry.as_bytes() > target_prefix.as_bytes() {
                    self.aborted_early = true;
                    return Ok(false);
                }
                self.mode = TarChunkMode::SkipPayload(total_padded);
                return Ok(true);
            }
        }

        if !is_regular {
            self.mode = TarChunkMode::SkipPayload(total_padded);
            return Ok(true);
        }

        let rel_inside_repo = if tp.is_empty() {
            clean_entry
        } else {
            clean_entry
                .strip_prefix(&format!("{tp}/"))
                .unwrap_or(clean_entry)
        };

        if rel_inside_repo.is_empty()
            || !is_allowed_repo_source_path(rel_inside_repo)
            || entry_size > self.max_single_file_bytes
        {
            self.mode = TarChunkMode::SkipPayload(total_padded);
            return Ok(true);
        }

        if self.subdir.is_none() && self.matched_files >= ROOT_TARBALL_MAX_FILES {
            self.aborted_early = true;
            return Ok(false);
        }

        if self.matched_bytes.saturating_add(entry_size) > self.max_tree_bytes {
            return Err(OnlineFetchError::SizeOrBinaryBlocked {
                url: self.source_url.clone(),
                bytes: (self.matched_bytes + entry_size) as u64,
                reason: format!(
                    "Sparse repository tree exceeds configured max_tree_bytes ({} bytes)",
                    self.max_tree_bytes
                ),
            });
        }

        let clean_hdr = build_clean_ustar_header(rel_inside_repo, entry_size)?;
        self.kept_tar_bytes.extend_from_slice(&clean_hdr);
        self.matched_files += 1;
        self.matched_bytes += entry_size;

        if entry_size == 0 {
            self.mode = TarChunkMode::Header;
        } else {
            self.mode = TarChunkMode::KeepPayload {
                remaining_data: entry_size,
                pad_after: pad,
            };
        }
        Ok(true)
    }

    fn finalize_tar_archive(mut self) -> Vec<u8> {
        if !self.kept_tar_bytes.is_empty() {
            let rem = self.kept_tar_bytes.len() % 512;
            if rem != 0 {
                self.kept_tar_bytes.resize(self.kept_tar_bytes.len() + (512 - rem), 0u8);
            }
            self.kept_tar_bytes.extend_from_slice(&[0u8; 1024]);
        }
        self.kept_tar_bytes
    }
}

fn fallback_extract_clean_ustar_regular_entries(
    tar_bytes: &[u8],
    max_total_bytes: usize,
) -> Result<Vec<ExtractedArchiveFile>, OnlineFetchError> {
    let mut out = Vec::new();
    let mut cursor = 0usize;
    let mut total = 0usize;
    while cursor + 512 <= tar_bytes.len() {
        let hdr: &[u8; 512] = tar_bytes[cursor..cursor + 512].try_into().unwrap();
        cursor += 512;
        if hdr.iter().all(|&b| b == 0) {
            break;
        }
        let typeflag = hdr[156];
        if typeflag != 0 && typeflag != b'0' {
            return Err(OnlineFetchError::SecurityBlocked(format!(
                "Non-regular tar entry type '{}' rejected",
                typeflag as char
            )));
        }
        let sz = parse_tar_size(&hdr[124..136])?;
        let padded = ((sz + 511) / 512) * 512;
        if cursor + padded > tar_bytes.len() {
            break;
        }
        total = total.saturating_add(sz);
        if total > max_total_bytes {
            return Err(OnlineFetchError::SizeOrBinaryBlocked {
                url: "archive".to_string(),
                bytes: total as u64,
                reason: "Extracted tar entries exceed max_total_bytes".to_string(),
            });
        }
        let path = parse_ustar_header_path(hdr);
        if let Some(clean_rel) = staging::normalize_and_validate_repo_rel_path(&path) {
            out.push(ExtractedArchiveFile {
                rel_path: clean_rel,
                bytes: tar_bytes[cursor..cursor + sz].to_vec(),
            });
        }
        cursor += padded;
    }
    Ok(out)
}

fn stream_and_extract_sparse_tarball(
    tarball_url: &str,
    headers: Vec<(String, String)>,
    subdir: Option<&str>,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<(Vec<StagedRepoFileInput>, Option<String>, bool), OnlineFetchError> {
    let max_single = effective_max_single_file_bytes(cfg);
    let max_tree = effective_max_tree_bytes(cfg);

    let req = OutboundHttpRequest {
        url: tarball_url.to_string(),
        method: "GET".to_string(),
        headers,
        body: None,
        max_bytes: max_tree.saturating_add(16 * 1024 * 1024),
        timeout_secs: 30,
        follow_redirects: true,
    };

    let mut scanner = StreamingTarSubdirScanner::new(subdir, tarball_url, max_single, max_tree);
    let max_decompressed = max_tree.saturating_mul(4).max(64 * 1024 * 1024);
    let mut inflator: Option<archive_tar::StreamingGunzipDecoder> = None;
    let mut format_detected: Option<bool> = None; // `Some(true)` = gzip, `Some(false)` = raw tar
    let mut sniff_buf: Vec<u8> = Vec::with_capacity(2);

    let (status, _resp_headers) = fetcher.execute_streaming(&req, cfg, &mut |chunk: &[u8]| {
        if chunk.is_empty() {
            return Ok(true);
        }
        if format_detected.is_none() {
            let need = (2 - sniff_buf.len()).min(chunk.len());
            sniff_buf.extend_from_slice(&chunk[..need]);
            if sniff_buf.len() < 2 {
                return Ok(true);
            }
            let is_gzip = sniff_buf[0] == 0x1F && sniff_buf[1] == 0x8B;
            format_detected = Some(is_gzip);
            if is_gzip {
                let mut inf = archive_tar::StreamingGunzipDecoder::new(max_decompressed)?;
                let mut combined = sniff_buf.clone();
                combined.extend_from_slice(&chunk[need..]);
                let cont =
                    inf.feed_with(&combined, |decompressed| scanner.feed_tar_bytes(decompressed))?;
                inflator = Some(inf);
                return Ok(cont);
            } else {
                let mut combined = sniff_buf.clone();
                combined.extend_from_slice(&chunk[need..]);
                return scanner.feed_tar_bytes(&combined);
            }
        }

        if format_detected == Some(true) {
            let inf = inflator.as_mut().expect("inflator initialized");
            inf.feed_with(chunk, |decompressed| scanner.feed_tar_bytes(decompressed))
        } else {
            scanner.feed_tar_bytes(chunk)
        }
    })?;

    if status >= 400 {
        return Err(OnlineFetchError::TransportError(format!(
            "HTTP {status} streaming archive {tarball_url}"
        )));
    }

    let detected_sha = scanner.detected_commit_sha.clone();
    let aborted_early = scanner.aborted_early;
    let top_prefix = scanner.top_prefix.clone();
    let clean_tar_bytes = scanner.finalize_tar_archive();
    if clean_tar_bytes.is_empty() {
        return Ok((Vec::new(), detected_sha, aborted_early));
    }

    let mut extracted = archive_tar::extract_safe_tar_entries(&clean_tar_bytes, max_tree)?;
    if extracted.is_empty() && clean_tar_bytes.len() > 1024 {
        extracted = fallback_extract_clean_ustar_regular_entries(&clean_tar_bytes, max_tree)?;
    }

    let mut files = Vec::with_capacity(extracted.len());
    for entry in extracted {
        let stripped = match top_prefix.as_deref() {
            Some(tp) if !tp.is_empty() => entry
                .rel_path
                .strip_prefix(&format!("{tp}/"))
                .unwrap_or(&entry.rel_path)
                .to_string(),
            _ => entry.rel_path,
        };
        if !is_allowed_repo_source_path(&stripped) {
            continue;
        }
        if let Ok(text) = validate_text_blob(&entry.bytes, max_single, tarball_url) {
            files.push(StagedRepoFileInput {
                rel_path: stripped,
                content: text,
            });
        }
    }
    Ok((files, detected_sha, aborted_early))
}

// ============================================================================
// 4. GitHub, GitLab & Codeberg Fetch Implementations
// ============================================================================

fn github_api_headers(cfg: &OnlineResearchConfig, accept: &str) -> Vec<(String, String)> {
    let mut headers = vec![
        ("Accept".to_string(), accept.to_string()),
        ("X-GitHub-Api-Version".to_string(), "2022-11-28".to_string()),
        ("User-Agent".to_string(), USER_AGENT.to_string()),
    ];
    if let Some(pat) = cfg
        .github_token
        .as_deref()
        .map(str::trim)
        .filter(|s| !s.is_empty())
    {
        headers.push(("Authorization".to_string(), format!("Bearer {pat}")));
    }
    headers
}

fn fetch_github_single_blob_bytes(
    owner: &str,
    repo: &str,
    sha_or_ref: &str,
    rel_path: &str,
    max_bytes: usize,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<(Vec<u8>, String), OnlineFetchError> {
    let enc_path = url_encode_repo_path(rel_path);
    let enc_ref = url_encode_component(sha_or_ref);

    if cfg
        .github_token
        .as_deref()
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .is_some()
    {
        let api_url =
            format!("https://api.github.com/repos/{owner}/{repo}/contents/{enc_path}?ref={enc_ref}");
        let req = OutboundHttpRequest {
            url: api_url.clone(),
            method: "GET".to_string(),
            headers: github_api_headers(cfg, "application/vnd.github.raw+json"),
            body: None,
            max_bytes,
            timeout_secs: 20,
            follow_redirects: true,
        };
        let resp = fetcher.execute(&req, cfg)?;
        if is_captcha_or_antibot_response(&resp) {
            return Err(OnlineFetchError::ChallengeOrRateLimited {
                domain: "api.github.com".to_string(),
                status: resp.status,
                detail: "Anti-bot challenge detected on api.github.com".to_string(),
            });
        }
        if let Some(cl) = parse_header_u64(&resp, "content-length") {
            if cl > max_bytes as u64 {
                return Err(OnlineFetchError::SizeOrBinaryBlocked {
                    url: api_url,
                    bytes: cl,
                    reason: format!("Content-Length ({cl}) exceeds single-file cap ({max_bytes})"),
                });
            }
        }
        if resp.status == 200 {
            return Ok((resp.body, api_url));
        }
        if resp.status != 403 && resp.status != 404 {
            return Err(OnlineFetchError::TransportError(format!(
                "GitHub contents API returned HTTP {} for {owner}/{repo}/{rel_path}",
                resp.status
            )));
        }
    }

    // Zero-key public fetch via `raw.githubusercontent.com` (never send PAT to Fastly raw CDN).
    let raw_url = format!("https://raw.githubusercontent.com/{owner}/{repo}/{enc_ref}/{enc_path}");
    let req = OutboundHttpRequest {
        url: raw_url.clone(),
        method: "GET".to_string(),
        headers: vec![("User-Agent".to_string(), USER_AGENT.to_string())],
        body: None,
        max_bytes,
        timeout_secs: 20,
        follow_redirects: true,
    };
    let resp = fetcher.execute(&req, cfg)?;
    if let Some(cl) = parse_header_u64(&resp, "content-length") {
        if cl > max_bytes as u64 {
            return Err(OnlineFetchError::SizeOrBinaryBlocked {
                url: raw_url,
                bytes: cl,
                reason: format!("Content-Length ({cl}) exceeds single-file cap ({max_bytes})"),
            });
        }
    }
    if resp.status >= 400 {
        return Err(OnlineFetchError::TransportError(format!(
            "HTTP {} fetching {}",
            resp.status, raw_url
        )));
    }
    Ok((resp.body, raw_url))
}

#[allow(clippy::too_many_arguments)]
fn fetch_single_forge_file(
    host: &str,
    owner: &str,
    repo: &str,
    full_repo_path: &str,
    resolved: &ResolvedGitRef,
    canonical_url: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let clean_rel = staging::normalize_and_validate_repo_rel_path(&resolved.subpath).ok_or_else(
        || {
            OnlineFetchError::SecurityBlocked(format!(
                "Rejected unsafe or empty repository file path '{}' in {canonical_url}",
                resolved.subpath
            ))
        },
    )?;
    if is_hf_denylisted_weight_or_archive(&clean_rel) {
        return Err(OnlineFetchError::SizeOrBinaryBlocked {
            url: canonical_url.to_string(),
            bytes: 0,
            reason: format!("Blocked binary weight/archive file extension: '{clean_rel}'"),
        });
    }

    let max_single = effective_max_single_file_bytes(cfg);
    let sha_or_ref = resolved
        .commit_sha
        .as_deref()
        .unwrap_or(resolved.ref_name.as_str());

    let (raw_bytes, fetched_url) = match host {
        "github.com" | "raw.githubusercontent.com" => fetch_github_single_blob_bytes(
            owner,
            repo,
            sha_or_ref,
            &clean_rel,
            max_single,
            cfg,
            fetcher,
        )?,
        "gitlab.com" => {
            let enc_ref = url_encode_component(sha_or_ref);
            let enc_path = url_encode_repo_path(&clean_rel);
            let url = format!("https://gitlab.com/{full_repo_path}/-/raw/{enc_ref}/{enc_path}");
            let mut req = OutboundHttpRequest::get(url.clone());
            req.max_bytes = max_single;
            req.headers
                .push(("User-Agent".to_string(), USER_AGENT.to_string()));
            let resp = fetcher.execute(&req, cfg)?;
            if resp.status >= 400 {
                return Err(OnlineFetchError::TransportError(format!(
                    "HTTP {} fetching GitLab raw file {}",
                    resp.status, url
                )));
            }
            (resp.body, url)
        }
        "codeberg.org" => {
            let enc_path = url_encode_repo_path(&clean_rel);
            let url = if let Some(ref sha) = resolved.commit_sha {
                format!("https://codeberg.org/{owner}/{repo}/raw/commit/{sha}/{enc_path}")
            } else if resolved.is_tag {
                let enc_ref = url_encode_component(&resolved.ref_name);
                format!("https://codeberg.org/{owner}/{repo}/raw/tag/{enc_ref}/{enc_path}")
            } else {
                let enc_ref = url_encode_component(&resolved.ref_name);
                format!("https://codeberg.org/{owner}/{repo}/raw/branch/{enc_ref}/{enc_path}")
            };
            let mut req = OutboundHttpRequest::get(url.clone());
            req.max_bytes = max_single;
            req.headers
                .push(("User-Agent".to_string(), USER_AGENT.to_string()));
            let resp = fetcher.execute(&req, cfg)?;
            if resp.status >= 400 {
                return Err(OnlineFetchError::TransportError(format!(
                    "HTTP {} fetching Codeberg raw file {}",
                    resp.status, url
                )));
            }
            (resp.body, url)
        }
        other => {
            return Err(OnlineFetchError::ExtractionError(format!(
                "Unsupported repository host: {other}"
            )))
        }
    };

    let content = validate_text_blob(&raw_bytes, max_single, &fetched_url)?;
    let focus_slice = resolved
        .line_range
        .map(|(s, e)| format_line_numbered_focus_slice(&clean_rel, &content, s, e));

    let pinned_label = resolved
        .commit_sha
        .as_deref()
        .unwrap_or(resolved.ref_name.as_str());
    let stage_host = if host == "raw.githubusercontent.com" {
        "github.com"
    } else {
        host
    };
    let tier_badge = match resolved.line_range {
        Some((s, e)) => format!("{stage_host} · Single File (L{s}–L{e})"),
        None => format!("{stage_host} · Single File"),
    };
    let title = format!("{full_repo_path}/{clean_rel}");

    staging::stage_repo_files_and_build_envelope(
        RepoStagingRequest {
            host: stage_host,
            owner,
            repo,
            commit_sha: pinned_label,
            canonical_url,
            tier_badge: &tier_badge,
            title: &title,
            subdir: None,
            focus_preview: focus_slice.as_deref(),
            files: vec![StagedRepoFileInput {
                rel_path: clean_rel,
                content,
            }],
        },
        cfg,
    )
}

#[derive(Debug)]
enum StrategyAOutcome {
    Fetched(Vec<StagedRepoFileInput>),
    SingleBlobAtPath(String),
    FallbackToStrategyB,
}

fn try_github_strategy_a_tree_walk(
    owner: &str,
    repo: &str,
    sha_or_ref: &str,
    subdir: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<StrategyAOutcome, OnlineFetchError> {
    let max_single = effective_max_single_file_bytes(cfg);
    let max_tree = effective_max_tree_bytes(cfg);
    let strategy_a_byte_cap = max_tree.min(STRATEGY_A_MAX_BYTES);

    let execute_tree_get = |url: String| -> Result<Option<Value>, OnlineFetchError> {
        let req = OutboundHttpRequest {
            url: url.clone(),
            method: "GET".to_string(),
            headers: github_api_headers(cfg, "application/vnd.github+json"),
            body: None,
            max_bytes: 8 * 1024 * 1024,
            timeout_secs: 15,
            follow_redirects: true,
        };
        match fetcher.execute(&req, cfg) {
            Ok(resp) => {
                if is_captcha_or_antibot_response(&resp) {
                    return Err(OnlineFetchError::ChallengeOrRateLimited {
                        domain: "api.github.com".to_string(),
                        status: resp.status,
                        detail: "Anti-bot challenge detected on api.github.com".to_string(),
                    });
                }
                if resp.status == 403 || resp.status == 429 {
                    // GitHub REST API quota exhausted -> switch to zero-quota Strategy B `codeload`!
                    return Ok(None);
                }
                if resp.status >= 400 {
                    return Err(OnlineFetchError::TransportError(format!(
                        "GitHub Tree API HTTP {} for {url}",
                        resp.status
                    )));
                }
                let parsed: Value = serde_json::from_slice(&resp.body).map_err(|e| {
                    OnlineFetchError::ExtractionError(format!(
                        "Invalid JSON from GitHub Tree API ({url}): {e}"
                    ))
                })?;
                Ok(Some(parsed))
            }
            Err(OnlineFetchError::ChallengeOrRateLimited { domain, status, detail })
                if domain == "api.github.com"
                    && (status == 403 || status == 429)
                    && !detail.to_ascii_lowercase().contains("captcha")
                    && !detail.to_ascii_lowercase().contains("turnstile") =>
            {
                Ok(None)
            }
            Err(e) => Err(e),
        }
    };

    // Walk segment-by-segment from `{sha_or_ref}` to `{subdir}`'s `tree_sha` without `?recursive=1`
    // on the monorepo root!
    let mut current_tree_id = sha_or_ref.to_string();
    let sub_segments: Vec<&str> = subdir.split('/').filter(|s| !s.is_empty()).collect();

    for (idx, seg) in sub_segments.iter().enumerate() {
        let step_url = format!(
            "https://api.github.com/repos/{owner}/{repo}/git/trees/{}",
            url_encode_component(&current_tree_id)
        );
        let Some(json) = execute_tree_get(step_url)? else {
            return Ok(StrategyAOutcome::FallbackToStrategyB);
        };
        if json.get("truncated").and_then(Value::as_bool) == Some(true) {
            return Ok(StrategyAOutcome::FallbackToStrategyB);
        }
        let entries = json
            .get("tree")
            .and_then(Value::as_array)
            .ok_or_else(|| OnlineFetchError::ExtractionError("Missing `tree` array".to_string()))?;

        let matched = entries
            .iter()
            .find(|item| item.get("path").and_then(Value::as_str) == Some(*seg));
        let Some(entry) = matched else {
            return Err(OnlineFetchError::ExtractionError(format!(
                "Subpath segment '{seg}' not found under '{owner}/{repo}' ({subdir})"
            )));
        };

        let entry_type = entry.get("type").and_then(Value::as_str).unwrap_or("");
        let entry_sha = entry
            .get("sha")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string();
        if entry_type == "blob" && idx + 1 == sub_segments.len() {
            return Ok(StrategyAOutcome::SingleBlobAtPath(sub_segments.join("/")));
        }
        if entry_type != "tree" || entry_sha.is_empty() {
            return Err(OnlineFetchError::ExtractionError(format!(
                "Segment '{seg}' is not a directory tree (type={entry_type})"
            )));
        }
        current_tree_id = entry_sha;
    }

    // Fetch `{target_tree_sha}?recursive=1` scoped strictly to `{subdir}`!
    let rec_url = format!(
        "https://api.github.com/repos/{owner}/{repo}/git/trees/{}?recursive=1",
        url_encode_component(&current_tree_id)
    );
    let Some(rec_json) = execute_tree_get(rec_url)? else {
        return Ok(StrategyAOutcome::FallbackToStrategyB);
    };
    if rec_json.get("truncated").and_then(Value::as_bool) == Some(true) {
        return Ok(StrategyAOutcome::FallbackToStrategyB);
    }

    let entries = rec_json
        .get("tree")
        .and_then(Value::as_array)
        .ok_or_else(|| OnlineFetchError::ExtractionError("Missing recursive `tree` array".to_string()))?;

    let clean_sub = sub_segments.join("/");
    let mut candidate_blobs: Vec<(String, usize)> = Vec::new();
    let mut total_bytes = 0usize;

    for item in entries {
        if item.get("type").and_then(Value::as_str) != Some("blob") {
            continue;
        }
        let mode = item.get("mode").and_then(Value::as_str).unwrap_or("100644");
        if mode == "120000" || mode == "160000" {
            // Skip symlinks and submodule gitlinks.
            continue;
        }
        let Some(rel_in_sub) = item.get("path").and_then(Value::as_str) else {
            continue;
        };
        let repo_rel = if clean_sub.is_empty() {
            rel_in_sub.to_string()
        } else {
            format!("{clean_sub}/{rel_in_sub}")
        };
        if !is_allowed_repo_source_path(&repo_rel) {
            continue;
        }
        let size = item.get("size").and_then(Value::as_u64).unwrap_or(0) as usize;
        if size > max_single {
            continue;
        }
        total_bytes = total_bytes.saturating_add(size);
        candidate_blobs.push((repo_rel, size));
    }

    if candidate_blobs.len() > STRATEGY_A_MAX_FILES || total_bytes > strategy_a_byte_cap {
        return Ok(StrategyAOutcome::FallbackToStrategyB);
    }
    if candidate_blobs.is_empty() {
        return Err(OnlineFetchError::ExtractionError(format!(
            "No stageable source text files matched in {owner}/{repo}/{clean_sub}"
        )));
    }

    let mut staged_inputs = Vec::with_capacity(candidate_blobs.len());
    for (repo_rel, _) in candidate_blobs {
        let (bytes, blob_url) = fetch_github_single_blob_bytes(
            owner,
            repo,
            sha_or_ref,
            &repo_rel,
            max_single,
            cfg,
            fetcher,
        )?;
        if let Ok(text) = validate_text_blob(&bytes, max_single, &blob_url) {
            staged_inputs.push(StagedRepoFileInput {
                rel_path: repo_rel,
                content: text,
            });
        }
    }
    Ok(StrategyAOutcome::Fetched(staged_inputs))
}

#[allow(clippy::too_many_arguments)]
fn fetch_forge_sparse_tree(
    host: &str,
    owner: &str,
    repo: &str,
    full_repo_path: &str,
    resolved: &ResolvedGitRef,
    canonical_url: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let sha_or_ref = resolved
        .commit_sha
        .as_deref()
        .unwrap_or(resolved.ref_name.as_str());
    let clean_sub = resolved.subpath.trim_matches('/').to_string();
    let subdir_opt = if clean_sub.is_empty() {
        None
    } else {
        Some(clean_sub.as_str())
    };

    if host == "github.com" {
        match try_github_strategy_a_tree_walk(owner, repo, sha_or_ref, &clean_sub, cfg, fetcher)? {
            StrategyAOutcome::SingleBlobAtPath(blob_path) => {
                let mut as_blob = resolved.clone();
                as_blob.subpath = blob_path;
                return fetch_single_forge_file(
                    host,
                    owner,
                    repo,
                    full_repo_path,
                    &as_blob,
                    canonical_url,
                    cfg,
                    fetcher,
                );
            }
            StrategyAOutcome::Fetched(files) => {
                let tier = format!("GitHub Tree (Strategy A) · {} files", files.len());
                let title = if clean_sub.is_empty() {
                    format!("{owner}/{repo}")
                } else {
                    format!("{owner}/{repo}/{clean_sub}")
                };
                return staging::stage_repo_files_and_build_envelope(
                    RepoStagingRequest {
                        host,
                        owner,
                        repo,
                        commit_sha: sha_or_ref,
                        canonical_url,
                        tier_badge: &tier,
                        title: &title,
                        subdir: subdir_opt,
                        focus_preview: None,
                        files,
                    },
                    cfg,
                );
            }
            StrategyAOutcome::FallbackToStrategyB => {}
        }
    }

    // Strategy B: Zero-quota streaming `.tar.gz` with lexicographical early abort!
    let enc_ref = url_encode_component(sha_or_ref);
    let (tarball_url, headers) = match host {
        "github.com" => {
            let mut hdrs = vec![("User-Agent".to_string(), USER_AGENT.to_string())];
            if let Some(pat) = cfg
                .github_token
                .as_deref()
                .map(str::trim)
                .filter(|s| !s.is_empty())
            {
                hdrs.push(("Authorization".to_string(), format!("Bearer {pat}")));
            }
            (
                format!("https://codeload.github.com/{owner}/{repo}/tar.gz/{enc_ref}"),
                hdrs,
            )
        }
        "gitlab.com" => {
            let base =
                format!("https://gitlab.com/{full_repo_path}/-/archive/{enc_ref}/{repo}-{enc_ref}.tar.gz");
            let url = match subdir_opt {
                Some(sd) => format!("{base}?path={}", url_encode_component(sd)),
                None => base,
            };
            (url, vec![("User-Agent".to_string(), USER_AGENT.to_string())])
        }
        "codeberg.org" => (
            format!("https://codeberg.org/{owner}/{repo}/archive/{enc_ref}.tar.gz"),
            vec![("User-Agent".to_string(), USER_AGENT.to_string())],
        ),
        other => {
            return Err(OnlineFetchError::ExtractionError(format!(
                "Unsupported repository tree host: {other}"
            )))
        }
    };

    let (files, detected_sha, aborted_early) =
        stream_and_extract_sparse_tarball(&tarball_url, headers, subdir_opt, cfg, fetcher)?;
    let pinned = resolved
        .commit_sha
        .as_deref()
        .or(detected_sha.as_deref())
        .unwrap_or(sha_or_ref);
    let abort_tag = if aborted_early { " · Early-Abort" } else { "" };
    let tier = format!("{host} Sparse Archive{abort_tag} · {} files", files.len());
    let title = if clean_sub.is_empty() {
        format!("{full_repo_path}")
    } else {
        format!("{full_repo_path}/{clean_sub}")
    };

    staging::stage_repo_files_and_build_envelope(
        RepoStagingRequest {
            host,
            owner,
            repo,
            commit_sha: pinned,
            canonical_url,
            tier_badge: &tier,
            title: &title,
            subdir: subdir_opt,
            focus_preview: None,
            files,
        },
        cfg,
    )
}

// ============================================================================
// 5. Pull Requests, Issues & Commits (`/pull/{n}`, `/issues/{n}`, `/commit/{sha}`)
// ============================================================================

fn filter_lockfiles_from_unified_diff(raw_diff: &str) -> (String, Vec<String>) {
    let mut kept = String::with_capacity(raw_diff.len());
    let mut skipped_locks: Vec<String> = Vec::new();
    let mut skip_current = false;

    for line in raw_diff.lines() {
        if let Some(rest) = line.strip_prefix("diff --git ") {
            let b_path = rest
                .rsplit_once(" b/")
                .map(|(_, b)| b.trim())
                .unwrap_or(rest.trim());
            if is_lockfile_path(b_path) {
                skip_current = true;
                skipped_locks.push(b_path.to_string());
                continue;
            } else {
                skip_current = false;
            }
        }
        if !skip_current {
            kept.push_str(line);
            kept.push('\n');
        }
    }
    (kept, skipped_locks)
}

fn fetch_github_pull_request(
    owner: &str,
    repo: &str,
    pr_num: &str,
    canonical_url: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let pr_api_url = format!("https://api.github.com/repos/{owner}/{repo}/pulls/{pr_num}");
    let meta_req = OutboundHttpRequest {
        url: pr_api_url.clone(),
        method: "GET".to_string(),
        headers: github_api_headers(cfg, "application/vnd.github+json"),
        body: None,
        max_bytes: 4 * 1024 * 1024,
        timeout_secs: 15,
        follow_redirects: true,
    };
    let meta_resp = fetcher.execute(&meta_req, cfg)?;
    if meta_resp.status >= 400 {
        return Err(OnlineFetchError::TransportError(format!(
            "HTTP {} fetching GitHub PR metadata {pr_api_url}",
            meta_resp.status
        )));
    }
    let meta: Value = serde_json::from_slice(&meta_resp.body).map_err(|e| {
        OnlineFetchError::ExtractionError(format!("Invalid GitHub PR JSON: {e}"))
    })?;

    let pr_title = meta
        .get("title")
        .and_then(Value::as_str)
        .unwrap_or("Pull Request");
    let state = meta
        .get("state")
        .and_then(Value::as_str)
        .unwrap_or("open");
    let merged = meta.get("merged").and_then(Value::as_bool).unwrap_or(false);
    let author = meta
        .pointer("/user/login")
        .and_then(Value::as_str)
        .unwrap_or("unknown");
    let base_ref = meta
        .pointer("/base/ref")
        .and_then(Value::as_str)
        .unwrap_or("main");
    let head_ref = meta
        .pointer("/head/ref")
        .and_then(Value::as_str)
        .unwrap_or("branch");
    let head_sha = meta
        .pointer("/head/sha")
        .and_then(Value::as_str)
        .unwrap_or("");
    let changed_files = meta.get("changed_files").and_then(Value::as_u64).unwrap_or(0);
    let additions = meta.get("additions").and_then(Value::as_u64).unwrap_or(0);
    let deletions = meta.get("deletions").and_then(Value::as_u64).unwrap_or(0);
    let pr_body = meta.get("body").and_then(Value::as_str).unwrap_or("");

    // Best-effort issue + inline review comments (tolerate 404/unmocked in lightweight fixtures).
    let mut comment_lines: Vec<String> = Vec::new();
    for endpoint in [
        format!("https://api.github.com/repos/{owner}/{repo}/issues/{pr_num}/comments?per_page=50"),
        format!("https://api.github.com/repos/{owner}/{repo}/pulls/{pr_num}/comments?per_page=50"),
    ] {
        let creq = OutboundHttpRequest {
            url: endpoint,
            method: "GET".to_string(),
            headers: github_api_headers(cfg, "application/vnd.github+json"),
            body: None,
            max_bytes: 4 * 1024 * 1024,
            timeout_secs: 12,
            follow_redirects: true,
        };
        if let Ok(cresp) = fetcher.execute(&creq, cfg) {
            if cresp.status == 200 {
                if let Ok(Value::Array(items)) = serde_json::from_slice::<Value>(&cresp.body) {
                    for c in items.iter().take(40) {
                        let user = c
                            .pointer("/user/login")
                            .and_then(Value::as_str)
                            .unwrap_or("reviewer");
                        let body = c.get("body").and_then(Value::as_str).unwrap_or("").trim();
                        if body.is_empty() {
                            continue;
                        }
                        if let Some(path) = c.get("path").and_then(Value::as_str) {
                            let line_no = c
                                .get("line")
                                .or_else(|| c.get("original_line"))
                                .and_then(Value::as_u64)
                                .unwrap_or(1);
                            comment_lines.push(format!(
                                "- **@{user}** on `{path}:L{line_no}`: {body}"
                            ));
                        } else {
                            comment_lines.push(format!("- **@{user}**: {body}"));
                        }
                    }
                }
            }
        }
    }

    // Fetch unified diff (`Accept: application/vnd.github.diff`); fall back to `/pulls/{n}/files?per_page=100`
    // automatically if GitHub returns `406 Not Acceptable` (`too_large`)!
    let diff_req = OutboundHttpRequest {
        url: pr_api_url.clone(),
        method: "GET".to_string(),
        headers: github_api_headers(cfg, "application/vnd.github.diff"),
        body: None,
        max_bytes: effective_max_single_file_bytes(cfg),
        timeout_secs: 20,
        follow_redirects: true,
    };
    let diff_resp = fetcher.execute(&diff_req, cfg)?;

    let (diff_section_md, tier_badge) = if diff_resp.status == 200 {
        let raw_diff = String::from_utf8_lossy(&diff_resp.body);
        let (filtered_diff, skipped_locks) = filter_lockfiles_from_unified_diff(&raw_diff);
        let skip_note = if skipped_locks.is_empty() {
            String::new()
        } else {
            format!(
                "*(Skipped {} lockfile diff(s): `{}`)*\n\n",
                skipped_locks.len(),
                skipped_locks.join("`, `")
            )
        };
        let md = format!("{skip_note}````diff\n{}\n````", filtered_diff.trim_end());
        (md, format!("GitHub PR #{pr_num} · Unified Diff"))
    } else if diff_resp.status == 406 || diff_resp.status == 422 {
        let files_url =
            format!("https://api.github.com/repos/{owner}/{repo}/pulls/{pr_num}/files?per_page=100");
        let files_req = OutboundHttpRequest {
            url: files_url.clone(),
            method: "GET".to_string(),
            headers: github_api_headers(cfg, "application/vnd.github+json"),
            body: None,
            max_bytes: effective_max_single_file_bytes(cfg),
            timeout_secs: 20,
            follow_redirects: true,
        };
        let files_resp = fetcher.execute(&files_req, cfg)?;
        if files_resp.status >= 400 {
            return Err(OnlineFetchError::TransportError(format!(
                "HTTP {} fetching fallback PR files {files_url}",
                files_resp.status
            )));
        }
        let files_val: Value = serde_json::from_slice(&files_resp.body).map_err(|e| {
            OnlineFetchError::ExtractionError(format!("Invalid GitHub PR `/files` JSON: {e}"))
        })?;
        let arr = files_val.as_array().ok_or_else(|| {
            OnlineFetchError::ExtractionError("Expected JSON array from `/pulls/{n}/files`".to_string())
        })?;

        let mut sections = String::new();
        let mut skipped_locks = Vec::new();
        let mut kept_count = 0usize;
        for f in arr {
            let Some(filename) = f.get("filename").and_then(Value::as_str) else {
                continue;
            };
            if is_lockfile_path(filename) {
                skipped_locks.push(filename.to_string());
                continue;
            }
            kept_count += 1;
            let f_status = f.get("status").and_then(Value::as_str).unwrap_or("modified");
            let f_add = f.get("additions").and_then(Value::as_u64).unwrap_or(0);
            let f_del = f.get("deletions").and_then(Value::as_u64).unwrap_or(0);
            sections.push_str(&format!(
                "### `{filename}` ({f_status}, +{f_add}/-{f_del})\n"
            ));
            if let Some(patch) = f.get("patch").and_then(Value::as_str) {
                sections.push_str(&format!("````diff\n{}\n````\n\n", patch.trim_end()));
            } else {
                sections.push_str("*(Binary or oversized per-file patch omitted upstream)*\n\n");
            }
        }
        if !skipped_locks.is_empty() {
            sections = format!(
                "*(Skipped {} lockfile(s): `{}`)*\n\n{sections}",
                skipped_locks.len(),
                skipped_locks.join("`, `")
            );
        }
        (
            sections,
            format!("GitHub PR #{pr_num} · Files Fallback ({kept_count} files)"),
        )
    } else {
        return Err(OnlineFetchError::TransportError(format!(
            "HTTP {} fetching GitHub PR diff {pr_api_url}",
            diff_resp.status
        )));
    };

    let status_label = if merged { "merged" } else { state };
    let comments_block = if comment_lines.is_empty() {
        "*(No comments)*".to_string()
    } else {
        comment_lines.join("\n")
    };
    let doc_title = format!("{owner}/{repo}#{pr_num}: {pr_title}");
    let full_md = format!(
        "# {doc_title}\n\n\
         ## Summary & Metadata\n\
         - **Author:** @{author}\n\
         - **Status:** `{status_label}` (`{head_ref}` `{head_sha}` -> `{base_ref}`)\n\
         - **Stats:** {changed_files} changed files (`+{additions}` / `-{deletions}`)\n\n\
         {pr_body}\n\n\
         ## Discussion & Review Comments\n\
         {comments_block}\n\n\
         ## Code Changes\n\
         {diff_section_md}\n"
    );

    staging::stage_single_document_and_build_envelope(
        &doc_title,
        canonical_url,
        &tier_badge,
        &full_md,
        Vec::new(),
        None,
        cfg,
    )
}

fn fetch_github_issue(
    owner: &str,
    repo: &str,
    issue_num: &str,
    canonical_url: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let issue_url = format!("https://api.github.com/repos/{owner}/{repo}/issues/{issue_num}");
    let req = OutboundHttpRequest {
        url: issue_url.clone(),
        method: "GET".to_string(),
        headers: github_api_headers(cfg, "application/vnd.github+json"),
        body: None,
        max_bytes: 4 * 1024 * 1024,
        timeout_secs: 15,
        follow_redirects: true,
    };
    let resp = fetcher.execute(&req, cfg)?;
    if resp.status >= 400 {
        return Err(OnlineFetchError::TransportError(format!(
            "HTTP {} fetching GitHub Issue {issue_url}",
            resp.status
        )));
    }
    let meta: Value = serde_json::from_slice(&resp.body)
        .map_err(|e| OnlineFetchError::ExtractionError(format!("Invalid Issue JSON: {e}")))?;
    let title_str = meta
        .get("title")
        .and_then(Value::as_str)
        .unwrap_or("Issue");
    let state = meta.get("state").and_then(Value::as_str).unwrap_or("open");
    let author = meta
        .pointer("/user/login")
        .and_then(Value::as_str)
        .unwrap_or("unknown");
    let body = meta.get("body").and_then(Value::as_str).unwrap_or("");

    let mut comments_md = String::new();
    let c_url =
        format!("https://api.github.com/repos/{owner}/{repo}/issues/{issue_num}/comments?per_page=50");
    let creq = OutboundHttpRequest {
        url: c_url,
        method: "GET".to_string(),
        headers: github_api_headers(cfg, "application/vnd.github+json"),
        body: None,
        max_bytes: 4 * 1024 * 1024,
        timeout_secs: 12,
        follow_redirects: true,
    };
    if let Ok(cresp) = fetcher.execute(&creq, cfg) {
        if cresp.status == 200 {
            if let Ok(Value::Array(items)) = serde_json::from_slice::<Value>(&cresp.body) {
                for item in items.iter().take(50) {
                    let u = item
                        .pointer("/user/login")
                        .and_then(Value::as_str)
                        .unwrap_or("user");
                    let b = item.get("body").and_then(Value::as_str).unwrap_or("").trim();
                    if !b.is_empty() {
                        comments_md.push_str(&format!("### Comment by @{u}\n{b}\n\n"));
                    }
                }
            }
        }
    }

    let doc_title = format!("{owner}/{repo}#{issue_num}: {title_str}");
    let md = format!(
        "# {doc_title}\n\n\
         ## Issue Description\n\
         - **Author:** @{author}\n\
         - **State:** `{state}`\n\n\
         {body}\n\n\
         ## Comments\n\
         {}\n",
        if comments_md.is_empty() {
            "*(No comments)*"
        } else {
            comments_md.trim_end()
        }
    );

    staging::stage_single_document_and_build_envelope(
        &doc_title,
        canonical_url,
        &format!("GitHub Issue #{issue_num}"),
        &md,
        Vec::new(),
        None,
        cfg,
    )
}

fn fetch_github_commit(
    owner: &str,
    repo: &str,
    sha: &str,
    canonical_url: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let commit_url = format!(
        "https://api.github.com/repos/{owner}/{repo}/commits/{}",
        url_encode_component(sha)
    );
    let req = OutboundHttpRequest {
        url: commit_url.clone(),
        method: "GET".to_string(),
        headers: github_api_headers(cfg, "application/vnd.github+json"),
        body: None,
        max_bytes: effective_max_single_file_bytes(cfg),
        timeout_secs: 20,
        follow_redirects: true,
    };
    let resp = fetcher.execute(&req, cfg)?;
    if resp.status >= 400 {
        return Err(OnlineFetchError::TransportError(format!(
            "HTTP {} fetching GitHub Commit {commit_url}",
            resp.status
        )));
    }
    let meta: Value = serde_json::from_slice(&resp.body)
        .map_err(|e| OnlineFetchError::ExtractionError(format!("Invalid Commit JSON: {e}")))?;

    let full_sha = meta.get("sha").and_then(Value::as_str).unwrap_or(sha);
    let message = meta
        .pointer("/commit/message")
        .and_then(Value::as_str)
        .unwrap_or("");
    let author = meta
        .pointer("/commit/author/name")
        .and_then(Value::as_str)
        .unwrap_or("unknown");
    let date = meta
        .pointer("/commit/author/date")
        .and_then(Value::as_str)
        .unwrap_or("");

    let mut patches = String::new();
    if let Some(files) = meta.get("files").and_then(Value::as_array) {
        for f in files {
            let Some(fname) = f.get("filename").and_then(Value::as_str) else {
                continue;
            };
            if is_lockfile_path(fname) {
                continue;
            }
            let status = f.get("status").and_then(Value::as_str).unwrap_or("modified");
            patches.push_str(&format!("### `{fname}` ({status})\n"));
            if let Some(patch) = f.get("patch").and_then(Value::as_str) {
                patches.push_str(&format!("````diff\n{}\n````\n\n", patch.trim_end()));
            }
        }
    }

    let first_line = message.lines().next().unwrap_or(full_sha);
    let doc_title = format!(
        "{owner}/{repo}@{} — {first_line}",
        &full_sha[..full_sha.len().min(12)]
    );
    let md = format!(
        "# {doc_title}\n\n\
         ## Commit Metadata\n\
         - **Commit SHA:** `{full_sha}`\n\
         - **Author:** {author} ({date})\n\n\
         ```\n{message}\n```\n\n\
         ## File Patches\n\
         {patches}\n"
    );

    staging::stage_single_document_and_build_envelope(
        &doc_title,
        canonical_url,
        "GitHub Commit",
        &md,
        Vec::new(),
        None,
        cfg,
    )
}

// ============================================================================
// 6. HuggingFace Hub Pre-Redirect Multi-GB Weight Guard
// ============================================================================

fn resolve_redirect_url(base_url: &str, location: &str) -> Result<String, OnlineFetchError> {
    let loc = location.trim();
    if loc.starts_with("https://") {
        return Ok(loc.to_string());
    }
    if loc.starts_with("http://") {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Blocked insecure HTTP redirect downgrade to '{loc}'"
        )));
    }
    if loc.starts_with('/') {
        let host = super::net_guard::extract_domain(base_url).ok_or_else(|| {
            OnlineFetchError::ExtractionError(format!("Cannot extract host from {base_url}"))
        })?;
        return Ok(format!("https://{host}{loc}"));
    }
    Err(OnlineFetchError::SecurityBlocked(format!(
        "Unsupported relative redirect Location '{loc}'"
    )))
}

fn fetch_hf_single_resolved_file(
    repo_prefix: &str,
    rev: &str,
    rel_path: &str,
    raw_url: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<(Vec<u8>, Option<String>), OnlineFetchError> {
    if is_hf_denylisted_weight_or_archive(rel_path) {
        return Err(OnlineFetchError::SizeOrBinaryBlocked {
            url: raw_url.to_string(),
            bytes: 0,
            reason: format!(
                "Denylisted model weight/archive extension on HuggingFace Hub: '{rel_path}'"
            ),
        });
    }

    let max_single = effective_max_single_file_bytes(cfg);
    let enc_rev = url_encode_component(rev);
    let enc_path = url_encode_repo_path(rel_path);
    let resolve_url = format!("https://huggingface.co/{repo_prefix}/resolve/{enc_rev}/{enc_path}");

    let mut hop_url = resolve_url.clone();
    let mut repo_commit: Option<String> = None;

    for _hop in 0..5 {
        let hop_host = super::net_guard::extract_domain(&hop_url).unwrap_or_default();
        let mut headers = vec![("User-Agent".to_string(), USER_AGENT.to_string())];
        // Attach HF token ONLY to `huggingface.co`; strip `Authorization` on cross-host CDN/Xet/S3!
        if hop_host == "huggingface.co" {
            if let Some(tok) = cfg
                .huggingface_token
                .as_deref()
                .map(str::trim)
                .filter(|s| !s.is_empty())
            {
                headers.push(("Authorization".to_string(), format!("Bearer {tok}")));
            }
        }

        let req = OutboundHttpRequest {
            url: hop_url.clone(),
            method: "GET".to_string(),
            headers,
            body: None,
            max_bytes: max_single,
            timeout_secs: 20,
            follow_redirects: false,
        };
        let resp = fetcher.execute(&req, cfg)?;
        if is_captcha_or_antibot_response(&resp) {
            return Err(OnlineFetchError::ChallengeOrRateLimited {
                domain: hop_host,
                status: resp.status,
                detail: "Anti-bot challenge detected on HuggingFace Hub".to_string(),
            });
        }

        if repo_commit.is_none() {
            if let Some(c) = resp.header("x-repo-commit").map(str::trim).filter(|s| !s.is_empty()) {
                repo_commit = Some(c.to_string());
            }
        }

        // Pre-redirect multi-GB weight guard: inspect `x-linked-size` AND `content-length` BEFORE
        // dialing `Location` on CDN/Xet (`0` payload bytes transferred!).
        let linked_size = parse_header_u64(&resp, "x-linked-size").unwrap_or(0);
        let content_len = parse_header_u64(&resp, "content-length").unwrap_or(0);
        let observed_bytes = linked_size.max(content_len);
        if observed_bytes > max_single as u64 {
            return Err(OnlineFetchError::SizeOrBinaryBlocked {
                url: resolve_url,
                bytes: observed_bytes,
                reason: format!(
                    "HuggingFace file size ({observed_bytes} bytes via X-Linked-Size/Content-Length) exceeds single-file cap ({max_single} bytes)"
                ),
            });
        }

        if matches!(resp.status, 301 | 302 | 303 | 307 | 308) {
            let location = resp.header("location").ok_or_else(|| {
                OnlineFetchError::TransportError(format!(
                    "HTTP {} redirect missing Location header on {hop_url}",
                    resp.status
                ))
            })?;
            hop_url = resolve_redirect_url(&hop_url, location)?;
            continue;
        }

        if resp.status >= 400 {
            return Err(OnlineFetchError::TransportError(format!(
                "HTTP {} fetching HuggingFace resource {hop_url}",
                resp.status
            )));
        }

        return Ok((resp.body, repo_commit));
    }

    Err(OnlineFetchError::TransportError(format!(
        "Too many redirects resolving HuggingFace resource {resolve_url}"
    )))
}

fn fetch_huggingface_resource(
    segments: &[String],
    line_range: Option<(usize, usize)>,
    raw_url: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let (hub_kind, ns_idx) = if segments.first().map(String::as_str) == Some("datasets")
        || segments.first().map(String::as_str) == Some("spaces")
        || segments.first().map(String::as_str) == Some("models")
    {
        (segments[0].as_str(), 1usize)
    } else {
        ("models", 0usize)
    };

    if segments.len() < ns_idx + 2 {
        return Err(OnlineFetchError::ExtractionError(format!(
            "HuggingFace URL requires `<namespace>/<repo>`: {raw_url}"
        )));
    }

    let ns = &segments[ns_idx];
    let repo = &segments[ns_idx + 1];
    let repo_prefix = if ns_idx == 1 && hub_kind != "models" {
        format!("{hub_kind}/{ns}/{repo}")
    } else {
        format!("{ns}/{repo}")
    };
    let tail = &segments[ns_idx + 2..];

    // Single-file `/blob/{rev}/{path}`, `/raw/{rev}/{path}`, or `/resolve/{rev}/{path}`
    if tail.len() >= 3 && matches!(tail[0].as_str(), "blob" | "raw" | "resolve") {
        let rev = &tail[1];
        let raw_rel_path = tail[2..].join("/");
        if is_hf_denylisted_weight_or_archive(&raw_rel_path) {
            return Err(OnlineFetchError::SizeOrBinaryBlocked {
                url: raw_url.to_string(),
                bytes: 0,
                reason: format!(
                    "Denylisted model weight/archive extension on HuggingFace Hub: '{raw_rel_path}'"
                ),
            });
        }
        let clean_rel =
            staging::normalize_and_validate_repo_rel_path(&raw_rel_path).ok_or_else(|| {
                OnlineFetchError::SecurityBlocked(format!(
                    "Rejected unsafe HuggingFace file path '{raw_rel_path}'"
                ))
            })?;

        let (bytes, commit_hdr) =
            fetch_hf_single_resolved_file(&repo_prefix, rev, &clean_rel, raw_url, cfg, fetcher)?;
        let max_single = effective_max_single_file_bytes(cfg);
        let content = validate_text_blob(&bytes, max_single, raw_url)?;
        let focus =
            line_range.map(|(s, e)| format_line_numbered_focus_slice(&clean_rel, &content, s, e));
        let pinned = commit_hdr.as_deref().unwrap_or(rev.as_str());
        let tier = match line_range {
            Some((s, e)) => format!("HuggingFace Hub · File (L{s}–L{e})"),
            None => "HuggingFace Hub · File".to_string(),
        };
        let title = format!("{repo_prefix}/{clean_rel}");

        return staging::stage_repo_files_and_build_envelope(
            RepoStagingRequest {
                host: "huggingface.co",
                owner: ns,
                repo,
                commit_sha: pinned,
                canonical_url: raw_url,
                tier_badge: &tier,
                title: &title,
                subdir: None,
                focus_preview: focus.as_deref(),
                files: vec![StagedRepoFileInput {
                    rel_path: clean_rel,
                    content,
                }],
            },
            cfg,
        );
    }

    // Bare repo `huggingface.co/{ns}/{repo}` or `/tree/{rev}/{subdir}` -> selective config/code/README pull
    let (rev, subdir) = if tail.first().map(String::as_str) == Some("tree") && tail.len() >= 2 {
        (tail[1].as_str(), tail[2..].join("/"))
    } else if tail.is_empty() {
        ("main", String::new())
    } else {
        return Err(OnlineFetchError::ExtractionError(format!(
            "Unsupported HuggingFace Hub route in URL: {raw_url}"
        )));
    };

    let enc_rev = url_encode_component(rev);
    let tree_api_url = if subdir.is_empty() {
        format!("https://huggingface.co/api/{hub_kind}/{ns}/{repo}/tree/{enc_rev}")
    } else {
        format!(
            "https://huggingface.co/api/{hub_kind}/{ns}/{repo}/tree/{enc_rev}/{}",
            url_encode_repo_path(&subdir)
        )
    };
    let mut api_headers = vec![
        ("Accept".to_string(), "application/json".to_string()),
        ("User-Agent".to_string(), USER_AGENT.to_string()),
    ];
    if let Some(tok) = cfg
        .huggingface_token
        .as_deref()
        .map(str::trim)
        .filter(|s| !s.is_empty())
    {
        api_headers.push(("Authorization".to_string(), format!("Bearer {tok}")));
    }

    let req = OutboundHttpRequest {
        url: tree_api_url.clone(),
        method: "GET".to_string(),
        headers: api_headers,
        body: None,
        max_bytes: 4 * 1024 * 1024,
        timeout_secs: 15,
        follow_redirects: true,
    };
    let resp = fetcher.execute(&req, cfg)?;
    if resp.status >= 400 {
        return Err(OnlineFetchError::TransportError(format!(
            "HTTP {} listing HuggingFace repo tree {tree_api_url}",
            resp.status
        )));
    }
    let items: Value = serde_json::from_slice(&resp.body).map_err(|e| {
        OnlineFetchError::ExtractionError(format!("Invalid HuggingFace tree JSON: {e}"))
    })?;
    let arr = items.as_array().ok_or_else(|| {
        OnlineFetchError::ExtractionError("Expected JSON array from HuggingFace tree API".to_string())
    })?;

    let max_single = effective_max_single_file_bytes(cfg);
    let max_tree = effective_max_tree_bytes(cfg);
    let mut total_bytes = 0usize;
    let mut staged_files = Vec::new();
    let mut pinned_commit: Option<String> = None;

    for entry in arr {
        if entry.get("type").and_then(Value::as_str) != Some("file") {
            continue;
        }
        let Some(p) = entry.get("path").and_then(Value::as_str) else {
            continue;
        };
        if is_hf_denylisted_weight_or_archive(p) || !is_allowed_repo_source_path(p) {
            continue;
        }
        let size = entry
            .pointer("/lfs/size")
            .and_then(Value::as_u64)
            .or_else(|| entry.get("size").and_then(Value::as_u64))
            .unwrap_or(0);
        if size > max_single as u64 {
            continue;
        }
        if total_bytes.saturating_add(size as usize) > max_tree || staged_files.len() >= 48 {
            break;
        }
        if pinned_commit.is_none() {
            if let Some(oid) = entry.get("oid").and_then(Value::as_str).filter(|s| is_hex_sha40(s)) {
                pinned_commit = Some(oid.to_string());
            }
        }
        let (bytes, hdr_commit) =
            fetch_hf_single_resolved_file(&repo_prefix, rev, p, raw_url, cfg, fetcher)?;
        if pinned_commit.is_none() {
            pinned_commit = hdr_commit;
        }
        if let Ok(text) = validate_text_blob(&bytes, max_single, raw_url) {
            total_bytes += text.len();
            staged_files.push(StagedRepoFileInput {
                rel_path: p.to_string(),
                content: text,
            });
        }
    }

    let pinned = pinned_commit.as_deref().unwrap_or(rev);
    let tier = format!("HuggingFace Hub · {} files", staged_files.len());
    staging::stage_repo_files_and_build_envelope(
        RepoStagingRequest {
            host: "huggingface.co",
            owner: ns,
            repo,
            commit_sha: pinned,
            canonical_url: raw_url,
            tier_badge: &tier,
            title: &repo_prefix,
            subdir: if subdir.is_empty() {
                None
            } else {
                Some(subdir.as_str())
            },
            focus_preview: None,
            files: staged_files,
        },
        cfg,
    )
}

// ============================================================================
// 7. Top-Level Router (`fetch_repo_or_hub_resource`)
// ============================================================================

/// Fetches a single file, sparse directory tree, repository root, PR/Issue/Commit, or HuggingFace
/// Hub model/dataset resource, staging clean read-only files under `.spell/research_cache/repos/`.
pub fn fetch_repo_or_hub_resource(
    raw_url: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let (host, segments, _query, frag) = split_url_parts(raw_url)?;
    let line_range = parse_line_range_fragment(frag.as_deref());

    match host.as_str() {
        "huggingface.co" | "hf.co" => {
            fetch_huggingface_resource(&segments, line_range, raw_url.trim(), cfg, fetcher)
        }
        "raw.githubusercontent.com" => {
            if segments.len() < 4 {
                return Err(OnlineFetchError::ExtractionError(format!(
                    "raw.githubusercontent.com URL requires `/<owner>/<repo>/<ref>/<path>`: {raw_url}"
                )));
            }
            let owner = &segments[0];
            let repo = segments[1].trim_end_matches(".git");
            let full_repo = format!("{owner}/{repo}");
            let resolved = resolve_git_v2_ref_and_subpath(
                "github.com",
                &full_repo,
                &segments[2..],
                true,
                line_range,
                cfg,
                fetcher,
            )?;
            fetch_single_forge_file(
                "raw.githubusercontent.com",
                owner,
                repo,
                &full_repo,
                &resolved,
                raw_url.trim(),
                cfg,
                fetcher,
            )
        }
        "github.com" => {
            if segments.len() < 2 {
                return Err(OnlineFetchError::ExtractionError(format!(
                    "GitHub URL requires `/<owner>/<repo>`: {raw_url}"
                )));
            }
            let owner = &segments[0];
            let repo = segments[1].trim_end_matches(".git");
            let full_repo = format!("{owner}/{repo}");
            let tail = &segments[2..];

            if tail.is_empty() {
                let resolved = resolve_git_v2_ref_and_subpath(
                    "github.com",
                    &full_repo,
                    &[],
                    false,
                    line_range,
                    cfg,
                    fetcher,
                )?;
                return fetch_forge_sparse_tree(
                    "github.com",
                    owner,
                    repo,
                    &full_repo,
                    &resolved,
                    raw_url.trim(),
                    cfg,
                    fetcher,
                );
            }

            match tail[0].as_str() {
                "blob" | "raw" => {
                    if tail.len() < 3 {
                        return Err(OnlineFetchError::ExtractionError(format!(
                            "GitHub blob/raw URL requires ref and file path: {raw_url}"
                        )));
                    }
                    let resolved = resolve_git_v2_ref_and_subpath(
                        "github.com",
                        &full_repo,
                        &tail[1..],
                        true,
                        line_range,
                        cfg,
                        fetcher,
                    )?;
                    fetch_single_forge_file(
                        "github.com",
                        owner,
                        repo,
                        &full_repo,
                        &resolved,
                        raw_url.trim(),
                        cfg,
                        fetcher,
                    )
                }
                "tree" => {
                    let resolved = resolve_git_v2_ref_and_subpath(
                        "github.com",
                        &full_repo,
                        &tail[1..],
                        false,
                        line_range,
                        cfg,
                        fetcher,
                    )?;
                    fetch_forge_sparse_tree(
                        "github.com",
                        owner,
                        repo,
                        &full_repo,
                        &resolved,
                        raw_url.trim(),
                        cfg,
                        fetcher,
                    )
                }
                "pull" | "pulls" if tail.len() >= 2 => fetch_github_pull_request(
                    owner,
                    repo,
                    &tail[1],
                    raw_url.trim(),
                    cfg,
                    fetcher,
                ),
                "issues" | "issue" if tail.len() >= 2 => fetch_github_issue(
                    owner,
                    repo,
                    &tail[1],
                    raw_url.trim(),
                    cfg,
                    fetcher,
                ),
                "commit" | "commits" if tail.len() >= 2 => fetch_github_commit(
                    owner,
                    repo,
                    &tail[1],
                    raw_url.trim(),
                    cfg,
                    fetcher,
                ),
                other => Err(OnlineFetchError::ExtractionError(format!(
                    "Unsupported GitHub route '/{other}' in {raw_url}"
                ))),
            }
        }
        "gitlab.com" => {
            if segments.len() < 2 {
                return Err(OnlineFetchError::ExtractionError(format!(
                    "GitLab URL requires `/<namespace>/<repo>`: {raw_url}"
                )));
            }
            // GitLab uses `/-/` between `<group/[subgroup/]repo>` and the route!
            let dash_pos = segments.iter().position(|s| s == "-");
            let (ns_repo_segs, route_segs): (&[String], &[String]) = match dash_pos {
                Some(idx) if idx >= 2 => (&segments[..idx], &segments[idx + 1..]),
                _ => {
                    // Check legacy `/owner/repo/{blob|raw|tree}/...` without `/-/`
                    if segments.len() >= 3
                        && matches!(segments[2].as_str(), "blob" | "raw" | "tree")
                    {
                        (&segments[..2], &segments[2..])
                    } else {
                        (&segments[..], &[])
                    }
                }
            };
            let repo = ns_repo_segs
                .last()
                .map(|s| s.trim_end_matches(".git"))
                .unwrap_or("repo");
            let owner = ns_repo_segs[..ns_repo_segs.len() - 1].join("/");
            let full_repo = format!("{owner}/{repo}");

            if route_segs.is_empty() {
                let resolved = resolve_git_v2_ref_and_subpath(
                    "gitlab.com",
                    &full_repo,
                    &[],
                    false,
                    line_range,
                    cfg,
                    fetcher,
                )?;
                return fetch_forge_sparse_tree(
                    "gitlab.com",
                    &owner,
                    repo,
                    &full_repo,
                    &resolved,
                    raw_url.trim(),
                    cfg,
                    fetcher,
                );
            }

            match route_segs[0].as_str() {
                "blob" | "raw" if route_segs.len() >= 3 => {
                    let resolved = resolve_git_v2_ref_and_subpath(
                        "gitlab.com",
                        &full_repo,
                        &route_segs[1..],
                        true,
                        line_range,
                        cfg,
                        fetcher,
                    )?;
                    fetch_single_forge_file(
                        "gitlab.com",
                        &owner,
                        repo,
                        &full_repo,
                        &resolved,
                        raw_url.trim(),
                        cfg,
                        fetcher,
                    )
                }
                "tree" => {
                    let resolved = resolve_git_v2_ref_and_subpath(
                        "gitlab.com",
                        &full_repo,
                        &route_segs[1..],
                        false,
                        line_range,
                        cfg,
                        fetcher,
                    )?;
                    fetch_forge_sparse_tree(
                        "gitlab.com",
                        &owner,
                        repo,
                        &full_repo,
                        &resolved,
                        raw_url.trim(),
                        cfg,
                        fetcher,
                    )
                }
                other => Err(OnlineFetchError::ExtractionError(format!(
                    "Unsupported GitLab route '/-/{other}' in {raw_url}"
                ))),
            }
        }
        "codeberg.org" => {
            if segments.len() < 2 {
                return Err(OnlineFetchError::ExtractionError(format!(
                    "Codeberg URL requires `/<owner>/<repo>`: {raw_url}"
                )));
            }
            let owner = &segments[0];
            let repo = segments[1].trim_end_matches(".git");
            let full_repo = format!("{owner}/{repo}");
            let tail = &segments[2..];

            if tail.is_empty() {
                let resolved = resolve_git_v2_ref_and_subpath(
                    "codeberg.org",
                    &full_repo,
                    &[],
                    false,
                    line_range,
                    cfg,
                    fetcher,
                )?;
                return fetch_forge_sparse_tree(
                    "codeberg.org",
                    owner,
                    repo,
                    &full_repo,
                    &resolved,
                    raw_url.trim(),
                    cfg,
                    fetcher,
                );
            }

            match tail[0].as_str() {
                "raw" | "blob" => {
                    let ref_slice = if tail.len() >= 4
                        && matches!(tail[1].as_str(), "branch" | "tag" | "commit")
                    {
                        &tail[2..]
                    } else {
                        &tail[1..]
                    };
                    let mut resolved = resolve_git_v2_ref_and_subpath(
                        "codeberg.org",
                        &full_repo,
                        ref_slice,
                        true,
                        line_range,
                        cfg,
                        fetcher,
                    )?;
                    if tail.get(1).map(String::as_str) == Some("tag") {
                        resolved.is_tag = true;
                    }
                    fetch_single_forge_file(
                        "codeberg.org",
                        owner,
                        repo,
                        &full_repo,
                        &resolved,
                        raw_url.trim(),
                        cfg,
                        fetcher,
                    )
                }
                "tree" | "src" => {
                    let ref_slice = if tail.len() >= 3
                        && matches!(tail[1].as_str(), "branch" | "tag" | "commit")
                    {
                        &tail[2..]
                    } else {
                        &tail[1..]
                    };
                    let resolved = resolve_git_v2_ref_and_subpath(
                        "codeberg.org",
                        &full_repo,
                        ref_slice,
                        false,
                        line_range,
                        cfg,
                        fetcher,
                    )?;
                    if tail[0] == "src" && line_range.is_some() && !resolved.subpath.is_empty() {
                        return fetch_single_forge_file(
                            "codeberg.org",
                            owner,
                            repo,
                            &full_repo,
                            &resolved,
                            raw_url.trim(),
                            cfg,
                            fetcher,
                        );
                    }
                    fetch_forge_sparse_tree(
                        "codeberg.org",
                        owner,
                        repo,
                        &full_repo,
                        &resolved,
                        raw_url.trim(),
                        cfg,
                        fetcher,
                    )
                }
                other => Err(OnlineFetchError::ExtractionError(format!(
                    "Unsupported Codeberg route '/{other}' in {raw_url}"
                ))),
            }
        }
        other => Err(OnlineFetchError::ExtractionError(format!(
            "Unsupported repository/hub host '{other}' in {raw_url}"
        ))),
    }
}

// ============================================================================
// Offline Unit Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use crate::online_fetch::staging::unlock_staged_dir_for_cleanup;
    use crate::online_fetch::OnlineResearchMode;
    use parking_lot::Mutex;
    use std::collections::BTreeMap;
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::sync::atomic::{AtomicUsize, Ordering};

    static TEST_ID: AtomicUsize = AtomicUsize::new(1);

    struct TempWorkspace(PathBuf);

    impl TempWorkspace {
        fn new(label: &str) -> Self {
            let id = TEST_ID.fetch_add(1, Ordering::SeqCst);
            let dir = std::env::temp_dir().join(format!(
                "spell_repo_fetch_test_{}_{}_{}",
                std::process::id(),
                label,
                id
            ));
            let _ = unlock_staged_dir_for_cleanup(&dir);
            let _ = fs::remove_dir_all(&dir);
            fs::create_dir_all(&dir).unwrap();
            Self(dir)
        }

        fn path(&self) -> &Path {
            &self.0
        }

        fn config(&self) -> OnlineResearchConfig {
            let mut cfg = OnlineResearchConfig::with_defaults();
            cfg.mode = OnlineResearchMode::CuratedAllowlist;
            cfg.workspace_root = Some(self.0.clone());
            cfg.session_nonce = "spell77889".to_string();
            cfg
        }
    }

    impl Drop for TempWorkspace {
        fn drop(&mut self) {
            let _ = unlock_staged_dir_for_cleanup(&self.0);
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn gzip_compress_in_memory(raw: &[u8]) -> Vec<u8> {
        use std::io::Write;
        let mut encoder =
            flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::default());
        encoder.write_all(raw).expect("gzip write failed");
        encoder.finish().expect("gzip finish failed")
    }

    fn build_synthetic_tar_gz(entries: &[(&str, &[u8])]) -> Vec<u8> {
        let mut tar = Vec::new();
        for (path, data) in entries {
            let hdr = build_clean_ustar_header(path, data.len()).unwrap();
            tar.extend_from_slice(&hdr);
            tar.extend_from_slice(data);
            let pad = (512 - (data.len() % 512)) % 512;
            if pad > 0 {
                tar.resize(tar.len() + pad, 0u8);
            }
        }
        tar.extend_from_slice(&[0u8; 1024]);
        gzip_compress_in_memory(&tar)
    }

    fn pkt_response(lines: &[&str]) -> Vec<u8> {
        let mut s = String::new();
        for l in lines {
            s.push_str(&encode_pkt_line(&format!("{l}\n")));
        }
        s.push_str("0000");
        s.into_bytes()
    }

    #[derive(Clone)]
    struct RecordedRequest {
        url: String,
        method: String,
        headers: Vec<(String, String)>,
        body: Option<Vec<u8>>,
        follow_redirects: bool,
    }

    struct MockRoute {
        status: u16,
        headers: BTreeMap<String, String>,
        body: Vec<u8>,
        stream_chunk_size: usize,
    }

    struct MockHttpsFetcher {
        routes: BTreeMap<String, Vec<MockRoute>>,
        recorded: Mutex<Vec<RecordedRequest>>,
        stream_chunks_delivered: AtomicUsize,
        stream_aborted: Mutex<bool>,
    }

    impl MockHttpsFetcher {
        fn new() -> Self {
            Self {
                routes: BTreeMap::new(),
                recorded: Mutex::new(Vec::new()),
                stream_chunks_delivered: AtomicUsize::new(0),
                stream_aborted: Mutex::new(false),
            }
        }

        fn add_route(
            &mut self,
            url: &str,
            status: u16,
            headers: &[(&str, &str)],
            body: impl Into<Vec<u8>>,
        ) {
            let mut hmap = BTreeMap::new();
            for (k, v) in headers {
                hmap.insert(k.to_ascii_lowercase(), (*v).to_string());
            }
            self.routes
                .entry(url.to_string())
                .or_default()
                .push(MockRoute {
                    status,
                    headers: hmap,
                    body: body.into(),
                    stream_chunk_size: 256,
                });
        }

        fn add_streaming_route(
            &mut self,
            url: &str,
            status: u16,
            chunk_size: usize,
            body: Vec<u8>,
        ) {
            self.routes
                .entry(url.to_string())
                .or_default()
                .push(MockRoute {
                    status,
                    headers: BTreeMap::new(),
                    body,
                    stream_chunk_size: chunk_size,
                });
        }

        fn requests(&self) -> Vec<RecordedRequest> {
            self.recorded.lock().clone()
        }
    }

    impl HttpsFetcher for MockHttpsFetcher {
        fn execute(
            &self,
            req: &OutboundHttpRequest,
            _cfg: &OnlineResearchConfig,
        ) -> Result<OutboundHttpResponse, OnlineFetchError> {
            let recorded_copy = RecordedRequest {
                url: req.url.clone(),
                method: req.method.clone(),
                headers: req.headers.clone(),
                body: req.body.clone(),
                follow_redirects: req.follow_redirects,
            };
            self.recorded.lock().push(recorded_copy);

            let Some(list) = self.routes.get(&req.url) else {
                return Ok(OutboundHttpResponse {
                    status: 404,
                    final_url: req.url.clone(),
                    headers: BTreeMap::new(),
                    body: b"unmocked 404".to_vec(),
                });
            };
            // Select route matching Accept header if multiple routes share the same URL (e.g. PR JSON vs PR diff).
            let req_accept = req
                .headers
                .iter()
                .find(|(k, _)| k.eq_ignore_ascii_case("accept"))
                .map(|(_, v)| v.as_str())
                .unwrap_or("");
            let chosen = list
                .iter()
                .find(|r| {
                    r.headers
                        .get("x-mock-match-accept")
                        .map(|a| a == req_accept)
                        .unwrap_or(false)
                })
                .or_else(|| {
                    list.iter()
                        .find(|r| !r.headers.contains_key("x-mock-match-accept"))
                })
                .unwrap_or(&list[0]);

            Ok(OutboundHttpResponse {
                status: chosen.status,
                final_url: req.url.clone(),
                headers: chosen.headers.clone(),
                body: chosen.body.clone(),
            })
        }

        fn execute_streaming(
            &self,
            req: &OutboundHttpRequest,
            cfg: &OnlineResearchConfig,
            on_chunk: &mut dyn FnMut(&[u8]) -> Result<bool, OnlineFetchError>,
        ) -> Result<(u16, BTreeMap<String, String>), OnlineFetchError> {
            let resp = self.execute(req, cfg)?;
            let chunk_sz = self
                .routes
                .get(&req.url)
                .and_then(|v| v.first())
                .map(|r| r.stream_chunk_size)
                .unwrap_or(256)
                .max(1);
            for chunk in resp.body.chunks(chunk_sz) {
                self.stream_chunks_delivered.fetch_add(1, Ordering::SeqCst);
                if !on_chunk(chunk)? {
                    *self.stream_aborted.lock() = true;
                    break;
                }
            }
            Ok((resp.status, resp.headers))
        }
    }

    #[test]
    fn test_is_repo_or_hub_url_recognition() {
        assert!(is_repo_or_hub_url("https://github.com/vllm-project/vllm"));
        assert!(is_repo_or_hub_url(
            "https://raw.githubusercontent.com/rust-lang/rust/master/Cargo.toml"
        ));
        assert!(is_repo_or_hub_url("https://gitlab.com/gitlab-org/gitlab"));
        assert!(is_repo_or_hub_url("https://codeberg.org/forgejo/forgejo"));
        assert!(is_repo_or_hub_url(
            "https://huggingface.co/Qwen/Qwen3.5-397B/blob/main/config.json"
        ));
        assert!(!is_repo_or_hub_url("https://arxiv.org/abs/2501.00001"));
        assert!(!is_repo_or_hub_url("https://docs.rs/tokio/latest/tokio/"));
    }

    #[test]
    fn test_git_v2_ls_refs_slash_branch_disambiguation_and_line_slice() {
        let ws = TempWorkspace::new("slash_branch");
        let cfg = ws.config();
        let mut mock = MockHttpsFetcher::new();

        let sha40 = "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678";
        mock.add_route(
            "https://github.com/vllm-project/vllm.git/git-upload-pack",
            200,
            &[],
            pkt_response(&[
                &format!("1111111111111111111111111111111111111111 refs/heads/releases"),
                &format!("{sha40} refs/heads/releases/v0.6.0"),
            ]),
        );

        let mut cu_src = String::new();
        for i in 1..=40 {
            cu_src.push_str(&format!("// CUDA kernel line {i}\n"));
        }
        mock.add_route(
            &format!("https://raw.githubusercontent.com/vllm-project/vllm/{sha40}/csrc/attn.cu"),
            200,
            &[],
            cu_src.clone().into_bytes(),
        );

        let outcome = fetch_repo_or_hub_resource(
            "https://github.com/vllm-project/vllm/blob/releases/v0.6.0/csrc/attn.cu#L10-L25",
            &cfg,
            &mock,
        )
        .unwrap();

        // Verify exact Git Protocol v2 headers & pkt-line request
        let reqs = mock.requests();
        assert_eq!(reqs.len(), 2);
        assert_eq!(reqs[0].method, "POST");
        assert!(reqs[0]
            .headers
            .iter()
            .any(|(k, v)| k == "Git-Protocol" && v == "version=2"));
        assert!(reqs[0]
            .headers
            .iter()
            .any(|(k, v)| k == "Content-Type" && v == "application/x-git-upload-pack-request"));
        let pkt_body = String::from_utf8(reqs[0].body.clone().unwrap()).unwrap();
        assert!(pkt_body.contains("0014command=ls-refs\n"));
        assert!(pkt_body.contains("ref-prefix refs/heads/releases\n"));
        assert!(pkt_body.contains("ref-prefix refs/tags/releases\n"));

        // Verify focused L10-L25 slice in envelope + full file staged at `.spell/research_cache/repos/...`
        assert!(outcome.envelope_markdown.contains("L10–L25"));
        assert!(outcome.envelope_markdown.contains("10 | // CUDA kernel line 10"));
        assert!(outcome.envelope_markdown.contains("25 | // CUDA kernel line 25"));
        assert!(!outcome.envelope_markdown.contains("30 | // CUDA kernel line 30"));

        let staged_file = outcome.staged_root.unwrap().join("csrc/attn.cu");
        assert_eq!(fs::read_to_string(staged_file).unwrap(), cu_src);
    }

    #[test]
    fn test_git_v2_ls_refs_annotated_tag_peeled_sha_resolution() {
        let cfg = OnlineResearchConfig::with_defaults();
        let mut mock = MockHttpsFetcher::new();
        let tag_obj_sha = "ffffffffffffffffffffffffffffffffffffffff";
        let peeled_commit_sha = "0123456789abcdef0123456789abcdef01234567";
        mock.add_route(
            "https://github.com/org/repo.git/git-upload-pack",
            200,
            &[],
            pkt_response(&[&format!(
                "{tag_obj_sha} refs/tags/v1.2.0 peeled:{peeled_commit_sha}"
            )]),
        );

        let segs = vec!["v1.2.0".to_string(), "src".to_string(), "lib.rs".to_string()];
        let res = resolve_git_v2_ref_and_subpath(
            "github.com",
            "org/repo",
            &segs,
            true,
            Some((1, 5)),
            &cfg,
            &mock,
        )
        .unwrap();

        assert_eq!(res.ref_name, "v1.2.0");
        assert_eq!(res.commit_sha.as_deref(), Some(peeled_commit_sha));
        assert_eq!(res.subpath, "src/lib.rs");
        assert!(res.is_tag);
    }

    #[test]
    fn test_git_v2_ls_refs_skipped_when_ref_is_40_hex_sha() {
        let cfg = OnlineResearchConfig::with_defaults();
        let mock = MockHttpsFetcher::new();
        let sha40 = "abcdef0123456789abcdef0123456789abcdef01";
        let segs = vec![sha40.to_string(), "csrc".to_string(), "gemm.cu".to_string()];

        let res = resolve_git_v2_ref_and_subpath(
            "github.com",
            "org/repo",
            &segs,
            true,
            None,
            &cfg,
            &mock,
        )
        .unwrap();

        assert_eq!(mock.requests().len(), 0);
        assert_eq!(res.commit_sha.as_deref(), Some(sha40));
        assert_eq!(res.subpath, "csrc/gemm.cu");
    }

    #[test]
    fn test_git_v2_ls_refs_404_fallback_to_first_segment_ref() {
        let cfg = OnlineResearchConfig::with_defaults();
        let mock = MockHttpsFetcher::new(); // returns 404 on unmocked git-upload-pack
        let segs = vec!["main".to_string(), "README.md".to_string()];
        let res = resolve_git_v2_ref_and_subpath(
            "github.com",
            "org/repo",
            &segs,
            true,
            None,
            &cfg,
            &mock,
        )
        .unwrap();
        assert_eq!(res.ref_name, "main");
        assert_eq!(res.commit_sha, None);
        assert_eq!(res.subpath, "README.md");
    }

    #[test]
    fn test_github_private_repo_pat_uses_api_contents_raw_header() {
        let ws = TempWorkspace::new("private_pat");
        let mut cfg = ws.config();
        cfg.github_token = Some("ghp_secret_private_token_123".to_string());

        let mut mock = MockHttpsFetcher::new();
        let sha40 = "9999888877776666555544443333222211110000";
        mock.add_route(
            &format!("https://api.github.com/repos/acme/priv-repo/contents/src/secret_algo.rs?ref={sha40}"),
            200,
            &[],
            b"pub fn private_kernel() -> usize { 42 }\n".to_vec(),
        );

        let out = fetch_repo_or_hub_resource(
            &format!("https://github.com/acme/priv-repo/blob/{sha40}/src/secret_algo.rs"),
            &cfg,
            &mock,
        )
        .unwrap();

        let reqs = mock.requests();
        assert_eq!(reqs.len(), 1);
        assert!(reqs[0].url.starts_with("https://api.github.com/repos/acme/priv-repo/contents/"));
        assert!(reqs[0]
            .headers
            .iter()
            .any(|(k, v)| k == "Accept" && v == "application/vnd.github.raw+json"));
        assert!(reqs[0]
            .headers
            .iter()
            .any(|(k, v)| k == "Authorization" && v == "Bearer ghp_secret_private_token_123"));
        assert!(out.envelope_markdown.contains("private_kernel"));
    }

    #[test]
    fn test_single_file_rejects_macho_elf_and_nul_binaries() {
        let ws = TempWorkspace::new("bin_guard");
        let cfg = ws.config();
        let sha40 = "1234567890abcdef1234567890abcdef12345678";

        for (name, payload) in [
            ("elf", b"\x7fELF\x02\x01\x01\x00binary".to_vec()),
            ("macho", vec![0xCF, 0xFA, 0xED, 0xFE, 0x0C, 0x00, 0x00, 0x01]),
            ("nul", b"some text\x00hidden binary".to_vec()),
        ] {
            let mut mock = MockHttpsFetcher::new();
            mock.add_route(
                &format!("https://raw.githubusercontent.com/org/repo/{sha40}/bin/{name}.dat"),
                200,
                &[],
                payload,
            );
            let err = fetch_repo_or_hub_resource(
                &format!("https://github.com/org/repo/blob/{sha40}/bin/{name}.dat"),
                &cfg,
                &mock,
            )
            .unwrap_err();
            assert!(matches!(err, OnlineFetchError::SizeOrBinaryBlocked { .. }));
        }
    }

    #[test]
    fn test_strategy_a_github_segment_tree_walk_avoids_root_recursive() {
        let ws = TempWorkspace::new("strategy_a");
        let cfg = ws.config();
        let mut mock = MockHttpsFetcher::new();

        let commit_sha = "aaaa0000bbbb1111cccc2222dddd3333eeee4444";
        let csrc_tree_sha = "1111111111111111111111111111111111111111";
        let kernels_tree_sha = "2222222222222222222222222222222222222222";

        // Root tree (NO `?recursive=1`!)
        mock.add_route(
            &format!("https://api.github.com/repos/org/mono/git/trees/{commit_sha}"),
            200,
            &[],
            serde_json::json!({
                "sha": commit_sha,
                "truncated": false,
                "tree": [{ "path": "csrc", "mode": "040000", "type": "tree", "sha": csrc_tree_sha }]
            })
            .to_string()
            .into_bytes(),
        );
        // Second segment `kernels` (NO `?recursive=1`!)
        mock.add_route(
            &format!("https://api.github.com/repos/org/mono/git/trees/{csrc_tree_sha}"),
            200,
            &[],
            serde_json::json!({
                "sha": csrc_tree_sha,
                "truncated": false,
                "tree": [{ "path": "kernels", "mode": "040000", "type": "tree", "sha": kernels_tree_sha }]
            })
            .to_string()
            .into_bytes(),
        );
        // Leaf subdir `?recursive=1` (includes lockfile + binary noise that must be filtered!)
        mock.add_route(
            &format!("https://api.github.com/repos/org/mono/git/trees/{kernels_tree_sha}?recursive=1"),
            200,
            &[],
            serde_json::json!({
                "sha": kernels_tree_sha,
                "truncated": false,
                "tree": [
                    { "path": "rms_norm.cu", "mode": "100644", "type": "blob", "sha": "b1", "size": 48 },
                    { "path": "rope.cuh", "mode": "100644", "type": "blob", "sha": "b2", "size": 42 },
                    { "path": "Cargo.lock", "mode": "100644", "type": "blob", "sha": "b3", "size": 9000 },
                    { "path": "weights.safetensors", "mode": "100644", "type": "blob", "sha": "b4", "size": 5000 }
                ]
            })
            .to_string()
            .into_bytes(),
        );
        mock.add_route(
            &format!("https://raw.githubusercontent.com/org/mono/{commit_sha}/csrc/kernels/rms_norm.cu"),
            200,
            &[],
            b"__global__ void rms_norm() {}\n".to_vec(),
        );
        mock.add_route(
            &format!("https://raw.githubusercontent.com/org/mono/{commit_sha}/csrc/kernels/rope.cuh"),
            200,
            &[],
            b"#pragma once\nvoid apply_rope();\n".to_vec(),
        );

        let out = fetch_repo_or_hub_resource(
            &format!("https://github.com/org/mono/tree/{commit_sha}/csrc/kernels"),
            &cfg,
            &mock,
        )
        .unwrap();

        assert_eq!(out.citation.cached_file_count, 2);
        assert!(out.citation.tier_badge.contains("Strategy A"));
        let req_urls: Vec<String> = mock.requests().into_iter().map(|r| r.url).collect();
        assert!(!req_urls
            .iter()
            .any(|u| u.ends_with(&format!("{commit_sha}?recursive=1"))));
    }

    #[test]
    fn test_strategy_b_codeload_streaming_tar_gz_early_abort_on_lexicographical_boundary() {
        let ws = TempWorkspace::new("strategy_b_abort");
        let cfg = ws.config();
        let mut mock = MockHttpsFetcher::new();

        let sha = "abcdefabcdefabcdefabcdefabcdefabcdefabcd";
        // Trigger Strategy B by returning > 64 blobs in Strategy A's recursive tree response!
        let mut many_blobs = Vec::new();
        for i in 0..70 {
            many_blobs.push(serde_json::json!({
                "path": format!("k_{i:03}.cu"),
                "mode": "100644",
                "type": "blob",
                "sha": format!("sha{i}"),
                "size": 64
            }));
        }
        mock.add_route(
            &format!("https://api.github.com/repos/vllm/vllm/git/trees/{sha}"),
            200,
            &[],
            serde_json::json!({
                "sha": sha,
                "truncated": false,
                "tree": [{ "path": "csrc", "mode": "040000", "type": "tree", "sha": "3333333333333333333333333333333333333333" }]
            })
            .to_string()
            .into_bytes(),
        );
        mock.add_route(
            "https://api.github.com/repos/vllm/vllm/git/trees/3333333333333333333333333333333333333333?recursive=1",
            200,
            &[],
            serde_json::json!({
                "sha": "3333333333333333333333333333333333333333",
                "truncated": false,
                "tree": many_blobs
            })
            .to_string()
            .into_bytes(),
        );

        // Build deterministic `git archive` unsigned-byte sorted `.tar.gz`:
        // Notice `csrc-ext/` ('-' = 0x2D < '/' = 0x2F) comes BEFORE `csrc/` and MUST NOT trigger
        // premature abort, whereas `docs/` ('d' > 'c') comes AFTER `csrc/` and MUST abort immediately!
        let mut tar_entries: Vec<(String, Vec<u8>)> = vec![
            (
                "vllm-abcdef/README.md".to_string(),
                b"# Root Readme\n".to_vec(),
            ),
            (
                "vllm-abcdef/csrc-ext/other.cu".to_string(),
                b"// before csrc/\n".to_vec(),
            ),
            (
                "vllm-abcdef/csrc/attn.cu".to_string(),
                b"__global__ void attn_k() {}\n".to_vec(),
            ),
            (
                "vllm-abcdef/csrc/sub/paged.cuh".to_string(),
                b"#pragma once\n// paged\n".to_vec(),
            ),
        ];
        // Append pseudo-random trailing monorepo payload AFTER `csrc/` so gzip stream has many chunks left.
        for idx in 0..80 {
            let mut trailing = Vec::with_capacity(2048);
            let mut state: u64 = 0x9E37_79B9_7F4A_7C15u64 ^ (idx as u64);
            for _ in 0..256 {
                state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
                trailing.extend_from_slice(&format!("{state:08x}").as_bytes()[..8]);
            }
            tar_entries.push((format!("vllm-abcdef/docs/doc_{idx:03}.md"), trailing));
        }
        let borrowed: Vec<(&str, &[u8])> = tar_entries
            .iter()
            .map(|(p, b)| (p.as_str(), b.as_slice()))
            .collect();
        let tar_gz = build_synthetic_tar_gz(&borrowed);
        let total_possible_chunks = (tar_gz.len() + 127) / 128;
        assert!(total_possible_chunks > 40);

        mock.add_streaming_route(
            &format!("https://codeload.github.com/vllm/vllm/tar.gz/{sha}"),
            200,
            128,
            tar_gz,
        );

        let out = fetch_repo_or_hub_resource(
            &format!("https://github.com/vllm/vllm/tree/{sha}/csrc"),
            &cfg,
            &mock,
        )
        .unwrap();

        assert!(*mock.stream_aborted.lock(), "codeload stream must abort early!");
        let delivered = mock.stream_chunks_delivered.load(Ordering::SeqCst);
        assert!(
            delivered < total_possible_chunks / 3,
            "Expected early abort well before EOF (delivered {delivered} of {total_possible_chunks} chunks)"
        );
        assert_eq!(out.citation.cached_file_count, 2);
        let root = out.staged_root.unwrap();
        assert!(root.join("csrc/attn.cu").exists());
        assert!(root.join("csrc/sub/paged.cuh").exists());
        assert!(!root.join("csrc-ext/other.cu").exists());
        assert!(!root.join("docs/doc_000.md").exists());
    }

    #[test]
    fn test_strategy_b_triggered_when_github_tree_api_returns_403_rate_limit() {
        let ws = TempWorkspace::new("rate_limit_failover");
        let cfg = ws.config();
        let mut mock = MockHttpsFetcher::new();
        let sha = "4444555566667777888899990000aaaabbbbcccc";

        mock.add_route(
            &format!("https://api.github.com/repos/org/lib/git/trees/{sha}"),
            403,
            &[("x-ratelimit-remaining", "0")],
            b"{\"message\":\"API rate limit exceeded\"}".to_vec(),
        );
        let tar_gz = build_synthetic_tar_gz(&[(
            "lib-4444/src/main.rs",
            b"fn main() { println!(\"zero quota ok\"); }\n",
        )]);
        mock.add_streaming_route(
            &format!("https://codeload.github.com/org/lib/tar.gz/{sha}"),
            200,
            256,
            tar_gz,
        );

        let out = fetch_repo_or_hub_resource(
            &format!("https://github.com/org/lib/tree/{sha}/src"),
            &cfg,
            &mock,
        )
        .unwrap();
        assert_eq!(out.citation.cached_file_count, 1);
        let content = fs::read_to_string(out.staged_root.unwrap().join("src/main.rs")).unwrap();
        assert!(content.contains("zero quota ok"));
    }

    #[test]
    fn test_github_pr_406_too_large_diff_falls_back_to_files_skipping_lockfiles() {
        let ws = TempWorkspace::new("pr_406");
        let cfg = ws.config();
        let mut mock = MockHttpsFetcher::new();

        mock.add_route(
            "https://api.github.com/repos/org/engine/pulls/42",
            200,
            &[("x-mock-match-accept", "application/vnd.github+json")],
            serde_json::json!({
                "title": "Add fused MLA Metal kernel",
                "state": "open",
                "merged": false,
                "user": { "login": "apollo" },
                "base": { "ref": "main", "sha": "base111" },
                "head": { "ref": "feat/mla", "sha": "head222" },
                "changed_files": 6,
                "additions": 120,
                "deletions": 14,
                "body": "Implements low-rank KV compression."
            })
            .to_string()
            .into_bytes(),
        );
        mock.add_route(
            "https://api.github.com/repos/org/engine/issues/42/comments?per_page=50",
            200,
            &[],
            serde_json::json!([{ "user": { "login": "reviewer1" }, "body": "Benchmarks look fast!" }])
                .to_string()
                .into_bytes(),
        );
        mock.add_route(
            "https://api.github.com/repos/org/engine/pulls/42/comments?per_page=50",
            200,
            &[],
            b"[]".to_vec(),
        );
        // Unified diff returns `406 Not Acceptable` ("too_large")!
        mock.add_route(
            "https://api.github.com/repos/org/engine/pulls/42",
            406,
            &[("x-mock-match-accept", "application/vnd.github.diff")],
            b"{\"message\":\"Sorry, the diff exceeded the maximum number of lines\",\"errors\":[{\"code\":\"too_large\"}]}".to_vec(),
        );
        // Fallback `/pulls/42/files?per_page=100` includes 5 lockfiles + 1 real source file!
        mock.add_route(
            "https://api.github.com/repos/org/engine/pulls/42/files?per_page=100",
            200,
            &[],
            serde_json::json!([
                { "filename": "Cargo.lock", "status": "modified", "additions": 5000, "deletions": 4000, "patch": "@@ lock noise 1 @@" },
                { "filename": "package-lock.json", "status": "modified", "additions": 9000, "deletions": 8000, "patch": "@@ lock noise 2 @@" },
                { "filename": "pnpm-lock.yaml", "status": "modified", "additions": 200, "deletions": 100, "patch": "@@ lock noise 3 @@" },
                { "filename": "uv.lock", "status": "modified", "additions": 300, "deletions": 100, "patch": "@@ lock noise 4 @@" },
                { "filename": "poetry.lock", "status": "modified", "additions": 400, "deletions": 100, "patch": "@@ lock noise 5 @@" },
                { "filename": "src/mla_kernel.rs", "status": "added", "additions": 25, "deletions": 0, "patch": "@@ -0,0 +1,2 @@\n+pub fn fused_mla() {}\n" }
            ])
            .to_string()
            .into_bytes(),
        );

        let out = fetch_repo_or_hub_resource(
            "https://github.com/org/engine/pull/42",
            &cfg,
            &mock,
        )
        .unwrap();

        let staged_doc = fs::read_to_string(out.staged_root.unwrap().join("document.md")).unwrap();
        assert!(staged_doc.contains("src/mla_kernel.rs"));
        assert!(staged_doc.contains("+pub fn fused_mla() {}"));
        assert!(staged_doc.contains("Benchmarks look fast!"));
        assert!(!staged_doc.contains("lock noise"));
        assert!(staged_doc.contains("Skipped 5 lockfile(s)"));
    }

    #[test]
    fn test_huggingface_safetensors_and_gguf_denylisted_with_zero_network_calls() {
        let ws = TempWorkspace::new("hf_denylist");
        let cfg = ws.config();
        let mock = MockHttpsFetcher::new();

        for bad_url in [
            "https://huggingface.co/Qwen/Qwen3.5-397B/blob/main/model-00001-of-00090.safetensors",
            "https://huggingface.co/bartowski/Model-GGUF/resolve/main/model-Q4_K_M.gguf",
            "https://huggingface.co/org/model/raw/main/pytorch_model.bin",
            "https://huggingface.co/datasets/org/ds/resolve/main/train-00000.parquet",
        ] {
            let err = fetch_repo_or_hub_resource(bad_url, &cfg, &mock).unwrap_err();
            assert!(
                matches!(err, OnlineFetchError::SizeOrBinaryBlocked { bytes: 0, .. }),
                "Expected 0-byte SizeOrBinaryBlocked for {bad_url}, got {err:?}"
            );
        }
        assert_eq!(mock.requests().len(), 0, "Zero network requests allowed for denylisted HF extensions!");
    }

    #[test]
    fn test_huggingface_x_linked_size_307_pre_redirect_blocks_multi_gb_file_zero_cdn_bytes() {
        let ws = TempWorkspace::new("hf_xlink_block");
        let cfg = ws.config();
        let mut mock = MockHttpsFetcher::new();

        // Non-denylisted extension (`.data`), but Hop-1 `/resolve/` returns `307` with `X-Linked-Size: 6.4 GiB`
        // while `Content-Length` on the redirect body itself is only 64 bytes!
        mock.add_route(
            "https://huggingface.co/org/custom-model/resolve/main/shard_00.data",
            307,
            &[
                ("location", "https://cas-bridge.xethub.hf.co/xet-bridge-us/huge-blob"),
                ("x-linked-size", "6871947673"),
                ("content-length", "64"),
            ],
            b"Temporary Redirect to Xet CDN".to_vec(),
        );
        mock.add_route(
            "https://cas-bridge.xethub.hf.co/xet-bridge-us/huge-blob",
            200,
            &[],
            b"SHOULD NEVER BE DOWNLOADED".to_vec(),
        );

        let err = fetch_repo_or_hub_resource(
            "https://huggingface.co/org/custom-model/blob/main/shard_00.data",
            &cfg,
            &mock,
        )
        .unwrap_err();

        match err {
            OnlineFetchError::SizeOrBinaryBlocked { bytes, .. } => {
                assert_eq!(bytes, 6_871_947_673);
            }
            other => panic!("Expected SizeOrBinaryBlocked, got {other:?}"),
        }

        // Assert Hop-1 used `follow_redirects == false` and Hop-2 CDN was NEVER contacted (`0` CDN requests)!
        let reqs = mock.requests();
        assert_eq!(reqs.len(), 1);
        assert!(!reqs[0].follow_redirects);
        assert_eq!(
            reqs[0].url,
            "https://huggingface.co/org/custom-model/resolve/main/shard_00.data"
        );
    }

    #[test]
    fn test_huggingface_resolve_allows_config_json_and_strips_auth_on_cdn_redirect() {
        let ws = TempWorkspace::new("hf_config_allow");
        let mut cfg = ws.config();
        cfg.huggingface_token = Some("hf_super_secret_token_999".to_string());

        let mut mock = MockHttpsFetcher::new();
        let hf_commit = "fedcba9876543210fedcba9876543210fedcba98";
        mock.add_route(
            "https://huggingface.co/Qwen/Qwen3.5-397B/resolve/main/config.json",
            302,
            &[
                ("location", "https://cdn-lfs.hf.co/repos/qwen/config_blob"),
                ("x-linked-size", "92"),
                ("x-repo-commit", hf_commit),
            ],
            b"Found".to_vec(),
        );
        mock.add_route(
            "https://cdn-lfs.hf.co/repos/qwen/config_blob",
            200,
            &[("content-length", "92")],
            br#"{"model_type":"qwen3_5_moe","num_hidden_layers":60,"num_experts":344}"#.to_vec(),
        );

        let out = fetch_repo_or_hub_resource(
            "https://huggingface.co/Qwen/Qwen3.5-397B/blob/main/config.json",
            &cfg,
            &mock,
        )
        .unwrap();

        let reqs = mock.requests();
        assert_eq!(reqs.len(), 2);
        assert!(!reqs[0].follow_redirects);
        assert!(reqs[0]
            .headers
            .iter()
            .any(|(k, v)| k == "Authorization" && v == "Bearer hf_super_secret_token_999"));
        // Hop 2 to `cdn-lfs.hf.co` MUST NOT leak `Authorization`!
        assert!(!reqs[1]
            .headers
            .iter()
            .any(|(k, _)| k.eq_ignore_ascii_case("authorization")));

        assert!(out.envelope_markdown.contains("qwen3_5_moe"));
        let staged_cfg = out.staged_root.unwrap().join("config.json");
        assert!(staged_cfg.exists());
        assert!(staged_cfg.to_string_lossy().contains("fedcba987654"));
    }

    #[test]
    fn test_gitlab_and_codeberg_single_file_fetch_with_ls_refs() {
        let ws = TempWorkspace::new("gitlab_codeberg");
        let cfg = ws.config();
        let mut mock = MockHttpsFetcher::new();

        let gl_sha = "1212121212121212121212121212121212121212";
        mock.add_route(
            "https://gitlab.com/group/sub/proj.git/git-upload-pack",
            200,
            &[],
            pkt_response(&[&format!("{gl_sha} refs/heads/release/2.0")]),
        );
        mock.add_route(
            &format!("https://gitlab.com/group/sub/proj/-/raw/{gl_sha}/src/lib.rs"),
            200,
            &[],
            b"pub fn gitlab_ok() -> bool { true }\n".to_vec(),
        );

        let gl_out = fetch_repo_or_hub_resource(
            "https://gitlab.com/group/sub/proj/-/blob/release/2.0/src/lib.rs#L1",
            &cfg,
            &mock,
        )
        .unwrap();
        assert!(gl_out.envelope_markdown.contains("gitlab_ok"));

        let cb_sha = "3434343434343434343434343434343434343434";
        mock.add_route(
            "https://codeberg.org/forgejo/runner.git/git-upload-pack",
            200,
            &[],
            pkt_response(&[&format!("{cb_sha} refs/heads/main")]),
        );
        mock.add_route(
            &format!("https://codeberg.org/forgejo/runner/raw/commit/{cb_sha}/main.go"),
            200,
            &[],
            b"package main\nfunc main() {}\n".to_vec(),
        );

        let cb_out = fetch_repo_or_hub_resource(
            "https://codeberg.org/forgejo/runner/raw/branch/main/main.go",
            &cfg,
            &mock,
        )
        .unwrap();
        assert!(cb_out.envelope_markdown.contains("package main"));
        assert!(ws.path().join(".spell/research_cache/repos").exists());
    }

    #[test]
    fn test_huggingface_tree_skips_weights_and_stages_modeling_and_jinja() {
        let ws = TempWorkspace::new("hf_tree");
        let cfg = ws.config();
        let mut mock = MockHttpsFetcher::new();

        mock.add_route(
            "https://huggingface.co/api/models/Qwen/Qwen3.5/tree/main",
            200,
            &[],
            serde_json::json!([
                { "type": "file", "path": "model-00001.safetensors", "size": 5000000000u64, "lfs": { "size": 5000000000u64 } },
                { "type": "file", "path": "chat_template.jinja", "size": 42 },
                { "type": "file", "path": "modeling_qwen3_5.py", "size": 55 }
            ])
            .to_string()
            .into_bytes(),
        );
        mock.add_route(
            "https://huggingface.co/Qwen/Qwen3.5/resolve/main/chat_template.jinja",
            200,
            &[("x-linked-size", "42")],
            b"{{ bos_token }}{% for m in messages %}{% endfor %}\n".to_vec(),
        );
        mock.add_route(
            "https://huggingface.co/Qwen/Qwen3.5/resolve/main/modeling_qwen3_5.py",
            200,
            &[("x-linked-size", "55")],
            b"class Qwen35ForCausalLM:\n    pass\n".to_vec(),
        );

        let out = fetch_repo_or_hub_resource("https://huggingface.co/Qwen/Qwen3.5", &cfg, &mock)
            .unwrap();
        assert_eq!(out.citation.cached_file_count, 2);
        let root = out.staged_root.unwrap();
        assert!(root.join("chat_template.jinja").exists());
        assert!(root.join("modeling_qwen3_5.py").exists());
        assert!(!root.join("model-00001.safetensors").exists());
    }

    #[test]
    fn test_github_issue_and_commit_fetch_skipping_lockfiles() {
        let ws = TempWorkspace::new("issue_commit");
        let cfg = ws.config();
        let mut mock = MockHttpsFetcher::new();

        mock.add_route(
            "https://api.github.com/repos/org/proj/issues/7",
            200,
            &[],
            serde_json::json!({
                "title": "GDN state precision regression",
                "state": "open",
                "user": { "login": "alice" },
                "body": "Reproduces at 128K context."
            })
            .to_string()
            .into_bytes(),
        );
        mock.add_route(
            "https://api.github.com/repos/org/proj/issues/7/comments?per_page=50",
            200,
            &[],
            serde_json::json!([{ "user": { "login": "bob" }, "body": "Fixed in fp32 accumulator." }])
                .to_string()
                .into_bytes(),
        );

        let issue_out =
            fetch_repo_or_hub_resource("https://github.com/org/proj/issues/7", &cfg, &mock)
                .unwrap();
        assert!(issue_out.envelope_markdown.contains("fp32 accumulator"));

        let csha = "7777777777777777777777777777777777777777";
        mock.add_route(
            &format!("https://api.github.com/repos/org/proj/commits/{csha}"),
            200,
            &[],
            serde_json::json!({
                "sha": csha,
                "commit": {
                    "message": "Fix GDN accumulator",
                    "author": { "name": "Bob", "date": "2026-11-15T00:00:00Z" }
                },
                "files": [
                    { "filename": "Cargo.lock", "status": "modified", "patch": "@@ lock @@" },
                    { "filename": "src/gdn.rs", "status": "modified", "patch": "@@ -1 +1 @@\n+let acc: f32 = 0.0;" }
                ]
            })
            .to_string()
            .into_bytes(),
        );

        let commit_out = fetch_repo_or_hub_resource(
            &format!("https://github.com/org/proj/commit/{csha}"),
            &cfg,
            &mock,
        )
        .unwrap();
        assert!(commit_out.envelope_markdown.contains("src/gdn.rs"));
        assert!(!commit_out.envelope_markdown.contains("@@ lock @@"));
    }
}
