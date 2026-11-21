#pragma once

#include "EditorDocument.h"

#include <QString>
#include <QStringList>
#include <QWidget>
#include <Qsci/qsciscintilla.h>

#include <memory>

namespace spell {

/**
 * Center code editor pane for Spell IDE built on QScintilla2 (`QsciScintilla`).
 *
 * Houses:
 * - Minimalist flat top `FileTabBar` (34px height, frameless horizontal scroll strip)
 *   managing open `EditorDocument`s with active elevated surface + 1px bottom underline
 *   bar, 1px right vertical hairline dividers, and a 12x12 right slot displaying a 6px
 *   dirty dot when unsaved & unhovered or a crisp compact close button on hover/active.
 * - Inline collapsible Find & Replace bar (`Cmd/Ctrl+F`) with next/previous match,
 *   case/whole-word/regex toggles, and replace / replace-all actions.
 * - `QsciScintilla` editor configured with line-number margin, code-folding margin,
 *   brace matching, auto-indent, UTF-8 encoding, caret line highlight, and a dynamic
 *   lexer factory (built-in QScintilla lexers + `SpellCustomLexer` for Rust, Swift,
 *   Go, TOML, and TypeScript) styled across all 8 `CodePalette` themes via `applyTheme()`.
 * - Minimalist monochrome empty-state view ("No file open" + "Open File…" chip button)
 *   when no documents are open, plus an inline banner for unavailable/binary files.
 * - `QFileSystemWatcher` monitoring open files to automatically reload clean documents
 *   when modified externally (e.g. by the AI agent).
 */
class CodeEditorPane : public QWidget {
    Q_OBJECT

public:
    explicit CodeEditorPane(QWidget* parent = nullptr);
    ~CodeEditorPane() override;

    CodeEditorPane(const CodeEditorPane&) = delete;
    CodeEditorPane& operator=(const CodeEditorPane&) = delete;
    CodeEditorPane(CodeEditorPane&&) = delete;
    CodeEditorPane& operator=(CodeEditorPane&&) = delete;

    /** Returns the active document's file path, or an empty string if no document is open. */
    QString currentFilePath() const;

    /** Returns the number of currently open documents/tabs. */
    int openDocumentCount() const;

    /** Returns the underlying `QsciScintilla` editor widget. */
    QsciScintilla* scintilla() const;

    /** Returns the ordered list of currently open document file paths (for session persistence). */
    QStringList openFilePaths() const;

    /** Returns the currently active `EditorDocument`, or `nullptr` if none is open. */
    EditorDocument* currentDocument() const;

    /** Returns `true` if any open document has unsaved modifications. */
    bool hasUnsavedChanges() const;

public slots:
    /**
     * Opens `filePath` (or switches to its existing tab), optionally jumping to a
     * 1-based line number (`jumpToLine > 0`) and applying the read-only flag.
     */
    void openFile(const QString& filePath, int jumpToLine = -1, bool readOnly = false);

    /** Closes the open tab for `filePath` if present. */
    void closeFile(const QString& filePath);

    /** Saves the currently active document to disk if writable and dirty. */
    void saveCurrentFile();

    /** Saves all dirty writable open documents to disk. */
    void saveAllFiles();

    /** Reveals and focuses the inline Find & Replace bar (`Cmd/Ctrl+F`). */
    void showFindBar();

    /** Checks all open documents for external disk modifications and reloads clean ones. */
    void reloadOpenDocumentsIfChanged();

    /** Re-applies the current `ThemeManager` UI and `CodePalette` theme to chrome and lexer. */
    void applyTheme();

signals:
    void currentFileChanged(const QString& filePath);
    void statusMessage(const QString& msg);
    void openDocumentsChanged();
    void openFileDialogRequested();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
