#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace spell {

/**
 * In-memory model of an open source file in Spell IDE's code editor.
 *
 * Tracks canonical identity (`id()`), disk synchronization state (`isDirty()`,
 * `loadFromDisk()`, `reloadIfChangedOnDisk()`, `saveToDisk()`), read-only and
 * availability status (e.g. binary or oversized files), detected syntax language,
 * and last known editor cursor position.
 */
class EditorDocument : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(QString filePath READ filePath CONSTANT)
    Q_PROPERTY(QString fileName READ fileName CONSTANT)
    Q_PROPERTY(QString language READ language CONSTANT)
    Q_PROPERTY(QString text READ text)
    Q_PROPERTY(bool dirty READ isDirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool readOnly READ isReadOnly WRITE setReadOnly NOTIFY readOnlyChanged)
    Q_PROPERTY(QString unavailableReason READ unavailableReason)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged)
    Q_PROPERTY(int cursorLine READ cursorLine)
    Q_PROPERTY(int cursorCol READ cursorCol)

public:
    explicit EditorDocument(const QString& filePath,
                            bool readOnly = false,
                            QObject* parent = nullptr);

    /** Convenience overload preventing accidental `QObject*` to `bool` conversion. */
    EditorDocument(const QString& filePath, QObject* parent)
        : EditorDocument(filePath, false, parent) {}

    ~EditorDocument() override;

    EditorDocument(const EditorDocument&) = delete;
    EditorDocument& operator=(const EditorDocument&) = delete;
    EditorDocument(EditorDocument&&) = delete;
    EditorDocument& operator=(EditorDocument&&) = delete;

    // -------------------------------------------------------------------------
    // Document properties
    // -------------------------------------------------------------------------

    /** Canonical path identifying this document (falls back to cleaned absolute path). */
    QString id() const;

    /** File path on disk. */
    QString filePath() const;

    /** Base file name (e.g. `main.rs`, `CMakeLists.txt`). */
    QString fileName() const;

    /** Normalized syntax language key returned by `detectLanguage()`. */
    QString language() const;

    /** Current in-memory UTF-8 text contents. */
    QString text() const;

    /** True when in-memory buffer has unsaved edits relative to disk. */
    bool isDirty() const;

    /** True when editing is disallowed (project lock, OS permissions, or unavailable file). */
    bool isReadOnly() const;

    /** Non-empty explanation when the file cannot be loaded into the text editor (e.g. binary/too large). */
    QString unavailableReason() const;

    /** True when `unavailableReason()` is empty and the document can be shown in the editor. */
    bool isAvailable() const { return unavailableReason().isEmpty(); }

    /** Most recent load/save error message, or empty string on success. */
    QString lastError() const;

    /** Last recorded cursor line in the editor (0-based). */
    int cursorLine() const;

    /** Last recorded cursor column in the editor (0-based). */
    int cursorCol() const;

    // -------------------------------------------------------------------------
    // Disk I/O & editor synchronization
    // -------------------------------------------------------------------------

    /** Loads the file contents from disk, updating `text()`, `unavailableReason()`, and `lastError()`. */
    bool loadFromDisk();

    /**
     * Checks whether the underlying file changed on disk (mtime/size/content) and reloads it
     * when the document is clean (`!isDirty()`), emitting `reloadedFromDisk(newText)`.
     * Returns `true` if a reload occurred.
     */
    bool reloadIfChangedOnDisk();

    /**
     * Writes `text()` atomically/safely back to disk unless `isReadOnly()` or unavailable.
     * Clears dirty state on success; updates `lastError()` and emits `errorChanged()` on failure.
     */
    bool saveToDisk();

    /** Updates the in-memory text buffer from QScintilla edits and updates dirty state. */
    void setTextFromEditor(const QString& text);

    /** Sets whether this document is read-only, emitting `readOnlyChanged()` if changed. */
    void setReadOnly(bool ro);

    /** Persists the current editor cursor coordinates for tab-switch restoration. */
    void setCursorPosition(int line, int col);

    /**
     * Infers a normalized language key from `filePath`'s extension or filename.
     *
     * Recognized keys:
     * `rust`, `cpp`, `c`, `h`, `swift`, `python`, `js`, `ts`, `json`, `yaml`,
     * `toml`, `markdown`, `bash`, `html`, `css`, `sql`, `cmake`, `diff`, `xml`,
     * `go`, `java`, `ruby`, `plain`.
     */
    static QString detectLanguage(const QString& filePath);

signals:
    void dirtyChanged(bool dirty);
    void reloadedFromDisk(const QString& newText);
    void errorChanged(const QString& err);
    void readOnlyChanged(bool readOnly);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
