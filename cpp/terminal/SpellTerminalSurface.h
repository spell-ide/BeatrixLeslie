#pragma once

#include "SpellPtyProcess.h"
#include "SpellTerminalEmulator.h"
#include "../theme/SpellTheme.h"

#include <QByteArray>
#include <QColor>
#include <QSize>
#include <QString>
#include <QVariant>
#include <QWidget>

#include <memory>

class QContextMenuEvent;
class QEvent;
class QFocusEvent;
class QHideEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;
class QWheelEvent;

namespace spell {

/**
 * Interactive monospace terminal canvas (`QWidget`) for Spell IDE.
 *
 * Owns and connects a `SpellPtyProcess` and a `SpellTerminalEmulator`, painting
 * the glyph grid via `QPainter` using `ThemeManager::instance().terminal()` colors
 * and `ThemeManager::instance().terminalFont()`.
 *
 * Features:
 * - ~40 ms debounced resize timer so pane drags never spam `SIGWINCH`.
 * - Integrated vertical scrollback scrollbar and wheel scrolling (translates
 *   wheel ticks into cursor keys on the alternate screen).
 * - Single-click, double-click (word), triple-click (line), and drag text selection.
 * - Clipboard Copy/Paste (`Ctrl/Cmd+C` when selection is non-empty, `Ctrl/Cmd+V`
 *   with bracketed-paste wrapping and injection stripping).
 * - Focused blinking block cursor and unfocused hollow cursor outline.
 * - Full keyboard-to-VT escape sequence encoder + IME composition support.
 * - `#spell <instruction>` line interception and context-menu hand-off.
 * - Safe to reparent seamlessly between a docked panel and a popped-out window
 *   without restarting the running PTY session.
 */
class SpellTerminalSurface : public QWidget {
    Q_OBJECT

public:
    explicit SpellTerminalSurface(QWidget* parent = nullptr);
    ~SpellTerminalSurface() override;

    SpellTerminalSurface(const SpellTerminalSurface&) = delete;
    SpellTerminalSurface& operator=(const SpellTerminalSurface&) = delete;
    SpellTerminalSurface(SpellTerminalSurface&&) = delete;
    SpellTerminalSurface& operator=(SpellTerminalSurface&&) = delete;

    // -------------------------------------------------------------------------
    // Shell lifecycle & I/O
    // -------------------------------------------------------------------------

    /** Starts a shell in `cwd` (uses default home directory if empty) if not already running. */
    void startShell(const QString& cwd = QString());

    /** Ensures a shell is running, starting one lazily in `cwd` if stopped. */
    void ensureShellRunning(const QString& cwd = QString());

    /** Stops any running shell, resets the screen buffer, and starts a fresh shell in `cwd`. */
    void restartShell(const QString& cwd = QString());

    /** Stops the running shell process cleanly while leaving the last screen output visible. */
    void stopShell();

    /**
     * Writes `text` into the running PTY, appending `\r` when `pressEnter` is `true`,
     * and scrolls the viewport back to the live bottom.
     */
    void sendCommandText(const QString& text, bool pressEnter = true);

    /** Writes raw bytes directly to the PTY (no newline appended). */
    void sendRawBytes(const QByteArray& bytes);

    /** Sends an interrupt (`Ctrl-C` / `0x03`) to the running PTY. */
    void sendInterrupt();

    /** Pastes `text` into the PTY, applying bracketed-paste framing when enabled by the app. */
    void pasteText(const QString& text);

    // -------------------------------------------------------------------------
    // Inspection, geometry & selection
    // -------------------------------------------------------------------------

    /** Returns up to `maxLines` trailing logical lines of plain terminal output. */
    QString recentOutputText(int maxLines = 120) const;

    /** Returns the currently selected text, or an empty string if nothing is selected. */
    QString selectedText() const;

    /** Returns `true` when there is a non-empty text selection. */
    bool hasSelection() const;

    /** Returns `true` while the underlying PTY shell process is running. */
    bool isShellRunning() const;

    /** Returns the shell's current working directory (updated via OSC 7 when reported). */
    QString workingDirectory() const;

    /** Returns the current terminal title (reported via OSC 0/2). */
    QString terminalTitle() const;

    int gridCols() const;
    int gridRows() const;

    /** Number of lines currently scrolled back into scrollback history (`0` = live bottom). */
    int scrollOffset() const;

    /** Scrolls the viewport by `lines` (positive = up into history, negative = down toward live bottom). */
    void scrollByLines(int lines);

    /** Controls whether Alt/Option sends an `ESC` Meta prefix for keystrokes. */
    void setOptionAsMeta(bool enabled);
    bool optionAsMeta() const;

    /** Non-owning accessors to the underlying PTY process and emulator (for diagnostics/tests). */
    SpellPtyProcess* ptyProcess() const;
    const SpellTerminalEmulator* emulator() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // -------------------------------------------------------------------------
    // Pure static color & VT key encoding helpers (testable standalone)
    // -------------------------------------------------------------------------

    /** Resolves a `TermColor` to a concrete `QColor` using `palette` (16 ANSI, 256 cube/gray, 24-bit RGB). */
    static QColor resolveColor(const TermColor& color,
                               const TerminalPalette& palette,
                               bool isForeground);

    /** Encodes a Qt key press into the byte sequence expected by an `xterm-256color` application. */
    static QByteArray encodeKeyEvent(int qtKey,
                                     Qt::KeyboardModifiers modifiers,
                                     const QString& text,
                                     bool applicationCursorKeys,
                                     bool optionAsMeta = true);

    /**
     * Normalizes newlines to CR and wraps `text` in `\x1b[200~ ... \x1b[201~` when
     * `bracketedPasteMode` is active, stripping any embedded paste-boundary markers.
     */
    static QByteArray encodeBracketedPaste(const QString& text, bool bracketedPasteMode);

public slots:
    /** Clears the screen and scrollback buffer and resets scroll offset to the live bottom. */
    void clearBuffer();

    /** Refreshes colors, monospace font metrics, and grid dimensions from `ThemeManager::instance()`. */
    void applyTheme();

    /** Copies the current text selection to the system clipboard (no-op if selection is empty). */
    void copySelection() const;

    /** Pastes plain text from the system clipboard into the terminal. */
    void pasteFromClipboard();

    /** Selects all retained scrollback and active screen cells. */
    void selectAll();

    /** Clears any active text selection. */
    void clearSelection();

    /** Scrolls immediately back to the live bottom row (`scrollOffset() == 0`). */
    void scrollToBottom();

signals:
    void gridSizeChanged(int cols, int rows);
    void titleChanged(const QString& title);
    void shellExited(int code);
    void shellStarted(const QString& cwd);
    void workingDirectoryChanged(const QString& cwd);
    void spellHandOffRequested(const QString& instruction,
                               const QString& recentOutput,
                               const QString& cwd);
    void bellRung();

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
