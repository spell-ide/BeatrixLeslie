#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>

namespace spell {

/**
 * Cell foreground/background color representation in `SpellTerminalEmulator`.
 *
 * - `DefaultFg` / `DefaultBg`: resolved dynamically from the active terminal theme.
 * - `Indexed`: 0..15 ANSI standard/bright, 16..231 6x6x6 cube, 232..255 grayscale.
 * - `Rgb`: 24-bit truecolor (`r`, `g`, `b`).
 */
struct TermColor {
    enum Kind {
        DefaultFg,
        DefaultBg,
        Indexed,
        Rgb
    };

    Kind kind = DefaultFg;
    uint8_t index = 0;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const TermColor&) const = default;

    static constexpr TermColor defaultFg() noexcept {
        return TermColor{DefaultFg, 0, 0, 0, 0};
    }

    static constexpr TermColor defaultBg() noexcept {
        return TermColor{DefaultBg, 0, 0, 0, 0};
    }

    static constexpr TermColor indexed(uint8_t idx) noexcept {
        return TermColor{Indexed, idx, 0, 0, 0};
    }

    static constexpr TermColor rgb(uint8_t red, uint8_t green, uint8_t blue) noexcept {
        return TermColor{Rgb, 0, red, green, blue};
    }
};

/**
 * A single character cell in the terminal screen or scrollback buffer.
 *
 * `width` is `1` for normal glyphs, `2` for the leading cell of a double-width
 * East Asian / emoji glyph, and `0` for the trailing continuation cell.
 */
struct TermCell {
    uint32_t codepoint = U' ';
    TermColor fg = TermColor::defaultFg();
    TermColor bg = TermColor::defaultBg();
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    bool dim = false;
    bool strikethrough = false;
    uint8_t width = 1;

    bool operator==(const TermCell&) const = default;

    static constexpr TermCell blank() noexcept {
        return TermCell{};
    }
};

/**
 * Pure C++20 VT100 / xterm-256color / 24-bit truecolor terminal state machine.
 *
 * Has zero OS or GUI dependencies (`QtCore` + C++20 standard library only) so it
 * can be instantiated and unit-tested completely standalone.
 *
 * Supports streaming UTF-8, C0 controls, ESC / CSI / OSC / DCS parsing, SGR
 * attributes (16-color, 256-color, 24-bit RGB, semicolon & colon forms), cursor
 * addressing, erase / insert / delete, DECSTBM scroll regions, primary & alternate
 * screen buffers (DECSET 47 / 1047 / 1049), DEC private modes (application cursor
 * keys, autowrap, cursor visibility, bracketed paste), OSC 0/2 title, OSC 7 cwd,
 * wide glyphs, bounded scrollback, and DSR/DA/XTWINOPS query responses.
 */
class SpellTerminalEmulator {
public:
    explicit SpellTerminalEmulator(int cols = 80, int rows = 24, int maxScrollback = 4000);
    ~SpellTerminalEmulator();

    SpellTerminalEmulator(const SpellTerminalEmulator&) = delete;
    SpellTerminalEmulator& operator=(const SpellTerminalEmulator&) = delete;
    SpellTerminalEmulator(SpellTerminalEmulator&&) noexcept;
    SpellTerminalEmulator& operator=(SpellTerminalEmulator&&) noexcept;

    /** Feeds raw PTY output bytes into the VT/UTF-8 state machine. */
    void feed(const QByteArray& bytes);

    /**
     * Resizes the active terminal grid to `cols x rows` (each clamped to >= 2),
     * preserving the cursor row by moving overflow lines between main screen and
     * scrollback.
     */
    void resize(int cols, int rows);

    /** Clears the active screen and scrollback and homes the cursor to `(0, 0)`. */
    void clear();

    /** Clears only retained scrollback history without disturbing the active screen. */
    void clearScrollback();

    /** Performs a full terminal reset (RIS: clears screens, modes, SGR, tab stops, title). */
    void reset();

    // -------------------------------------------------------------------------
    // Geometry, cursor & DEC/ANSI mode accessors
    // -------------------------------------------------------------------------

    int cols() const;
    int rows() const;
    int scrollbackCount() const;
    int maxScrollback() const;

    /**
     * Total accessible lines via `lineAt()`:
     * `scrollbackCount() + rows()` on the primary screen, or `rows()` when the
     * alternate screen buffer is active.
     */
    int totalLineCount() const;

    /** Cumulative count of scrollback lines evicted from the head (for stable selection anchors). */
    int droppedScrollbackLines() const;

    int cursorRow() const;
    int cursorCol() const;
    bool cursorVisible() const;

    bool applicationCursorKeys() const;
    bool applicationKeypad() const;
    bool bracketedPasteMode() const;
    bool isAlternateScreen() const;
    bool mouseReporting() const;

    uint64_t bellCount() const;
    uint64_t generation() const;

    // -------------------------------------------------------------------------
    // Unified scrollback + active screen inspection & text extraction
    // -------------------------------------------------------------------------

    /**
     * Unified row accessor across retained scrollback and active screen:
     * - On the primary screen: `[0, scrollbackCount())` indexes oldest-to-newest
     *   scrollback lines, and `[scrollbackCount(), scrollbackCount() + rows())`
     *   indexes active screen rows `[0, rows())`.
     * - On the alternate screen: `[0, rows())` indexes active screen rows directly.
     * - Out-of-range offsets safely return a static empty vector.
     */
    const QVector<TermCell>& lineAt(int historyOffsetFromTop) const;

    /** Returns `true` if the row at `historyOffsetFromTop` soft-wrapped into the next row. */
    bool isLineWrapped(int historyOffsetFromTop) const;

    /** Plain text of the visible active screen (trailing spaces trimmed per row, `\n`-joined). */
    QString screenText() const;

    /**
     * Plain text of up to `maxLines` trailing logical lines from scrollback + active screen
     * (soft-wrapped rows joined, trailing blank lines trimmed).
     */
    QString recentText(int maxLines = 120) const;

    /**
     * Extracts plain text between unified history coordinates `(r1, c1)` and `(r2, c2)`
     * matching `lineAt()` indexing (automatically normalizes reversed endpoints, skips
     * wide-glyph continuation cells, and joins soft-wrapped rows).
     */
    QString textRange(int r1, int c1, int r2, int c2) const;

    /** Window title reported via OSC 0 or OSC 2. */
    QString title() const;

    /** Working directory path reported by the shell via OSC 7 (`file://host/path`), if any. */
    QString reportedWorkingDirectory() const;

    /** Column display width of a Unicode codepoint: `0` (combining/zero-width), `1`, or `2` (wide/emoji). */
    static int displayWidth(uint32_t codepoint) noexcept;

    /**
     * Optional callback invoked synchronously during `feed()` when the terminal
     * generates device query responses (DSR `\x1b[5n` / `\x1b[6n`, DA1 `\x1b[c`,
     * DA2 `\x1b[>c`, XTWINOPS `\x1b[18t`) that should be written back to the PTY.
     */
    std::function<void(const QByteArray&)> onResponseBytes;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
