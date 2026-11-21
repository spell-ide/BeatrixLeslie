use std::collections::BTreeMap;
#[cfg(not(test))]
use std::time::{Duration, Instant};

use once_cell::sync::Lazy;
#[cfg(not(test))]
use parking_lot::Mutex;
use regex::Regex;
use serde::Serialize;

use super::archive_tar::ExtractedArchiveFile;
use super::html_clean::{
    build_markdown_toc, decode_html_entities, emit_dynamic_fenced_code,
    extract_clean_markdown_and_toc, extract_latexml_authors_and_date, has_latexml_equation_error,
    normalize_markdown_blank_lines, sanitize_untrusted_text,
};
use super::{
    HttpsFetcher, OnlineFetchError, OnlineFetchOutcome, OnlineResearchConfig, OutboundHttpRequest,
};

const MAX_EPRINT_COMPRESSED_BYTES: usize = 24 * 1024 * 1024;
const MAX_EPRINT_TAR_BYTES: usize = 16 * 1024 * 1024;
const MAX_PDF_BYTES: usize = 32 * 1024 * 1024;
const MAX_TEX_INPUT_DEPTH: usize = 8;

#[cfg(not(test))]
static ARXIV_API_RATE_GATE: Lazy<Mutex<Option<Instant>>> = Lazy::new(|| Mutex::new(None));

static RE_MODERN_ARXIV: Lazy<Regex> = Lazy::new(|| {
    Regex::new(r"(?i)^(\d{4}\.\d{4,5})(?:v(\d+))?$").expect("valid modern arXiv regex")
});

static RE_LEGACY_ARXIV: Lazy<Regex> = Lazy::new(|| {
    Regex::new(r"(?i)^([a-z\-]+(?:\.[a-z]{2})?/\d{7})(?:v(\d+))?$")
        .expect("valid legacy arXiv regex")
});

static RE_ARXIV_IN_TEXT: Lazy<Regex> = Lazy::new(|| {
    Regex::new(
        r"(?i)(?:arxiv\.org/(?:abs|pdf|html|e-print)/|ar5iv\.labs\.arxiv\.org/html/|10\.48550/arxiv\.|arxiv:)((?:\d{4}\.\d{4,5}|[a-z\-]+(?:\.[a-z]{2})?/\d{7})(?:v\d+)?)",
    )
    .expect("valid arXiv text scanner")
});

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArxivPaperId {
    /// Base identifier without version suffix (e.g. `"2502.12345"` or `"hep-th/9901001"`).
    pub base_id: String,
    /// Optional explicit version number (e.g. `Some(2)` for `v2`).
    pub version: Option<u32>,
}

impl ArxivPaperId {
    pub fn id_with_version(&self) -> String {
        match self.version {
            Some(v) => format!("{}v{v}", self.base_id),
            None => self.base_id.clone(),
        }
    }

    pub fn canonical_abs_url(&self) -> String {
        format!("https://arxiv.org/abs/{}", self.id_with_version())
    }
}

#[derive(Debug, Clone, Default)]
struct PaperMetadata {
    title: Option<String>,
    authors: Vec<String>,
    abstract_text: Option<String>,
    published: Option<String>,
    doi: Option<String>,
    arxiv_id: Option<ArxivPaperId>,
    oa_pdf_url: Option<String>,
}

impl PaperMetadata {
    fn merge_missing_from(&mut self, other: PaperMetadata) {
        if self.title.is_none() {
            self.title = other.title;
        }
        if self.authors.is_empty() && !other.authors.is_empty() {
            self.authors = other.authors;
        }
        if self.abstract_text.is_none() {
            self.abstract_text = other.abstract_text;
        }
        if self.published.is_none() {
            self.published = other.published;
        }
        if self.doi.is_none() {
            self.doi = other.doi;
        }
        if self.arxiv_id.is_none() {
            self.arxiv_id = other.arxiv_id;
        }
        if self.oa_pdf_url.is_none() {
            self.oa_pdf_url = other.oa_pdf_url;
        }
    }
}

#[derive(Debug, Clone)]
enum PaperVenueTarget {
    Arxiv(ArxivPaperId),
    Doi(String),
    OpenReview { forum_id: String, raw_url: String },
    AclAnthology { acl_id: String, raw_url: String },
    Pmlr { raw_url: String },
}

#[derive(Debug, Clone, Serialize)]
struct BibSidecarEntry {
    id: String,
    cite_key: String,
    text: String,
}

/// Parses and normalizes modern (`2502.12345v2`) and legacy (`hep-th/9901001`) `arXiv` identifiers
/// from `arxiv.org/{abs,pdf,html,e-print,format}/...`, `ar5iv.labs.arxiv.org/html/...`,
/// `doi.org/10.48550/arXiv....`, `arXiv:...`, or bare identifier strings.
pub fn parse_arxiv_id(raw: &str) -> Option<ArxivPaperId> {
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return None;
    }

    // Strip query string and fragment
    let no_qf = trimmed.split(['?', '#']).next().unwrap_or(trimmed).trim();
    let no_slash = no_qf.trim_end_matches('/');

    // Check `arxiv:` prefix
    if let Some(rest) = strip_prefix_ignore_case(no_slash, "arxiv:") {
        return parse_bare_arxiv_token(rest.trim_start_matches('/'));
    }

    // Check `doi:10.48550/arXiv.<id>` or `https://doi.org/10.48550/arXiv.<id>`
    if let Some(doi) = parse_doi_string(no_slash) {
        if let Some(rest) = strip_prefix_ignore_case(&doi, "10.48550/arxiv.") {
            return parse_bare_arxiv_token(rest);
        }
    }

    // Check URL forms on arxiv.org / export.arxiv.org / ar5iv
    if let Some(after_scheme) = no_slash
        .strip_prefix("https://")
        .or_else(|| no_slash.strip_prefix("http://"))
    {
        if let Some((host, path)) = after_scheme.split_once('/') {
            let low_host = host.to_ascii_lowercase();
            if low_host == "arxiv.org"
                || low_host.ends_with(".arxiv.org")
                || low_host == "ar5iv.org"
                || low_host.ends_with(".ar5iv.org")
            {
                let clean_path = path.trim_matches('/');
                for prefix in ["abs/", "pdf/", "html/", "e-print/", "format/"] {
                    if let Some(rest) = strip_prefix_ignore_case(clean_path, prefix) {
                        return parse_bare_arxiv_token(rest);
                    }
                }
            }
        }
        return None;
    }

    parse_bare_arxiv_token(no_slash)
}

fn parse_bare_arxiv_token(token: &str) -> Option<ArxivPaperId> {
    let mut s = token.trim().trim_matches('/');
    for ext in [".pdf", ".html", ".htm", ".gz", ".tar.gz"] {
        if s.len() > ext.len() && s[s.len() - ext.len()..].eq_ignore_ascii_case(ext) {
            s = &s[..s.len() - ext.len()];
            break;
        }
    }
    s = s.trim_matches('/');

    if let Some(caps) = RE_MODERN_ARXIV.captures(s) {
        let base_id = caps.get(1)?.as_str().to_string();
        let version = caps.get(2).and_then(|m| m.as_str().parse::<u32>().ok());
        return Some(ArxivPaperId { base_id, version });
    }

    if let Some(caps) = RE_LEGACY_ARXIV.captures(s) {
        let raw_base = caps.get(1)?.as_str();
        // Normalize archive prefix lowercase while preserving subject class letters if needed
        let base_id = if let Some((arch, num)) = raw_base.split_once('/') {
            let norm_arch = if let Some((cat, sub)) = arch.split_once('.') {
                format!("{}.{}", cat.to_ascii_lowercase(), sub.to_ascii_uppercase())
            } else {
                arch.to_ascii_lowercase()
            };
            format!("{norm_arch}/{num}")
        } else {
            raw_base.to_ascii_lowercase()
        };
        let version = caps.get(2).and_then(|m| m.as_str().parse::<u32>().ok());
        return Some(ArxivPaperId { base_id, version });
    }

    None
}

fn strip_prefix_ignore_case<'a>(s: &'a str, prefix: &str) -> Option<&'a str> {
    if s.len() >= prefix.len()
        && s.is_char_boundary(prefix.len())
        && s.as_bytes()[..prefix.len()].eq_ignore_ascii_case(prefix.as_bytes())
    {
        Some(&s[prefix.len()..])
    } else {
        None
    }
}

fn parse_doi_string(raw: &str) -> Option<String> {
    let trimmed = raw.trim();
    let rest = if let Some(r) = strip_prefix_ignore_case(trimmed, "doi:") {
        r.trim()
    } else if let Some(r) = strip_prefix_ignore_case(trimmed, "https://doi.org/")
        .or_else(|| strip_prefix_ignore_case(trimmed, "http://doi.org/"))
        .or_else(|| strip_prefix_ignore_case(trimmed, "https://dx.doi.org/"))
        .or_else(|| strip_prefix_ignore_case(trimmed, "http://dx.doi.org/"))
    {
        r.trim()
    } else {
        return None;
    };

    let no_frag = rest.split('#').next().unwrap_or(rest).trim_matches('/');
    let decoded = percent_decode_basic(no_frag);
    if decoded.starts_with("10.") && decoded.contains('/') {
        Some(decoded)
    } else {
        None
    }
}

/// Returns `true` when `raw` targets an academic paper venue (`arXiv`, `ar5iv`, `DOI`,
/// `OpenReview`, `ACL Anthology`, or `PMLR`).
pub fn is_academic_paper_url(raw: &str) -> bool {
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return false;
    }
    let low = trimmed.to_ascii_lowercase();
    if low.starts_with("arxiv:") && parse_arxiv_id(trimmed).is_some() {
        return true;
    }
    if low.starts_with("doi:10.")
        || low.starts_with("https://doi.org/")
        || low.starts_with("http://doi.org/")
        || low.starts_with("https://dx.doi.org/")
        || low.starts_with("doi:")
    {
        return true;
    }
    low.contains("arxiv.org/")
        || low.contains("ar5iv.labs.arxiv.org/")
        || low.contains("ar5iv.org/")
        || low.contains("openreview.net/")
        || low.contains("aclanthology.org/")
        || low.contains("proceedings.mlr.press/")
}

fn classify_paper_target(raw_url: &str) -> Result<PaperVenueTarget, OnlineFetchError> {
    let trimmed = raw_url.trim();
    if let Some(arxiv_id) = parse_arxiv_id(trimmed) {
        return Ok(PaperVenueTarget::Arxiv(arxiv_id));
    }
    if let Some(doi) = parse_doi_string(trimmed) {
        return Ok(PaperVenueTarget::Doi(doi));
    }

    let low = trimmed.to_ascii_lowercase();
    if low.contains("openreview.net/") {
        if let Some(forum_id) = extract_query_param(trimmed, "id") {
            return Ok(PaperVenueTarget::OpenReview {
                forum_id,
                raw_url: trimmed.to_string(),
            });
        }
        return Err(OnlineFetchError::ExtractionError(format!(
            "Could not extract OpenReview forum `id` from '{trimmed}'"
        )));
    }

    if low.contains("aclanthology.org/") {
        if let Some(acl_id) = extract_acl_anthology_id(trimmed) {
            return Ok(PaperVenueTarget::AclAnthology {
                acl_id,
                raw_url: trimmed.to_string(),
            });
        }
    }

    if low.contains("proceedings.mlr.press/") {
        return Ok(PaperVenueTarget::Pmlr {
            raw_url: trimmed.to_string(),
        });
    }

    Err(OnlineFetchError::ExtractionError(format!(
        "Unrecognized academic paper URL or identifier: '{trimmed}'"
    )))
}

fn extract_query_param(url: &str, key: &str) -> Option<String> {
    let without_frag = url.split('#').next().unwrap_or(url);
    let (_, query) = without_frag.split_once('?')?;
    for pair in query.split('&') {
        let (k, v) = pair.split_once('=')?;
        if k.eq_ignore_ascii_case(key) && !v.trim().is_empty() {
            return Some(percent_decode_basic(v.trim()));
        }
    }
    None
}

fn extract_acl_anthology_id(url: &str) -> Option<String> {
    let no_qf = url.split(['?', '#']).next().unwrap_or(url);
    let after_host = no_qf.split("aclanthology.org/").nth(1)?;
    let seg = after_host.trim_matches('/');
    let id = seg.strip_suffix(".pdf").unwrap_or(seg).trim_matches('/');
    if id.is_empty() {
        None
    } else {
        Some(id.to_string())
    }
}

fn is_hard_stop_error(err: &OnlineFetchError) -> bool {
    matches!(
        err,
        OnlineFetchError::OfflineAirGapped(_)
            | OnlineFetchError::SecurityBlocked(_)
            | OnlineFetchError::DomainApprovalRequired { .. }
            | OnlineFetchError::TaintConfirmationRequired { .. }
            | OnlineFetchError::ChallengeOrRateLimited { .. }
    )
}

/// Executes the 4-Tier Academic Paper Cascade:
/// - **Tier 0:** `export.arxiv.org` Atom XML + `OpenAlex` / `OpenReview` / `Semantic Scholar` cross-venue resolver
///   (automatically upgrading DOI / OpenReview / ACL / PMLR papers to lossless `arXiv` HTML5 / TeX when mapped).
/// - **Tier 1:** Native LaTeXML HTML5 (`arxiv.org/html/{id_v}` -> `ar5iv.labs.arxiv.org/html/{id}` with `/abs/` redirect & `ltx_ERROR` guards).
/// - **Tier 2:** Official `export.arxiv.org/e-print/{id_v}` `.tar.gz` / `.tex` source with recursive `\input{}` (`depth <= 8`) & `.bbl`/`.bib`.
/// - **Tier 4:** CPU-only `PDFKit` extractor hook (`current_pdf_extractor_hook()`) + pure-Rust `BT..ET` / `FlateDecode` stream fallback.
pub fn fetch_and_extract_paper(
    raw_url: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let target = classify_paper_target(raw_url)?;
    let mut meta = PaperMetadata::default();
    let mut fallback_pdf_url: Option<String> = None;

    match target {
        PaperVenueTarget::Arxiv(arxiv_id) => {
            meta.arxiv_id = Some(arxiv_id.clone());
            if let Some(atom_meta) = query_arxiv_atom_metadata(&arxiv_id, cfg, fetcher)? {
                meta.merge_missing_from(atom_meta.clone());
                if arxiv_id.version.is_none() {
                    if let Some(resolved_id) = atom_meta.arxiv_id {
                        meta.arxiv_id = Some(resolved_id);
                    }
                }
            }
        }
        PaperVenueTarget::Doi(doi) => {
            meta.doi = Some(doi.clone());
            if let Some(oa_meta) = query_openalex_by_doi(&doi, cfg, fetcher)? {
                meta.merge_missing_from(oa_meta);
            }
            if meta.arxiv_id.is_none() {
                let s2_key = format!("DOI:{}", url_encode_component(&doi));
                if let Some(s2_meta) = query_semantic_scholar(&s2_key, cfg, fetcher)? {
                    meta.merge_missing_from(s2_meta);
                }
            }
            if let Some(ref upgraded_arxiv) = meta.arxiv_id.clone() {
                if let Some(atom_meta) = query_arxiv_atom_metadata(upgraded_arxiv, cfg, fetcher)? {
                    if upgraded_arxiv.version.is_none() {
                        if let Some(ref v_id) = atom_meta.arxiv_id {
                            meta.arxiv_id = Some(v_id.clone());
                        }
                    }
                    meta.merge_missing_from(atom_meta);
                }
            } else {
                fallback_pdf_url = meta.oa_pdf_url.clone();
            }
        }
        PaperVenueTarget::OpenReview {
            forum_id,
            raw_url: orig,
        } => {
            fallback_pdf_url = Some(format!("https://openreview.net/pdf?id={forum_id}"));
            if let Some(or_meta) = query_openreview_note(&forum_id, cfg, fetcher)? {
                meta.merge_missing_from(or_meta);
            }
            if meta.arxiv_id.is_none() {
                if let Some(ref doi) = meta.doi.clone() {
                    if let Some(oa_meta) = query_openalex_by_doi(doi, cfg, fetcher)? {
                        meta.merge_missing_from(oa_meta);
                    }
                }
            }
            if meta.arxiv_id.is_none() {
                let s2_key = format!(
                    "URL:{}",
                    url_encode_component(&format!("https://openreview.net/forum?id={forum_id}"))
                );
                if let Some(s2_meta) = query_semantic_scholar(&s2_key, cfg, fetcher)? {
                    meta.merge_missing_from(s2_meta);
                }
            }
            if let Some(ref upgraded_arxiv) = meta.arxiv_id.clone() {
                if let Some(atom_meta) = query_arxiv_atom_metadata(upgraded_arxiv, cfg, fetcher)? {
                    meta.merge_missing_from(atom_meta);
                }
            } else if orig.to_ascii_lowercase().contains("/pdf") {
                fallback_pdf_url = Some(orig);
            }
        }
        PaperVenueTarget::AclAnthology {
            acl_id,
            raw_url: orig,
        } => {
            fallback_pdf_url = Some(format!("https://aclanthology.org/{acl_id}.pdf"));
            let acl_doi = format!("10.18653/v1/{acl_id}");
            if let Some(oa_meta) = query_openalex_by_doi(&acl_doi, cfg, fetcher)? {
                meta.merge_missing_from(oa_meta);
            }
            if meta.arxiv_id.is_none() {
                let s2_key = format!("ACL:{acl_id}");
                if let Some(s2_meta) = query_semantic_scholar(&s2_key, cfg, fetcher)? {
                    meta.merge_missing_from(s2_meta);
                }
            }
            if let Some(ref upgraded_arxiv) = meta.arxiv_id.clone() {
                if let Some(atom_meta) = query_arxiv_atom_metadata(upgraded_arxiv, cfg, fetcher)? {
                    meta.merge_missing_from(atom_meta);
                }
            } else if orig.to_ascii_lowercase().ends_with(".pdf") {
                fallback_pdf_url = Some(orig);
            }
        }
        PaperVenueTarget::Pmlr { raw_url: orig } => {
            if orig.to_ascii_lowercase().ends_with(".pdf") {
                fallback_pdf_url = Some(orig.clone());
            } else {
                // Inspect PMLR landing page for citation_pdf_url or arXiv link
                if let Some((pmlr_meta, pdf_u)) = inspect_pmlr_landing_page(&orig, cfg, fetcher)? {
                    meta.merge_missing_from(pmlr_meta);
                    fallback_pdf_url = pdf_u;
                }
            }
            if meta.arxiv_id.is_none() {
                let s2_key = format!("URL:{}", url_encode_component(&orig));
                if let Some(s2_meta) = query_semantic_scholar(&s2_key, cfg, fetcher)? {
                    meta.merge_missing_from(s2_meta);
                }
            }
            if fallback_pdf_url.is_none() {
                fallback_pdf_url = meta.oa_pdf_url.clone();
            }
        }
    }

    // If we have an `arXiv` ID (either direct or auto-upgraded via Tier 0), execute Tier 1 -> Tier 2 -> Tier 4!
    if let Some(arxiv_id) = meta.arxiv_id.clone() {
        return execute_arxiv_cascade(&arxiv_id, meta, cfg, fetcher);
    }

    // Non-arXiv venue without arXiv mapping -> Tier 4 PDF fallback
    let pdf_url = fallback_pdf_url
        .or_else(|| meta.oa_pdf_url.clone())
        .ok_or_else(|| {
            OnlineFetchError::ExtractionError(format!(
                "No open-access arXiv HTML/TeX or PDF URL discovered for '{}'",
                raw_url.trim()
            ))
        })?;

    let canonical = if let Some(ref doi) = meta.doi {
        format!("https://doi.org/{doi}")
    } else {
        raw_url.trim().to_string()
    };

    extract_tier4_pdf_from_url(&pdf_url, &canonical, None, meta, cfg, fetcher)
}

fn execute_arxiv_cascade(
    arxiv_id: &ArxivPaperId,
    mut meta: PaperMetadata,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let canonical_url = arxiv_id.canonical_abs_url();
    let id_v = arxiv_id.id_with_version();

    // ------------------------------------------------------------------------
    // Tier 1A: Official `https://arxiv.org/html/{id_v}`
    // ------------------------------------------------------------------------
    let arxiv_html_url = format!("https://arxiv.org/html/{id_v}");
    let req_t1a = OutboundHttpRequest::get(arxiv_html_url);
    match fetcher.execute(&req_t1a, cfg) {
        Ok(resp) => {
            if resp.status == 200
                && !is_abs_redirect_url(&resp.final_url)
                && is_valid_tier1_latexml_html(&resp.body)
            {
                return finalize_tier1_html_outcome(
                    &resp.body,
                    &canonical_url,
                    "arXiv HTML5 · Lossless Math",
                    &mut meta,
                    cfg,
                );
            }
        }
        Err(err) if is_hard_stop_error(&err) => return Err(err),
        Err(_) => {}
    }

    // ------------------------------------------------------------------------
    // Tier 1B: `https://ar5iv.labs.arxiv.org/html/{id}` with `follow_redirects = false`
    // ------------------------------------------------------------------------
    let ar5iv_url = format!("https://ar5iv.labs.arxiv.org/html/{}", arxiv_id.base_id);
    let mut req_t1b = OutboundHttpRequest::get(ar5iv_url);
    req_t1b.follow_redirects = false;
    match fetcher.execute(&req_t1b, cfg) {
        Ok(resp) => {
            // 302/307 redirect to `/abs/` means ar5iv does not have a valid build -> fall through immediately!
            if resp.status == 200
                && !is_abs_redirect_url(&resp.final_url)
                && is_valid_tier1_latexml_html(&resp.body)
            {
                return finalize_tier1_html_outcome(
                    &resp.body,
                    &canonical_url,
                    "ar5iv HTML5 · Lossless Math",
                    &mut meta,
                    cfg,
                );
            } else if (300..400).contains(&resp.status) {
                if let Some(loc) = resp.header("location") {
                    if !is_abs_redirect_url(loc) && loc.contains("/html/") {
                        let resolved_hop = if loc.starts_with("https://") {
                            loc.to_string()
                        } else if loc.starts_with('/') {
                            format!("https://ar5iv.labs.arxiv.org{loc}")
                        } else {
                            String::new()
                        };
                        if !resolved_hop.is_empty() {
                            let mut hop_req = OutboundHttpRequest::get(resolved_hop);
                            hop_req.follow_redirects = false;
                            match fetcher.execute(&hop_req, cfg) {
                                Ok(hop_resp)
                                    if hop_resp.status == 200
                                        && !is_abs_redirect_url(&hop_resp.final_url)
                                        && is_valid_tier1_latexml_html(&hop_resp.body) =>
                                {
                                    return finalize_tier1_html_outcome(
                                        &hop_resp.body,
                                        &canonical_url,
                                        "ar5iv HTML5 · Lossless Math",
                                        &mut meta,
                                        cfg,
                                    );
                                }
                                Err(err) if is_hard_stop_error(&err) => return Err(err),
                                _ => {}
                            }
                        }
                    }
                }
            }
        }
        Err(err) if is_hard_stop_error(&err) => return Err(err),
        Err(_) => {}
    }

    // ------------------------------------------------------------------------
    // Tier 2: Official Streaming TeX Source (`https://export.arxiv.org/e-print/{id_v}`)
    // ------------------------------------------------------------------------
    let mut preloaded_pdf_bytes: Option<Vec<u8>> = None;
    let eprint_url = format!("https://export.arxiv.org/e-print/{id_v}");
    let mut req_t2 = OutboundHttpRequest::get(eprint_url);
    req_t2.max_bytes = MAX_EPRINT_COMPRESSED_BYTES;

    match fetcher.execute(&req_t2, cfg) {
        Ok(resp) if resp.status == 200 && !resp.body.is_empty() => {
            match unpack_and_extract_eprint_payload(&resp.body, &mut meta) {
                Ok(EprintPayloadResult::Markdown { markdown, bib_json }) => {
                    let clean_md = sanitize_untrusted_text(&markdown);
                    let toc = build_markdown_toc(&clean_md);
                    let title = meta
                        .title
                        .clone()
                        .unwrap_or_else(|| format!("arXiv:{id_v}"));
                    let mut outcome = super::staging::stage_single_document_and_build_envelope(
                        &title,
                        &canonical_url,
                        "arXiv e-print TeX",
                        &clean_md,
                        toc,
                        bib_json.as_deref(),
                        cfg,
                    )?;
                    apply_metadata_to_outcome(&mut outcome, &meta);
                    return Ok(outcome);
                }
                Ok(EprintPayloadResult::EmbeddedPdf(pdf_bytes)) => {
                    preloaded_pdf_bytes = Some(pdf_bytes);
                }
                Err(err) if is_hard_stop_error(&err) => return Err(err),
                Err(_) => {}
            }
        }
        Ok(_) => {}
        Err(err) if is_hard_stop_error(&err) => return Err(err),
        Err(_) => {}
    }

    // ------------------------------------------------------------------------
    // Tier 4: CPU-Only PDF Fallback (`https://arxiv.org/pdf/{id_v}.pdf`)
    // ------------------------------------------------------------------------
    let pdf_url = format!("https://arxiv.org/pdf/{id_v}.pdf");
    extract_tier4_pdf_from_url(
        &pdf_url,
        &canonical_url,
        preloaded_pdf_bytes,
        meta,
        cfg,
        fetcher,
    )
}

fn is_abs_redirect_url(url: &str) -> bool {
    let low = url.to_ascii_lowercase();
    low.contains("/abs/") || low.ends_with("/abs")
}

fn is_valid_tier1_latexml_html(body: &[u8]) -> bool {
    if body.is_empty() {
        return false;
    }
    let text = String::from_utf8_lossy(body);
    let low = text.to_ascii_lowercase();
    let has_doc_container = low.contains("ltx_document") || low.contains("<article");
    if !has_doc_container {
        return false;
    }
    // Guard against `/abs/` abstract pages disguised with an `<article>` wrapper
    if !low.contains("ltx_document") && low.contains("blockquote class=\"abstract") {
        return false;
    }
    !has_latexml_equation_error(body)
}

fn finalize_tier1_html_outcome(
    html_bytes: &[u8],
    canonical_url: &str,
    tier_badge: &str,
    meta: &mut PaperMetadata,
    cfg: &OnlineResearchConfig,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let cleaned = extract_clean_markdown_and_toc(html_bytes, canonical_url)?;
    if meta.authors.is_empty() || meta.published.is_none() {
        let (dom_authors, dom_date) = extract_latexml_authors_and_date(html_bytes);
        if meta.authors.is_empty() && !dom_authors.is_empty() {
            meta.authors = dom_authors;
        }
        if meta.published.is_none() {
            meta.published = dom_date;
        }
    }
    let title = if !cleaned.title.is_empty() && cleaned.title != canonical_url {
        cleaned.title
    } else {
        meta.title
            .clone()
            .unwrap_or_else(|| canonical_url.to_string())
    };
    let mut outcome = super::staging::stage_single_document_and_build_envelope(
        &title,
        canonical_url,
        tier_badge,
        &cleaned.markdown,
        cleaned.toc,
        cleaned.bib_json.as_deref(),
        cfg,
    )?;
    apply_metadata_to_outcome(&mut outcome, meta);
    Ok(outcome)
}

fn apply_metadata_to_outcome(outcome: &mut OnlineFetchOutcome, meta: &PaperMetadata) {
    if !meta.authors.is_empty() {
        outcome.citation.authors = meta
            .authors
            .iter()
            .map(|a| sanitize_untrusted_text(a).trim().to_string())
            .filter(|a| !a.is_empty())
            .collect();
    }
    if let Some(ref pub_date) = meta.published {
        let clean = sanitize_untrusted_text(pub_date).trim().to_string();
        if !clean.is_empty() {
            outcome.citation.published = Some(clean);
        }
    }
}

// ============================================================================
// Tier 0: Atom XML, OpenAlex, OpenReview & Semantic Scholar Resolvers
// ============================================================================

fn enforce_arxiv_api_polite_spacing() {
    #[cfg(not(test))]
    {
        let min_gap = Duration::from_millis(3_100);
        let mut guard = ARXIV_API_RATE_GATE.lock();
        if let Some(prev) = *guard {
            let elapsed = prev.elapsed();
            if elapsed < min_gap {
                std::thread::sleep(min_gap - elapsed);
            }
        }
        *guard = Some(Instant::now());
    }
}

fn query_arxiv_atom_metadata(
    arxiv_id: &ArxivPaperId,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<Option<PaperMetadata>, OnlineFetchError> {
    enforce_arxiv_api_polite_spacing();
    let query_id = arxiv_id.id_with_version();
    let url = format!("https://export.arxiv.org/api/query?id_list={query_id}");
    let mut req = OutboundHttpRequest::get(url);
    req.max_bytes = 2 * 1024 * 1024;

    match fetcher.execute(&req, cfg) {
        Ok(resp) if resp.status == 200 => {
            let xml = String::from_utf8_lossy(&resp.body);
            Ok(parse_arxiv_atom_xml(&xml))
        }
        Ok(_) => Ok(None),
        Err(err) if is_hard_stop_error(&err) => Err(err),
        Err(_) => Ok(None),
    }
}

fn parse_arxiv_atom_xml(xml: &str) -> Option<PaperMetadata> {
    let entry_block = extract_xml_tag_body(xml, "entry")?;
    let mut meta = PaperMetadata::default();

    if let Some(raw_title) = extract_xml_tag_body(entry_block, "title") {
        let clean = collapse_spaces(&decode_html_entities(raw_title));
        if !clean.is_empty() && !clean.eq_ignore_ascii_case("error") {
            meta.title = Some(sanitize_untrusted_text(&clean));
        }
    }

    if let Some(raw_summary) = extract_xml_tag_body(entry_block, "summary") {
        let clean = collapse_spaces(&decode_html_entities(raw_summary));
        if !clean.is_empty() {
            meta.abstract_text = Some(sanitize_untrusted_text(&clean));
        }
    }

    if let Some(raw_pub) = extract_xml_tag_body(entry_block, "published") {
        let trimmed = raw_pub.trim();
        let date_part = if trimmed.len() >= 10 && trimmed.as_bytes()[4] == b'-' {
            &trimmed[..10]
        } else {
            trimmed
        };
        if !date_part.is_empty() {
            meta.published = Some(sanitize_untrusted_text(date_part));
        }
    }

    if let Some(raw_id_url) = extract_xml_tag_body(entry_block, "id") {
        if let Some(parsed) = parse_arxiv_id(raw_id_url.trim()) {
            meta.arxiv_id = Some(parsed);
        }
    }

    if let Some(raw_doi) = extract_xml_tag_body(entry_block, "arxiv:doi") {
        let d = decode_html_entities(raw_doi).trim().to_string();
        if !d.is_empty() {
            meta.doi = Some(sanitize_untrusted_text(&d));
        }
    }

    // Collect all `<author>...<name>...</name>...</author>` blocks
    let mut cursor = entry_block;
    while let Some((author_body, rest)) = extract_next_xml_tag_body(cursor, "author") {
        if let Some(name_raw) = extract_xml_tag_body(author_body, "name") {
            let clean = collapse_spaces(&decode_html_entities(name_raw));
            if !clean.is_empty() {
                meta.authors.push(sanitize_untrusted_text(&clean));
            }
        }
        cursor = rest;
    }

    if meta.title.is_none() && meta.arxiv_id.is_none() && meta.authors.is_empty() {
        None
    } else {
        Some(meta)
    }
}

fn extract_xml_tag_body<'a>(xml: &'a str, tag: &str) -> Option<&'a str> {
    extract_next_xml_tag_body(xml, tag).map(|(body, _)| body)
}

fn extract_next_xml_tag_body<'a>(xml: &'a str, tag: &str) -> Option<(&'a str, &'a str)> {
    let open_pat = format!("<{tag}");
    let close_pat = format!("</{tag}>");
    let mut search_from = 0usize;

    while let Some(rel_idx) = xml[search_from..].find(&open_pat) {
        let abs_idx = search_from + rel_idx;
        let after_name = abs_idx + open_pat.len();
        if after_name < xml.len() {
            let next_ch = xml.as_bytes()[after_name];
            if next_ch == b'>' || next_ch == b'/' || next_ch.is_ascii_whitespace() {
                let tag_end_rel = xml[after_name..].find('>')?;
                let content_start = after_name + tag_end_rel + 1;
                if xml.as_bytes()[content_start - 2] == b'/' {
                    search_from = content_start;
                    continue;
                }
                let close_rel = xml[content_start..].find(&close_pat)?;
                let content_end = content_start + close_rel;
                let rest_start = content_end + close_pat.len();
                return Some((&xml[content_start..content_end], &xml[rest_start..]));
            }
        }
        search_from = after_name;
    }
    None
}

fn query_openalex_by_doi(
    doi: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<Option<PaperMetadata>, OnlineFetchError> {
    let mut url = format!("https://api.openalex.org/works/https://doi.org/{doi}");
    if let Some(ref key) = cfg.openalex_api_key {
        let trimmed_key = key.trim();
        if !trimmed_key.is_empty() {
            url.push_str("?api_key=");
            url.push_str(&url_encode_component(trimmed_key));
        }
    }

    let mut req = OutboundHttpRequest::get(url);
    req.headers
        .push(("Accept".to_string(), "application/json".to_string()));
    req.max_bytes = 4 * 1024 * 1024;

    match fetcher.execute(&req, cfg) {
        Ok(resp) if resp.status == 200 => {
            let val: serde_json::Value = match serde_json::from_slice(&resp.body) {
                Ok(v) => v,
                Err(_) => return Ok(None),
            };
            Ok(Some(parse_openalex_work_json(&val)))
        }
        Ok(_) => Ok(None),
        Err(err) if is_hard_stop_error(&err) => Err(err),
        Err(_) => Ok(None),
    }
}

fn parse_openalex_work_json(val: &serde_json::Value) -> PaperMetadata {
    let mut meta = PaperMetadata::default();

    if let Some(title) = val
        .get("title")
        .or_else(|| val.get("display_name"))
        .and_then(|v| v.as_str())
    {
        let clean = collapse_spaces(title);
        if !clean.is_empty() {
            meta.title = Some(sanitize_untrusted_text(&clean));
        }
    }

    if let Some(pub_date) = val.get("publication_date").and_then(|v| v.as_str()) {
        if !pub_date.trim().is_empty() {
            meta.published = Some(sanitize_untrusted_text(pub_date.trim()));
        }
    } else if let Some(year) = val.get("publication_year").and_then(|v| v.as_i64()) {
        meta.published = Some(year.to_string());
    }

    if let Some(authorships) = val.get("authorships").and_then(|v| v.as_array()) {
        for auth in authorships {
            if let Some(name) = auth
                .get("author")
                .and_then(|a| a.get("display_name"))
                .and_then(|n| n.as_str())
            {
                let clean = collapse_spaces(name);
                if !clean.is_empty() {
                    meta.authors.push(sanitize_untrusted_text(&clean));
                }
            }
        }
    }

    if let Some(inv) = val
        .get("abstract_inverted_index")
        .and_then(|v| v.as_object())
    {
        let reconstructed = reconstruct_openalex_abstract(inv);
        if !reconstructed.is_empty() {
            meta.abstract_text = Some(sanitize_untrusted_text(&reconstructed));
        }
    }

    // Discover arXiv ID across `ids`, `best_oa_location`, `primary_location`, and `locations`
    let mut candidate_urls: Vec<&str> = Vec::new();
    if let Some(ids) = val.get("ids").and_then(|v| v.as_object()) {
        for k in ["arxiv", "doi"] {
            if let Some(s) = ids.get(k).and_then(|v| v.as_str()) {
                candidate_urls.push(s);
            }
        }
    }
    for loc_key in ["best_oa_location", "primary_location"] {
        if let Some(loc) = val.get(loc_key) {
            if let Some(u) = loc.get("landing_page_url").and_then(|v| v.as_str()) {
                candidate_urls.push(u);
            }
            if let Some(u) = loc.get("pdf_url").and_then(|v| v.as_str()) {
                candidate_urls.push(u);
                if meta.oa_pdf_url.is_none() {
                    meta.oa_pdf_url = Some(u.to_string());
                }
            }
        }
    }
    if let Some(locs) = val.get("locations").and_then(|v| v.as_array()) {
        for loc in locs {
            if let Some(u) = loc.get("landing_page_url").and_then(|v| v.as_str()) {
                candidate_urls.push(u);
            }
            if let Some(u) = loc.get("pdf_url").and_then(|v| v.as_str()) {
                candidate_urls.push(u);
                if meta.oa_pdf_url.is_none() {
                    meta.oa_pdf_url = Some(u.to_string());
                }
            }
        }
    }
    if meta.oa_pdf_url.is_none() {
        if let Some(oa_u) = val
            .get("open_access")
            .and_then(|oa| oa.get("oa_url"))
            .and_then(|v| v.as_str())
        {
            meta.oa_pdf_url = Some(oa_u.to_string());
        }
    }

    for cand in candidate_urls {
        if let Some(aid) = parse_arxiv_id(cand) {
            meta.arxiv_id = Some(aid);
            break;
        }
    }

    meta
}

fn reconstruct_openalex_abstract(inv: &serde_json::Map<String, serde_json::Value>) -> String {
    let mut positioned: Vec<(usize, &str)> = Vec::new();
    for (word, positions_val) in inv {
        if let Some(arr) = positions_val.as_array() {
            for pos_v in arr {
                if let Some(idx) = pos_v.as_u64() {
                    if (idx as usize) < 32_768 {
                        positioned.push((idx as usize, word.as_str()));
                    }
                }
            }
        }
    }
    if positioned.is_empty() {
        return String::new();
    }
    positioned.sort_by_key(|&(idx, _)| idx);
    positioned
        .into_iter()
        .map(|(_, w)| w)
        .collect::<Vec<_>>()
        .join(" ")
}

fn query_openreview_note(
    forum_id: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<Option<PaperMetadata>, OnlineFetchError> {
    let url = format!(
        "https://api2.openreview.net/notes?id={}",
        url_encode_component(forum_id)
    );
    let mut req = OutboundHttpRequest::get(url);
    req.headers
        .push(("Accept".to_string(), "application/json".to_string()));
    req.max_bytes = 4 * 1024 * 1024;

    match fetcher.execute(&req, cfg) {
        Ok(resp) if resp.status == 200 => {
            let val: serde_json::Value = match serde_json::from_slice(&resp.body) {
                Ok(v) => v,
                Err(_) => return Ok(None),
            };
            Ok(parse_openreview_json(&val, forum_id))
        }
        Ok(_) => Ok(None),
        Err(err) if is_hard_stop_error(&err) => Err(err),
        Err(_) => Ok(None),
    }
}

fn parse_openreview_json(val: &serde_json::Value, forum_id: &str) -> Option<PaperMetadata> {
    let note = val.get("notes")?.as_array()?.first()?;
    let content = note.get("content")?;
    let mut meta = PaperMetadata {
        oa_pdf_url: Some(format!("https://openreview.net/pdf?id={forum_id}")),
        ..Default::default()
    };

    if let Some(title) = extract_or_field_str(content, "title") {
        meta.title = Some(sanitize_untrusted_text(&collapse_spaces(title)));
    }
    if let Some(abs) = extract_or_field_str(content, "abstract") {
        meta.abstract_text = Some(sanitize_untrusted_text(&collapse_spaces(abs)));
    }
    if let Some(authors_val) = content.get("authors") {
        let arr = authors_val
            .get("value")
            .and_then(|v| v.as_array())
            .or_else(|| authors_val.as_array());
        if let Some(list) = arr {
            for item in list {
                if let Some(name) = item.as_str() {
                    let clean = collapse_spaces(name);
                    if !clean.is_empty() {
                        meta.authors.push(sanitize_untrusted_text(&clean));
                    }
                }
            }
        }
    }

    if let Some(ms) = note
        .get("pdate")
        .or_else(|| note.get("cdate"))
        .and_then(|v| v.as_i64())
    {
        if ms > 0 {
            meta.published = Some(epoch_millis_to_ymd(ms));
        }
    }

    // Scan serialized note content for any embedded arXiv ID or DOI
    let raw_content_str = content.to_string();
    if let Some(caps) = RE_ARXIV_IN_TEXT.captures(&raw_content_str) {
        if let Some(m) = caps.get(1) {
            meta.arxiv_id = parse_bare_arxiv_token(m.as_str());
        }
    }
    if let Some(doi_str) = extract_or_field_str(content, "doi") {
        if let Some(d) = parse_doi_string(doi_str).or_else(|| {
            if doi_str.starts_with("10.") {
                Some(doi_str.to_string())
            } else {
                None
            }
        }) {
            meta.doi = Some(d);
        }
    }

    Some(meta)
}

fn extract_or_field_str<'a>(content: &'a serde_json::Value, field: &str) -> Option<&'a str> {
    let f = content.get(field)?;
    f.as_str()
        .or_else(|| f.get("value").and_then(|v| v.as_str()))
}

fn query_semantic_scholar(
    paper_key: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<Option<PaperMetadata>, OnlineFetchError> {
    let url = format!(
        "https://api.semanticscholar.org/graph/v1/paper/{paper_key}?fields=title,authors,year,publicationDate,abstract,externalIds,openAccessPdf"
    );
    let mut req = OutboundHttpRequest::get(url);
    req.headers
        .push(("Accept".to_string(), "application/json".to_string()));
    req.max_bytes = 2 * 1024 * 1024;

    match fetcher.execute(&req, cfg) {
        Ok(resp) if resp.status == 200 => {
            let val: serde_json::Value = match serde_json::from_slice(&resp.body) {
                Ok(v) => v,
                Err(_) => return Ok(None),
            };
            let mut meta = PaperMetadata::default();
            if let Some(t) = val.get("title").and_then(|v| v.as_str()) {
                meta.title = Some(sanitize_untrusted_text(&collapse_spaces(t)));
            }
            if let Some(abs) = val.get("abstract").and_then(|v| v.as_str()) {
                meta.abstract_text = Some(sanitize_untrusted_text(&collapse_spaces(abs)));
            }
            if let Some(pd) = val.get("publicationDate").and_then(|v| v.as_str()) {
                if !pd.trim().is_empty() {
                    meta.published = Some(sanitize_untrusted_text(pd.trim()));
                }
            } else if let Some(yr) = val.get("year").and_then(|v| v.as_i64()) {
                meta.published = Some(yr.to_string());
            }
            if let Some(authors) = val.get("authors").and_then(|v| v.as_array()) {
                for a in authors {
                    if let Some(name) = a.get("name").and_then(|v| v.as_str()) {
                        let clean = collapse_spaces(name);
                        if !clean.is_empty() {
                            meta.authors.push(sanitize_untrusted_text(&clean));
                        }
                    }
                }
            }
            if let Some(ext) = val.get("externalIds").and_then(|v| v.as_object()) {
                if let Some(arx) = ext
                    .get("ArXiv")
                    .or_else(|| ext.get("arxiv"))
                    .and_then(|v| v.as_str())
                {
                    meta.arxiv_id = parse_arxiv_id(arx);
                }
                if let Some(doi) = ext
                    .get("DOI")
                    .or_else(|| ext.get("doi"))
                    .and_then(|v| v.as_str())
                {
                    meta.doi = Some(doi.to_string());
                }
            }
            if let Some(pdf_u) = val
                .get("openAccessPdf")
                .and_then(|p| p.get("url"))
                .and_then(|v| v.as_str())
            {
                if !pdf_u.trim().is_empty() {
                    meta.oa_pdf_url = Some(pdf_u.trim().to_string());
                }
            }
            Ok(Some(meta))
        }
        Ok(_) => Ok(None),
        Err(err) if is_hard_stop_error(&err) => Err(err),
        Err(_) => Ok(None),
    }
}

fn inspect_pmlr_landing_page(
    landing_url: &str,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<Option<(PaperMetadata, Option<String>)>, OnlineFetchError> {
    let req = OutboundHttpRequest::get(landing_url);
    match fetcher.execute(&req, cfg) {
        Ok(resp) if resp.status == 200 => {
            let html = String::from_utf8_lossy(&resp.body);
            let mut meta = PaperMetadata::default();
            if let Some(caps) = RE_ARXIV_IN_TEXT.captures(&html) {
                if let Some(m) = caps.get(1) {
                    meta.arxiv_id = parse_bare_arxiv_token(m.as_str());
                }
            }
            let pdf_url = extract_meta_content(&html, "citation_pdf_url");
            if let Some(t) = extract_meta_content(&html, "citation_title") {
                meta.title = Some(sanitize_untrusted_text(&decode_html_entities(&t)));
            }
            if let Some(d) = extract_meta_content(&html, "citation_publication_date") {
                meta.published = Some(sanitize_untrusted_text(&d));
            }
            Ok(Some((meta, pdf_url)))
        }
        Ok(_) => Ok(None),
        Err(err) if is_hard_stop_error(&err) => Err(err),
        Err(_) => Ok(None),
    }
}

fn extract_meta_content(html: &str, meta_name: &str) -> Option<String> {
    static RE_META: Lazy<Regex> =
        Lazy::new(|| Regex::new(r"(?is)<meta\b[^>]*>").expect("valid meta regex"));
    static RE_NAME: Lazy<Regex> = Lazy::new(|| {
        Regex::new(r#"(?i)\bname\s*=\s*["']([^"']+)["']"#).expect("valid meta name regex")
    });
    static RE_CONTENT: Lazy<Regex> = Lazy::new(|| {
        Regex::new(r#"(?i)\bcontent\s*=\s*["']([^"']+)["']"#).expect("valid meta content regex")
    });

    for m in RE_META.find_iter(html) {
        let tag = m.as_str();
        let Some(name_cap) = RE_NAME.captures(tag) else {
            continue;
        };
        if name_cap[1].eq_ignore_ascii_case(meta_name) {
            if let Some(val_cap) = RE_CONTENT.captures(tag) {
                return Some(val_cap[1].trim().to_string());
            }
        }
    }
    None
}

// ============================================================================
// Tier 2: Streaming Official `e-print` `.tar.gz` / `.tex` Unpacker & Converter
// ============================================================================

enum EprintPayloadResult {
    Markdown {
        markdown: String,
        bib_json: Option<String>,
    },
    EmbeddedPdf(Vec<u8>),
}

fn unpack_and_extract_eprint_payload(
    raw_body: &[u8],
    meta: &mut PaperMetadata,
) -> Result<EprintPayloadResult, OnlineFetchError> {
    let decompressed = if raw_body.len() >= 2 && raw_body[0] == 0x1f && raw_body[1] == 0x8b {
        decompress_gzip_via_archive_tar(raw_body, MAX_EPRINT_COMPRESSED_BYTES)?
    } else {
        raw_body.to_vec()
    };

    if decompressed.starts_with(b"%PDF-") {
        return Ok(EprintPayloadResult::EmbeddedPdf(decompressed));
    }

    if super::archive_tar::is_tar_ustar_header(&decompressed) {
        let mut entries =
            super::archive_tar::extract_safe_tar_entries(&decompressed, MAX_EPRINT_TAR_BYTES)?;
        // Safe fallback only if `archive_tar` is temporarily unimplemented during isolated testing
        if entries.is_empty() && is_archive_tar_stub() {
            entries = extract_ustar_regular_files_fallback(&decompressed, MAX_EPRINT_TAR_BYTES)?;
        }
        return assemble_multifile_tex_archive(&entries, meta);
    }

    // Standalone single `.tex` payload
    if decompressed.iter().take(512).any(|&b| b == 0) {
        return Err(OnlineFetchError::ExtractionError(
            "Unsupported binary e-print payload".to_string(),
        ));
    }
    let tex_str = String::from_utf8_lossy(&decompressed);
    let low_prefix: String = tex_str.chars().take(256).collect::<String>().to_ascii_lowercase();
    if low_prefix.contains("<!doctype html") || low_prefix.contains("<html") {
        return Err(OnlineFetchError::ExtractionError(
            "e-print returned HTML error page instead of TeX source".to_string(),
        ));
    }

    let uncommented = strip_latex_comments(&tex_str);
    let (md, extracted_title, extracted_authors, bib_entries) =
        convert_latex_to_markdown(&uncommented, &[]);
    if meta.title.is_none() {
        meta.title = extracted_title;
    }
    if meta.authors.is_empty() && !extracted_authors.is_empty() {
        meta.authors = extracted_authors;
    }
    let bib_json = if bib_entries.is_empty() {
        None
    } else {
        serde_json::to_string(&bib_entries).ok()
    };
    Ok(EprintPayloadResult::Markdown {
        markdown: md,
        bib_json,
    })
}

fn decompress_gzip_via_archive_tar(
    compressed: &[u8],
    max_bytes: usize,
) -> Result<Vec<u8>, OnlineFetchError> {
    match super::archive_tar::gunzip_bytes(compressed, max_bytes) {
        Ok(bytes) => Ok(bytes),
        Err(OnlineFetchError::ExtractionError(ref msg)) if msg == "stub" => {
            inflate_gzip_or_zlib_pure_rust(compressed, max_bytes)
        }
        Err(err) => Err(err),
    }
}

fn is_archive_tar_stub() -> bool {
    matches!(
        super::archive_tar::gunzip_bytes(&[], 64),
        Err(OnlineFetchError::ExtractionError(ref msg)) if msg == "stub"
    )
}

fn assemble_multifile_tex_archive(
    entries: &[ExtractedArchiveFile],
    meta: &mut PaperMetadata,
) -> Result<EprintPayloadResult, OnlineFetchError> {
    let mut text_files: BTreeMap<String, String> = BTreeMap::new();
    let mut bbl_blobs: Vec<String> = Vec::new();
    let mut bib_blobs: Vec<String> = Vec::new();

    for entry in entries {
        let norm_path = normalize_rel_tex_path(&entry.rel_path);
        if norm_path.is_empty() {
            continue;
        }
        let low = norm_path.to_ascii_lowercase();
        if low.ends_with(".tex") || low.ends_with(".ltx") || low.ends_with(".sty") {
            let content = String::from_utf8_lossy(&entry.bytes).into_owned();
            text_files.insert(norm_path, content);
        } else if low.ends_with(".bbl") {
            let content = String::from_utf8_lossy(&entry.bytes).into_owned();
            bbl_blobs.push(content.clone());
            text_files.insert(norm_path, content);
        } else if low.ends_with(".bib") {
            let content = String::from_utf8_lossy(&entry.bytes).into_owned();
            bib_blobs.push(content);
        }
    }

    let main_path = select_main_tex_file(&text_files).ok_or_else(|| {
        OnlineFetchError::ExtractionError(
            "No `.tex` source file found in arXiv e-print archive".to_string(),
        )
    })?;

    let mut stack = vec![main_path.clone()];
    let expanded_tex = expand_tex_inputs_recursive(&main_path, &text_files, 0, &mut stack);

    let mut extra_bib_sources = bbl_blobs;
    extra_bib_sources.extend(bib_blobs);

    let (md, extracted_title, extracted_authors, bib_entries) =
        convert_latex_to_markdown(&expanded_tex, &extra_bib_sources);

    if meta.title.is_none() {
        meta.title = extracted_title;
    }
    if meta.authors.is_empty() && !extracted_authors.is_empty() {
        meta.authors = extracted_authors;
    }

    let bib_json = if bib_entries.is_empty() {
        None
    } else {
        serde_json::to_string(&bib_entries).ok()
    };

    Ok(EprintPayloadResult::Markdown {
        markdown: md,
        bib_json,
    })
}

fn select_main_tex_file(files: &BTreeMap<String, String>) -> Option<String> {
    let mut best_path: Option<&String> = None;
    let mut best_score = i64::MIN;

    for (path, content) in files {
        let low_path = path.to_ascii_lowercase();
        if !low_path.ends_with(".tex") && !low_path.ends_with(".ltx") {
            continue;
        }
        let uncommented = strip_latex_comments(content);
        let mut score = 0i64;
        if uncommented.contains("\\documentclass") {
            score += 200;
        }
        if uncommented.contains("\\begin{document}") {
            score += 200;
        }
        if uncommented.contains("\\section") {
            score += 25;
        }
        let fname = low_path.rsplit('/').next().unwrap_or(&low_path);
        if matches!(
            fname,
            "main.tex" | "paper.tex" | "ms.tex" | "article.tex" | "root.tex"
        ) {
            score += 40;
        }
        if !low_path.contains('/') {
            score += 15;
        }
        score += (uncommented.len().min(60_000) / 512) as i64;

        if score > best_score {
            best_score = score;
            best_path = Some(path);
        }
    }

    best_path.cloned()
}

fn normalize_rel_tex_path(raw: &str) -> String {
    let slash = raw.trim().replace('\\', "/");
    let mut parts: Vec<&str> = Vec::new();
    for seg in slash.split('/') {
        let s = seg.trim();
        if s.is_empty() || s == "." {
            continue;
        }
        if s == ".." {
            return String::new();
        }
        parts.push(s);
    }
    parts.join("/")
}

fn resolve_tex_include_target(
    current_file: &str,
    raw_target: &str,
    files: &BTreeMap<String, String>,
) -> Option<String> {
    let clean_target = normalize_rel_tex_path(raw_target.trim().trim_matches('"'));
    if clean_target.is_empty() {
        return None;
    }

    let cur_dir = current_file
        .rfind('/')
        .map(|idx| &current_file[..idx])
        .unwrap_or("");

    let mut base_candidates = Vec::new();
    if !cur_dir.is_empty() {
        base_candidates.push(normalize_rel_tex_path(&format!("{cur_dir}/{clean_target}")));
    }
    base_candidates.push(clean_target);

    let mut full_candidates = Vec::new();
    for cand in base_candidates {
        if cand.is_empty() {
            continue;
        }
        full_candidates.push(cand.clone());
        if !cand.to_ascii_lowercase().ends_with(".tex")
            && !cand.to_ascii_lowercase().ends_with(".ltx")
            && !cand.to_ascii_lowercase().ends_with(".bbl")
        {
            full_candidates.push(format!("{cand}.tex"));
            full_candidates.push(format!("{cand}.ltx"));
            full_candidates.push(format!("{cand}.bbl"));
        }
    }

    for cand in &full_candidates {
        if files.contains_key(cand) {
            return Some(cand.clone());
        }
    }
    // Case-insensitive fallback for mixed-case TeX filenames
    for cand in &full_candidates {
        if let Some((found_k, _)) = files
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case(cand))
        {
            return Some(found_k.clone());
        }
    }
    None
}

fn expand_tex_inputs_recursive(
    file_path: &str,
    files: &BTreeMap<String, String>,
    depth: usize,
    active_stack: &mut Vec<String>,
) -> String {
    static RE_TEX_INPUT: Lazy<Regex> = Lazy::new(|| {
        Regex::new(r"\\(?:input|include|subfile)\s*\{([^}]+)\}")
            .expect("valid TeX input regex")
    });

    let Some(raw_src) = files.get(file_path) else {
        return String::new();
    };
    let uncommented = strip_latex_comments(raw_src);
    if depth >= MAX_TEX_INPUT_DEPTH {
        return uncommented;
    }

    let mut out = String::with_capacity(uncommented.len());
    let mut last_end = 0usize;

    for caps in RE_TEX_INPUT.captures_iter(&uncommented) {
        let m = caps.get(0).unwrap();
        let target_arg = caps.get(1).unwrap().as_str();
        out.push_str(&uncommented[last_end..m.start()]);
        last_end = m.end();

        if let Some(resolved_path) = resolve_tex_include_target(file_path, target_arg, files) {
            if !active_stack.contains(&resolved_path) {
                active_stack.push(resolved_path.clone());
                let inlined =
                    expand_tex_inputs_recursive(&resolved_path, files, depth + 1, active_stack);
                active_stack.pop();
                out.push('\n');
                out.push_str(&inlined);
                out.push('\n');
                continue;
            }
        }
    }
    out.push_str(&uncommented[last_end..]);
    out
}

/// Strips unescaped `%` comments line-by-line while preserving `\%` and verbatim/listing blocks.
pub(crate) fn strip_latex_comments(tex: &str) -> String {
    let mut out = String::with_capacity(tex.len());
    let mut in_verbatim = false;

    for line in tex.lines() {
        let trimmed = line.trim();
        if trimmed.contains("\\begin{verbatim}") || trimmed.contains("\\begin{lstlisting}") || trimmed.contains("\\begin{minted}") {
            in_verbatim = true;
        }
        if in_verbatim {
            out.push_str(line);
            out.push('\n');
            if trimmed.contains("\\end{verbatim}") || trimmed.contains("\\end{lstlisting}") || trimmed.contains("\\end{minted}") {
                in_verbatim = false;
            }
            continue;
        }

        let bytes = line.as_bytes();
        let mut cut = bytes.len();
        let mut i = 0usize;
        while i < bytes.len() {
            if bytes[i] == b'%' {
                let mut backslashes = 0usize;
                let mut b = i;
                while b > 0 && bytes[b - 1] == b'\\' {
                    backslashes += 1;
                    b -= 1;
                }
                if backslashes % 2 == 0 {
                    cut = i;
                    break;
                }
            }
            i += 1;
        }

        if cut < bytes.len() {
            let kept = &line[..cut];
            if !kept.trim().is_empty() || cut == 0 {
                if cut > 0 {
                    out.push_str(kept.trim_end());
                    out.push('\n');
                }
            }
        } else {
            out.push_str(line);
            out.push('\n');
        }
    }
    out
}

/// Converts uncommented TeX source into clean Markdown while preserving `$...$`, `$$...$$`,
/// `\begin{equation}`, and `\begin{align}` intact, and rendering code listings & references.
fn convert_latex_to_markdown(
    tex: &str,
    extra_bib_sources: &[String],
) -> (String, Option<String>, Vec<String>, Vec<BibSidecarEntry>) {
    let extracted_title = extract_latex_command_arg(tex, "title").map(|t| clean_latex_inline(&t));
    let extracted_authors = extract_latex_authors(tex);

    let doc_body = if let Some(begin_pos) = tex.find("\\begin{document}") {
        let start = begin_pos + "\\begin{document}".len();
        let end = tex[start..]
            .find("\\end{document}")
            .map(|rel| start + rel)
            .unwrap_or(tex.len());
        &tex[start..end]
    } else {
        tex
    };

    let mut protected_blocks: Vec<String> = Vec::new();
    let mut working = protect_verbatim_and_code_envs(doc_body, &mut protected_blocks);
    working = protect_math_environments(&working, &mut protected_blocks);

    // Extract inline `\begin{thebibliography}...\end{thebibliography}` if present
    let mut bib_entries: Vec<BibSidecarEntry> = Vec::new();
    working = convert_inline_thebibliography(&working, &mut bib_entries);

    // Also parse sidecar `.bbl` / `.bib` files if not already captured
    for src in extra_bib_sources {
        parse_bbl_or_bib_entries(src, &mut bib_entries);
    }

    // Abstract environment -> `## Abstract`
    working = replace_simple_env(&working, "abstract", |inner| {
        format!("\n\n## Abstract\n\n{}\n\n", inner.trim())
    });

    // Theorem / Lemma / Proposition / Proof environments -> Titled Blockquotes
    for (env_name, label) in [
        ("theorem", "Theorem"),
        ("lemma", "Lemma"),
        ("proposition", "Proposition"),
        ("corollary", "Corollary"),
        ("definition", "Definition"),
        ("proof", "Proof"),
    ] {
        working = replace_simple_env(&working, env_name, |inner| {
            let trimmed = inner.trim();
            let (opt_title, rest) = if let Some(after_br) = trimmed.strip_prefix('[') {
                if let Some(end_b) = after_br.find(']') {
                    (Some(after_br[..end_b].trim()), after_br[end_b + 1..].trim())
                } else {
                    (None, trimmed)
                }
            } else {
                (None, trimmed)
            };
            let header = match opt_title {
                Some(t) if !t.is_empty() => format!("**{label} ({t}).**"),
                _ => format!("**{label}.**"),
            };
            let combined = format!("{header} {rest}");
            let quoted: String = combined
                .lines()
                .map(|l| {
                    if l.trim().is_empty() {
                        ">".to_string()
                    } else {
                        format!("> {}", l.trim())
                    }
                })
                .collect::<Vec<_>>()
                .join("\n");
            format!("\n\n{quoted}\n\n")
        });
    }

    // Simple tabular -> GFM pipe table
    working = convert_latex_tabular_blocks(&working);

    // Lists: itemize & enumerate
    working = convert_latex_lists(&working);

    // Section headings (`\section` -> `#`, `\subsection` -> `##`, `\subsubsection` -> `###`, `\paragraph` -> `####`)
    for (cmd, hashes) in [
        ("section", "#"),
        ("subsection", "##"),
        ("subsubsection", "###"),
        ("paragraph", "####"),
    ] {
        working = replace_command_1arg(&working, cmd, |arg| {
            let clean = clean_latex_inline(arg);
            format!("\n\n{hashes} {clean}\n\n")
        });
    }

    // Two-arg `\href{url}{text}` -> `[text](url)`
    working = replace_href_commands(&working);

    // Inline formatting commands (`\textbf`, `\emph`, `\textit`, `\texttt`, `\url`, `\cite*`, `\ref*`)
    for _ in 0..3 {
        working = replace_command_1arg(&working, "textbf", |a| format!("**{}**", a.trim()));
        working = replace_command_1arg(&working, "emph", |a| format!("*{}*", a.trim()));
        working = replace_command_1arg(&working, "textit", |a| format!("*{}*", a.trim()));
        working = replace_command_1arg(&working, "texttt", |a| format!("`{}`", a.trim()));
        working = replace_command_1arg(&working, "url", |a| format!("<{}>", a.trim()));
        for cite_cmd in ["cite", "citep", "citet", "citeauthor", "citeyear"] {
            working = replace_command_1arg(&working, cite_cmd, |a| format!("[{}]", a.trim()));
        }
        for ref_cmd in ["ref", "eqref", "autoref", "cref", "Cref"] {
            working = replace_command_1arg(&working, ref_cmd, |a| format!("({})", a.trim()));
        }
    }

    // Strip layout/housekeeping commands with 1 arg
    for drop_cmd in [
        "label",
        "vspace",
        "hspace",
        "bibliographystyle",
        "bibliography",
        "pagestyle",
        "thispagestyle",
        "thanks",
    ] {
        working = replace_command_1arg(&working, drop_cmd, |_| String::new());
    }
    // Remove `\title{...}`, `\author{...}`, `\date{...}` from body if placed inside `\begin{document}`
    for meta_cmd in ["title", "author", "date"] {
        working = replace_command_1arg(&working, meta_cmd, |_| String::new());
    }

    // Strip zero-arg layout tokens
    for bare_tok in [
        "\\maketitle",
        "\\centering",
        "\\noindent",
        "\\newpage",
        "\\clearpage",
        "\\tableofcontents",
        "\\allowdisplaybreaks",
    ] {
        working = working.replace(bare_tok, "");
    }

    working = unescape_latex_prose(&working);

    // Append sidecar references if not already rendered in body
    if !bib_entries.is_empty() && !working.contains("## References") && !working.contains("# References") {
        working.push_str("\n\n# References\n\n");
        for entry in &bib_entries {
            working.push_str("- **[");
            working.push_str(&entry.cite_key);
            working.push_str("]** ");
            working.push_str(&entry.text);
            working.push('\n');
        }
    }

    // Prepend paper title heading if known and not yet emitted
    if let Some(ref t) = extracted_title {
        if !t.is_empty() && !working.trim_start().starts_with("# ") {
            working = format!("# {t}\n\n{working}");
        }
    }

    // Restore protected math & code placeholders
    let restored = restore_placeholders(&working, &protected_blocks);
    let normalized = normalize_markdown_blank_lines(&restored);

    (normalized, extracted_title, extracted_authors, bib_entries)
}

fn make_placeholder(idx: usize) -> String {
    format!("\x07SPELLPH{idx}X\x07")
}

fn restore_placeholders(text: &str, blocks: &[String]) -> String {
    let mut current = text.to_string();
    // Restore in reverse order so nested placeholders (if any) resolve cleanly
    for (idx, block) in blocks.iter().enumerate().rev() {
        let token = make_placeholder(idx);
        if current.contains(&token) {
            current = current.replace(&token, block);
        }
    }
    current
}

fn protect_verbatim_and_code_envs(input: &str, blocks: &mut Vec<String>) -> String {
    static RE_CODE_ENV: Lazy<Regex> = Lazy::new(|| {
        Regex::new(
            r"(?s)\\begin\{(lstlisting|verbatim|minted)\}(?:\[([^\]]*)\])?(?:\{([^}]*)\})?(.*?)\\end\{(?:lstlisting|verbatim|minted)\}",
        )
        .expect("valid LaTeX code env regex")
    });
    static RE_VERB_INLINE: Lazy<Regex> =
        Lazy::new(|| Regex::new(r"\\verb\*?([^\s\w])(.*?)").expect("valid verb prefix"));

    let mut out = String::with_capacity(input.len());
    let mut last = 0usize;
    for caps in RE_CODE_ENV.captures_iter(input) {
        let m = caps.get(0).unwrap();
        out.push_str(&input[last..m.start()]);
        last = m.end();

        let env_kind = &caps[1];
        let opt_bracket = caps.get(2).map(|c| c.as_str()).unwrap_or("");
        let opt_brace = caps.get(3).map(|c| c.as_str()).unwrap_or("");
        let code_raw = caps.get(4).map(|c| c.as_str()).unwrap_or("");

        let lang = if env_kind == "minted" && !opt_brace.trim().is_empty() {
            opt_brace.trim().to_ascii_lowercase()
        } else {
            extract_lstlisting_lang(opt_bracket)
        };

        let trimmed_code = code_raw
            .strip_prefix('\n')
            .unwrap_or(code_raw)
            .trim_end_matches([' ', '\t', '\n']);
        let mut fenced = String::new();
        emit_dynamic_fenced_code(trimmed_code, &lang, &mut fenced);

        let ph = make_placeholder(blocks.len());
        blocks.push(format!("\n\n{}\n\n", fenced.trim()));
        out.push_str(&ph);
    }
    out.push_str(&input[last..]);

    // Protect `\verb|...|` inline verbatim spans
    let _ = &*RE_VERB_INLINE;
    protect_inline_verb(&out, blocks)
}

fn protect_inline_verb(input: &str, blocks: &mut Vec<String>) -> String {
    let mut out = String::with_capacity(input.len());
    let bytes = input.as_bytes();
    let mut i = 0usize;
    while i < bytes.len() {
        if input[i..].starts_with("\\verb") {
            let mut j = i + 5;
            if j < bytes.len() && bytes[j] == b'*' {
                j += 1;
            }
            if j < bytes.len() && !bytes[j].is_ascii_alphabetic() && !bytes[j].is_ascii_whitespace()
            {
                let delim = bytes[j];
                if let Some(rel_end) = bytes[j + 1..].iter().position(|&b| b == delim) {
                    let content = &input[j + 1..j + 1 + rel_end];
                    let ph = make_placeholder(blocks.len());
                    blocks.push(format!("`{content}`"));
                    out.push_str(&ph);
                    i = j + 1 + rel_end + 1;
                    continue;
                }
            }
        }
        let ch = input[i..].chars().next().unwrap();
        out.push(ch);
        i += ch.len_utf8();
    }
    out
}

fn extract_lstlisting_lang(opts: &str) -> String {
    static RE_LANG: Lazy<Regex> =
        Lazy::new(|| Regex::new(r"(?i)language\s*=\s*([A-Za-z0-9_+#\-]+)").unwrap());
    if let Some(caps) = RE_LANG.captures(opts) {
        caps[1].to_ascii_lowercase()
    } else {
        String::new()
    }
}

fn protect_math_environments(input: &str, blocks: &mut Vec<String>) -> String {
    static RE_NAMED_MATH_ENV: Lazy<Regex> = Lazy::new(|| {
        Regex::new(
            r"(?s)\\begin\{((?:equation|align|gather|multline|eqnarray|flalign)\*?)\}(.*?)\\end\{(?:equation|align|gather|multline|eqnarray|flalign)\*?\}",
        )
        .expect("valid math env regex")
    });

    // 1. Named display math environments (`\begin{equation}...\end{equation}`, `\begin{align}...\end{align}`, etc.)
    let mut step1 = String::with_capacity(input.len());
    let mut last = 0usize;
    for caps in RE_NAMED_MATH_ENV.captures_iter(input) {
        let m = caps.get(0).unwrap();
        step1.push_str(&input[last..m.start()]);
        last = m.end();
        let preserved = format!("\n\n$$\n{}\n$$\n\n", m.as_str().trim());
        let ph = make_placeholder(blocks.len());
        blocks.push(preserved);
        step1.push_str(&ph);
    }
    step1.push_str(&input[last..]);

    // 2. `\[ ... \]` and `\( ... \)`
    let mut step2 = String::with_capacity(step1.len());
    let mut i = 0usize;
    let b1 = step1.as_bytes();
    while i < b1.len() {
        if step1[i..].starts_with("\\[") {
            if let Some(rel_end) = step1[i + 2..].find("\\]") {
                let inner = step1[i + 2..i + 2 + rel_end].trim();
                let ph = make_placeholder(blocks.len());
                blocks.push(format!("\n\n$$\n{inner}\n$$\n\n"));
                step2.push_str(&ph);
                i += 2 + rel_end + 2;
                continue;
            }
        }
        if step1[i..].starts_with("\\(") {
            if let Some(rel_end) = step1[i + 2..].find("\\)") {
                let inner = collapse_spaces(&step1[i + 2..i + 2 + rel_end]);
                let ph = make_placeholder(blocks.len());
                blocks.push(format!("${inner}$"));
                step2.push_str(&ph);
                i += 2 + rel_end + 2;
                continue;
            }
        }
        let ch = step1[i..].chars().next().unwrap();
        step2.push(ch);
        i += ch.len_utf8();
    }

    // 3. `$$ ... $$` and inline `$ ... $` (ignoring escaped `\$`)
    let mut step3 = String::with_capacity(step2.len());
    let b2 = step2.as_bytes();
    let mut k = 0usize;
    while k < b2.len() {
        if b2[k] == b'$' && !is_escaped_at(&step2, k) {
            if k + 1 < b2.len() && b2[k + 1] == b'$' {
                if let Some(end_dd) = find_unescaped_double_dollar(&step2, k + 2) {
                    let inner = step2[k + 2..end_dd].trim();
                    let ph = make_placeholder(blocks.len());
                    blocks.push(format!("\n\n$$\n{inner}\n$$\n\n"));
                    step3.push_str(&ph);
                    k = end_dd + 2;
                    continue;
                }
            } else if let Some(end_d) = find_unescaped_single_dollar(&step2, k + 1) {
                let inner = step2[k + 1..end_d].trim();
                if !inner.is_empty() && !inner.contains("\n\n") {
                    let ph = make_placeholder(blocks.len());
                    blocks.push(format!("${}$", collapse_spaces(inner)));
                    step3.push_str(&ph);
                    k = end_d + 1;
                    continue;
                }
            }
        }
        let ch = step2[k..].chars().next().unwrap();
        step3.push(ch);
        k += ch.len_utf8();
    }

    step3
}

fn is_escaped_at(s: &str, byte_idx: usize) -> bool {
    let bytes = s.as_bytes();
    let mut count = 0usize;
    let mut p = byte_idx;
    while p > 0 && bytes[p - 1] == b'\\' {
        count += 1;
        p -= 1;
    }
    count % 2 == 1
}

fn find_unescaped_double_dollar(s: &str, start: usize) -> Option<usize> {
    let bytes = s.as_bytes();
    let mut i = start;
    while i + 1 < bytes.len() {
        if bytes[i] == b'$' && bytes[i + 1] == b'$' && !is_escaped_at(s, i) {
            return Some(i);
        }
        i += 1;
    }
    None
}

fn find_unescaped_single_dollar(s: &str, start: usize) -> Option<usize> {
    let bytes = s.as_bytes();
    let mut i = start;
    while i < bytes.len() {
        if bytes[i] == b'$' && !is_escaped_at(s, i) {
            if i + 1 < bytes.len() && bytes[i + 1] == b'$' {
                return None;
            }
            return Some(i);
        }
        i += 1;
    }
    None
}

fn replace_simple_env<F>(input: &str, env: &str, mut render: F) -> String
where
    F: FnMut(&str) -> String,
{
    let begin_tok = format!("\\begin{{{env}}}");
    let end_tok = format!("\\end{{{env}}}");
    let mut out = String::with_capacity(input.len());
    let mut cursor = 0usize;

    while let Some(rel_b) = input[cursor..].find(&begin_tok) {
        let abs_b = cursor + rel_b;
        let inner_start = abs_b + begin_tok.len();
        let Some(rel_e) = input[inner_start..].find(&end_tok) else {
            break;
        };
        let abs_e = inner_start + rel_e;
        out.push_str(&input[cursor..abs_b]);
        out.push_str(&render(&input[inner_start..abs_e]));
        cursor = abs_e + end_tok.len();
    }
    out.push_str(&input[cursor..]);
    out
}

/// Balanced-brace single-argument LaTeX command rewriter (`\cmd*[opt]{arg}`).
fn replace_command_1arg<F>(input: &str, cmd: &str, mut transform: F) -> String
where
    F: FnMut(&str) -> String,
{
    let needle = format!("\\{cmd}");
    let bytes = input.as_bytes();
    let mut out = String::with_capacity(input.len());
    let mut i = 0usize;

    while let Some(rel_pos) = input[i..].find(&needle) {
        let abs_pos = i + rel_pos;
        let after_cmd = abs_pos + needle.len();
        // Ensure we didn't match a prefix of a longer command (e.g. `\section` vs `\sectionmark`)
        if after_cmd < bytes.len() && bytes[after_cmd].is_ascii_alphabetic() {
            out.push_str(&input[i..after_cmd]);
            i = after_cmd;
            continue;
        }

        let mut p = after_cmd;
        if p < bytes.len() && bytes[p] == b'*' {
            p += 1;
        }
        while p < bytes.len() && bytes[p].is_ascii_whitespace() {
            p += 1;
        }
        // Skip optional `[short]` argument
        if p < bytes.len() && bytes[p] == b'[' {
            if let Some((_, after_opt)) = parse_balanced_delim(input, p, b'[', b']') {
                p = after_opt;
                while p < bytes.len() && bytes[p].is_ascii_whitespace() {
                    p += 1;
                }
            }
        }

        if p < bytes.len() && bytes[p] == b'{' {
            if let Some((arg, after_brace)) = parse_balanced_delim(input, p, b'{', b'}') {
                out.push_str(&input[i..abs_pos]);
                out.push_str(&transform(arg));
                i = after_brace;
                continue;
            }
        }

        out.push_str(&input[i..after_cmd]);
        i = after_cmd;
    }
    out.push_str(&input[i..]);
    out
}

fn replace_href_commands(input: &str) -> String {
    let needle = "\\href";
    let bytes = input.as_bytes();
    let mut out = String::with_capacity(input.len());
    let mut i = 0usize;

    while let Some(rel) = input[i..].find(needle) {
        let abs = i + rel;
        let after = abs + needle.len();
        if after < bytes.len() && bytes[after].is_ascii_alphabetic() {
            out.push_str(&input[i..after]);
            i = after;
            continue;
        }
        let mut p = after;
        while p < bytes.len() && bytes[p].is_ascii_whitespace() {
            p += 1;
        }
        if let Some((url_arg, p2)) = parse_balanced_delim(input, p, b'{', b'}') {
            let mut p3 = p2;
            while p3 < bytes.len() && bytes[p3].is_ascii_whitespace() {
                p3 += 1;
            }
            if let Some((label_arg, p4)) = parse_balanced_delim(input, p3, b'{', b'}') {
                out.push_str(&input[i..abs]);
                out.push('[');
                out.push_str(label_arg.trim());
                out.push_str("](");
                out.push_str(url_arg.trim());
                out.push(')');
                i = p4;
                continue;
            }
        }
        out.push_str(&input[i..after]);
        i = after;
    }
    out.push_str(&input[i..]);
    out
}

fn parse_balanced_delim(s: &str, start: usize, open_b: u8, close_b: u8) -> Option<(&str, usize)> {
    let bytes = s.as_bytes();
    if start >= bytes.len() || bytes[start] != open_b {
        return None;
    }
    let mut depth = 1usize;
    let mut i = start + 1;
    while i < bytes.len() {
        if bytes[i] == b'\\' {
            i += 2;
            continue;
        }
        if bytes[i] == open_b {
            depth += 1;
        } else if bytes[i] == close_b {
            depth -= 1;
            if depth == 0 {
                return Some((&s[start + 1..i], i + 1));
            }
        }
        i += 1;
    }
    None
}

fn extract_latex_command_arg(tex: &str, cmd: &str) -> Option<String> {
    let needle = format!("\\{cmd}");
    let bytes = tex.as_bytes();
    let mut i = 0usize;
    while let Some(rel) = tex[i..].find(&needle) {
        let abs = i + rel;
        let after = abs + needle.len();
        if after < bytes.len() && bytes[after].is_ascii_alphabetic() {
            i = after;
            continue;
        }
        let mut p = after;
        if p < bytes.len() && bytes[p] == b'*' {
            p += 1;
        }
        while p < bytes.len() && bytes[p].is_ascii_whitespace() {
            p += 1;
        }
        if p < bytes.len() && bytes[p] == b'[' {
            if let Some((_, p_opt)) = parse_balanced_delim(tex, p, b'[', b']') {
                p = p_opt;
                while p < bytes.len() && bytes[p].is_ascii_whitespace() {
                    p += 1;
                }
            }
        }
        if let Some((arg, _)) = parse_balanced_delim(tex, p, b'{', b'}') {
            return Some(arg.to_string());
        }
        i = after;
    }
    None
}

fn extract_latex_authors(tex: &str) -> Vec<String> {
    let Some(raw) = extract_latex_command_arg(tex, "author") else {
        return Vec::new();
    };
    let cleaned = replace_command_1arg(&raw, "thanks", |_| String::new());
    let cleaned = replace_command_1arg(&cleaned, "affiliation", |_| String::new());
    let cleaned = replace_command_1arg(&cleaned, "inst", |_| String::new());
    let mut out = Vec::new();
    for seg in cleaned.split("\\and") {
        let first_line = seg.split("\\\\").next().unwrap_or(seg);
        let inline = clean_latex_inline(first_line);
        if !inline.is_empty() && !out.contains(&inline) {
            out.push(inline);
        }
    }
    out
}

fn clean_latex_inline(raw: &str) -> String {
    let mut s = raw.replace("\\\\", " ");
    s = replace_command_1arg(&s, "thanks", |_| String::new());
    s = replace_command_1arg(&s, "textbf", |a| a.to_string());
    s = replace_command_1arg(&s, "emph", |a| a.to_string());
    s = replace_command_1arg(&s, "textit", |a| a.to_string());
    s = replace_command_1arg(&s, "texttt", |a| format!("`{}`", a.trim()));
    s = unescape_latex_prose(&s);
    collapse_spaces(&s)
}

fn convert_latex_lists(input: &str) -> String {
    let mut current = input.to_string();
    // Iterate innermost-first up to 4 nesting levels
    for _ in 0..4 {
        let next_i = replace_innermost_list(&current, "itemize", false);
        let next_e = replace_innermost_list(&next_i, "enumerate", true);
        if next_e == current {
            break;
        }
        current = next_e;
    }
    current
}

fn replace_innermost_list(input: &str, env: &str, ordered: bool) -> String {
    let begin_tok = format!("\\begin{{{env}}}");
    let end_tok = format!("\\end{{{env}}}");
    let mut out = String::with_capacity(input.len());
    let mut cursor = 0usize;

    while let Some(rel_b) = input[cursor..].find(&begin_tok) {
        let abs_b = cursor + rel_b;
        let inner_start = abs_b + begin_tok.len();
        let Some(rel_e) = input[inner_start..].find(&end_tok) else {
            break;
        };
        let abs_e = inner_start + rel_e;
        let inner = &input[inner_start..abs_e];
        // Skip outer list if a nested same-type list is inside; inner pass will handle it first
        if inner.contains(&begin_tok) {
            out.push_str(&input[cursor..inner_start]);
            cursor = inner_start;
            continue;
        }

        out.push_str(&input[cursor..abs_b]);
        out.push_str("\n\n");
        let mut counter = 1usize;
        for (idx, part) in inner.split("\\item").enumerate() {
            if idx == 0 {
                continue;
            }
            let trimmed = part.trim();
            if trimmed.is_empty() {
                continue;
            }
            let (custom_label, item_body) = if let Some(after_br) = trimmed.strip_prefix('[') {
                if let Some(close_br) = after_br.find(']') {
                    (
                        Some(after_br[..close_br].trim()),
                        after_br[close_br + 1..].trim(),
                    )
                } else {
                    (None, trimmed)
                }
            } else {
                (None, trimmed)
            };

            let single_line = collapse_spaces(item_body);
            if let Some(lbl) = custom_label.filter(|l| !l.is_empty()) {
                out.push_str(&format!("- **{lbl}** {single_line}\n"));
            } else if ordered {
                out.push_str(&format!("{counter}. {single_line}\n"));
                counter += 1;
            } else {
                out.push_str(&format!("- {single_line}\n"));
            }
        }
        out.push('\n');
        cursor = abs_e + end_tok.len();
    }
    out.push_str(&input[cursor..]);
    out
}

fn convert_latex_tabular_blocks(input: &str) -> String {
    static RE_TABULAR: Lazy<Regex> = Lazy::new(|| {
        Regex::new(r"(?s)\\begin\{tabular\}(?:\[[^\]]*\])?\{[^}]*\}(.*?)\\end\{tabular\}")
            .expect("valid tabular regex")
    });

    let mut out = String::with_capacity(input.len());
    let mut last = 0usize;
    for caps in RE_TABULAR.captures_iter(input) {
        let m = caps.get(0).unwrap();
        out.push_str(&input[last..m.start()]);
        last = m.end();

        let body = &caps[1];
        let mut rows: Vec<Vec<String>> = Vec::new();
        let mut num_cols = 0usize;

        for raw_row in body.split("\\\\") {
            let mut r = raw_row.to_string();
            for rule in ["\\toprule", "\\midrule", "\\bottomrule", "\\hline"] {
                r = r.replace(rule, "");
            }
            r = replace_command_1arg(&r, "cline", |_| String::new());
            let trimmed = r.trim();
            if trimmed.is_empty() {
                continue;
            }
            let cells: Vec<String> = trimmed
                .split('&')
                .map(|c| clean_latex_inline(c).replace('|', "\\|"))
                .collect();
            if cells.iter().any(|c| !c.is_empty()) {
                num_cols = num_cols.max(cells.len());
                rows.push(cells);
            }
        }

        if !rows.is_empty() && num_cols > 0 {
            out.push_str("\n\n");
            for (r_idx, row) in rows.iter_mut().enumerate() {
                while row.len() < num_cols {
                    row.push(String::new());
                }
                out.push('|');
                for c in row.iter() {
                    out.push(' ');
                    out.push_str(c);
                    out.push_str(" |");
                }
                out.push('\n');
                if r_idx == 0 {
                    out.push('|');
                    for _ in 0..num_cols {
                        out.push_str(" --- |");
                    }
                    out.push('\n');
                }
            }
            out.push('\n');
        }
    }
    out.push_str(&input[last..]);
    out
}

fn convert_inline_thebibliography(input: &str, entries: &mut Vec<BibSidecarEntry>) -> String {
    static RE_THEBIB: Lazy<Regex> = Lazy::new(|| {
        Regex::new(r"(?s)\\begin\{thebibliography\}(?:\{[^}]*\})?(.*?)\\end\{thebibliography\}")
            .expect("valid thebibliography regex")
    });

    let mut out = String::with_capacity(input.len());
    let mut last = 0usize;
    for caps in RE_THEBIB.captures_iter(input) {
        let m = caps.get(0).unwrap();
        out.push_str(&input[last..m.start()]);
        last = m.end();

        let start_len = entries.len();
        parse_bbl_bibitem_sequence(&caps[1], entries);
        if entries.len() > start_len {
            out.push_str("\n\n# References\n\n");
            for item in &entries[start_len..] {
                out.push_str(&format!("- **[{}]** {}\n", item.cite_key, item.text));
            }
            out.push('\n');
        }
    }
    out.push_str(&input[last..]);
    out
}

fn parse_bbl_or_bib_entries(raw: &str, entries: &mut Vec<BibSidecarEntry>) {
    let uncommented = strip_latex_comments(raw);
    if uncommented.contains("\\bibitem") {
        parse_bbl_bibitem_sequence(&uncommented, entries);
    } else if uncommented.contains('@') {
        parse_bibtex_entries(&uncommented, entries);
    }
}

fn parse_bbl_bibitem_sequence(body: &str, entries: &mut Vec<BibSidecarEntry>) {
    let clean_body = body.replace("\\end{thebibliography}", "");
    for (idx, chunk) in clean_body.split("\\bibitem").enumerate() {
        if idx == 0 {
            continue;
        }
        let mut p = 0usize;
        let bytes = chunk.as_bytes();
        while p < bytes.len() && bytes[p].is_ascii_whitespace() {
            p += 1;
        }
        let mut opt_label = None;
        if p < bytes.len() && bytes[p] == b'[' {
            if let Some((lbl, next_p)) = parse_balanced_delim(chunk, p, b'[', b']') {
                opt_label = Some(clean_latex_inline(lbl));
                p = next_p;
                while p < bytes.len() && bytes[p].is_ascii_whitespace() {
                    p += 1;
                }
            }
        }
        let Some((key, after_key)) = parse_balanced_delim(chunk, p, b'{', b'}') else {
            continue;
        };
        let cite_id = key.trim().to_string();
        if cite_id.is_empty() || entries.iter().any(|e| e.id == cite_id) {
            continue;
        }
        let text_raw = chunk[after_key..].replace("\\newblock", " ");
        let text = clean_latex_inline(&text_raw);
        if !text.is_empty() {
            let cite_key = opt_label
                .filter(|s| !s.is_empty())
                .unwrap_or_else(|| cite_id.clone());
            entries.push(BibSidecarEntry {
                id: sanitize_untrusted_text(&cite_id),
                cite_key: sanitize_untrusted_text(&cite_key),
                text: sanitize_untrusted_text(&text),
            });
        }
    }
}

fn parse_bibtex_entries(bibtex: &str, entries: &mut Vec<BibSidecarEntry>) {
    static RE_BIB_HEAD: Lazy<Regex> =
        Lazy::new(|| Regex::new(r"@([A-Za-z]+)\s*\{\s*([^,\s]+)\s*,").unwrap());
    static RE_FIELD: Lazy<Regex> = Lazy::new(|| {
        Regex::new(r#"(?is)\b(title|author|year|journal|booktitle)\s*=\s*(?:\{([^{}]*(?:\{[^{}]*\}[^{}]*)*)\}|"([^"]*)")"#).unwrap()
    });

    for caps in RE_BIB_HEAD.captures_iter(bibtex) {
        let kind = caps[1].to_ascii_lowercase();
        if matches!(kind.as_str(), "comment" | "preamble" | "string") {
            continue;
        }
        let key = caps[2].trim().to_string();
        if entries.iter().any(|e| e.id == key) {
            continue;
        }
        let m = caps.get(0).unwrap();
        let brace_open = bibtex[m.start()..].find('{').map(|r| m.start() + r).unwrap();
        let entry_slice = parse_balanced_delim(bibtex, brace_open, b'{', b'}')
            .map(|(s, _)| s)
            .unwrap_or(&bibtex[m.end()..]);

        let mut title = String::new();
        let mut author = String::new();
        let mut venue = String::new();
        let mut year = String::new();

        for fcap in RE_FIELD.captures_iter(entry_slice) {
            let fname = fcap[1].to_ascii_lowercase();
            let fval = fcap
                .get(2)
                .or_else(|| fcap.get(3))
                .map(|v| clean_latex_inline(v.as_str()))
                .unwrap_or_default();
            match fname.as_str() {
                "title" => title = fval,
                "author" => author = fval,
                "journal" | "booktitle" if venue.is_empty() => venue = fval,
                "year" => year = fval,
                _ => {}
            }
        }

        let mut parts = Vec::new();
        if !author.is_empty() {
            parts.push(author);
        }
        if !title.is_empty() {
            parts.push(format!("\"{title}\""));
        }
        if !venue.is_empty() {
            parts.push(venue);
        }
        if !year.is_empty() {
            parts.push(format!("({year})"));
        }
        let summary = parts.join(", ");
        if !summary.is_empty() {
            entries.push(BibSidecarEntry {
                id: sanitize_untrusted_text(&key),
                cite_key: sanitize_untrusted_text(&key),
                text: sanitize_untrusted_text(&summary),
            });
        }
    }
}

fn unescape_latex_prose(s: &str) -> String {
    s.replace("\\%", "%")
        .replace("\\&", "&")
        .replace("\\_", "_")
        .replace("\\#", "#")
        .replace("\\$", "$")
        .replace("\\{", "{")
        .replace("\\}", "}")
        .replace("---", "—")
        .replace("--", "–")
        .replace("``", "\"")
        .replace("''", "\"")
        .replace('~', " ")
}

// ============================================================================
// Tier 4: CPU-Only PDF Fallback (`PDFKit` Hook + Pure-Rust `BT..ET` Extractor)
// ============================================================================

fn extract_tier4_pdf_from_url(
    pdf_url: &str,
    canonical_url: &str,
    preloaded_bytes: Option<Vec<u8>>,
    meta: PaperMetadata,
    cfg: &OnlineResearchConfig,
    fetcher: &dyn HttpsFetcher,
) -> Result<OnlineFetchOutcome, OnlineFetchError> {
    let pdf_bytes = match preloaded_bytes {
        Some(bytes) => bytes,
        None => {
            let mut req = OutboundHttpRequest::get(pdf_url);
            req.max_bytes = MAX_PDF_BYTES;
            let resp = fetcher.execute(&req, cfg)?;
            if resp.status >= 400 {
                return Err(OnlineFetchError::TransportError(format!(
                    "HTTP {} fetching PDF '{}'",
                    resp.status, resp.final_url
                )));
            }
            resp.body
        }
    };

    if pdf_bytes.is_empty() {
        return Err(OnlineFetchError::ExtractionError(
            "Empty PDF response body".to_string(),
        ));
    }

    let (raw_extracted, tier_badge) = extract_pdf_text_with_hook_or_pure_rust(&pdf_bytes)?;
    let sanitized_body = sanitize_untrusted_text(&raw_extracted);

    let title = meta
        .title
        .clone()
        .unwrap_or_else(|| canonical_url.to_string());
    let mut full_md = String::new();
    if !sanitized_body.trim_start().starts_with("# ") {
        full_md.push_str("# ");
        full_md.push_str(&title);
        full_md.push_str("\n\n");
    }
    if let Some(ref abs) = meta.abstract_text {
        let low_body = sanitized_body.to_ascii_lowercase();
        if !abs.is_empty() && !low_body.contains("## abstract") && !low_body.contains("# abstract")
        {
            full_md.push_str("## Abstract\n\n");
            full_md.push_str(abs.trim());
            full_md.push_str("\n\n");
        }
    }
    full_md.push_str(sanitized_body.trim());

    let clean_md = normalize_markdown_blank_lines(&sanitize_untrusted_text(&full_md));
    let toc = build_markdown_toc(&clean_md);

    let mut outcome = super::staging::stage_single_document_and_build_envelope(
        &title,
        canonical_url,
        tier_badge,
        &clean_md,
        toc,
        None,
        cfg,
    )?;
    apply_metadata_to_outcome(&mut outcome, &meta);
    Ok(outcome)
}

fn extract_pdf_text_with_hook_or_pure_rust(
    pdf_bytes: &[u8],
) -> Result<(String, &'static str), OnlineFetchError> {
    if let Some(hook) = super::current_pdf_extractor_hook() {
        if let Ok(text) = hook(pdf_bytes) {
            if !text.trim().is_empty() {
                return Ok((text, "PDFKit 2-Col · CPU"));
            }
        }
    }

    let extracted = extract_pdf_streams_pure_rust(pdf_bytes);
    if !extracted.trim().is_empty() {
        return Ok((extracted, "PDF Fallback · Pure Rust"));
    }

    Err(OnlineFetchError::ExtractionError(
        "Unable to extract readable text streams from PDF".to_string(),
    ))
}

/// Pure-Rust PDF content stream text extractor (`BT ... ET` `Tj` / `TJ` / `'` / `"` + `FlateDecode`).
pub(crate) fn extract_pdf_streams_pure_rust(pdf_bytes: &[u8]) -> String {
    let mut blocks_out: Vec<String> = Vec::new();
    let mut cursor = 0usize;

    while let Some(rel_stream) = find_subslice(&pdf_bytes[cursor..], b"stream") {
        let stream_kw_pos = cursor + rel_stream;
        // Verify word boundary before `stream` (`endstream` must not match!)
        if stream_kw_pos >= 3 && &pdf_bytes[stream_kw_pos - 3..stream_kw_pos] == b"end" {
            cursor = stream_kw_pos + 6;
            continue;
        }

        let after_kw = stream_kw_pos + 6;
        let data_start = if pdf_bytes.get(after_kw..after_kw + 2) == Some(b"\r\n") {
            after_kw + 2
        } else if matches!(pdf_bytes.get(after_kw), Some(b'\n' | b'\r')) {
            after_kw + 1
        } else {
            cursor = after_kw;
            continue;
        };

        let Some(rel_end) = find_subslice(&pdf_bytes[data_start..], b"endstream") else {
            break;
        };
        let mut data_end = data_start + rel_end;
        while data_end > data_start && matches!(pdf_bytes[data_end - 1], b'\r' | b'\n') {
            data_end -= 1;
        }
        cursor = data_start + rel_end + 9;

        // Inspect preceding `<< ... >>` object dictionary
        let dict_window_start = stream_kw_pos.saturating_sub(512);
        let dict_ascii = String::from_utf8_lossy(&pdf_bytes[dict_window_start..stream_kw_pos]);
        if dict_ascii.contains("/Subtype /Image")
            || dict_ascii.contains("/Subtype/Image")
            || dict_ascii.contains("/Length1 ")
        {
            continue;
        }

        let raw_slice = &pdf_bytes[data_start..data_end];
        let decoded_stream = if dict_ascii.contains("FlateDecode") {
            match super::archive_tar::gunzip_bytes(raw_slice, 16 * 1024 * 1024) {
                Ok(buf) => buf,
                Err(_) => match inflate_gzip_or_zlib_pure_rust(raw_slice, 16 * 1024 * 1024) {
                    Ok(buf) => buf,
                    Err(_) => continue,
                },
            }
        } else {
            raw_slice.to_vec()
        };

        let text = parse_pdf_bt_et_operators(&decoded_stream);
        if !text.trim().is_empty() {
            blocks_out.push(text);
        }
    }

    // Promote clean numbered section lines (`1 Introduction`, `Abstract`, `References`) to ATX headings
    promote_pdf_section_headings(&blocks_out.join("\n\n"))
}

fn find_subslice(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || haystack.len() < needle.len() {
        return None;
    }
    haystack.windows(needle.len()).position(|w| w == needle)
}

fn parse_pdf_bt_et_operators(stream: &[u8]) -> String {
    let mut lines: Vec<String> = Vec::new();
    let mut cur_line = String::new();
    let mut pos = 0usize;

    while let Some(rel_bt) = find_pdf_keyword(&stream[pos..], b"BT") {
        let bt_start = pos + rel_bt + 2;
        let bt_end = find_pdf_keyword(&stream[bt_start..], b"ET")
            .map(|rel| bt_start + rel)
            .unwrap_or(stream.len());
        pos = (bt_end + 2).min(stream.len());

        parse_single_bt_block(&stream[bt_start..bt_end], &mut cur_line, &mut lines);
        if !cur_line.trim().is_empty() {
            lines.push(cur_line.trim().to_string());
            cur_line.clear();
        }
    }

    lines.join("\n")
}

fn find_pdf_keyword(buf: &[u8], kw: &[u8]) -> Option<usize> {
    let mut i = 0usize;
    while i + kw.len() <= buf.len() {
        if &buf[i..i + kw.len()] == kw {
            let prev_ok = i == 0 || is_pdf_delim_or_ws(buf[i - 1]);
            let next_ok = i + kw.len() == buf.len() || is_pdf_delim_or_ws(buf[i + kw.len()]);
            if prev_ok && next_ok {
                return Some(i);
            }
        }
        // Skip string literals so `(BETTER)` never matches `ET`!
        if buf[i] == b'(' {
            let (_, next_i) = parse_pdf_literal_string(buf, i);
            i = next_i;
            continue;
        }
        i += 1;
    }
    None
}

fn is_pdf_delim_or_ws(b: u8) -> bool {
    b.is_ascii_whitespace() || matches!(b, b'(' | b')' | b'<' | b'>' | b'[' | b']' | b'/' | b'%')
}

#[derive(Debug)]
enum PdfToken {
    Str(String),
    Array(Vec<PdfArrayItem>),
    Number(f64),
    Op(String),
}

#[derive(Debug)]
enum PdfArrayItem {
    Str(String),
    Num(f64),
}

fn parse_single_bt_block(block: &[u8], cur_line: &mut String, lines: &mut Vec<String>) {
    let tokens = tokenize_pdf_bt_block(block);
    let mut operand_stack: Vec<PdfToken> = Vec::new();

    for tok in tokens {
        match tok {
            PdfToken::Op(op) => {
                match op.as_str() {
                    "Tj" => {
                        if let Some(PdfToken::Str(s)) = operand_stack.pop() {
                            cur_line.push_str(&s);
                        }
                    }
                    "'" => {
                        if !cur_line.trim().is_empty() {
                            lines.push(cur_line.trim().to_string());
                            cur_line.clear();
                        }
                        if let Some(PdfToken::Str(s)) = operand_stack.pop() {
                            cur_line.push_str(&s);
                        }
                    }
                    "\"" => {
                        if !cur_line.trim().is_empty() {
                            lines.push(cur_line.trim().to_string());
                            cur_line.clear();
                        }
                        if let Some(PdfToken::Str(s)) = operand_stack.pop() {
                            cur_line.push_str(&s);
                        }
                    }
                    "TJ" => {
                        if let Some(PdfToken::Array(items)) = operand_stack.pop() {
                            for item in items {
                                match item {
                                    PdfArrayItem::Str(s) => cur_line.push_str(&s),
                                    PdfArrayItem::Num(kern) => {
                                        if kern < -120.0 && !cur_line.ends_with(' ') {
                                            cur_line.push(' ');
                                        }
                                    }
                                }
                            }
                        }
                    }
                    "T*" => {
                        if !cur_line.trim().is_empty() {
                            lines.push(cur_line.trim().to_string());
                            cur_line.clear();
                        }
                    }
                    "Td" | "TD" => {
                        let dy = match operand_stack.pop() {
                            Some(PdfToken::Number(v)) => v,
                            _ => 0.0,
                        };
                        if dy.abs() > 0.5 && !cur_line.trim().is_empty() {
                            lines.push(cur_line.trim().to_string());
                            cur_line.clear();
                        }
                    }
                    _ => {}
                }
                operand_stack.clear();
            }
            other => operand_stack.push(other),
        }
    }
}

fn tokenize_pdf_bt_block(buf: &[u8]) -> Vec<PdfToken> {
    let mut out = Vec::new();
    let mut i = 0usize;
    while i < buf.len() {
        let b = buf[i];
        if b.is_ascii_whitespace() {
            i += 1;
            continue;
        }
        if b == b'%' {
            while i < buf.len() && buf[i] != b'\n' && buf[i] != b'\r' {
                i += 1;
            }
            continue;
        }
        if b == b'(' {
            let (s, next_i) = parse_pdf_literal_string(buf, i);
            out.push(PdfToken::Str(s));
            i = next_i;
            continue;
        }
        if b == b'<' && buf.get(i + 1) != Some(&b'<') {
            let (s, next_i) = parse_pdf_hex_string(buf, i);
            out.push(PdfToken::Str(s));
            i = next_i;
            continue;
        }
        if b == b'[' {
            let (arr, next_i) = parse_pdf_tj_array(buf, i + 1);
            out.push(PdfToken::Array(arr));
            i = next_i;
            continue;
        }
        if b == b'/' {
            i += 1;
            while i < buf.len() && !is_pdf_delim_or_ws(buf[i]) {
                i += 1;
            }
            continue;
        }
        if b == b'\'' || b == b'"' {
            out.push(PdfToken::Op((b as char).to_string()));
            i += 1;
            continue;
        }
        let start = i;
        while i < buf.len() && !is_pdf_delim_or_ws(buf[i]) {
            i += 1;
        }
        if i > start {
            let word = String::from_utf8_lossy(&buf[start..i]).into_owned();
            if let Ok(num) = word.parse::<f64>() {
                out.push(PdfToken::Number(num));
            } else {
                out.push(PdfToken::Op(word));
            }
        } else {
            i += 1;
        }
    }
    out
}

fn parse_pdf_tj_array(buf: &[u8], mut i: usize) -> (Vec<PdfArrayItem>, usize) {
    let mut items = Vec::new();
    while i < buf.len() {
        let b = buf[i];
        if b.is_ascii_whitespace() {
            i += 1;
            continue;
        }
        if b == b']' {
            return (items, i + 1);
        }
        if b == b'(' {
            let (s, next_i) = parse_pdf_literal_string(buf, i);
            items.push(PdfArrayItem::Str(s));
            i = next_i;
            continue;
        }
        if b == b'<' {
            let (s, next_i) = parse_pdf_hex_string(buf, i);
            items.push(PdfArrayItem::Str(s));
            i = next_i;
            continue;
        }
        let start = i;
        while i < buf.len() && !is_pdf_delim_or_ws(buf[i]) {
            i += 1;
        }
        if i > start {
            if let Ok(n) = std::str::from_utf8(&buf[start..i])
                .unwrap_or("")
                .parse::<f64>()
            {
                items.push(PdfArrayItem::Num(n));
            }
        } else {
            i += 1;
        }
    }
    (items, i)
}

fn parse_pdf_literal_string(buf: &[u8], start: usize) -> (String, usize) {
    let mut raw_bytes = Vec::new();
    let mut depth = 1usize;
    let mut i = start + 1;

    while i < buf.len() && depth > 0 {
        let b = buf[i];
        if b == b'\\' {
            i += 1;
            if i >= buf.len() {
                break;
            }
            match buf[i] {
                b'n' => raw_bytes.push(b'\n'),
                b'r' => raw_bytes.push(b'\r'),
                b't' => raw_bytes.push(b'\t'),
                b'b' => raw_bytes.push(0x08),
                b'f' => raw_bytes.push(0x0C),
                b'(' => raw_bytes.push(b'('),
                b')' => raw_bytes.push(b')'),
                b'\\' => raw_bytes.push(b'\\'),
                b'\r' => {
                    if buf.get(i + 1) == Some(&b'\n') {
                        i += 1;
                    }
                }
                b'\n' => {}
                d @ b'0'..=b'7' => {
                    let mut oct = (d - b'0') as u32;
                    for _ in 0..2 {
                        if let Some(&nd) = buf.get(i + 1) {
                            if (b'0'..=b'7').contains(&nd) {
                                oct = oct * 8 + (nd - b'0') as u32;
                                i += 1;
                            } else {
                                break;
                            }
                        }
                    }
                    raw_bytes.push((oct & 0xFF) as u8);
                }
                other => raw_bytes.push(other),
            }
            i += 1;
            continue;
        }
        if b == b'(' {
            depth += 1;
            raw_bytes.push(b'(');
        } else if b == b')' {
            depth -= 1;
            if depth > 0 {
                raw_bytes.push(b')');
            }
        } else {
            raw_bytes.push(b);
        }
        i += 1;
    }

    (decode_pdf_string_bytes(&raw_bytes), i)
}

fn parse_pdf_hex_string(buf: &[u8], start: usize) -> (String, usize) {
    let mut hex_digits = Vec::new();
    let mut i = start + 1;
    while i < buf.len() && buf[i] != b'>' {
        if buf[i].is_ascii_hexdigit() {
            hex_digits.push(buf[i]);
        }
        i += 1;
    }
    if i < buf.len() && buf[i] == b'>' {
        i += 1;
    }
    if hex_digits.len() % 2 == 1 {
        hex_digits.push(b'0');
    }
    let mut raw = Vec::with_capacity(hex_digits.len() / 2);
    for pair in hex_digits.chunks_exact(2) {
        if let Ok(s) = std::str::from_utf8(pair) {
            if let Ok(v) = u8::from_str_radix(s, 16) {
                raw.push(v);
            }
        }
    }
    (decode_pdf_string_bytes(&raw), i)
}

fn decode_pdf_string_bytes(bytes: &[u8]) -> String {
    if bytes.len() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF {
        let u16s: Vec<u16> = bytes[2..]
            .chunks_exact(2)
            .map(|c| u16::from_be_bytes([c[0], c[1]]))
            .collect();
        return String::from_utf16_lossy(&u16s);
    }
    if let Ok(utf8) = std::str::from_utf8(bytes) {
        return utf8.to_string();
    }
    bytes.iter().map(|&b| b as char).collect()
}

fn promote_pdf_section_headings(raw_text: &str) -> String {
    static RE_PDF_SEC: Lazy<Regex> = Lazy::new(|| {
        Regex::new(r"^(\d+(?:\.\d+){0,2})\.?\s+([A-Z][A-Za-z0-9\s\-:,()]{2,64})$").unwrap()
    });
    let mut out = String::with_capacity(raw_text.len());
    for line in raw_text.lines() {
        let t = line.trim();
        if t.eq_ignore_ascii_case("abstract") || t.eq_ignore_ascii_case("references") {
            out.push_str("\n## ");
            out.push_str(t);
            out.push_str("\n\n");
            continue;
        }
        if let Some(caps) = RE_PDF_SEC.captures(t) {
            let depth = caps[1].chars().filter(|&c| c == '.').count();
            let hashes = match depth {
                0 => "##",
                1 => "###",
                _ => "####",
            };
            out.push('\n');
            out.push_str(hashes);
            out.push(' ');
            out.push_str(t);
            out.push_str("\n\n");
            continue;
        }
        out.push_str(t);
        out.push('\n');
    }
    out
}

// ============================================================================
// Pure-Rust RFC 1950 / 1951 / 1952 GZIP + ZLIB + DEFLATE & Safe `ustar` Fallback
// ============================================================================

pub(crate) fn inflate_gzip_or_zlib_pure_rust(
    input: &[u8],
    max_output: usize,
) -> Result<Vec<u8>, OnlineFetchError> {
    if input.len() < 2 {
        return Err(OnlineFetchError::ExtractionError(
            "Compressed stream truncated".to_string(),
        ));
    }

    // 1. RFC 1952 GZIP (`0x1F 0x8B 0x08`)
    if input[0] == 0x1F && input[1] == 0x8B {
        if input.len() < 18 || input[2] != 8 {
            return Err(OnlineFetchError::ExtractionError(
                "Invalid GZIP header".to_string(),
            ));
        }
        let flg = input[3];
        let mut pos = 10usize;
        if flg & 0x04 != 0 {
            if pos + 2 > input.len() {
                return Err(OnlineFetchError::ExtractionError("Truncated FEXTRA".into()));
            }
            let xlen = u16::from_le_bytes([input[pos], input[pos + 1]]) as usize;
            pos += 2 + xlen;
        }
        if flg & 0x08 != 0 {
            while pos < input.len() && input[pos] != 0 {
                pos += 1;
            }
            pos += 1;
        }
        if flg & 0x10 != 0 {
            while pos < input.len() && input[pos] != 0 {
                pos += 1;
            }
            pos += 1;
        }
        if flg & 0x02 != 0 {
            pos += 2;
        }
        if pos >= input.len() {
            return Err(OnlineFetchError::ExtractionError(
                "Truncated GZIP payload".into(),
            ));
        }
        return inflate_raw_deflate(&input[pos..], max_output);
    }

    // 2. RFC 1950 ZLIB (`CMF & 0x0F == 8` and `(CMF*256 + FLG) % 31 == 0`, no FDICT)
    let cmf = input[0] as u16;
    let flg = input[1] as u16;
    if (cmf & 0x0F) == 8 && ((cmf << 8) + flg) % 31 == 0 && (flg & 0x20) == 0 {
        return inflate_raw_deflate(&input[2..], max_output);
    }

    inflate_raw_deflate(input, max_output)
}

struct BitReader<'a> {
    data: &'a [u8],
    byte_pos: usize,
    bit_buf: u64,
    bit_count: u32,
}

impl<'a> BitReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self {
            data,
            byte_pos: 0,
            bit_buf: 0,
            bit_count: 0,
        }
    }

    fn ensure_bits(&mut self, n: u32) -> Result<(), OnlineFetchError> {
        while self.bit_count < n {
            let b = *self.data.get(self.byte_pos).ok_or_else(|| {
                OnlineFetchError::ExtractionError("Unexpected EOF in DEFLATE stream".to_string())
            })?;
            self.byte_pos += 1;
            self.bit_buf |= (b as u64) << self.bit_count;
            self.bit_count += 8;
        }
        Ok(())
    }

    fn read_bits(&mut self, n: u32) -> Result<u32, OnlineFetchError> {
        if n == 0 {
            return Ok(0);
        }
        self.ensure_bits(n)?;
        let val = (self.bit_buf & ((1u64 << n) - 1)) as u32;
        self.bit_buf >>= n;
        self.bit_count -= n;
        Ok(val)
    }

    fn align_to_byte(&mut self) {
        let drop_bits = self.bit_count % 8;
        self.bit_buf >>= drop_bits;
        self.bit_count -= drop_bits;
    }

    fn read_u16_le(&mut self) -> Result<u16, OnlineFetchError> {
        let lo = self.read_bits(8)? as u16;
        let hi = self.read_bits(8)? as u16;
        Ok(lo | (hi << 8))
    }
}

struct HuffmanDecoder {
    counts: [u16; 16],
    symbols: Vec<u16>,
}

impl HuffmanDecoder {
    fn from_lengths(lengths: &[u8]) -> Result<Self, OnlineFetchError> {
        let mut counts = [0u16; 16];
        for &len in lengths {
            if len as usize >= 16 {
                return Err(OnlineFetchError::ExtractionError(
                    "Invalid Huffman code length".into(),
                ));
            }
            if len > 0 {
                counts[len as usize] += 1;
            }
        }
        let mut offsets = [0u16; 16];
        let mut total = 0u16;
        for bits in 1..16 {
            offsets[bits] = total;
            total += counts[bits];
        }
        let mut symbols = vec![0u16; total as usize];
        for (sym, &len) in lengths.iter().enumerate() {
            if len > 0 {
                let slot = offsets[len as usize] as usize;
                symbols[slot] = sym as u16;
                offsets[len as usize] += 1;
            }
        }
        Ok(Self { counts, symbols })
    }

    fn decode(&self, br: &mut BitReader<'_>) -> Result<u16, OnlineFetchError> {
        let mut code = 0i32;
        let mut first = 0i32;
        let mut index = 0i32;
        for len in 1..16 {
            let bit = br.read_bits(1)? as i32;
            code |= bit;
            let count = self.counts[len] as i32;
            if code - count < first {
                return Ok(self.symbols[(index + (code - first)) as usize]);
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        Err(OnlineFetchError::ExtractionError(
            "Invalid Huffman code in DEFLATE stream".into(),
        ))
    }
}

fn inflate_raw_deflate(data: &[u8], max_output: usize) -> Result<Vec<u8>, OnlineFetchError> {
    const LEN_BASE: [usize; 29] = [
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
        131, 163, 195, 227, 258,
    ];
    const LEN_EXTRA: [u32; 29] = [
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
    ];
    const DIST_BASE: [usize; 30] = [
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
        2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
    ];
    const DIST_EXTRA: [u32; 30] = [
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
        13, 13,
    ];
    const CL_ORDER: [usize; 19] = [
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
    ];

    let mut br = BitReader::new(data);
    let mut out = Vec::new();

    loop {
        let bfinal = br.read_bits(1)?;
        let btype = br.read_bits(2)?;

        match btype {
            0 => {
                br.align_to_byte();
                let len = br.read_u16_le()? as usize;
                let nlen = br.read_u16_le()? as usize;
                if (len ^ 0xFFFF) != nlen {
                    return Err(OnlineFetchError::ExtractionError(
                        "Corrupt stored DEFLATE block length".into(),
                    ));
                }
                if out.len() + len > max_output {
                    return Err(OnlineFetchError::SizeOrBinaryBlocked {
                        url: "deflate://stream".into(),
                        bytes: (out.len() + len) as u64,
                        reason: "Decompressed stream exceeds byte limit".into(),
                    });
                }
                for _ in 0..len {
                    out.push(br.read_bits(8)? as u8);
                }
            }
            1 | 2 => {
                let (lit_dec, dist_dec) = if btype == 1 {
                    let mut lit_lens = [0u8; 288];
                    lit_lens[..144].fill(8);
                    lit_lens[144..256].fill(9);
                    lit_lens[256..280].fill(7);
                    lit_lens[280..288].fill(8);
                    let dist_lens = [5u8; 32];
                    (
                        HuffmanDecoder::from_lengths(&lit_lens)?,
                        HuffmanDecoder::from_lengths(&dist_lens)?,
                    )
                } else {
                    let hlit = br.read_bits(5)? as usize + 257;
                    let hdist = br.read_bits(5)? as usize + 1;
                    let hclen = br.read_bits(4)? as usize + 4;
                    let mut cl_lens = [0u8; 19];
                    for idx in 0..hclen {
                        cl_lens[CL_ORDER[idx]] = br.read_bits(3)? as u8;
                    }
                    let cl_dec = HuffmanDecoder::from_lengths(&cl_lens)?;
                    let mut combined = Vec::with_capacity(hlit + hdist);
                    while combined.len() < hlit + hdist {
                        let sym = cl_dec.decode(&mut br)?;
                        match sym {
                            0..=15 => combined.push(sym as u8),
                            16 => {
                                let prev = *combined.last().ok_or_else(|| {
                                    OnlineFetchError::ExtractionError("Invalid Huffman repeat".into())
                                })?;
                                let rep = br.read_bits(2)? as usize + 3;
                                combined.extend(std::iter::repeat(prev).take(rep));
                            }
                            17 => {
                                let rep = br.read_bits(3)? as usize + 3;
                                combined.extend(std::iter::repeat(0u8).take(rep));
                            }
                            18 => {
                                let rep = br.read_bits(7)? as usize + 11;
                                combined.extend(std::iter::repeat(0u8).take(rep));
                            }
                            _ => {
                                return Err(OnlineFetchError::ExtractionError(
                                    "Invalid code-length symbol".into(),
                                ))
                            }
                        }
                    }
                    (
                        HuffmanDecoder::from_lengths(&combined[..hlit])?,
                        HuffmanDecoder::from_lengths(&combined[hlit..hlit + hdist])?,
                    )
                };

                loop {
                    let sym = lit_dec.decode(&mut br)? as usize;
                    if sym < 256 {
                        if out.len() + 1 > max_output {
                            return Err(OnlineFetchError::SizeOrBinaryBlocked {
                                url: "deflate://stream".into(),
                                bytes: (out.len() + 1) as u64,
                                reason: "Decompressed stream exceeds byte limit".into(),
                            });
                        }
                        out.push(sym as u8);
                    } else if sym == 256 {
                        break;
                    } else if sym <= 285 {
                        let l_idx = sym - 257;
                        let length =
                            LEN_BASE[l_idx] + (br.read_bits(LEN_EXTRA[l_idx])? as usize);
                        let d_sym = dist_dec.decode(&mut br)? as usize;
                        if d_sym >= 30 {
                            return Err(OnlineFetchError::ExtractionError(
                                "Invalid DEFLATE distance symbol".into(),
                            ));
                        }
                        let dist =
                            DIST_BASE[d_sym] + (br.read_bits(DIST_EXTRA[d_sym])? as usize);
                        if dist == 0 || dist > out.len() {
                            return Err(OnlineFetchError::ExtractionError(
                                "DEFLATE backreference precedes output buffer".into(),
                            ));
                        }
                        if out.len() + length > max_output {
                            return Err(OnlineFetchError::SizeOrBinaryBlocked {
                                url: "deflate://stream".into(),
                                bytes: (out.len() + length) as u64,
                                reason: "Decompressed stream exceeds byte limit".into(),
                            });
                        }
                        for _ in 0..length {
                            let b = out[out.len() - dist];
                            out.push(b);
                        }
                    } else {
                        return Err(OnlineFetchError::ExtractionError(
                            "Reserved DEFLATE literal/length symbol".into(),
                        ));
                    }
                }
            }
            _ => {
                return Err(OnlineFetchError::ExtractionError(
                    "Invalid DEFLATE BTYPE=3".into(),
                ))
            }
        }

        if bfinal == 1 {
            break;
        }
    }

    Ok(out)
}

fn extract_ustar_regular_files_fallback(
    tar_bytes: &[u8],
    max_total_bytes: usize,
) -> Result<Vec<ExtractedArchiveFile>, OnlineFetchError> {
    let mut entries = Vec::new();
    let mut offset = 0usize;
    let mut total_bytes = 0usize;

    while offset + 512 <= tar_bytes.len() {
        let header = &tar_bytes[offset..offset + 512];
        offset += 512;
        if header.iter().all(|&b| b == 0) {
            break;
        }

        let size = parse_tar_octal(&header[124..136]);
        let typeflag = header[156];
        let padded = (size + 511) & !511;
        if offset + padded > tar_bytes.len() {
            return Err(OnlineFetchError::ExtractionError(
                "Truncated tar entry payload".into(),
            ));
        }

        // Accept strictly Regular files (`'0'` or `'\0'`), rejecting symlinks/hardlinks/FIFOs/devices!
        if typeflag == b'0' || typeflag == 0 {
            let name = read_cstr(&header[0..100]);
            let prefix = read_cstr(&header[345..500]);
            let raw_path = if prefix.is_empty() {
                name
            } else {
                format!("{prefix}/{name}")
            };
            let norm = normalize_rel_tex_path(&raw_path);
            if norm.is_empty() || raw_path.starts_with('/') || raw_path.contains("..") {
                return Err(OnlineFetchError::SecurityBlocked(format!(
                    "Unsafe tar entry path rejected: '{raw_path}'"
                )));
            }
            total_bytes = total_bytes.saturating_add(size);
            if total_bytes > max_total_bytes {
                return Err(OnlineFetchError::SizeOrBinaryBlocked {
                    url: "tar://e-print".into(),
                    bytes: total_bytes as u64,
                    reason: "Tar extraction exceeds max_total_bytes".into(),
                });
            }
            entries.push(ExtractedArchiveFile {
                rel_path: norm,
                bytes: tar_bytes[offset..offset + size].to_vec(),
            });
        } else if !matches!(typeflag, b'5' | b'x' | b'g') {
            return Err(OnlineFetchError::SecurityBlocked(format!(
                "Non-regular tar entry type '{}' rejected",
                typeflag as char
            )));
        }

        offset += padded;
    }

    Ok(entries)
}

fn parse_tar_octal(field: &[u8]) -> usize {
    let s = std::str::from_utf8(field)
        .unwrap_or("")
        .trim_matches(|c: char| c == '\0' || c.is_ascii_whitespace());
    usize::from_str_radix(s, 8).unwrap_or(0)
}

fn read_cstr(field: &[u8]) -> String {
    let end = field.iter().position(|&b| b == 0).unwrap_or(field.len());
    String::from_utf8_lossy(&field[..end]).trim().to_string()
}

// ============================================================================
// Small Pure-Rust Date & URL Utilities
// ============================================================================

fn collapse_spaces(s: &str) -> String {
    s.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn url_encode_component(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for &b in s.as_bytes() {
        if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.' | b'~' | b'/' | b':') {
            out.push(b as char);
        } else {
            out.push_str(&format!("%{b:02X}"));
        }
    }
    out
}

fn percent_decode_basic(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0usize;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let Ok(hex) = std::str::from_utf8(&bytes[i + 1..i + 3]) {
                if let Ok(val) = u8::from_str_radix(hex, 16) {
                    out.push(val);
                    i += 3;
                    continue;
                }
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Howard Hinnant's civil-from-days algorithm for UTC epoch milliseconds -> `YYYY-MM-DD`.
fn epoch_millis_to_ymd(epoch_ms: i64) -> String {
    let days = epoch_ms.div_euclid(86_400_000);
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = (yoe as i64) + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = if m <= 2 { y + 1 } else { y };
    format!("{year:04}-{m:02}-{d:02}")
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;
    use std::path::PathBuf;
    use std::sync::Arc;

    use parking_lot::Mutex;

    use super::super::{
        OnlineResearchMode, OutboundHttpResponse, register_pdf_extractor_hook,
    };
    use super::*;

    static PDF_HOOK_TEST_LOCK: Lazy<Mutex<()>> = Lazy::new(|| Mutex::new(()));

    #[derive(Default)]
    struct MockRouteFetcher {
        routes: Mutex<HashMap<String, OutboundHttpResponse>>,
        seen_requests: Mutex<Vec<OutboundHttpRequest>>,
    }

    impl MockRouteFetcher {
        fn new() -> Self {
            Self::default()
        }

        fn add_response(
            &self,
            url: &str,
            status: u16,
            headers: Vec<(&str, &str)>,
            body: Vec<u8>,
        ) {
            let mut hdr_map = BTreeMap::new();
            for (k, v) in headers {
                hdr_map.insert(k.to_ascii_lowercase(), v.to_string());
            }
            self.routes.lock().insert(
                url.to_string(),
                OutboundHttpResponse {
                    status,
                    final_url: url.to_string(),
                    headers: hdr_map,
                    body,
                },
            );
        }
    }

    impl HttpsFetcher for MockRouteFetcher {
        fn execute(
            &self,
            req: &OutboundHttpRequest,
            _cfg: &OnlineResearchConfig,
        ) -> Result<OutboundHttpResponse, OnlineFetchError> {
            self.seen_requests.lock().push(req.clone());
            if let Some(resp) = self.routes.lock().get(&req.url).cloned() {
                Ok(resp)
            } else {
                Ok(OutboundHttpResponse {
                    status: 404,
                    final_url: req.url.clone(),
                    headers: BTreeMap::new(),
                    body: b"not found".to_vec(),
                })
            }
        }
    }

    struct ScopedTestConfig {
        cfg: OnlineResearchConfig,
        dir: PathBuf,
    }

    impl Drop for ScopedTestConfig {
        fn drop(&mut self) {
            let _ = super::super::staging::unlock_staged_dir_for_cleanup(&self.dir);
            let _ = std::fs::remove_dir_all(&self.dir);
        }
    }

    fn test_config(tag: &str) -> ScopedTestConfig {
        let mut cfg = OnlineResearchConfig::with_defaults();
        cfg.mode = OnlineResearchMode::CuratedAllowlist;
        cfg.session_nonce = "deadbeef".to_string();
        let dir = PathBuf::from(format!(
            "/tmp/spell_of_t2_paper_cache_{}_{}",
            std::process::id(),
            tag
        ));
        cfg.fallback_data_dir = Some(dir.clone());
        ScopedTestConfig { cfg, dir }
    }

    /// Builds a genuine RFC 1952 GZIP byte stream using RFC 1951 stored DEFLATE blocks + IEEE CRC32.
    fn build_gzip_stored(payload: &[u8]) -> Vec<u8> {
        let mut out = vec![0x1F, 0x8B, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x03];
        if payload.is_empty() {
            out.extend_from_slice(&[0x01, 0x00, 0x00, 0xFF, 0xFF]);
        } else {
            let chunks: Vec<&[u8]> = payload.chunks(32_768).collect();
            for (idx, chunk) in chunks.iter().enumerate() {
                let bfinal = if idx + 1 == chunks.len() { 0x01u8 } else { 0x00u8 };
                out.push(bfinal);
                let len = chunk.len() as u16;
                let nlen = !len;
                out.extend_from_slice(&len.to_le_bytes());
                out.extend_from_slice(&nlen.to_le_bytes());
                out.extend_from_slice(chunk);
            }
        }
        let crc = crc32_ieee(payload);
        out.extend_from_slice(&crc.to_le_bytes());
        out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
        out
    }

    fn crc32_ieee(data: &[u8]) -> u32 {
        let mut crc = 0xFFFF_FFFFu32;
        for &b in data {
            crc ^= b as u32;
            for _ in 0..8 {
                if crc & 1 != 0 {
                    crc = (crc >> 1) ^ 0xEDB8_8320;
                } else {
                    crc >>= 1;
                }
            }
        }
        !crc
    }

    /// Builds a genuine POSIX `ustar` tar archive in memory.
    fn build_ustar_tar(files: &[(&str, &[u8])]) -> Vec<u8> {
        let mut tar = Vec::new();
        for &(path, content) in files {
            let mut header = [0u8; 512];
            header[..path.len()].copy_from_slice(path.as_bytes());
            header[100..108].copy_from_slice(b"0000644\0");
            header[108..116].copy_from_slice(b"0000000\0");
            header[116..124].copy_from_slice(b"0000000\0");
            let size_oct = format!("{:011o}\0", content.len());
            header[124..136].copy_from_slice(size_oct.as_bytes());
            header[136..148].copy_from_slice(b"14700000000\0");
            header[148..156].fill(b' ');
            header[156] = b'0';
            header[257..263].copy_from_slice(b"ustar\0");
            header[263..265].copy_from_slice(b"00");
            let cksum: u32 = header.iter().map(|&b| b as u32).sum();
            let cksum_oct = format!("{cksum:06o}\0 ");
            header[148..156].copy_from_slice(cksum_oct.as_bytes());

            tar.extend_from_slice(&header);
            tar.extend_from_slice(content);
            let rem = content.len() % 512;
            if rem != 0 {
                tar.extend(std::iter::repeat(0u8).take(512 - rem));
            }
        }
        tar.extend_from_slice(&[0u8; 1024]);
        tar
    }

    #[test]
    fn test_parse_arxiv_id_and_is_academic_paper_url_modern_legacy_and_venues() {
        let m = parse_arxiv_id("https://arxiv.org/pdf/2502.12345v2.pdf").expect("modern pdf");
        assert_eq!(m.base_id, "2502.12345");
        assert_eq!(m.version, Some(2));
        assert_eq!(m.id_with_version(), "2502.12345v2");

        let leg = parse_arxiv_id("https://arxiv.org/abs/hep-th/9901001v1").expect("legacy abs");
        assert_eq!(leg.base_id, "hep-th/9901001");
        assert_eq!(leg.version, Some(1));

        let ar5 = parse_arxiv_id("https://ar5iv.labs.arxiv.org/html/1706.03762").expect("ar5iv");
        assert_eq!(ar5.base_id, "1706.03762");
        assert_eq!(ar5.version, None);

        let doi_arx = parse_arxiv_id("https://doi.org/10.48550/arXiv.2401.09999").expect("arxiv doi");
        assert_eq!(doi_arx.base_id, "2401.09999");

        assert!(is_academic_paper_url("https://doi.org/10.1145/3581783.3612345"));
        assert!(is_academic_paper_url("doi:10.18653/v1/2024.acl-long.1"));
        assert!(is_academic_paper_url("https://openreview.net/forum?id=AbCdEf1234"));
        assert!(is_academic_paper_url("https://aclanthology.org/2024.acl-long.1/"));
        assert!(is_academic_paper_url("https://proceedings.mlr.press/v235/smith24a.html"));
        assert!(!is_academic_paper_url("https://github.com/rust-lang/rust"));
    }

    #[test]
    fn test_doi_to_arxiv_openalex_auto_upgrade_into_tier1_latexml() {
        let fetcher = MockRouteFetcher::new();
        let cfg = test_config("doi_upgrade");

        let openalex_json = br#"{
          "title": "Sparse Expert Routing",
          "publication_date": "2026-03-14",
          "authorships": [
            {"author": {"display_name": "Ada Lovelace"}},
            {"author": {"display_name": "Alan Turing"}}
          ],
          "abstract_inverted_index": {
            "Sparse": [0],
            "routing": [1],
            "accelerates": [2],
            "prefill.": [3]
          },
          "ids": {
            "doi": "https://doi.org/10.1145/9999999.1111111",
            "arxiv": "https://arxiv.org/abs/2603.04321"
          }
        }"#;

        let atom_xml = br#"<?xml version="1.0" encoding="UTF-8"?>
        <feed xmlns="http://www.w3.org/2005/Atom">
          <entry>
            <id>http://arxiv.org/abs/2603.04321v2</id>
            <published>2026-03-14T18:00:00Z</published>
            <title>Sparse Expert Routing</title>
            <summary>Sparse routing accelerates prefill.</summary>
            <author><name>Ada Lovelace</name></author>
            <author><name>Alan Turing</name></author>
          </entry>
        </feed>"#;

        let latexml_html = br#"<!DOCTYPE html><html><body>
          <article class="ltx_document">
            <h1 class="ltx_title ltx_title_document">Sparse Expert Routing</h1>
            <section class="ltx_section">
              <h2 class="ltx_title ltx_title_section">1 Method</h2>
              <p class="ltx_p">Expert score <math><annotation encoding="application/x-tex">s_i = W_r h</annotation></math>.</p>
            </section>
          </article>
        </body></html>"#;

        fetcher.add_response(
            "https://api.openalex.org/works/https://doi.org/10.1145/9999999.1111111",
            200,
            vec![("content-type", "application/json")],
            openalex_json.to_vec(),
        );
        fetcher.add_response(
            "https://export.arxiv.org/api/query?id_list=2603.04321",
            200,
            vec![("content-type", "application/atom+xml")],
            atom_xml.to_vec(),
        );
        fetcher.add_response(
            "https://arxiv.org/html/2603.04321v2",
            200,
            vec![("content-type", "text/html")],
            latexml_html.to_vec(),
        );

        let outcome = fetch_and_extract_paper(
            "https://doi.org/10.1145/9999999.1111111",
            &cfg.cfg,
            &fetcher,
        )
        .expect("DOI upgraded to arXiv Tier 1 HTML5");

        assert_eq!(outcome.citation.canonical_url, "https://arxiv.org/abs/2603.04321v2");
        assert_eq!(outcome.citation.tier_badge, "arXiv HTML5 · Lossless Math");
        assert_eq!(
            outcome.citation.authors,
            vec!["Ada Lovelace".to_string(), "Alan Turing".to_string()]
        );
        assert_eq!(outcome.citation.published.as_deref(), Some("2026-03-14"));
        assert!(outcome.envelope_markdown.contains("$s_i = W_r h$"));
    }

    #[test]
    fn test_tier1_redirect_and_ltx_error_fallback_to_tier2_eprint_tar_gz_with_input() {
        let fetcher = MockRouteFetcher::new();
        let cfg = test_config("t1_to_t2_targz");

        // Tier 1A (`arxiv.org/html`) returns HTML with `ltx_ERROR` inside an equation -> rejected!
        let broken_html = br#"<article class="ltx_document">
          <table class="ltx_equation"><tr class="ltx_eqn_row"><td class="ltx_eqn_cell">
            <math><span class="ltx_ERROR">\brokenmacro</span></math>
          </td></tr></table>
        </article>"#;
        fetcher.add_response(
            "https://arxiv.org/html/2502.54321v1",
            200,
            vec![("content-type", "text/html")],
            broken_html.to_vec(),
        );

        // Tier 1B (`ar5iv`) redirects 307 to `/abs/2502.54321` -> detected with `follow_redirects = false`!
        fetcher.add_response(
            "https://ar5iv.labs.arxiv.org/html/2502.54321",
            307,
            vec![("location", "https://arxiv.org/abs/2502.54321")],
            Vec::new(),
        );

        // Tier 2 (`export.arxiv.org/e-print/2502.54321v1`) serves `.tar.gz` with main.tex + \input{sections/method} + main.bbl!
        let main_tex = br#"\documentclass{article}
\title{Exact Latent Verification}
\author{Grace Hopper \and Emmy Noether}
\begin{document}
\maketitle
\begin{abstract}
We prove 100\% convergence % trailing TeX comment stripped!
under bounded step sizes.
\end{abstract}
% \input{sections/deleted_draft}
\input{sections/method}
\end{document}
"#;

        let method_tex = br#"\section{Core Algorithm}
Given $\alpha_t > 0$, update via \textbf{projected} gradient:
\begin{equation}
x_{t+1} = \Pi_{\mathcal{X}}(x_t - \alpha_t \nabla f(x_t))
\end{equation}
\subsection{Reference Kernel}
\begin{lstlisting}[language=Python]
def update(x, grad, lr):
    return x - lr * grad
\end{lstlisting}
"#;

        let main_bbl = br#"\begin{thebibliography}{1}
\bibitem[Hopper(2025)]{hopper2025}
G.~Hopper.
\newblock Verified compilers for tensor algebra, 2025.
\end{thebibliography}
"#;

        let tar_bytes = build_ustar_tar(&[
            ("main.tex", main_tex),
            ("sections/method.tex", method_tex),
            ("main.bbl", main_bbl),
        ]);
        let gz_bytes = build_gzip_stored(&tar_bytes);

        fetcher.add_response(
            "https://export.arxiv.org/e-print/2502.54321v1",
            200,
            vec![("content-type", "application/x-eprint-tar")],
            gz_bytes,
        );

        let outcome = fetch_and_extract_paper(
            "https://arxiv.org/abs/2502.54321v1",
            &cfg.cfg,
            &fetcher,
        )
        .expect("Tier 2 e-print .tar.gz extracted");

        // Verify `ar5iv` was called with `follow_redirects == false`
        let reqs = fetcher.seen_requests.lock();
        let ar5iv_req = reqs
            .iter()
            .find(|r| r.url.contains("ar5iv.labs.arxiv.org"))
            .expect("ar5iv probed");
        assert!(!ar5iv_req.follow_redirects);

        assert_eq!(outcome.citation.tier_badge, "arXiv e-print TeX");
        assert_eq!(outcome.citation.title, "Exact Latent Verification");
        assert_eq!(
            outcome.citation.authors,
            vec!["Grace Hopper".to_string(), "Emmy Noether".to_string()]
        );

        let doc_path = outcome
            .staged_files
            .iter()
            .find(|p| p.ends_with("document.md"))
            .expect("staged document.md");
        let full_md = std::fs::read_to_string(doc_path).unwrap();

        // Escaped `100\%` preserved while `% trailing TeX comment` and commented `\input{deleted_draft}` stripped
        assert!(full_md.contains("## Abstract"));
        assert!(full_md.contains("100% convergence"));
        assert!(!full_md.contains("trailing TeX comment"));
        // Recursive `\input{sections/method}` resolved `.tex` suffix and rendered sections, math & code fence
        assert!(full_md.contains("# Core Algorithm"));
        assert!(full_md.contains("$\\alpha_t > 0$"));
        assert!(full_md.contains("**projected**"));
        assert!(full_md.contains("\\begin{equation}\nx_{t+1} = \\Pi_{\\mathcal{X}}(x_t - \\alpha_t \\nabla f(x_t))\n\\end{equation}"));
        assert!(full_md.contains("```python\ndef update(x, grad, lr):\n    return x - lr * grad\n```"));
        // `.bbl` references attached + staged `bib.json`
        assert!(full_md.contains("# References"));
        assert!(full_md.contains("Verified compilers for tensor algebra"));
        assert!(outcome.staged_files.iter().any(|p| p.ends_with("bib.json")));
    }

    #[test]
    fn test_tier2_standalone_single_file_tex_eprint() {
        let fetcher = MockRouteFetcher::new();
        let cfg = test_config("t2_single_tex");

        let single_tex = br#"\documentclass{article}
\title{Single File Preprint}
\begin{document}
\section{Introduction}
Inline $\beta \in (0, 1)$ and aligned system:
\begin{align}
u &= A x \\
v &= B y
\end{align}
\end{document}
"#;
        let gz_single = build_gzip_stored(single_tex);
        fetcher.add_response(
            "https://export.arxiv.org/e-print/9901001",
            404,
            vec![],
            Vec::new(),
        );
        fetcher.add_response(
            "https://export.arxiv.org/e-print/hep-th/9901001",
            200,
            vec![("content-type", "application/x-eprint")],
            gz_single,
        );

        let outcome = fetch_and_extract_paper(
            "https://arxiv.org/pdf/hep-th/9901001.pdf",
            &cfg.cfg,
            &fetcher,
        )
        .expect("single-file legacy e-print");

        assert_eq!(outcome.citation.tier_badge, "arXiv e-print TeX");
        assert!(outcome.envelope_markdown.contains("# Introduction"));
        assert!(outcome.envelope_markdown.contains("$\\beta \\in (0, 1)$"));
        assert!(outcome.envelope_markdown.contains("\\begin{align}\nu &= A x \\\\\nv &= B y\n\\end{align}"));
    }

    #[test]
    fn test_tier4_pdf_hook_and_openreview_metadata_population() {
        let _hook_guard = PDF_HOOK_TEST_LOCK.lock();
        let fetcher = MockRouteFetcher::new();
        let cfg = test_config("t4_pdf_hook");

        let openreview_json = br#"{
          "notes": [{
            "pdate": 1739836800000,
            "content": {
              "title": {"value": "Anonymous Double-Blind Submission"},
              "authors": {"value": ["Anonymous"]},
              "abstract": {"value": "Two-column layout benchmark."}
            }
          }]
        }"#;
        fetcher.add_response(
            "https://api2.openreview.net/notes?id=XyZ987",
            200,
            vec![("content-type", "application/json")],
            openreview_json.to_vec(),
        );
        fetcher.add_response(
            "https://openreview.net/pdf?id=XyZ987",
            200,
            vec![("content-type", "application/pdf")],
            b"%PDF-1.7\nmock binary pdf bytes\n%%EOF".to_vec(),
        );

        // Register CPU-only PDFKit hook
        register_pdf_extractor_hook(Arc::new(|bytes: &[u8]| {
            assert!(bytes.starts_with(b"%PDF-1.7"));
            Ok("## 1 Introduction\n\nﬁrst column deﬁnition before right column.\n\n## 2 Experiments\n\n285 tok/s throughput.".to_string())
        }));

        let outcome = fetch_and_extract_paper(
            "https://openreview.net/forum?id=XyZ987",
            &cfg.cfg,
            &fetcher,
        )
        .expect("Tier 4 PDFKit hook extraction");

        assert_eq!(outcome.citation.tier_badge, "PDFKit 2-Col · CPU");
        assert_eq!(outcome.citation.title, "Anonymous Double-Blind Submission");
        assert_eq!(outcome.citation.authors, vec!["Anonymous".to_string()]);
        assert_eq!(outcome.citation.published.as_deref(), Some("2025-02-18"));
        // Ligatures `ﬁ` normalized to `fi` and Tier 0 Abstract prepended
        assert!(outcome.envelope_markdown.contains("## Abstract"));
        assert!(outcome.envelope_markdown.contains("first column definition before right column."));

        register_pdf_extractor_hook(Arc::new(|_| Err("hook reset".into())));
    }

    #[test]
    fn test_tier4_pure_rust_pdf_bt_et_flatedecode_stream_fallback() {
        let _hook_guard = PDF_HOOK_TEST_LOCK.lock();
        register_pdf_extractor_hook(Arc::new(|_| Err("force pure-rust fallback".into())));

        let fetcher = MockRouteFetcher::new();
        let cfg = test_config("t4_pure_rust_pdf");

        let content_stream = b"BT\n/F1 12 Tf\n72 720 Td\n(1 Introduction) Tj\n0 -18 Td\n[(Pure) -220 (Rust) -220 (\xEF\xAC\x82ow) -220 (extractions!)] TJ\nET\n";
        let compressed_stream = build_gzip_stored(content_stream);

        let mut synthetic_pdf = b"%PDF-1.5\n1 0 obj\n<< /Length 128 /Filter /FlateDecode >>\nstream\r\n".to_vec();
        synthetic_pdf.extend_from_slice(&compressed_stream);
        synthetic_pdf.extend_from_slice(b"\r\nendstream\nendobj\n%%EOF");

        fetcher.add_response(
            "https://arxiv.org/pdf/9501.0001.pdf",
            200,
            vec![("content-type", "application/pdf")],
            synthetic_pdf,
        );

        let outcome = fetch_and_extract_paper(
            "https://arxiv.org/pdf/9501.0001.pdf",
            &cfg.cfg,
            &fetcher,
        )
        .expect("pure-Rust PDF stream fallback");

        assert_eq!(outcome.citation.tier_badge, "PDF Fallback · Pure Rust");
        assert!(outcome.envelope_markdown.contains("## 1 Introduction"));
        // `TJ` kerning inserted spaces for `-220` and normalized `\u{FB02}` (`ﬂ`) -> `fl`!
        assert!(outcome.envelope_markdown.contains("Pure Rust flow extractions!"));
    }

    #[test]
    fn test_acl_anthology_to_arxiv_semantic_scholar_upgrade_and_ar5iv_fallback() {
        let fetcher = MockRouteFetcher::new();
        let cfg = test_config("acl_s2_ar5iv");

        let s2_json = br#"{
          "title": "Sub-Microsecond IPC for MoE Inference",
          "publicationDate": "2025-07-10",
          "authors": [{"name": "John von Neumann"}],
          "externalIds": {"ArXiv": "2507.01010"}
        }"#;

        let ar5iv_html = br#"<!DOCTYPE html><html><body>
          <article class="ltx_document">
            <h1 class="ltx_title ltx_title_document">Sub-Microsecond IPC for MoE Inference</h1>
            <section class="ltx_section">
              <h2 class="ltx_title ltx_title_section">1 Doorbell Design</h2>
              <p class="ltx_p">Latency bounded by <math><annotation encoding="application/x-tex">\Delta t \le 0.8\,\mu\text{s}</annotation></math>.</p>
            </section>
          </article>
        </body></html>"#;

        fetcher.add_response(
            "https://api.semanticscholar.org/graph/v1/paper/ACL:2025.acl-long.42?fields=title,authors,year,publicationDate,abstract,externalIds,openAccessPdf",
            200,
            vec![("content-type", "application/json")],
            s2_json.to_vec(),
        );
        // arxiv.org/html returns 404, ar5iv succeeds with 200!
        fetcher.add_response(
            "https://ar5iv.labs.arxiv.org/html/2507.01010",
            200,
            vec![("content-type", "text/html")],
            ar5iv_html.to_vec(),
        );

        let outcome = fetch_and_extract_paper(
            "https://aclanthology.org/2025.acl-long.42/",
            &cfg.cfg,
            &fetcher,
        )
        .expect("ACL upgraded to ar5iv HTML5");

        assert_eq!(outcome.citation.tier_badge, "ar5iv HTML5 · Lossless Math");
        assert_eq!(outcome.citation.authors, vec!["John von Neumann".to_string()]);
        assert_eq!(outcome.citation.published.as_deref(), Some("2025-07-10"));
        assert!(outcome.envelope_markdown.contains("$\\Delta t \\le 0.8\\,\\mu\\text{s}$"));
    }

    #[test]
    fn test_tex_input_cycle_and_depth_guard_with_bibtex_parsing() {
        let fetcher = MockRouteFetcher::new();
        let cfg = test_config("tex_cycle_bib");

        // `main.tex` -> `a.tex` -> `b.tex` -> `a.tex` cycle must terminate cleanly without infinite recursion!
        let main_tex = br#"\documentclass{article}
\title{Cycle Safe TeX Expansion}
\begin{document}
\input{a}
\bibliography{refs}
\end{document}
"#;
        let a_tex = br#"\section{Alpha}
Content A.
\input{b}
"#;
        let b_tex = br#"\subsection{Beta}
Content B.
\input{a}
"#;
        let refs_bib = br#"@inproceedings{vaswani2017,
  title = {Attention Is All You Need},
  author = {Vaswani, Ashish and Shazeer, Noam},
  booktitle = {NeurIPS},
  year = {2017}
}
"#;

        let tar_bytes = build_ustar_tar(&[
            ("main.tex", main_tex),
            ("a.tex", a_tex),
            ("b.tex", b_tex),
            ("refs.bib", refs_bib),
        ]);
        let gz_bytes = build_gzip_stored(&tar_bytes);

        fetcher.add_response(
            "https://export.arxiv.org/e-print/2508.00001",
            200,
            vec![("content-type", "application/x-eprint-tar")],
            gz_bytes,
        );

        let outcome = fetch_and_extract_paper(
            "https://arxiv.org/abs/2508.00001",
            &cfg.cfg,
            &fetcher,
        )
        .expect("cycle-safe TeX + .bib extraction");

        assert!(outcome.envelope_markdown.contains("# Alpha"));
        assert!(outcome.envelope_markdown.contains("## Beta"));
        assert!(outcome.envelope_markdown.contains("Attention Is All You Need"));
        assert!(outcome.staged_files.iter().any(|p| p.ends_with("bib.json")));
    }

    #[test]
    fn test_pure_rust_deflate_dynamic_and_fixed_huffman_roundtrip() {
        // 1. Genuine RFC 1952 gzip payload (`BTYPE=01` Fixed Huffman + LZ77 backreference)
        let gz_fixed: &[u8] = &[
            31, 139, 8, 0, 0, 0, 0, 0, 0, 255, 243, 72, 205, 201, 201, 87, 72, 203, 172, 72, 77,
            81, 240, 40, 77, 75, 203, 77, 204, 83, 72, 175, 202, 44, 80, 40, 207, 47, 202, 73, 81,
            84, 240, 192, 47, 15, 0, 221, 162, 219, 31, 63, 0, 0, 0,
        ];
        let decoded_fixed =
            inflate_gzip_or_zlib_pure_rust(gz_fixed, 1024).expect("fixed Huffman + LZ77");
        assert_eq!(
            std::str::from_utf8(&decoded_fixed).unwrap(),
            "Hello fixed Huffman gzip world! Hello fixed Huffman gzip world!"
        );

        // 2. Genuine RFC 1950 zlib (`0x78 0xDA`, `BTYPE=10` Dynamic Huffman + LZ77, matching PDF FlateDecode)
        let zlib_dynamic: &[u8] = &[
            120, 218, 237, 205, 49, 14, 131, 48, 16, 68, 209, 171, 12, 18, 93, 206, 64, 71, 145,
            67, 184, 25, 204, 58, 94, 201, 235, 88, 246, 146, 40, 183, 15, 199, 160, 160, 249, 205,
            47, 222, 250, 171, 52, 141, 120, 30, 41, 25, 43, 156, 91, 17, 124, 164, 107, 210, 72,
            215, 119, 197, 87, 61, 163, 75, 19, 186, 236, 48, 122, 150, 51, 231, 45, 104, 185, 115,
            200, 192, 28, 88, 90, 38, 30, 8, 155, 56, 177, 32, 188, 104, 198, 121, 194, 122, 3, 55,
            112, 125, 224, 15, 254, 206, 21, 56,
        ];
        let decoded_dyn =
            inflate_gzip_or_zlib_pure_rust(zlib_dynamic, 4096).expect("dynamic Huffman + LZ77");
        let expected_dyn = "Dynamic Huffman table verification with repeated mathematical phrases $\\alpha + \\beta = \\gamma$! ".repeat(8);
        assert_eq!(std::str::from_utf8(&decoded_dyn).unwrap(), expected_dyn);
    }

    #[test]
    fn test_hard_stop_challenge_or_security_error_never_bypassed_across_tiers() {
        struct ChallengeFetcher;
        impl HttpsFetcher for ChallengeFetcher {
            fn execute(
                &self,
                _req: &OutboundHttpRequest,
                _cfg: &OnlineResearchConfig,
            ) -> Result<OutboundHttpResponse, OnlineFetchError> {
                Err(OnlineFetchError::ChallengeOrRateLimited {
                    domain: "arxiv.org".to_string(),
                    status: 429,
                    detail: "Anti-bot CAPTCHA gate".to_string(),
                })
            }
        }

        let cfg = test_config("hard_stop");
        let err = fetch_and_extract_paper(
            "https://arxiv.org/abs/2502.12345",
            &cfg.cfg,
            &ChallengeFetcher,
        )
        .expect_err("must halt immediately on challenge");
        assert!(matches!(err, OnlineFetchError::ChallengeOrRateLimited { status: 429, .. }));
    }
}
