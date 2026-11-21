#ifndef SPELL_CORE_H
#define SPELL_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque session handle for `SpellCoreSession` (`rust/spell_core/src/agent_loop.rs`).
 */
typedef struct SpellCoreSession SpellCoreSession;

/**
 * Creates a new `SpellCoreSession` backed by SQLite3 at `db_path` (or the default
 * `EnginePathLocator` data directory if `db_path` is NULL or empty; pass `":memory:"` for
 * isolated in-memory testing).
 *
 * Caller must eventually free the returned pointer with `spell_session_free`.
 */
SpellCoreSession* spell_session_new(const char* db_path);

/**
 * Frees a `SpellCoreSession` handle previously returned by `spell_session_new`.
 */
void spell_session_free(SpellCoreSession* ptr);

/**
 * Dispatches a UTF-8 JSON command to `SpellCoreSession` and returns a newly allocated
 * UTF-8 JSON string containing the updated workspace snapshot (or error object).
 *
 * Caller must free the returned string with `spell_free_string`.
 */
char* spell_session_dispatch_json(SpellCoreSession* ptr, const char* cmd_json);

/**
 * Frees a heap-allocated C string previously returned by `spell_core`.
 */
void spell_free_string(char* ptr);

/**
 * Returns the current atomic runtime patch generation counter (`SPELL_PATCH_GEN`).
 */
uint64_t spell_get_patch_gen(void);

/**
 * Atomically increments and returns the new runtime patch generation counter.
 */
uint64_t spell_bump_patch_gen(void);

/**
 * Returns a heap-allocated UTF-8 JSON string containing the latest cached workspace snapshot
 * without acquiring any lock on `SpellCoreSession`.
 *
 * Caller must free the returned string with `spell_free_string`.
 */
char* spell_get_cached_snapshot_json(void);

/**
 * Signals lock-free cancellation of any active streaming turn (`< 50 ms`) and bumps `SPELL_PATCH_GEN`.
 */
bool spell_request_turn_cancel(void);

/**
 * Reads a file with zero line truncation (`max_line_len = usize::MAX`), falling back to a
 * formatted directory listing when `path_ptr` points to a directory.
 *
 * Caller must free the returned string with `spell_free_string`.
 */
char* spell_read_file_unrestricted(
    const char* path_ptr,
    size_t start_line,
    size_t end_line
);

#ifdef __cplusplus
}
#endif

#endif /* SPELL_CORE_H */
