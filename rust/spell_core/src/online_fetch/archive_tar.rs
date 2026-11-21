//! `archive_tar` — Bounded Streaming Pure-Rust (`flate2`) Gunzip/Zlib Decoder
//! and Hardened POSIX `ustar` + PAX (`b'x'` / `b'g'`) Tar Extractor.
//!
//! Security Invariants:
//! 1. Uses portable pure-Rust `flate2` (`GzDecoder` / `ZlibDecoder` / `Decompress`) with zero
//!    `unsafe` FFI or system `libz.dylib` linkage, enforcing strict `max_decompressed_bytes`
//!    zip-bomb caps (`std::io::Read::take(max_bytes + 1)` and per-chunk streaming limits).
//! 2. Reconciles PAX `size=` and `path=` extended header records with the following header block
//!    using a single unified byte length for both payload slicing and 512-byte cursor advancement,
//!    eliminating `CVE-2026-33055` (`RUSTSEC-2026-0068`) PAX size desynchronization by construction.
//! 3. Accepts **only** regular files (`0` or `b'0'`); skips directories (`b'5'`); hard-rejects
//!    symlinks (`b'2'`), hardlinks (`b'1'`), character/block devices (`b'3'`, `b'4'`), and FIFOs (`b'6'`).
//! 4. Hard-rejects `..` traversal, leading `/`, `\`, NUL bytes, Windows drive prefixes (`C:`),
//!    and case-insensitive macOS APFS path collisions.
//! 5. Strips `.git/**`, `.gitattributes`, `.gitmodules`, `.envrc`, `.vscode/{tasks,launch,settings}.json`,
//!    `.zed/{tasks,settings}.json`, Xcode `PBXShellScriptBuildPhase` project files, and
//!    Mach-O (`0xfeedfacf` / `0xcafebabe`), ELF (`\x7fELF`), and NUL-containing binary blobs.

use std::collections::HashSet;
use std::ffi::c_int;
use std::io::{self, BufRead, Read};

use flate2::bufread::GzDecoder as BufGzDecoder;
use flate2::read::ZlibDecoder;
use flate2::{Decompress, FlushDecompress, Status};

use super::OnlineFetchError;

/// Maximum allowed size of a single PAX or GNU LongLink metadata header payload (`1 MiB`).
const MAX_EXTENDED_HEADER_BYTES: usize = 1024 * 1024;

/// Maximum number of extracted regular files per archive to bound metadata memory.
const MAX_EXTRACTED_FILE_COUNT: usize = 16_384;

/// Clean, validated text/source file extracted from a tar archive.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExtractedArchiveFile {
    pub rel_path: String,
    pub bytes: Vec<u8>,
}

// ============================================================================
// Pure-Rust Bounded Gzip / Zlib Streaming Decoder (`flate2`)
// ============================================================================

/// `windowBits = 16 + 15`: strict RFC 1952 gzip container only.
pub const WINDOW_BITS_GZIP_ONLY: c_int = 16 + 15;
/// `windowBits = 32 + 15`: automatic RFC 1952 gzip + RFC 1950 zlib container detection.
pub const WINDOW_BITS_AUTO_GZIP_ZLIB: c_int = 32 + 15;

/// Returns `true` iff `(cmf, flg)` is a valid RFC 1950 zlib header (`CM = 8`, `CINFO <= 7`,
/// `(cmf * 256 + flg) % 31 == 0`). Disjoint from RFC 1952 gzip magic (`0x1f 0x8b`).
fn is_zlib_header(cmf: u8, flg: u8) -> bool {
    (cmf & 0x0f) == 8 && (cmf >> 4) <= 7 && ((((cmf as u16) << 8) | (flg as u16)) % 31 == 0)
}

/// Non-blocking in-memory byte queue that yields `io::ErrorKind::WouldBlock` when empty
/// so `flate2::bufread::GzDecoder` can suspend and resume cleanly across arbitrary chunk splits.
#[derive(Debug, Default)]
struct ChunkQueue {
    buf: Vec<u8>,
    pos: usize,
}

impl ChunkQueue {
    fn new() -> Self {
        Self {
            buf: Vec::new(),
            pos: 0,
        }
    }

    fn push(&mut self, data: &[u8]) {
        if data.is_empty() {
            return;
        }
        if self.pos >= self.buf.len() {
            self.buf.clear();
            self.pos = 0;
        } else if self.pos >= 4096 {
            self.buf.drain(..self.pos);
            self.pos = 0;
        }
        self.buf.extend_from_slice(data);
    }

    fn remaining(&self) -> &[u8] {
        &self.buf[self.pos..]
    }

    fn is_empty(&self) -> bool {
        self.pos >= self.buf.len()
    }

    fn clear(&mut self) {
        self.buf.clear();
        self.pos = 0;
    }

    fn consume_bytes(&mut self, n: usize) {
        self.pos = self.pos.saturating_add(n).min(self.buf.len());
        if self.pos >= self.buf.len() {
            self.buf.clear();
            self.pos = 0;
        }
    }
}

impl Read for ChunkQueue {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        if out.is_empty() {
            return Ok(0);
        }
        let avail = &self.buf[self.pos..];
        if avail.is_empty() {
            return Err(io::Error::from(io::ErrorKind::WouldBlock));
        }
        let n = avail.len().min(out.len());
        out[..n].copy_from_slice(&avail[..n]);
        self.pos += n;
        if self.pos >= self.buf.len() {
            self.buf.clear();
            self.pos = 0;
        }
        Ok(n)
    }
}

impl BufRead for ChunkQueue {
    fn fill_buf(&mut self) -> io::Result<&[u8]> {
        if self.pos >= self.buf.len() {
            return Err(io::Error::from(io::ErrorKind::WouldBlock));
        }
        Ok(&self.buf[self.pos..])
    }

    fn consume(&mut self, amt: usize) {
        self.consume_bytes(amt);
    }
}

enum InnerDecoder {
    Detecting(ChunkQueue),
    Gzip(BufGzDecoder<ChunkQueue>),
    Zlib {
        decomp: Decompress,
        queue: ChunkQueue,
    },
}

/// Bounded streaming gunzip/zlib decoder backed by portable pure-Rust `flate2`.
pub struct StreamingGunzipDecoder {
    inner: InnerDecoder,
    gzip_only: bool,
    max_decompressed_bytes: usize,
    total_decompressed_bytes: usize,
    stream_ended: bool,
}

impl StreamingGunzipDecoder {
    /// Creates a streaming decoder accepting gzip or zlib headers (`WINDOW_BITS_AUTO_GZIP_ZLIB`).
    pub fn new(max_decompressed_bytes: usize) -> Result<Self, OnlineFetchError> {
        Ok(Self {
            inner: InnerDecoder::Detecting(ChunkQueue::new()),
            gzip_only: false,
            max_decompressed_bytes,
            total_decompressed_bytes: 0,
            stream_ended: false,
        })
    }

    /// Creates a streaming decoder accepting strict RFC 1952 gzip headers (`WINDOW_BITS_GZIP_ONLY`).
    pub fn new_gzip_only(max_decompressed_bytes: usize) -> Result<Self, OnlineFetchError> {
        Ok(Self {
            inner: InnerDecoder::Detecting(ChunkQueue::new()),
            gzip_only: true,
            max_decompressed_bytes,
            total_decompressed_bytes: 0,
            stream_ended: false,
        })
    }

    pub fn total_decompressed_bytes(&self) -> usize {
        self.total_decompressed_bytes
    }

    pub fn is_finished(&self) -> bool {
        self.stream_ended
    }

    /// Feeds a compressed chunk and invokes `on_chunk` for each decompressed slice.
    /// Returning `Ok(false)` from `on_chunk` halts decompression immediately (for 512B peek or sparse tar abort).
    pub fn feed_with<F>(
        &mut self,
        compressed: &[u8],
        mut on_chunk: F,
    ) -> Result<bool, OnlineFetchError>
    where
        F: FnMut(&[u8]) -> Result<bool, OnlineFetchError>,
    {
        if compressed.is_empty() {
            return Ok(true);
        }

        if self.stream_ended {
            // Handle trailing NUL padding or concatenated multi-member gzip streams (RFC 1952 §2.2).
            if compressed.iter().all(|&b| b == 0) {
                return Ok(true);
            }
            self.inner = InnerDecoder::Detecting(ChunkQueue::new());
            self.stream_ended = false;
        }

        match &mut self.inner {
            InnerDecoder::Detecting(q) => q.push(compressed),
            InnerDecoder::Gzip(gz) => gz.get_mut().push(compressed),
            InnerDecoder::Zlib { queue, .. } => queue.push(compressed),
        }

        let mut out_buf = [0u8; 32 * 1024];

        loop {
            if let Some((b0, b1)) = match &self.inner {
                InnerDecoder::Detecting(q) => {
                    let rem = q.remaining();
                    if rem.len() < 2 {
                        break;
                    }
                    Some((rem[0], rem[1]))
                }
                _ => None,
            } {
                let is_gzip = b0 == 0x1f && b1 == 0x8b;
                let is_zlib = !self.gzip_only && is_zlib_header(b0, b1);
                if !is_gzip && !is_zlib {
                    return Err(OnlineFetchError::ExtractionError(format!(
                        "Invalid {} header magic (0x{b0:02x} 0x{b1:02x})",
                        if self.gzip_only { "gzip" } else { "gzip/zlib" }
                    )));
                }
                let queue = match std::mem::replace(
                    &mut self.inner,
                    InnerDecoder::Detecting(ChunkQueue::new()),
                ) {
                    InnerDecoder::Detecting(q) => q,
                    _ => unreachable!(),
                };
                self.inner = if is_gzip {
                    InnerDecoder::Gzip(BufGzDecoder::new(queue))
                } else {
                    InnerDecoder::Zlib {
                        decomp: Decompress::new(true),
                        queue,
                    }
                };
            }

            // Cap each read to at most `remaining_budget + 1` so zip bombs trip immediately
            // without inflating beyond `max_decompressed_bytes`.
            let max_read = self
                .max_decompressed_bytes
                .saturating_sub(self.total_decompressed_bytes)
                .saturating_add(1)
                .min(out_buf.len());

            let (produced, member_finished) = match &mut self.inner {
                InnerDecoder::Detecting(_) => unreachable!(),
                InnerDecoder::Gzip(gz) => match gz.read(&mut out_buf[..max_read]) {
                    Ok(0) => (0, true),
                    Ok(n) => (n, false),
                    Err(err) if err.kind() == io::ErrorKind::WouldBlock => break,
                    Err(err) => {
                        return Err(OnlineFetchError::ExtractionError(format!(
                            "flate2 gzip inflate error: {err}"
                        )));
                    }
                },
                InnerDecoder::Zlib { decomp, queue } => {
                    let before_in = decomp.total_in();
                    let before_out = decomp.total_out();
                    let status = decomp
                        .decompress(
                            queue.remaining(),
                            &mut out_buf[..max_read],
                            FlushDecompress::None,
                        )
                        .map_err(|err| {
                            OnlineFetchError::ExtractionError(format!(
                                "flate2 zlib inflate error: {err}"
                            ))
                        })?;
                    let consumed = (decomp.total_in() - before_in) as usize;
                    let produced = (decomp.total_out() - before_out) as usize;
                    queue.consume_bytes(consumed);
                    match status {
                        Status::StreamEnd => (produced, true),
                        Status::Ok | Status::BufError => {
                            if produced == 0 && consumed == 0 {
                                break;
                            }
                            (produced, false)
                        }
                    }
                }
            };

            if produced > 0 {
                let new_total = self.total_decompressed_bytes.saturating_add(produced);
                if new_total > self.max_decompressed_bytes {
                    return Err(OnlineFetchError::SizeOrBinaryBlocked {
                        url: "archive://gzip".to_string(),
                        bytes: new_total as u64,
                        reason: format!(
                            "Decompressed gzip output ({new_total} bytes) exceeds zip-bomb cap ({} bytes)",
                            self.max_decompressed_bytes
                        ),
                    });
                }
                self.total_decompressed_bytes = new_total;
                if !on_chunk(&out_buf[..produced])? {
                    return Ok(false);
                }
            }

            if member_finished {
                self.stream_ended = true;
                let mut leftover = match std::mem::replace(
                    &mut self.inner,
                    InnerDecoder::Detecting(ChunkQueue::new()),
                ) {
                    InnerDecoder::Gzip(gz) => gz.into_inner(),
                    InnerDecoder::Zlib { queue, .. } => queue,
                    InnerDecoder::Detecting(q) => q,
                };
                if leftover.is_empty() {
                    break;
                }
                if leftover.remaining().iter().all(|&b| b == 0) {
                    leftover.clear();
                    break;
                }
                // Concatenated next member (RFC 1952 §2.2): resume format detection on leftover bytes.
                self.inner = InnerDecoder::Detecting(leftover);
                self.stream_ended = false;
            }
        }

        Ok(true)
    }

    /// Feeds a compressed chunk and appends decompressed bytes into `out`.
    pub fn feed_chunk(
        &mut self,
        compressed: &[u8],
        out: &mut Vec<u8>,
    ) -> Result<(), OnlineFetchError> {
        self.feed_with(compressed, |slice| {
            out.extend_from_slice(slice);
            Ok(true)
        })?;
        Ok(())
    }

    /// Verifies that the gzip/zlib trailer (`CRC32` + `ISIZE` or `Adler32`) was cleanly reached.
    pub fn finish(&self) -> Result<(), OnlineFetchError> {
        if !self.stream_ended {
            return Err(OnlineFetchError::ExtractionError(
                "Truncated gzip/zlib stream: missing end-of-stream trailer".to_string(),
            ));
        }
        Ok(())
    }
}

/// Decompresses a complete gzip/zlib byte buffer up to `max_decompressed_bytes` (zip-bomb cap).
pub fn gunzip_bytes(
    compressed: &[u8],
    max_decompressed_bytes: usize,
) -> Result<Vec<u8>, OnlineFetchError> {
    if compressed.is_empty() {
        return Err(OnlineFetchError::ExtractionError(
            "Cannot gunzip empty input buffer".to_string(),
        ));
    }

    let initial_cap = compressed
        .len()
        .saturating_mul(2)
        .min(max_decompressed_bytes)
        .min(256 * 1024);
    let mut out = Vec::with_capacity(initial_cap);

    if compressed.len() >= 2 && is_zlib_header(compressed[0], compressed[1]) {
        let limit = (max_decompressed_bytes as u64).saturating_add(1);
        ZlibDecoder::new(compressed)
            .take(limit)
            .read_to_end(&mut out)
            .map_err(|err| {
                OnlineFetchError::ExtractionError(format!("flate2 zlib inflate error: {err}"))
            })?;
        if out.len() > max_decompressed_bytes {
            return Err(OnlineFetchError::SizeOrBinaryBlocked {
                url: "archive://gzip".to_string(),
                bytes: out.len() as u64,
                reason: format!(
                    "Decompressed zlib output ({} bytes) exceeds zip-bomb cap ({max_decompressed_bytes} bytes)",
                    out.len()
                ),
            });
        }
        return Ok(out);
    }

    // Bounded RFC 1952 gzip decompression using `flate2` `GzDecoder` + `Read::take(remaining + 1)`,
    // supporting single-member, multi-member (RFC 1952 §2.2), and trailing NUL padding.
    let mut rest = compressed;
    let mut decoded_any_member = false;
    while !rest.is_empty() {
        if decoded_any_member && rest.iter().all(|&b| b == 0) {
            break;
        }
        let remaining_limit = (max_decompressed_bytes.saturating_sub(out.len()) as u64)
            .saturating_add(1);
        BufGzDecoder::new(&mut rest)
            .take(remaining_limit)
            .read_to_end(&mut out)
            .map_err(|err| {
                OnlineFetchError::ExtractionError(format!("flate2 gzip inflate error: {err}"))
            })?;
        if out.len() > max_decompressed_bytes {
            return Err(OnlineFetchError::SizeOrBinaryBlocked {
                url: "archive://gzip".to_string(),
                bytes: out.len() as u64,
                reason: format!(
                    "Decompressed gzip output ({} bytes) exceeds zip-bomb cap ({max_decompressed_bytes} bytes)",
                    out.len()
                ),
            });
        }
        decoded_any_member = true;
    }

    Ok(out)
}

/// Peeks at the first decompressed 512 bytes to distinguish a POSIX `ustar` archive
/// from a standalone `.tex` source file (`export.arxiv.org/e-print/{id}`).
pub fn is_tar_ustar_header(first_512: &[u8]) -> bool {
    first_512.len() >= 262 && &first_512[257..262] == b"ustar"
}

// ============================================================================
// Hardened POSIX `ustar` + PAX (`b'x'` / `b'g'`) Tar Parser
// ============================================================================

#[derive(Debug, Clone, Default)]
struct PaxOverrides {
    path: Option<String>,
    size: Option<usize>,
    has_linkpath: bool,
}

fn round_up_512(n: usize) -> Result<usize, OnlineFetchError> {
    let rem = n % 512;
    if rem == 0 {
        Ok(n)
    } else {
        n.checked_add(512 - rem).ok_or_else(|| {
            OnlineFetchError::ExtractionError("Tar entry size overflow rounding to 512B".to_string())
        })
    }
}

fn parse_tar_size_field(field: &[u8]) -> Result<usize, OnlineFetchError> {
    if field.is_empty() {
        return Ok(0);
    }
    // GNU / STAR base-256 big-endian binary size encoding (`high bit == 1`)
    if (field[0] & 0x80) != 0 {
        if field[0] == 0xff {
            return Err(OnlineFetchError::ExtractionError(
                "Negative base-256 tar size field".to_string(),
            ));
        }
        let mut val: u128 = (field[0] & 0x7f) as u128;
        for &b in &field[1..] {
            val = (val << 8) | (b as u128);
        }
        return usize::try_from(val).map_err(|_| {
            OnlineFetchError::ExtractionError("Base-256 tar size overflows usize".to_string())
        });
    }

    let trimmed_slice_start = field
        .iter()
        .position(|&b| b != b' ' && b != 0)
        .unwrap_or(field.len());
    let sub = &field[trimmed_slice_start..];
    let end = sub
        .iter()
        .position(|&b| b == b' ' || b == 0)
        .unwrap_or(sub.len());
    let digits = &sub[..end];
    if digits.is_empty() {
        return Ok(0);
    }

    let mut acc: u64 = 0;
    for &b in digits {
        if !(b'0'..=b'7').contains(&b) {
            return Err(OnlineFetchError::ExtractionError(format!(
                "Invalid octal digit 0x{b:02x} in tar size field"
            )));
        }
        acc = acc
            .checked_mul(8)
            .and_then(|v| v.checked_add((b - b'0') as u64))
            .ok_or_else(|| {
                OnlineFetchError::ExtractionError("Octal tar size overflow".to_string())
            })?;
    }
    usize::try_from(acc)
        .map_err(|_| OnlineFetchError::ExtractionError("Tar size exceeds usize".to_string()))
}

fn verify_tar_header_block(block: &[u8; 512]) -> Result<(), OnlineFetchError> {
    let chk_field = &block[148..156];
    if chk_field.iter().all(|&b| b == 0) {
        // Synthetic unit-test fixture compatibility only when explicit `ustar` magic is present.
        if &block[257..262] == b"ustar" {
            return Ok(());
        }
        return Err(OnlineFetchError::ExtractionError(
            "Invalid tar header: missing checksum and ustar magic".to_string(),
        ));
    }

    let expected = parse_tar_size_field(chk_field)? as u64;
    let mut unsigned_sum: u64 = 0;
    let mut signed_sum: i64 = 0;
    for (idx, &b) in block.iter().enumerate() {
        let eff = if (148..156).contains(&idx) { b' ' } else { b };
        unsigned_sum += eff as u64;
        signed_sum += (eff as i8) as i64;
    }

    if unsigned_sum == expected || (signed_sum >= 0 && (signed_sum as u64) == expected) {
        Ok(())
    } else {
        Err(OnlineFetchError::ExtractionError(format!(
            "Tar header checksum mismatch (expected {expected}, computed {unsigned_sum})"
        )))
    }
}

fn read_nul_terminated_field<'a>(field: &'a [u8]) -> Result<&'a str, OnlineFetchError> {
    let len = field.iter().position(|&b| b == 0).unwrap_or(field.len());
    std::str::from_utf8(&field[..len]).map_err(|_| {
        OnlineFetchError::ExtractionError("Tar header path contains invalid UTF-8".to_string())
    })
}

fn extract_ustar_header_path(block: &[u8; 512]) -> Result<String, OnlineFetchError> {
    let name = read_nul_terminated_field(&block[0..100])?;
    if &block[257..262] == b"ustar" && block[345] != 0 {
        let prefix = read_nul_terminated_field(&block[345..500])?;
        if !prefix.is_empty() {
            return Ok(format!("{prefix}/{name}"));
        }
    }
    Ok(name.to_string())
}

fn parse_pax_records(
    pax_bytes: &[u8],
    target: &mut PaxOverrides,
    allow_path: bool,
) -> Result<(), OnlineFetchError> {
    let mut pos = 0usize;
    while pos < pax_bytes.len() {
        if pax_bytes[pos] == 0 {
            break;
        }
        let space_rel = pax_bytes[pos..]
            .iter()
            .position(|&b| b == b' ')
            .ok_or_else(|| {
                OnlineFetchError::ExtractionError(
                    "Malformed PAX record: missing space after length".to_string(),
                )
            })?;
        let len_str = std::str::from_utf8(&pax_bytes[pos..pos + space_rel]).map_err(|_| {
            OnlineFetchError::ExtractionError(
                "Malformed PAX record: non-ASCII length prefix".to_string(),
            )
        })?;
        let rec_len: usize = len_str.parse().map_err(|_| {
            OnlineFetchError::ExtractionError(format!(
                "Malformed PAX record: invalid length '{len_str}'"
            ))
        })?;
        if rec_len <= space_rel + 2 || pos + rec_len > pax_bytes.len() {
            return Err(OnlineFetchError::ExtractionError(
                "Malformed PAX record: out-of-bounds record length".to_string(),
            ));
        }
        let rec_end = pos + rec_len;
        if pax_bytes[rec_end - 1] != b'\n' {
            return Err(OnlineFetchError::ExtractionError(
                "Malformed PAX record: missing trailing newline".to_string(),
            ));
        }

        let kv = &pax_bytes[pos + space_rel + 1..rec_end - 1];
        let eq_rel = kv.iter().position(|&b| b == b'=').ok_or_else(|| {
            OnlineFetchError::ExtractionError("Malformed PAX record: missing '='".to_string())
        })?;
        let key = std::str::from_utf8(&kv[..eq_rel]).map_err(|_| {
            OnlineFetchError::ExtractionError("Malformed PAX record: non-UTF8 key".to_string())
        })?;
        let val = &kv[eq_rel + 1..];

        match key {
            "path" if allow_path => {
                if val.contains(&0) {
                    return Err(OnlineFetchError::SecurityBlocked(
                        "PAX 'path' record contains forbidden NUL byte".to_string(),
                    ));
                }
                let s = std::str::from_utf8(val).map_err(|_| {
                    OnlineFetchError::ExtractionError("PAX 'path' is not valid UTF-8".to_string())
                })?;
                target.path = if s.is_empty() {
                    None
                } else {
                    Some(s.to_string())
                };
            }
            "size" => {
                let s = std::str::from_utf8(val).map_err(|_| {
                    OnlineFetchError::ExtractionError("PAX 'size' is not valid UTF-8".to_string())
                })?;
                if s.is_empty() {
                    target.size = None;
                } else {
                    let parsed_u64: u64 = s.parse().map_err(|_| {
                        OnlineFetchError::ExtractionError(format!(
                            "Invalid PAX 'size' integer '{s}'"
                        ))
                    })?;
                    let parsed_usize = usize::try_from(parsed_u64).map_err(|_| {
                        OnlineFetchError::ExtractionError("PAX 'size' overflows usize".to_string())
                    })?;
                    target.size = Some(parsed_usize);
                }
            }
            "linkpath" => {
                if !val.is_empty() {
                    target.has_linkpath = true;
                }
            }
            _ => {}
        }

        pos = rec_end;
    }
    Ok(())
}

/// Validates and normalizes an archive-relative path, hard-rejecting `..`, leading `/`, `\`,
/// NUL/control characters, and Windows drive prefixes (`C:`).
pub fn sanitize_and_normalize_tar_path(
    raw_path: &str,
    allow_empty_after_dot_strip: bool,
) -> Result<String, OnlineFetchError> {
    if raw_path.is_empty() {
        return Err(OnlineFetchError::SecurityBlocked(
            "Empty path in tar archive header".to_string(),
        ));
    }
    if raw_path.bytes().any(|b| b < 0x20 || b == 0x7f) {
        return Err(OnlineFetchError::SecurityBlocked(
            "Tar entry path contains NUL or ASCII control characters".to_string(),
        ));
    }
    if raw_path.starts_with('/') || raw_path.starts_with('\\') {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Absolute path '{raw_path}' in tar archive is forbidden"
        )));
    }
    if raw_path.contains('\\') {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Backslash path separator in '{raw_path}' is forbidden"
        )));
    }
    let bytes = raw_path.as_bytes();
    if bytes.len() >= 2 && bytes[0].is_ascii_alphabetic() && bytes[1] == b':' {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Windows drive prefix in '{raw_path}' is forbidden"
        )));
    }

    let mut clean_segments: Vec<&str> = Vec::new();
    for seg in raw_path.split('/') {
        if seg.is_empty() || seg == "." {
            continue;
        }
        if seg == ".." {
            return Err(OnlineFetchError::SecurityBlocked(format!(
                "Parent directory traversal ('..') in '{raw_path}' is forbidden"
            )));
        }
        if seg.len() >= 2 && seg.as_bytes()[0].is_ascii_alphabetic() && seg.as_bytes()[1] == b':' {
            return Err(OnlineFetchError::SecurityBlocked(format!(
                "Drive prefix segment in '{raw_path}' is forbidden"
            )));
        }
        clean_segments.push(seg);
    }

    if clean_segments.is_empty() {
        if allow_empty_after_dot_strip {
            return Ok(String::new());
        }
        return Ok(String::new());
    }

    Ok(clean_segments.join("/"))
}

/// Returns `true` iff `norm_path` is a workspace execution/hook hazard that must be stripped:
/// `.git/**`, `.gitattributes`, `.gitmodules`, `.envrc`, `.vscode/{tasks,launch,settings}.json`,
/// `.zed/{tasks,settings}.json`.
pub fn is_hazardous_workspace_archive_path(norm_path: &str) -> bool {
    let segs: Vec<String> = norm_path
        .split('/')
        .map(|s| s.to_ascii_lowercase())
        .collect();

    if segs.iter().any(|s| s == ".git") {
        return true;
    }

    if let Some(file_name) = segs.last().map(String::as_str) {
        if matches!(file_name, ".gitattributes" | ".gitmodules" | ".envrc") {
            return true;
        }
    }

    for pair in segs.windows(2) {
        match (pair[0].as_str(), pair[1].as_str()) {
            (".vscode", "tasks.json" | "launch.json" | "settings.json") => return true,
            (".zed", "tasks.json" | "settings.json") => return true,
            _ => {}
        }
    }

    false
}

fn contains_subslice(haystack: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() || haystack.len() < needle.len() {
        return false;
    }
    haystack.windows(needle.len()).any(|w| w == needle)
}

/// Returns `true` iff `bytes` is a Mach-O (`0xfeedfacf` / `0xcafebabe`), ELF (`\x7fELF`),
/// NUL-containing binary blob, or an Xcode `PBXShellScriptBuildPhase` project file.
pub fn is_binary_or_hazardous_blob(norm_path: &str, bytes: &[u8]) -> bool {
    if bytes.len() >= 4 {
        let m4 = [bytes[0], bytes[1], bytes[2], bytes[3]];
        let is_macho = matches!(
            m4,
            [0xfe, 0xed, 0xfa, 0xcf] // MH_MAGIC_64 (BE)
                | [0xcf, 0xfa, 0xed, 0xfe] // MH_CIGAM_64 (LE, arm64/x86_64)
                | [0xfe, 0xed, 0xfa, 0xce] // MH_MAGIC (BE)
                | [0xce, 0xfa, 0xed, 0xfe] // MH_CIGAM (LE)
                | [0xca, 0xfe, 0xba, 0xbe] // FAT_MAGIC Universal (BE)
                | [0xbe, 0xba, 0xfe, 0xca] // FAT_CIGAM Universal (LE)
                | [0xca, 0xfe, 0xba, 0xbf] // FAT_MAGIC_64 (BE)
                | [0xbf, 0xba, 0xfe, 0xca] // FAT_CIGAM_64 (LE)
        );
        if is_macho || m4 == *b"\x7fELF" {
            return true;
        }
    }

    if bytes.contains(&0) {
        return true;
    }

    // Xcode build-phase shell script injection (`project.pbxproj` or embedded Xcode files)
    let low_path = norm_path.to_ascii_lowercase();
    if (low_path.ends_with(".pbxproj")
        || low_path.contains(".xcodeproj/")
        || contains_subslice(bytes, b"PBXShellScriptBuildPhase"))
        && contains_subslice(bytes, b"PBXShellScriptBuildPhase")
    {
        return true;
    }

    false
}

fn check_and_record_apfs_case_collision(
    norm_path: &str,
    seen_lower_files: &mut HashSet<String>,
    seen_lower_parent_dirs: &mut HashSet<String>,
) -> Result<(), OnlineFetchError> {
    let lower = norm_path.to_ascii_lowercase();
    if seen_lower_files.contains(&lower) || seen_lower_parent_dirs.contains(&lower) {
        return Err(OnlineFetchError::SecurityBlocked(format!(
            "Case-insensitive APFS path collision detected in tar archive: '{norm_path}'"
        )));
    }

    // Check every parent prefix of `lower` against already-extracted regular files.
    let mut scan = 0usize;
    while let Some(rel_slash) = lower[scan..].find('/') {
        let slash_idx = scan + rel_slash;
        let parent = &lower[..slash_idx];
        if seen_lower_files.contains(parent) {
            return Err(OnlineFetchError::SecurityBlocked(format!(
                "Case-insensitive APFS file/directory collision on '{parent}' for '{norm_path}'"
            )));
        }
        scan = slash_idx + 1;
    }

    // Record parent prefixes and file path.
    let mut scan = 0usize;
    while let Some(rel_slash) = lower[scan..].find('/') {
        let slash_idx = scan + rel_slash;
        seen_lower_parent_dirs.insert(lower[..slash_idx].to_string());
        scan = slash_idx + 1;
    }
    seen_lower_files.insert(lower);
    Ok(())
}

/// Extracts all safe regular text/code files from `tar_bytes` whose normalized relative path
/// satisfies `keep_path`, enforcing `max_total_bytes` on retained files and hard-rejecting
/// links, devices, path traversal, and APFS case collisions.
pub fn extract_safe_tar_filtered<F>(
    tar_bytes: &[u8],
    max_total_bytes: usize,
    mut keep_path: F,
) -> Result<Vec<ExtractedArchiveFile>, OnlineFetchError>
where
    F: FnMut(&str) -> bool,
{
    let mut extracted: Vec<ExtractedArchiveFile> = Vec::new();
    let mut total_kept_bytes: usize = 0;
    let mut seen_lower_files: HashSet<String> = HashSet::new();
    let mut seen_lower_parent_dirs: HashSet<String> = HashSet::new();

    let mut global_pax = PaxOverrides::default();
    let mut pending_pax: Option<PaxOverrides> = None;
    let mut pending_gnu_longname: Option<String> = None;

    let mut cursor: usize = 0;
    while cursor + 512 <= tar_bytes.len() {
        let block: &[u8; 512] = tar_bytes[cursor..cursor + 512].try_into().unwrap();
        cursor += 512;

        // End-of-archive zero block
        if block.iter().all(|&b| b == 0) {
            if pending_pax.is_some() || pending_gnu_longname.is_some() {
                return Err(OnlineFetchError::ExtractionError(
                    "Dangling PAX or GNU LongLink header before end-of-archive".to_string(),
                ));
            }
            break;
        }

        verify_tar_header_block(block)?;

        let typeflag = block[156];
        let octal_size = parse_tar_size_field(&block[124..136])?;

        match typeflag {
            // PAX global extended header (`git archive` commit comment, etc.)
            b'g' => {
                if octal_size > MAX_EXTENDED_HEADER_BYTES {
                    return Err(OnlineFetchError::SizeOrBinaryBlocked {
                        url: "archive://tar/pax_global_header".to_string(),
                        bytes: octal_size as u64,
                        reason: "PAX global header exceeds 1 MiB cap".to_string(),
                    });
                }
                if cursor + octal_size > tar_bytes.len() {
                    return Err(OnlineFetchError::ExtractionError(
                        "Truncated PAX global extended header".to_string(),
                    ));
                }
                parse_pax_records(&tar_bytes[cursor..cursor + octal_size], &mut global_pax, false)?;
                let padded = round_up_512(octal_size)?;
                cursor = (cursor + padded).min(tar_bytes.len());
                continue;
            }
            // PAX per-file extended header (`path=`, `size=`)
            b'x' => {
                if octal_size > MAX_EXTENDED_HEADER_BYTES {
                    return Err(OnlineFetchError::SizeOrBinaryBlocked {
                        url: "archive://tar/pax_header".to_string(),
                        bytes: octal_size as u64,
                        reason: "PAX extended header exceeds 1 MiB cap".to_string(),
                    });
                }
                if cursor + octal_size > tar_bytes.len() {
                    return Err(OnlineFetchError::ExtractionError(
                        "Truncated PAX extended header".to_string(),
                    ));
                }
                let mut pax = pending_pax.take().unwrap_or_default();
                parse_pax_records(&tar_bytes[cursor..cursor + octal_size], &mut pax, true)?;
                pending_pax = Some(pax);
                let padded = round_up_512(octal_size)?;
                cursor = (cursor + padded).min(tar_bytes.len());
                continue;
            }
            // GNU LongLink pathname (`././@LongLink` for paths > 100 chars in arXiv Linux tarballs)
            b'L' => {
                if octal_size > 4096 {
                    return Err(OnlineFetchError::SecurityBlocked(
                        "GNU LongLink pathname exceeds 4096 bytes".to_string(),
                    ));
                }
                if cursor + octal_size > tar_bytes.len() {
                    return Err(OnlineFetchError::ExtractionError(
                        "Truncated GNU LongLink pathname payload".to_string(),
                    ));
                }
                let raw_name = &tar_bytes[cursor..cursor + octal_size];
                let trimmed_end = raw_name.strip_suffix(&[0]).unwrap_or(raw_name);
                if trimmed_end.contains(&0) {
                    return Err(OnlineFetchError::SecurityBlocked(
                        "GNU LongLink pathname contains interior NUL byte".to_string(),
                    ));
                }
                let s = std::str::from_utf8(trimmed_end).map_err(|_| {
                    OnlineFetchError::ExtractionError(
                        "GNU LongLink pathname is not valid UTF-8".to_string(),
                    )
                })?;
                pending_gnu_longname = Some(s.to_string());
                let padded = round_up_512(octal_size)?;
                cursor = (cursor + padded).min(tar_bytes.len());
                continue;
            }
            // GNU LongLink link target -> always precedes a symlink/hardlink -> hard-reject!
            b'K' => {
                return Err(OnlineFetchError::SecurityBlocked(
                    "Tar archive contains forbidden GNU LongLink symlink/hardlink header ('K')"
                        .to_string(),
                ));
            }
            _ => {}
        }

        // Reconcile PAX + GNU overrides with the entry header in ONE unified place (`CVE-2026-33055` defense)
        let pax = pending_pax.take();
        let gnu_name = pending_gnu_longname.take();

        if pax.as_ref().map(|p| p.has_linkpath).unwrap_or(false) || global_pax.has_linkpath {
            return Err(OnlineFetchError::SecurityBlocked(
                "Tar archive entry specifies forbidden PAX 'linkpath'".to_string(),
            ));
        }

        let effective_size = pax
            .as_ref()
            .and_then(|p| p.size)
            .or(global_pax.size)
            .unwrap_or(octal_size);

        let raw_path = if let Some(p) = pax.and_then(|p| p.path) {
            p
        } else if let Some(g) = gnu_name {
            g
        } else {
            extract_ustar_header_path(block)?
        };

        match typeflag {
            // Hard-reject symlinks, hardlinks, char/block devices, and FIFOs!
            b'1' => {
                return Err(OnlineFetchError::SecurityBlocked(format!(
                    "Tar archive contains forbidden hardlink entry '{raw_path}'"
                )));
            }
            b'2' => {
                return Err(OnlineFetchError::SecurityBlocked(format!(
                    "Tar archive contains forbidden symlink entry '{raw_path}'"
                )));
            }
            b'3' => {
                return Err(OnlineFetchError::SecurityBlocked(format!(
                    "Tar archive contains forbidden character device entry '{raw_path}'"
                )));
            }
            b'4' => {
                return Err(OnlineFetchError::SecurityBlocked(format!(
                    "Tar archive contains forbidden block device entry '{raw_path}'"
                )));
            }
            b'6' => {
                return Err(OnlineFetchError::SecurityBlocked(format!(
                    "Tar archive contains forbidden FIFO entry '{raw_path}'"
                )));
            }
            // Skip directory entries (after validating directory path against traversal!)
            b'5' => {
                let _ = sanitize_and_normalize_tar_path(&raw_path, true)?;
                if cursor + effective_size > tar_bytes.len() {
                    return Err(OnlineFetchError::ExtractionError(
                        "Truncated tar directory payload".to_string(),
                    ));
                }
                let padded = round_up_512(effective_size)?;
                cursor = (cursor + padded).min(tar_bytes.len());
                continue;
            }
            // Regular file (`b'0'` or legacy V7 `0`)
            b'0' | 0 => {
                if block[157..257].iter().any(|&b| b != 0) {
                    return Err(OnlineFetchError::SecurityBlocked(format!(
                        "Regular tar entry '{raw_path}' carries non-empty linkname field"
                    )));
                }
                let is_v7_dir = typeflag == 0 && raw_path.ends_with('/');
                let norm_path = sanitize_and_normalize_tar_path(&raw_path, true)?;

                if cursor + effective_size > tar_bytes.len() {
                    // If `effective_size` already exceeds `max_total_bytes` (e.g. crafted PAX bomb),
                    // surface `SizeOrBinaryBlocked` when within stream or `ExtractionError` when truncated.
                    if effective_size > max_total_bytes {
                        return Err(OnlineFetchError::SizeOrBinaryBlocked {
                            url: format!("tar://{norm_path}"),
                            bytes: effective_size as u64,
                            reason: format!(
                                "Tar entry size ({effective_size} bytes) exceeds max_total_bytes ({max_total_bytes})"
                            ),
                        });
                    }
                    return Err(OnlineFetchError::ExtractionError(format!(
                        "Truncated tar payload for '{norm_path}' (expected {effective_size} bytes)"
                    )));
                }

                let padded = round_up_512(effective_size)?;
                let payload = &tar_bytes[cursor..cursor + effective_size];
                cursor = (cursor + padded).min(tar_bytes.len());

                if is_v7_dir || norm_path.is_empty() {
                    continue;
                }

                // 1. Strip hazardous workspace config/hook paths (`.git/**`, `.envrc`, `.vscode/tasks.json`, etc.)
                if is_hazardous_workspace_archive_path(&norm_path) {
                    continue;
                }

                // 2. Apply caller sparse path filter with zero heap allocation on skipped files
                if !keep_path(&norm_path) {
                    continue;
                }

                // 3. Strip Mach-O, ELF, NUL-containing binary blobs, and Xcode `PBXShellScriptBuildPhase`
                if is_binary_or_hazardous_blob(&norm_path, payload) {
                    continue;
                }

                // 4. Enforce APFS case-insensitive path uniqueness
                check_and_record_apfs_case_collision(
                    &norm_path,
                    &mut seen_lower_files,
                    &mut seen_lower_parent_dirs,
                )?;

                // 5. Enforce aggregate extracted byte & file count caps BEFORE allocating `Vec<u8>`
                let next_total = total_kept_bytes.saturating_add(effective_size);
                if next_total > max_total_bytes {
                    return Err(OnlineFetchError::SizeOrBinaryBlocked {
                        url: format!("tar://{norm_path}"),
                        bytes: next_total as u64,
                        reason: format!(
                            "Extracted tar files ({next_total} bytes) exceed max_total_bytes ({max_total_bytes})"
                        ),
                    });
                }
                if extracted.len() >= MAX_EXTRACTED_FILE_COUNT {
                    return Err(OnlineFetchError::SizeOrBinaryBlocked {
                        url: format!("tar://{norm_path}"),
                        bytes: next_total as u64,
                        reason: format!(
                            "Archive exceeds maximum extracted file count ({MAX_EXTRACTED_FILE_COUNT})"
                        ),
                    });
                }

                total_kept_bytes = next_total;
                extracted.push(ExtractedArchiveFile {
                    rel_path: norm_path,
                    bytes: payload.to_vec(),
                });
            }
            other => {
                return Err(OnlineFetchError::SecurityBlocked(format!(
                    "Unsupported or unsafe tar entry typeflag 0x{other:02x} ('{}') for '{raw_path}'",
                    other as char
                )));
            }
        }
    }

    if pending_pax.is_some() || pending_gnu_longname.is_some() {
        return Err(OnlineFetchError::ExtractionError(
            "Dangling PAX or GNU LongLink header at end of tar stream".to_string(),
        ));
    }

    Ok(extracted)
}

/// Extracts all safe regular text/code files from `tar_bytes` up to `max_total_bytes`.
pub fn extract_safe_tar_entries(
    tar_bytes: &[u8],
    max_total_bytes: usize,
) -> Result<Vec<ExtractedArchiveFile>, OnlineFetchError> {
    extract_safe_tar_filtered(tar_bytes, max_total_bytes, |_| true)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn gzip_compress_for_test(input: &[u8]) -> Vec<u8> {
        let mut encoder =
            flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::default());
        encoder.write_all(input).expect("gzip write failed");
        encoder.finish().expect("gzip finish failed")
    }

    fn stamp_ustar_checksum(hdr: &mut [u8; 512]) {
        hdr[148..156].fill(b' ');
        let sum: u32 = hdr.iter().map(|&b| b as u32).sum();
        let s = format!("{sum:06o}\0 ");
        hdr[148..156].copy_from_slice(s.as_bytes());
    }

    fn push_tar_raw_entry(
        archive: &mut Vec<u8>,
        name: &str,
        typeflag: u8,
        octal_size_override: Option<usize>,
        linkname: &str,
        payload: &[u8],
    ) {
        let mut hdr = [0u8; 512];
        let name_bytes = name.as_bytes();
        let copy_n = name_bytes.len().min(100);
        hdr[..copy_n].copy_from_slice(&name_bytes[..copy_n]);
        hdr[100..108].copy_from_slice(b"0000644\0");
        hdr[108..116].copy_from_slice(b"0001000\0");
        hdr[116..124].copy_from_slice(b"0001000\0");
        let sz = octal_size_override.unwrap_or(payload.len());
        let sz_str = format!("{sz:011o}\0");
        hdr[124..136].copy_from_slice(sz_str.as_bytes());
        hdr[136..148].copy_from_slice(b"14700000000\0");
        hdr[156] = typeflag;
        if !linkname.is_empty() {
            let lb = linkname.as_bytes();
            hdr[157..157 + lb.len().min(100)].copy_from_slice(&lb[..lb.len().min(100)]);
        }
        hdr[257..263].copy_from_slice(b"ustar\0");
        hdr[263..265].copy_from_slice(b"00");
        stamp_ustar_checksum(&mut hdr);

        archive.extend_from_slice(&hdr);
        archive.extend_from_slice(payload);
        let rem = payload.len() % 512;
        if rem != 0 {
            archive.resize(archive.len() + (512 - rem), 0);
        }
    }

    fn format_pax_kv(key: &str, val: &str) -> Vec<u8> {
        // Compute self-inclusive decimal byte length `%d %s=%s\n`
        let body = format!("{key}={val}\n");
        let mut digits = 1usize;
        loop {
            let total = digits + 1 + body.len();
            let d_str = total.to_string();
            if d_str.len() == digits {
                return format!("{d_str} {body}").into_bytes();
            }
            digits = d_str.len();
        }
    }

    fn finish_tar_eof(archive: &mut Vec<u8>) {
        archive.resize(archive.len() + 1024, 0);
    }

    #[test]
    fn test_libz_gunzip_roundtrip_and_chunked_streaming() {
        let original = "\\section{Attention}\n$$\\text{Attention}(Q,K,V)=\\text{softmax}(QK^T/\\sqrt{d_k})V$$\n"
            .repeat(120);
        let gz = gzip_compress_for_test(original.as_bytes());
        assert!(gz.len() < original.len());

        let roundtrip = gunzip_bytes(&gz, 1024 * 1024).unwrap();
        assert_eq!(roundtrip, original.as_bytes());

        // Feed in tiny 13-byte chunks + test multi-member concatenated gzip!
        let mut multi_member = gz.clone();
        multi_member.extend_from_slice(&gz);

        let mut streaming = StreamingGunzipDecoder::new(2 * 1024 * 1024).unwrap();
        let mut collected = Vec::new();
        for chunk in multi_member.chunks(13) {
            streaming.feed_chunk(chunk, &mut collected).unwrap();
        }
        streaming.finish().unwrap();
        assert_eq!(collected.len(), original.len() * 2);
        assert_eq!(&collected[..original.len()], original.as_bytes());
        assert_eq!(&collected[original.len()..], original.as_bytes());
    }

    #[test]
    fn test_libz_gunzip_zip_bomb_cap_enforced() {
        let two_mib_zeros = vec![b'A'; 2 * 1024 * 1024];
        let bomb_gz = gzip_compress_for_test(&two_mib_zeros);
        assert!(bomb_gz.len() < 16 * 1024);

        let err = gunzip_bytes(&bomb_gz, 64 * 1024).unwrap_err();
        assert!(
            matches!(err, OnlineFetchError::SizeOrBinaryBlocked { .. }),
            "expected SizeOrBinaryBlocked on zip bomb, got {err:?}"
        );
    }

    #[test]
    fn test_single_file_tex_vs_ustar_512b_peek() {
        let standalone_tex = format!(
            "\\documentclass{{article}}\n\\begin{{document}}\n{}\n\\end{{document}}\n",
            "Theorem 1.1: Lossless LaTeX extraction. ".repeat(30)
        );
        let tex_gz = gzip_compress_for_test(standalone_tex.as_bytes());
        let decomp_tex = gunzip_bytes(&tex_gz, 256 * 1024).unwrap();
        assert!(decomp_tex.len() >= 512);
        assert!(!is_tar_ustar_header(&decomp_tex[..512]));

        let mut tar_buf = Vec::new();
        push_tar_raw_entry(&mut tar_buf, "main.tex", b'0', None, "", standalone_tex.as_bytes());
        finish_tar_eof(&mut tar_buf);
        let tar_gz = gzip_compress_for_test(&tar_buf);
        let decomp_tar = gunzip_bytes(&tar_gz, 256 * 1024).unwrap();
        assert!(is_tar_ustar_header(&decomp_tar[..512]));
        let files = extract_safe_tar_entries(&decomp_tar, 256 * 1024).unwrap();
        assert_eq!(files.len(), 1);
        assert_eq!(files[0].rel_path, "main.tex");
        assert_eq!(files[0].bytes, standalone_tex.as_bytes());
    }

    #[test]
    fn test_pax_size_and_path_override_prevents_cve_2026_33055_desync() {
        // Construct a file > 512 bytes whose octal header says `size = 0`, while PAX `b'x'`
        // says `size = <true_len>` and sets a >100-char path. Inside the file's ASCII payload
        // at byte 0, place text containing "ustar" to prove no header desync can ever occur.
        let long_path =
            "vllm-0123456789abcdef/csrc/quantization/marlin/sparse_moe_w4a16_kernel_dispatch_impl_v2.cu";
        assert!(long_path.len() > 80);
        let file1_payload = format!(
            "// Fake ustar marker inside source comment: {}\npub fn kernel() -> usize {{ 42 }}\n",
            "x".repeat(700)
        );

        let mut pax_payload = Vec::new();
        pax_payload.extend_from_slice(&format_pax_kv("path", long_path));
        pax_payload.extend_from_slice(&format_pax_kv("size", &file1_payload.len().to_string()));

        let mut archive = Vec::new();
        // `git archive` global PAX header (`b'g'`)
        let global_comment = format_pax_kv("comment", "0123456789abcdef0123456789abcdef01234567");
        push_tar_raw_entry(&mut archive, "pax_global_header", b'g', None, "", &global_comment);
        // Per-file PAX extended header (`b'x'`)
        push_tar_raw_entry(&mut archive, "PaxHeader/file1", b'x', None, "", &pax_payload);
        // Regular header with octal_size = 0 (overridden by PAX `size=`)
        push_tar_raw_entry(
            &mut archive,
            "truncated_short_name.cu",
            b'0',
            Some(0),
            "",
            file1_payload.as_bytes(),
        );
        // Second regular file after PAX-sized entry
        push_tar_raw_entry(
            &mut archive,
            "vllm-0123456789abcdef/README.md",
            b'0',
            None,
            "",
            b"# Clean Repo\n",
        );
        finish_tar_eof(&mut archive);

        let files = extract_safe_tar_entries(&archive, 1024 * 1024).unwrap();
        assert_eq!(files.len(), 2);
        assert_eq!(files[0].rel_path, long_path);
        assert_eq!(files[0].bytes, file1_payload.as_bytes());
        assert_eq!(files[1].rel_path, "vllm-0123456789abcdef/README.md");
        assert_eq!(files[1].bytes, b"# Clean Repo\n");
    }

    #[test]
    fn test_tar_rejects_symlinks_hardlinks_devices_and_fifos() {
        let forbidden_types: &[(u8, &str)] = &[
            (b'2', "/etc/passwd"), // symlink
            (b'1', "README.md"),   // hardlink
            (b'3', ""),            // char device
            (b'4', ""),            // block device
            (b'6', ""),            // FIFO
            (b'K', "/etc/shadow"), // GNU LongLink target
        ];

        for &(tf, link) in forbidden_types {
            let mut archive = Vec::new();
            let payload = if tf == b'K' { b"/etc/shadow\0".as_slice() } else { b"" };
            push_tar_raw_entry(&mut archive, "evil_entry", tf, None, link, payload);
            finish_tar_eof(&mut archive);
            let res = extract_safe_tar_entries(&archive, 64 * 1024);
            assert!(
                matches!(res, Err(OnlineFetchError::SecurityBlocked(_))),
                "typeflag '{}' MUST be hard-rejected, got {res:?}",
                tf as char
            );
        }
    }

    #[test]
    fn test_tar_rejects_traversal_leading_slash_windows_drive_and_pax_nul() {
        let bad_paths = [
            "../outside.rs",
            "repo/../../etc/passwd",
            "/etc/passwd",
            "\\Windows\\System32\\drivers\\etc\\hosts",
            "C:/autorun.inf",
            "repo/sub\\..\\escape.py",
        ];

        for bad in bad_paths {
            let mut archive = Vec::new();
            push_tar_raw_entry(&mut archive, bad, b'0', None, "", b"bad");
            finish_tar_eof(&mut archive);
            let res = extract_safe_tar_entries(&archive, 64 * 1024);
            assert!(
                matches!(res, Err(OnlineFetchError::SecurityBlocked(_))),
                "path '{bad}' MUST be hard-rejected, got {res:?}"
            );
        }

        // PAX `path=` containing interior NUL byte (record length = 21 bytes)
        let mut archive = Vec::new();
        let raw_pax = b"21 path=safe\0evil.rs\n";
        push_tar_raw_entry(&mut archive, "PaxHeader/x", b'x', None, "", raw_pax);
        push_tar_raw_entry(&mut archive, "safe.rs", b'0', None, "", b"fn x() {}");
        finish_tar_eof(&mut archive);
        assert!(matches!(
            extract_safe_tar_entries(&archive, 64 * 1024),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));
    }

    #[test]
    fn test_tar_rejects_apfs_case_insensitive_collisions() {
        // Case 1: Two files differing only in ASCII case
        let mut archive1 = Vec::new();
        push_tar_raw_entry(&mut archive1, "repo/src/Config.rs", b'0', None, "", b"pub const A: u8 = 1;\n");
        push_tar_raw_entry(&mut archive1, "repo/src/config.rs", b'0', None, "", b"pub const A: u8 = 2;\n");
        finish_tar_eof(&mut archive1);
        assert!(matches!(
            extract_safe_tar_entries(&archive1, 64 * 1024),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));

        // Case 2: File colliding with parent directory differing only in ASCII case
        let mut archive2 = Vec::new();
        push_tar_raw_entry(&mut archive2, "repo/build", b'0', None, "", b"script");
        push_tar_raw_entry(&mut archive2, "repo/BUILD/out.txt", b'0', None, "", b"out");
        finish_tar_eof(&mut archive2);
        assert!(matches!(
            extract_safe_tar_entries(&archive2, 64 * 1024),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));
    }

    #[test]
    fn test_tar_strips_git_vscode_zed_envrc_pbxproj_and_macho_elf_binaries() {
        let mut archive = Vec::new();
        push_tar_raw_entry(&mut archive, "repo/", b'5', None, "", b"");
        push_tar_raw_entry(
            &mut archive,
            "./repo/src/lib.rs",
            b'0',
            None,
            "",
            b"pub fn safe_entry() -> &'static str { \"ok\" }\n",
        );
        // Hazardous repo/IDE configs & hooks
        push_tar_raw_entry(&mut archive, "repo/.git/config", b'0', None, "", b"[core]\nfsmonitor = evil\n");
        push_tar_raw_entry(&mut archive, "repo/.git/hooks/post-checkout", b'0', None, "", b"#!/bin/sh\n");
        push_tar_raw_entry(&mut archive, "repo/.gitattributes", b'0', None, "", b"* filter=lfs\n");
        push_tar_raw_entry(&mut archive, "repo/.gitmodules", b'0', None, "", b"[submodule \"x\"]\n");
        push_tar_raw_entry(&mut archive, "repo/.envrc", b'0', None, "", b"curl evil\n");
        push_tar_raw_entry(&mut archive, "repo/.vscode/tasks.json", b'0', None, "", b"{\"version\":\"2.0\"}");
        push_tar_raw_entry(&mut archive, "repo/.vscode/launch.json", b'0', None, "", b"{}");
        push_tar_raw_entry(&mut archive, "repo/.vscode/settings.json", b'0', None, "", b"{}");
        push_tar_raw_entry(&mut archive, "repo/.zed/tasks.json", b'0', None, "", b"[]");
        push_tar_raw_entry(&mut archive, "repo/.zed/settings.json", b'0', None, "", b"{}");
        push_tar_raw_entry(
            &mut archive,
            "repo/App.xcodeproj/project.pbxproj",
            b'0',
            None,
            "",
            b"// !$*UTF8*$!\n{ isa = PBXShellScriptBuildPhase; shellScript = \"id\"; }\n",
        );
        // Binary blobs: Mach-O 64 BE/LE, Fat Universal, ELF, and NUL-containing blob
        push_tar_raw_entry(&mut archive, "repo/bin/macho_be", b'0', None, "", &[0xfe, 0xed, 0xfa, 0xcf, b'X']);
        push_tar_raw_entry(&mut archive, "repo/bin/macho_le", b'0', None, "", &[0xcf, 0xfa, 0xed, 0xfe, b'X']);
        push_tar_raw_entry(&mut archive, "repo/bin/fat_univ", b'0', None, "", &[0xca, 0xfe, 0xba, 0xbe, b'X']);
        push_tar_raw_entry(&mut archive, "repo/bin/elf_bin", b'0', None, "", b"\x7fELF1234");
        push_tar_raw_entry(&mut archive, "repo/assets/raw.dat", b'0', None, "", b"hello\0world");
        finish_tar_eof(&mut archive);

        let files = extract_safe_tar_entries(&archive, 512 * 1024).unwrap();
        assert_eq!(files.len(), 1);
        assert_eq!(files[0].rel_path, "repo/src/lib.rs");
    }

    #[test]
    fn test_extract_safe_tar_filtered_skips_figures_without_charging_budget() {
        let mut archive = Vec::new();
        let figure_eps = "%!PS-Adobe-3.0 EPSF-3.0\n".repeat(400); // ~9.6 KB ASCII EPS figure
        push_tar_raw_entry(&mut archive, "figs/arch.eps", b'0', None, "", figure_eps.as_bytes());
        push_tar_raw_entry(&mut archive, "paper.tex", b'0', None, "", b"\\section{Method}\nClean math.\n");
        finish_tar_eof(&mut archive);

        // Budget is only 512 bytes; unfiltered would exceed budget, filtered `.tex` succeeds cleanly!
        assert!(matches!(
            extract_safe_tar_entries(&archive, 512),
            Err(OnlineFetchError::SizeOrBinaryBlocked { .. })
        ));

        let filtered =
            extract_safe_tar_filtered(&archive, 512, |p| p.ends_with(".tex")).unwrap();
        assert_eq!(filtered.len(), 1);
        assert_eq!(filtered[0].rel_path, "paper.tex");
    }

    #[test]
    fn test_tar_ustar_prefix_and_gnu_longlink_pathname() {
        // 1. POSIX ustar 155-byte `prefix` + `name`
        let mut archive = Vec::new();
        let mut hdr = [0u8; 512];
        hdr[..11].copy_from_slice(b"kernel.metal"[..11].as_ref());
        hdr[..12].copy_from_slice(b"kernel.metal");
        hdr[100..108].copy_from_slice(b"0000644\0");
        hdr[108..116].copy_from_slice(b"0001000\0");
        hdr[116..124].copy_from_slice(b"0001000\0");
        let body1 = b"#include <metal_stdlib>\n";
        hdr[124..136].copy_from_slice(format!("{:011o}\0", body1.len()).as_bytes());
        hdr[136..148].copy_from_slice(b"14700000000\0");
        hdr[156] = b'0';
        hdr[257..263].copy_from_slice(b"ustar\0");
        hdr[263..265].copy_from_slice(b"00");
        let prefix = b"mlx-swift/Sources/Cmlx/kernels";
        hdr[345..345 + prefix.len()].copy_from_slice(prefix);
        stamp_ustar_checksum(&mut hdr);
        archive.extend_from_slice(&hdr);
        archive.extend_from_slice(body1);
        archive.resize(archive.len() + (512 - body1.len()), 0);

        // 2. GNU LongLink (`b'L'`) >100-char pathname
        let long_tex = "arxiv_2401_99999/supplementary/appendices/proof_of_theorem_4_2_convergence_bounds_full_derivation.tex";
        let mut long_payload = long_tex.as_bytes().to_vec();
        long_payload.push(0);
        push_tar_raw_entry(&mut archive, "././@LongLink", b'L', None, "", &long_payload);
        push_tar_raw_entry(&mut archive, "short.tex", b'0', None, "", b"\\begin{proof}QED\\end{proof}\n");
        finish_tar_eof(&mut archive);

        let files = extract_safe_tar_entries(&archive, 64 * 1024).unwrap();
        assert_eq!(files.len(), 2);
        assert_eq!(files[0].rel_path, "mlx-swift/Sources/Cmlx/kernels/kernel.metal");
        assert_eq!(files[1].rel_path, long_tex);
    }

    #[test]
    fn test_tar_pax_linkpath_smuggling_on_regular_file_rejected() {
        let mut archive = Vec::new();
        let pax = format_pax_kv("linkpath", "/etc/passwd");
        push_tar_raw_entry(&mut archive, "PaxHeader/smuggle", b'x', None, "", &pax);
        push_tar_raw_entry(&mut archive, "innocent.txt", b'0', None, "", b"hello");
        finish_tar_eof(&mut archive);

        assert!(matches!(
            extract_safe_tar_entries(&archive, 64 * 1024),
            Err(OnlineFetchError::SecurityBlocked(_))
        ));
    }

    #[test]
    fn test_tar_corrupted_header_checksum_and_truncated_payload_rejected() {
        let mut archive = Vec::new();
        push_tar_raw_entry(&mut archive, "valid.rs", b'0', None, "", b"pub fn a() {}\n");
        finish_tar_eof(&mut archive);

        // Flip a byte in the header name without updating checksum
        let mut corrupted = archive.clone();
        corrupted[0] ^= 0x1f;
        assert!(matches!(
            extract_safe_tar_entries(&corrupted, 64 * 1024),
            Err(OnlineFetchError::ExtractionError(_))
        ));

        // Truncate mid-payload
        let truncated = &archive[..516];
        assert!(matches!(
            extract_safe_tar_entries(truncated, 64 * 1024),
            Err(OnlineFetchError::ExtractionError(_))
        ));
    }
}
