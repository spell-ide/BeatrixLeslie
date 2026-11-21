#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace spell {

/** Custom URL scheme emitted by `linkifyHtml` and decoded by `parseSpellFileUrl`. */
inline constexpr const char* kSpellFileUrlScheme = "spellfile";

/**
 * A detected file/line citation span inside assistant prose or tool output.
 *
 * - `start` and `length` are UTF-16 code unit offsets into the input `QString`.
 * - `rawText` is the exact matched substring.
 * - `filePath` is the resolved absolute path (when matched against `projectRoots`
 *   or `.spell/research_cache/`) or normalized relative path.
 * - `startLine` and `endLine` are 1-based line numbers (`0` when no line number was cited).
 */
struct FileCitation {
    int start = 0;
    int length = 0;
    QString rawText;
    QString filePath;
    int startLine = 0;
    int endLine = 0;

    bool operator==(const FileCitation&) const = default;
};

/**
 * Scans `text` for file citations such as `src/foo.cpp:42`, `src/foo.cpp:L12-L30`,
 * `src/foo.cpp#L12`, `src/foo.cpp (L12-L30)`, and `.spell/research_cache/...`,
 * resolving relative paths against `projectRoots` when present.
 */
QVector<FileCitation> findCitations(const QString& text,
                                    const QStringList& projectRoots = {});

/**
 * Rewrites HTML-escaped prose (`escapedHtml`) so recognized file citations become
 * clickable links of the form `<a href="spellfile:///abs/path#L12">...</a>`
 * (or without `#L...` when no line is specified), matching both workspace paths
 * resolved via `projectRoots` and `.spell/research_cache/`.
 *
 * Existing HTML tags, anchor elements (`<a ...>...</a>`), and `<pre>...</pre>`
 * blocks are preserved untouched.
 */
QString linkifyHtml(const QString& escapedHtml,
                    const QStringList& projectRoots = {});

/**
 * Parses a `spellfile://` URL (e.g. `spellfile:///abs/path/file.cpp#L42` or
 * `spellfile:///abs/path/file.cpp#L42-L58`) into its filesystem path and optional
 * 1-based line number (`0` if no line fragment was present).
 *
 * Returns `true` if `url` has the `spellfile` scheme and a non-empty path.
 */
bool parseSpellFileUrl(const QUrl& url, QString* outPath, int* outLine);

} // namespace spell
