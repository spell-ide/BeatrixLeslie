#include "SpellTerminalEmulator.h"

#include <QChar>
#include <QList>
#include <QUrl>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace spell {

namespace {

constexpr int kMinGridDim = 2;
constexpr size_t kMaxCsiParamBytes = 128;
constexpr size_t kMaxCsiIntermediates = 4;
constexpr size_t kMaxOscBytes = 4096;
constexpr int kMaxOscTitleChars = 256;
constexpr int kMaxCsiParamValue = 99999;

inline void appendCodepoint(QString& out, uint32_t cp) {
    if (cp <= 0xFFFFu) {
        out.append(QChar(static_cast<char16_t>(cp)));
    } else if (cp <= 0x10FFFFu) {
        const uint32_t u = cp - 0x10000u;
        out.append(QChar(static_cast<char16_t>(0xD800u + (u >> 10))));
        out.append(QChar(static_cast<char16_t>(0xDC00u + (u & 0x3FFu))));
    } else {
        out.append(QChar(static_cast<char16_t>(0xFFFDu)));
    }
}

inline void trimTrailingSpaces(QString& s) {
    while (!s.isEmpty() && s.back() == QChar(u' ')) {
        s.chop(1);
    }
}

inline bool lineHasVisibleNonSpace(const QVector<TermCell>& cells) noexcept {
    for (const TermCell& cell : cells) {
        if (cell.width != 0 && cell.codepoint != U' ') {
            return true;
        }
    }
    return false;
}

QVector<TermCell> fitCells(const QVector<TermCell>& cells, int targetWidth) {
    const int current = static_cast<int>(cells.size());
    if (current == targetWidth) {
        return cells;
    }
    QVector<TermCell> out;
    out.reserve(targetWidth);
    if (current < targetWidth) {
        out = cells;
        out.resize(targetWidth, TermCell::blank());
        return out;
    }
    out = cells.mid(0, targetWidth);
    if (targetWidth > 0 && out.back().width == 2) {
        // A wide glyph cut in half at the right margin becomes blank.
        out.back() = TermCell::blank();
    }
    return out;
}

std::vector<bool> makeDefaultTabStops(int cols) {
    std::vector<bool> stops(static_cast<size_t>(cols), false);
    for (int c = 8; c < cols; c += 8) {
        stops[static_cast<size_t>(c)] = true;
    }
    return stops;
}

struct TermLine {
    QVector<TermCell> cells;
    bool isWrapped = false;
    // Column count at the time this row soft-wrapped (0 means full cells.size()).
    int wrappedWidth = 0;

    static TermLine makeBlank(int cols, const TermCell& fill = TermCell::blank()) {
        TermLine line;
        line.cells.fill(fill, cols);
        line.isWrapped = false;
        line.wrappedWidth = 0;
        return line;
    }

    QString visibleText(bool trimTrailing) const {
        const int limit = (!trimTrailing && isWrapped && wrappedWidth > 0)
            ? std::min(wrappedWidth, static_cast<int>(cells.size()))
            : static_cast<int>(cells.size());
        QString out;
        out.reserve(limit);
        for (int i = 0; i < limit; ++i) {
            if (cells[i].width != 0) {
                appendCodepoint(out, cells[i].codepoint);
            }
        }
        if (trimTrailing) {
            trimTrailingSpaces(out);
        }
        return out;
    }
};

struct TermModes {
    bool applicationCursorKeys = false;
    bool applicationKeypad = false;
    bool bracketedPaste = false;
    bool cursorVisible = true;
    bool autoWrap = true;
    bool insertMode = false;
    bool originMode = false;
    bool isAlternateScreen = false;
    bool mouseReporting = false;
};

struct TermStyle {
    TermColor fg = TermColor::defaultFg();
    TermColor bg = TermColor::defaultBg();
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    bool dim = false;
    bool strikethrough = false;

    TermCell makeCell(uint32_t cp, uint8_t width) const noexcept {
        TermCell cell;
        cell.codepoint = cp;
        cell.fg = fg;
        cell.bg = bg;
        cell.bold = bold;
        cell.italic = italic;
        cell.underline = underline;
        cell.inverse = inverse;
        cell.dim = dim;
        cell.strikethrough = strikethrough;
        cell.width = width;
        return cell;
    }
};

struct SavedCursor {
    int row = 0;
    int col = 0;
    TermStyle style;
    bool pendingWrap = false;
    bool originMode = false;
};

struct ScrollRegion {
    int top = 0;
    int bottom = 0;
};

enum class ParserState {
    Ground,
    Escape,
    EscapeIntermediate,
    Csi,
    Osc,
    OscEscape,
    IgnoreString,
    IgnoreStringEscape
};

using ParamGroup = std::vector<std::optional<int>>;

} // namespace

struct SpellTerminalEmulator::Impl {
    int cols = 80;
    int rows = 24;
    int maxScrollback = 4000;

    QString title;
    QString reportedWorkingDirectory;
    TermModes modes;
    uint64_t bellCount = 0;
    uint64_t generation = 0;
    int droppedScrollbackLines = 0;

    // screens[0] = primary screen, screens[1] = alternate screen.
    std::array<std::vector<TermLine>, 2> screens;
    std::deque<TermLine> scrollback;
    std::array<ScrollRegion, 2> scrollRegions;

    int cursorRow = 0;
    int cursorCol = 0;
    bool pendingWrap = false;
    TermStyle style;
    std::vector<bool> tabStops;
    std::optional<std::pair<int, int>> lastPrinted;
    std::optional<uint32_t> lastPrintedCodepoint;
    std::array<std::optional<SavedCursor>, 2> savedCursors;

    // Streaming parser state.
    ParserState state = ParserState::Ground;
    uint8_t escIntermediate = 0;
    std::vector<uint8_t> csiParamBytes;
    uint8_t csiPrivate = 0;
    std::vector<uint8_t> csiIntermediates;
    std::vector<uint8_t> oscBuffer;
    std::array<uint8_t, 4> utf8Buf{};
    int utf8Len = 0;
    int utf8Remaining = 0;

    // Non-owning pointer active only during feed().
    const std::function<void(const QByteArray&)>* activeResponseCallback = nullptr;

    Impl(int initialCols, int initialRows, int initialMaxScrollback)
        : cols(std::max(kMinGridDim, initialCols))
        , rows(std::max(kMinGridDim, initialRows))
        , maxScrollback(std::max(0, initialMaxScrollback))
        , tabStops(makeDefaultTabStops(cols)) {
        const TermLine blank = TermLine::makeBlank(cols);
        screens[0].assign(static_cast<size_t>(rows), blank);
        screens[1].assign(static_cast<size_t>(rows), blank);
        scrollRegions[0] = {0, rows - 1};
        scrollRegions[1] = {0, rows - 1};
        csiParamBytes.reserve(32);
        csiIntermediates.reserve(kMaxCsiIntermediates);
        oscBuffer.reserve(128);
    }

    int activeScreen() const noexcept {
        return modes.isAlternateScreen ? 1 : 0;
    }

    int& scrollTop() noexcept {
        return scrollRegions[static_cast<size_t>(activeScreen())].top;
    }

    int scrollTop() const noexcept {
        return scrollRegions[static_cast<size_t>(activeScreen())].top;
    }

    int& scrollBottom() noexcept {
        return scrollRegions[static_cast<size_t>(activeScreen())].bottom;
    }

    int scrollBottom() const noexcept {
        return scrollRegions[static_cast<size_t>(activeScreen())].bottom;
    }

    TermCell blankCell() const noexcept {
        TermCell cell = TermCell::blank();
        cell.bg = style.bg; // Background Color Erase (BCE)
        return cell;
    }

    TermLine blankLine() const {
        return TermLine::makeBlank(cols, blankCell());
    }

    void respond(const QByteArray& bytes) const {
        if (activeResponseCallback && *activeResponseCallback && !bytes.isEmpty()) {
            (*activeResponseCallback)(bytes);
        }
    }

    // -------------------------------------------------------------------------
    // Public-facing operations
    // -------------------------------------------------------------------------

    void feed(const QByteArray& bytes, const std::function<void(const QByteArray&)>& callback) {
        if (bytes.isEmpty()) {
            return;
        }
        activeResponseCallback = &callback;
        struct Guard {
            Impl* self;
            ~Guard() { self->activeResponseCallback = nullptr; }
        } guard{this};

        for (char ch : bytes) {
            processByte(static_cast<uint8_t>(ch));
        }
        ++generation;
    }

    void resize(int newCols, int newRows) {
        const int c = std::max(kMinGridDim, newCols);
        const int r = std::max(kMinGridDim, newRows);
        if (c == cols && r == rows) {
            return;
        }

        for (int s = 0; s < 2; ++s) {
            for (TermLine& line : screens[static_cast<size_t>(s)]) {
                line.cells = fitCells(line.cells, c);
                if (line.isWrapped && line.wrappedWidth > c) {
                    line.wrappedWidth = c;
                }
            }
        }
        for (TermLine& line : scrollback) {
            if (line.cells.size() > c) {
                line.cells = fitCells(line.cells, c);
                if (line.isWrapped && line.wrappedWidth > c) {
                    line.wrappedWidth = c;
                }
            }
        }

        const TermLine plainBlank = TermLine::makeBlank(c);

        // Main screen vertical resize: preserve cursor row using scrollback.
        int mainCursorRow = (activeScreen() == 0)
            ? cursorRow
            : (savedCursors[0].has_value() ? savedCursors[0]->row : 0);

        if (r < rows) {
            const int pushUp = std::max(0, (mainCursorRow + 1) - r);
            const int actualPush = std::min(pushUp, static_cast<int>(screens[0].size()));
            for (int i = 0; i < actualPush; ++i) {
                appendScrollback(std::move(screens[0][static_cast<size_t>(i)]));
            }
            if (actualPush > 0) {
                screens[0].erase(screens[0].begin(), screens[0].begin() + actualPush);
            }
            mainCursorRow -= actualPush;
            if (static_cast<int>(screens[0].size()) > r) {
                screens[0].resize(static_cast<size_t>(r));
            }
        } else if (r > rows) {
            int pulled = 0;
            const int wantPull = r - rows;
            std::vector<TermLine> restored;
            restored.reserve(static_cast<size_t>(wantPull));
            while (pulled < wantPull && !scrollback.empty()) {
                TermLine line = std::move(scrollback.back());
                scrollback.pop_back();
                const int prevWidth = static_cast<int>(line.cells.size());
                if (line.isWrapped && line.wrappedWidth == 0) {
                    line.wrappedWidth = prevWidth;
                }
                line.cells = fitCells(line.cells, c);
                if (line.isWrapped && line.wrappedWidth > c) {
                    line.wrappedWidth = c;
                }
                restored.push_back(std::move(line));
                ++pulled;
            }
            if (!restored.empty()) {
                std::reverse(restored.begin(), restored.end());
                screens[0].insert(screens[0].begin(),
                                  std::make_move_iterator(restored.begin()),
                                  std::make_move_iterator(restored.end()));
            }
            mainCursorRow += pulled;
            while (static_cast<int>(screens[0].size()) < r) {
                screens[0].push_back(plainBlank);
            }
        }

        // Alternate screen vertical resize: keep top, trim/pad bottom.
        int altCursorRow = (activeScreen() == 1)
            ? cursorRow
            : (savedCursors[1].has_value() ? savedCursors[1]->row : 0);

        if (static_cast<int>(screens[1].size()) > r) {
            const int excess = static_cast<int>(screens[1].size()) - r;
            const int trimTop = std::max(0, std::min(excess, (altCursorRow + 1) - r));
            if (trimTop > 0) {
                screens[1].erase(screens[1].begin(), screens[1].begin() + trimTop);
                altCursorRow -= trimTop;
            }
            if (static_cast<int>(screens[1].size()) > r) {
                screens[1].resize(static_cast<size_t>(r));
            }
        }
        while (static_cast<int>(screens[1].size()) < r) {
            screens[1].push_back(plainBlank);
        }

        cols = c;
        rows = r;
        scrollRegions[0] = {0, r - 1};
        scrollRegions[1] = {0, r - 1};
        tabStops = makeDefaultTabStops(c);
        pendingWrap = false;
        lastPrinted.reset();

        if (activeScreen() == 0) {
            cursorRow = std::clamp(mainCursorRow, 0, r - 1);
            if (savedCursors[1].has_value()) {
                savedCursors[1]->row = std::clamp(altCursorRow, 0, r - 1);
            }
        } else {
            cursorRow = std::clamp(altCursorRow, 0, r - 1);
            if (savedCursors[0].has_value()) {
                savedCursors[0]->row = std::clamp(mainCursorRow, 0, r - 1);
            }
        }
        cursorCol = std::clamp(cursorCol, 0, c - 1);
        for (int s = 0; s < 2; ++s) {
            if (savedCursors[static_cast<size_t>(s)].has_value()) {
                savedCursors[static_cast<size_t>(s)]->col =
                    std::clamp(savedCursors[static_cast<size_t>(s)]->col, 0, c - 1);
            }
        }

        ++generation;
    }

    void clearScrollbackInternal() {
        droppedScrollbackLines += static_cast<int>(scrollback.size());
        scrollback.clear();
    }

    void clear() {
        const TermLine plainBlank = TermLine::makeBlank(cols);
        screens[static_cast<size_t>(activeScreen())].assign(static_cast<size_t>(rows), plainBlank);
        clearScrollbackInternal();
        cursorRow = 0;
        cursorCol = 0;
        pendingWrap = false;
        lastPrinted.reset();
        ++generation;
    }

    void fullReset() {
        const TermLine plainBlank = TermLine::makeBlank(cols);
        screens[0].assign(static_cast<size_t>(rows), plainBlank);
        screens[1].assign(static_cast<size_t>(rows), plainBlank);
        clearScrollbackInternal();
        modes = TermModes{};
        style = TermStyle{};
        cursorRow = 0;
        cursorCol = 0;
        pendingWrap = false;
        scrollRegions[0] = {0, rows - 1};
        scrollRegions[1] = {0, rows - 1};
        tabStops = makeDefaultTabStops(cols);
        savedCursors = {std::nullopt, std::nullopt};
        title.clear();
        reportedWorkingDirectory.clear();
        lastPrinted.reset();
        lastPrintedCodepoint.reset();

        state = ParserState::Ground;
        escIntermediate = 0;
        csiParamBytes.clear();
        csiPrivate = 0;
        csiIntermediates.clear();
        oscBuffer.clear();
        utf8Len = 0;
        utf8Remaining = 0;
    }

    void softReset() {
        modes.cursorVisible = true;
        modes.originMode = false;
        modes.autoWrap = true;
        modes.insertMode = false;
        modes.applicationCursorKeys = false;
        modes.applicationKeypad = false;
        style = TermStyle{};
        scrollRegions[0] = {0, rows - 1};
        scrollRegions[1] = {0, rows - 1};
        savedCursors = {std::nullopt, std::nullopt};
        pendingWrap = false;
        lastPrinted.reset();
    }

    // -------------------------------------------------------------------------
    // Unified history indexing helpers
    // -------------------------------------------------------------------------

    int totalLineCount() const noexcept {
        return modes.isAlternateScreen
            ? rows
            : static_cast<int>(scrollback.size()) + rows;
    }

    const TermLine* termLineAt(int offset) const noexcept {
        if (offset < 0) {
            return nullptr;
        }
        if (modes.isAlternateScreen) {
            if (offset < rows) {
                return &screens[1][static_cast<size_t>(offset)];
            }
            return nullptr;
        }
        const int sb = static_cast<int>(scrollback.size());
        if (offset < sb) {
            return &scrollback[static_cast<size_t>(offset)];
        }
        const int screenRow = offset - sb;
        if (screenRow < rows) {
            return &screens[0][static_cast<size_t>(screenRow)];
        }
        return nullptr;
    }

    // -------------------------------------------------------------------------
    // Byte-level state machine
    // -------------------------------------------------------------------------

    void processByte(uint8_t b) {
        switch (state) {
        case ParserState::Ground:
            handleGround(b);
            break;
        case ParserState::Escape:
            handleEscape(b);
            break;
        case ParserState::EscapeIntermediate:
            handleEscapeIntermediate(b);
            break;
        case ParserState::Csi:
            handleCsi(b);
            break;
        case ParserState::Osc:
            if (b == 0x07u) {
                finishOsc();
                state = ParserState::Ground;
            } else if (b == 0x1Bu) {
                state = ParserState::OscEscape;
            } else if (b == 0x18u || b == 0x1Au) {
                oscBuffer.clear();
                state = ParserState::Ground;
            } else if (oscBuffer.size() < kMaxOscBytes) {
                oscBuffer.push_back(b);
            }
            break;
        case ParserState::OscEscape:
            finishOsc();
            if (b == 0x5Cu) { // ST (ESC \)
                state = ParserState::Ground;
            } else {
                state = ParserState::Escape;
                handleEscape(b);
            }
            break;
        case ParserState::IgnoreString:
            if (b == 0x1Bu) {
                state = ParserState::IgnoreStringEscape;
            } else if (b == 0x18u || b == 0x1Au) {
                state = ParserState::Ground;
            }
            break;
        case ParserState::IgnoreStringEscape:
            if (b == 0x5Cu) {
                state = ParserState::Ground;
            } else if (b != 0x1Bu) {
                state = ParserState::IgnoreString;
            }
            break;
        }
    }

    void handleGround(uint8_t b) {
        if (utf8Remaining > 0) {
            if ((b & 0xC0u) == 0x80u) {
                utf8Buf[static_cast<size_t>(utf8Len++)] = b;
                --utf8Remaining;
                if (utf8Remaining == 0) {
                    const uint32_t cp = decodeUtf8Sequence();
                    utf8Len = 0;
                    printCodepoint(cp);
                }
                return;
            }
            // Truncated UTF-8 sequence: emit replacement and reprocess current byte.
            utf8Len = 0;
            utf8Remaining = 0;
            printCodepoint(0xFFFDu);
        }

        if (b == 0x1Bu) {
            state = ParserState::Escape;
        } else if (b < 0x20u || b == 0x7Fu) {
            executeControl(b);
        } else if (b < 0x7Fu) {
            printCodepoint(static_cast<uint32_t>(b));
        } else if (b >= 0xC2u && b <= 0xDFu) {
            utf8Buf[0] = b;
            utf8Len = 1;
            utf8Remaining = 1;
        } else if (b >= 0xE0u && b <= 0xEFu) {
            utf8Buf[0] = b;
            utf8Len = 1;
            utf8Remaining = 2;
        } else if (b >= 0xF0u && b <= 0xF4u) {
            utf8Buf[0] = b;
            utf8Len = 1;
            utf8Remaining = 3;
        } else {
            printCodepoint(0xFFFDu);
        }
    }

    uint32_t decodeUtf8Sequence() const noexcept {
        if (utf8Len == 2) {
            const uint32_t cp =
                (static_cast<uint32_t>(utf8Buf[0] & 0x1Fu) << 6) |
                static_cast<uint32_t>(utf8Buf[1] & 0x3Fu);
            return (cp >= 0x80u) ? cp : 0xFFFDu;
        }
        if (utf8Len == 3) {
            const uint32_t cp =
                (static_cast<uint32_t>(utf8Buf[0] & 0x0Fu) << 12) |
                (static_cast<uint32_t>(utf8Buf[1] & 0x3Fu) << 6) |
                static_cast<uint32_t>(utf8Buf[2] & 0x3Fu);
            if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
                return 0xFFFDu;
            }
            return cp;
        }
        if (utf8Len == 4) {
            const uint32_t cp =
                (static_cast<uint32_t>(utf8Buf[0] & 0x07u) << 18) |
                (static_cast<uint32_t>(utf8Buf[1] & 0x3Fu) << 12) |
                (static_cast<uint32_t>(utf8Buf[2] & 0x3Fu) << 6) |
                static_cast<uint32_t>(utf8Buf[3] & 0x3Fu);
            if (cp < 0x10000u || cp > 0x10FFFFu) {
                return 0xFFFDu;
            }
            return cp;
        }
        return 0xFFFDu;
    }

    void executeControl(uint8_t b) {
        switch (b) {
        case 0x07u: // BEL
            ++bellCount;
            break;
        case 0x08u: // BS
            pendingWrap = false;
            lastPrinted.reset();
            if (cursorCol > 0) {
                --cursorCol;
            }
            break;
        case 0x09u: // HT
            pendingWrap = false;
            lastPrinted.reset();
            cursorCol = nextTabStop(cursorCol);
            break;
        case 0x0Au: // LF
        case 0x0Bu: // VT
        case 0x0Cu: // FF
            index();
            break;
        case 0x0Du: // CR
            pendingWrap = false;
            lastPrinted.reset();
            cursorCol = 0;
            break;
        default:
            break;
        }
    }

    void handleEscape(uint8_t b) {
        if (b == 0x1Bu) {
            return; // Restart ESC sequence.
        }
        if (b == 0x18u || b == 0x1Au) {
            state = ParserState::Ground;
            return;
        }
        if (b < 0x20u) {
            executeControl(b);
            return;
        }
        if (b == 0x7Fu) {
            return;
        }

        state = ParserState::Ground;
        switch (b) {
        case 0x5Bu: // '[' CSI
            csiParamBytes.clear();
            csiIntermediates.clear();
            csiPrivate = 0;
            state = ParserState::Csi;
            break;
        case 0x5Du: // ']' OSC
            oscBuffer.clear();
            state = ParserState::Osc;
            break;
        case 0x50u: // 'P' DCS
        case 0x58u: // 'X' SOS
        case 0x5Eu: // '^' PM
        case 0x5Fu: // '_' APC
            state = ParserState::IgnoreString;
            break;
        case 0x37u: // '7' DECSC
            saveCursor();
            break;
        case 0x38u: // '8' DECRC
            restoreCursor();
            break;
        case 0x44u: // 'D' IND
            index();
            break;
        case 0x45u: // 'E' NEL
            cursorCol = 0;
            index();
            break;
        case 0x48u: // 'H' HTS
            if (cursorCol >= 0 && cursorCol < static_cast<int>(tabStops.size())) {
                tabStops[static_cast<size_t>(cursorCol)] = true;
            }
            break;
        case 0x4Du: // 'M' RI
            reverseIndex();
            break;
        case 0x63u: // 'c' RIS
            fullReset();
            break;
        case 0x3Du: // '=' DECKPAM
            modes.applicationKeypad = true;
            break;
        case 0x3Eu: // '>' DECKPNM
            modes.applicationKeypad = false;
            break;
        default:
            if (b >= 0x20u && b <= 0x2Fu) {
                escIntermediate = b;
                state = ParserState::EscapeIntermediate;
            }
            break;
        }
    }

    void handleEscapeIntermediate(uint8_t b) {
        if (b == 0x1Bu) {
            state = ParserState::Escape;
            return;
        }
        if (b == 0x18u || b == 0x1Au) {
            state = ParserState::Ground;
            return;
        }
        if (b < 0x20u) {
            executeControl(b);
            return;
        }
        if (b == 0x7Fu) {
            return;
        }
        if (b >= 0x20u && b <= 0x2Fu) {
            return;
        }
        // DECALN: ESC # 8 fills active screen with 'E'.
        if (escIntermediate == '#' && b == '8') {
            TermCell eCell = TermCell::blank();
            eCell.codepoint = U'E';
            const TermLine eLine = TermLine::makeBlank(cols, eCell);
            screens[static_cast<size_t>(activeScreen())].assign(static_cast<size_t>(rows), eLine);
            scrollRegions[static_cast<size_t>(activeScreen())] = {0, rows - 1};
            cursorRow = 0;
            cursorCol = 0;
            pendingWrap = false;
            lastPrinted.reset();
        }
        escIntermediate = 0;
        state = ParserState::Ground;
    }

    void handleCsi(uint8_t b) {
        if (b >= 0x30u && b <= 0x3Fu) {
            if ((b == '<' || b == '=' || b == '>' || b == '?') &&
                csiParamBytes.empty() && csiPrivate == 0 && csiIntermediates.empty()) {
                csiPrivate = b;
            } else if (csiParamBytes.size() < kMaxCsiParamBytes) {
                csiParamBytes.push_back(b);
            }
        } else if (b >= 0x20u && b <= 0x2Fu) {
            if (csiIntermediates.size() < kMaxCsiIntermediates) {
                csiIntermediates.push_back(b);
            }
        } else if (b >= 0x40u && b <= 0x7Eu) {
            dispatchCsi(b);
            state = ParserState::Ground;
        } else if (b == 0x1Bu) {
            state = ParserState::Escape;
        } else if (b == 0x18u || b == 0x1Au) {
            state = ParserState::Ground;
        } else if (b < 0x20u) {
            executeControl(b);
        }
    }

    // -------------------------------------------------------------------------
    // CSI parsing & dispatch
    // -------------------------------------------------------------------------

    std::vector<ParamGroup> parseParamGroups() const {
        if (csiParamBytes.empty()) {
            return {};
        }
        std::vector<ParamGroup> groups(1);
        std::optional<int> current;
        for (uint8_t byte : csiParamBytes) {
            if (byte >= '0' && byte <= '9') {
                const int digit = static_cast<int>(byte - '0');
                current = std::min(kMaxCsiParamValue, current.value_or(0) * 10 + digit);
            } else if (byte == ':') {
                groups.back().push_back(current);
                current.reset();
            } else if (byte == ';') {
                groups.back().push_back(current);
                groups.emplace_back();
                current.reset();
            }
        }
        groups.back().push_back(current);
        return groups;
    }

    static int groupArg(const std::vector<ParamGroup>& groups, size_t idx, int def) noexcept {
        if (idx < groups.size() && !groups[idx].empty() && groups[idx][0].has_value()) {
            return *groups[idx][0];
        }
        return def;
    }

    static int groupCount(const std::vector<ParamGroup>& groups, size_t idx) noexcept {
        return std::max(1, groupArg(groups, idx, 1));
    }

    void dispatchCsi(uint8_t finalByte) {
        const std::vector<ParamGroup> groups = parseParamGroups();

        if (csiPrivate == '?') {
            if (!csiIntermediates.empty()) {
                return;
            }
            if (finalByte == 'h' || finalByte == 'l') {
                const bool enabled = (finalByte == 'h');
                for (const ParamGroup& g : groups) {
                    const int v = (!g.empty() && g[0].has_value()) ? *g[0] : 0;
                    setPrivateMode(v, enabled);
                }
            } else if (finalByte == 'n') {
                const int code = groupArg(groups, 0, 0);
                if (code == 5) {
                    respond(QByteArrayLiteral("\x1b[?0n"));
                } else if (code == 6) {
                    const int r = modes.originMode ? (cursorRow - scrollTop()) : cursorRow;
                    respond(QByteArrayLiteral("\x1b[?") +
                            QByteArray::number(r + 1) + ';' +
                            QByteArray::number(cursorCol + 1) + 'R');
                }
            }
            return;
        }

        if (csiPrivate == '>') {
            if (finalByte == 'c' && csiIntermediates.empty() && groupArg(groups, 0, 0) == 0) {
                respond(QByteArrayLiteral("\x1b[>0;10;1c"));
            }
            return;
        }

        if (csiPrivate != 0) {
            return;
        }

        if (!csiIntermediates.empty()) {
            if (finalByte == 'p' &&
                std::find(csiIntermediates.cbegin(), csiIntermediates.cend(), static_cast<uint8_t>('!')) !=
                    csiIntermediates.cend()) {
                softReset(); // CSI ! p (DECSTR)
            }
            return;
        }

        switch (finalByte) {
        case '@': // ICH - Insert Blank Characters
            insertBlanks(groupCount(groups, 0));
            break;
        case 'A': // CUU - Cursor Up
            moveCursorUp(groupCount(groups, 0));
            break;
        case 'B': // CUD - Cursor Down
        case 'e': // VPR - Vertical Position Relative
            moveCursorDown(groupCount(groups, 0));
            break;
        case 'C': // CUF - Cursor Forward
        case 'a': // HPR - Horizontal Position Relative
            setCursor(cursorRow, cursorCol + groupCount(groups, 0));
            break;
        case 'D': // CUB - Cursor Backward
            setCursor(cursorRow, cursorCol - groupCount(groups, 0));
            break;
        case 'E': // CNL - Cursor Next Line
            moveCursorDown(groupCount(groups, 0));
            cursorCol = 0;
            break;
        case 'F': // CPL - Cursor Previous Line
            moveCursorUp(groupCount(groups, 0));
            cursorCol = 0;
            break;
        case 'G': // CHA - Cursor Character Absolute
        case '`': // HPA - Character Position Absolute
            setCursor(cursorRow, std::max(1, groupArg(groups, 0, 1)) - 1);
            break;
        case 'H': // CUP - Cursor Position
        case 'f': // HVP - Horizontal and Vertical Position
        {
            const int base = modes.originMode ? scrollTop() : 0;
            const int targetRow = base + std::max(1, groupArg(groups, 0, 1)) - 1;
            const int targetCol = std::max(1, groupArg(groups, 1, 1)) - 1;
            setCursor(targetRow, targetCol);
            break;
        }
        case 'I': // CHT - Cursor Forward Tabulation
        {
            pendingWrap = false;
            lastPrinted.reset();
            const int n = groupCount(groups, 0);
            for (int k = 0; k < n; ++k) {
                cursorCol = nextTabStop(cursorCol);
            }
            break;
        }
        case 'Z': // CBT - Cursor Backward Tabulation
        {
            pendingWrap = false;
            lastPrinted.reset();
            const int n = groupCount(groups, 0);
            for (int k = 0; k < n; ++k) {
                cursorCol = previousTabStop(cursorCol);
            }
            break;
        }
        case 'J': // ED - Erase in Display
            eraseInDisplay(groupArg(groups, 0, 0));
            break;
        case 'K': // EL - Erase in Line
            eraseInLine(groupArg(groups, 0, 0));
            break;
        case 'L': // IL - Insert Lines
            insertLines(groupCount(groups, 0));
            break;
        case 'M': // DL - Delete Lines
            deleteLines(groupCount(groups, 0));
            break;
        case 'P': // DCH - Delete Characters
            deleteChars(groupCount(groups, 0));
            break;
        case 'S': // SU - Scroll Up
            scrollUp(groupCount(groups, 0));
            break;
        case 'T': // SD - Scroll Down
            if (groups.size() <= 1) {
                scrollDown(groupCount(groups, 0));
            }
            break;
        case 'X': // ECH - Erase Characters
            eraseChars(groupCount(groups, 0));
            break;
        case 'b': // REP - Repeat Preceding Graphic Character
            if (lastPrintedCodepoint.has_value()) {
                const int n = std::min(groupCount(groups, 0), cols * rows);
                const uint32_t cp = *lastPrintedCodepoint;
                for (int k = 0; k < n; ++k) {
                    printCodepoint(cp);
                }
            }
            break;
        case 'c': // DA1 - Primary Device Attributes
            if (groupArg(groups, 0, 0) == 0) {
                respond(QByteArrayLiteral("\x1b[?1;2c"));
            }
            break;
        case 'd': // VPA - Line Position Absolute
        {
            const int base = modes.originMode ? scrollTop() : 0;
            setCursor(base + std::max(1, groupArg(groups, 0, 1)) - 1, cursorCol);
            break;
        }
        case 'g': // TBC - Tab Clear
            switch (groupArg(groups, 0, 0)) {
            case 0:
                if (cursorCol >= 0 && cursorCol < static_cast<int>(tabStops.size())) {
                    tabStops[static_cast<size_t>(cursorCol)] = false;
                }
                break;
            case 3:
                std::fill(tabStops.begin(), tabStops.end(), false);
                break;
            default:
                break;
            }
            break;
        case 'h': // SM - Set Mode
        case 'l': // RM - Reset Mode
        {
            const bool enabled = (finalByte == 'h');
            for (const ParamGroup& g : groups) {
                const int v = (!g.empty() && g[0].has_value()) ? *g[0] : 0;
                if (v == 4) {
                    modes.insertMode = enabled;
                }
            }
            break;
        }
        case 'm': // SGR - Select Graphic Rendition
            selectGraphicRendition(groups);
            break;
        case 'n': // DSR - Device Status Report
            switch (groupArg(groups, 0, 0)) {
            case 5:
                respond(QByteArrayLiteral("\x1b[0n"));
                break;
            case 6: {
                const int r = modes.originMode ? (cursorRow - scrollTop()) : cursorRow;
                respond(QByteArrayLiteral("\x1b[") +
                        QByteArray::number(r + 1) + ';' +
                        QByteArray::number(cursorCol + 1) + 'R');
                break;
            }
            default:
                break;
            }
            break;
        case 'r': // DECSTBM - Set Top and Bottom Margins
        {
            const int rawTop = groupArg(groups, 0, 1);
            const int rawBot = groupArg(groups, 1, rows);
            const int top = (rawTop <= 0 ? 1 : rawTop) - 1;
            const int bot = (rawBot <= 0 ? rows : rawBot) - 1;
            if (top >= 0 && bot < rows && top < bot) {
                scrollTop() = top;
                scrollBottom() = bot;
            } else {
                scrollTop() = 0;
                scrollBottom() = rows - 1;
            }
            setCursor(modes.originMode ? scrollTop() : 0, 0);
            break;
        }
        case 's': // SCOSC - Save Cursor Position
            if (groups.empty()) {
                saveCursor();
            }
            break;
        case 'u': // SCORC - Restore Cursor Position
            restoreCursor();
            break;
        case 't': // XTWINOPS
            if (groupArg(groups, 0, 0) == 18) {
                respond(QByteArrayLiteral("\x1b[8;") +
                        QByteArray::number(rows) + ';' +
                        QByteArray::number(cols) + 't');
            }
            break;
        default:
            break;
        }
    }

    void setPrivateMode(int mode, bool enabled) {
        switch (mode) {
        case 1: // DECCKM - Application Cursor Keys
            modes.applicationCursorKeys = enabled;
            break;
        case 6: // DECOM - Origin Mode
            modes.originMode = enabled;
            setCursor(enabled ? scrollTop() : 0, 0);
            break;
        case 7: // DECAWM - Auto-Wrap Mode
            modes.autoWrap = enabled;
            break;
        case 25: // DECTCEM - Text Cursor Enable
            modes.cursorVisible = enabled;
            break;
        case 47:
            switchScreen(enabled, /*saveRestoreCursor=*/false, /*clearAlt=*/false);
            break;
        case 1047:
            switchScreen(enabled, /*saveRestoreCursor=*/false, /*clearAlt=*/enabled);
            break;
        case 1048:
            if (enabled) {
                saveCursor();
            } else {
                restoreCursor();
            }
            break;
        case 1049:
            switchScreen(enabled, /*saveRestoreCursor=*/true, /*clearAlt=*/enabled);
            break;
        case 1000:
        case 1002:
        case 1003:
        case 1006:
        case 1015:
            modes.mouseReporting = enabled;
            break;
        case 2004: // Bracketed Paste Mode
            modes.bracketedPaste = enabled;
            break;
        default:
            break;
        }
    }

    // -------------------------------------------------------------------------
    // SGR handling (16-color, 256-color, 24-bit RGB, semicolon & colon forms)
    // -------------------------------------------------------------------------

    static uint8_t clampByte(std::optional<int> v) noexcept {
        return static_cast<uint8_t>(std::clamp(v.value_or(0), 0, 255));
    }

    static std::pair<std::optional<TermColor>, size_t>
    parseExtendedColor(const std::vector<ParamGroup>& groups, size_t idx) noexcept {
        const ParamGroup& group = groups[idx];
        // Colon form: e.g. 38:5:N, 38:2:R:G:B, 38:2:CS:R:G:B
        if (group.size() > 1) {
            const int kind = group[1].value_or(0);
            if (kind == 5 && group.size() >= 3) {
                return {TermColor::indexed(clampByte(group[2])), 0};
            }
            if (kind == 2 && group.size() >= 5) {
                const size_t rgbStart = (group.size() >= 6) ? 3u : 2u;
                return {TermColor::rgb(clampByte(group[rgbStart]),
                                       clampByte(group[rgbStart + 1]),
                                       clampByte(group[rgbStart + 2])),
                        0};
            }
            return {std::nullopt, 0};
        }

        // Semicolon form: e.g. 38;5;N or 38;2;R;G;B
        if (idx + 1 >= groups.size()) {
            return {std::nullopt, 0};
        }
        const int kind = (!groups[idx + 1].empty() && groups[idx + 1][0].has_value())
            ? *groups[idx + 1][0]
            : 0;
        if (kind == 5 && idx + 2 < groups.size()) {
            const std::optional<int> n = groups[idx + 2].empty() ? std::nullopt : groups[idx + 2][0];
            return {TermColor::indexed(clampByte(n)), 2};
        }
        if (kind == 2 && idx + 4 < groups.size()) {
            const std::optional<int> r = groups[idx + 2].empty() ? std::nullopt : groups[idx + 2][0];
            const std::optional<int> g = groups[idx + 3].empty() ? std::nullopt : groups[idx + 3][0];
            const std::optional<int> b = groups[idx + 4].empty() ? std::nullopt : groups[idx + 4][0];
            return {TermColor::rgb(clampByte(r), clampByte(g), clampByte(b)), 4};
        }
        return {std::nullopt, 1};
    }

    void selectGraphicRendition(const std::vector<ParamGroup>& groups) {
        if (groups.empty()) {
            style = TermStyle{};
            return;
        }
        for (size_t i = 0; i < groups.size(); ++i) {
            const ParamGroup& group = groups[i];
            const int code = (!group.empty() && group[0].has_value()) ? *group[0] : 0;
            if (code == 0) {
                style = TermStyle{};
            } else if (code == 1) {
                style.bold = true;
            } else if (code == 2) {
                style.dim = true;
            } else if (code == 3) {
                style.italic = true;
            } else if (code == 4) {
                const int sub = (group.size() > 1) ? group[1].value_or(1) : 1;
                style.underline = (sub != 0);
            } else if (code == 7) {
                style.inverse = true;
            } else if (code == 9) {
                style.strikethrough = true;
            } else if (code == 21) {
                style.underline = true;
            } else if (code == 22) {
                style.bold = false;
                style.dim = false;
            } else if (code == 23) {
                style.italic = false;
            } else if (code == 24) {
                style.underline = false;
            } else if (code == 27) {
                style.inverse = false;
            } else if (code == 29) {
                style.strikethrough = false;
            } else if (code >= 30 && code <= 37) {
                style.fg = TermColor::indexed(static_cast<uint8_t>(code - 30));
            } else if (code == 39) {
                style.fg = TermColor::defaultFg();
            } else if (code >= 40 && code <= 47) {
                style.bg = TermColor::indexed(static_cast<uint8_t>(code - 40));
            } else if (code == 49) {
                style.bg = TermColor::defaultBg();
            } else if (code >= 90 && code <= 97) {
                style.fg = TermColor::indexed(static_cast<uint8_t>(code - 90 + 8));
            } else if (code >= 100 && code <= 107) {
                style.bg = TermColor::indexed(static_cast<uint8_t>(code - 100 + 8));
            } else if (code == 38 || code == 48) {
                const auto [color, consumed] = parseExtendedColor(groups, i);
                if (color.has_value()) {
                    if (code == 38) {
                        style.fg = *color;
                    } else {
                        style.bg = *color;
                    }
                }
                i += consumed;
            }
        }
    }

    // -------------------------------------------------------------------------
    // OSC handling (OSC 0/2 title, OSC 7 working directory)
    // -------------------------------------------------------------------------

    void finishOsc() {
        const QString text = QString::fromUtf8(
            reinterpret_cast<const char*>(oscBuffer.data()),
            static_cast<qsizetype>(oscBuffer.size()));
        oscBuffer.clear();

        const qsizetype sep = text.indexOf(QChar(u';'));
        if (sep < 0) {
            return;
        }
        const QStringView code = QStringView(text).left(sep);
        const QString value = text.mid(sep + 1);

        if (code == u"0" || code == u"2") {
            title = value.left(kMaxOscTitleChars);
        } else if (code == u"7") {
            const QUrl url(value);
            if (url.isValid() && url.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0) {
                QString path = url.path(QUrl::FullyDecoded);
                if (path.size() >= 3 && path[0] == QChar(u'/') && path[1].isLetter() && path[2] == QChar(u':')) {
                    path.remove(0, 1);
                }
                if (!path.isEmpty()) {
                    reportedWorkingDirectory = path;
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Glyph printing & wide-glyph repair
    // -------------------------------------------------------------------------

    void printCodepoint(uint32_t cp) {
        const int width = SpellTerminalEmulator::displayWidth(cp);
        const int a = activeScreen();

        if (width == 0) {
            if (lastPrinted.has_value() && QChar::isMark(cp)) {
                const int lr = lastPrinted->first;
                const int lc = lastPrinted->second;
                if (lr >= 0 && lr < rows) {
                    auto& cells = screens[static_cast<size_t>(a)][static_cast<size_t>(lr)].cells;
                    if (lc >= 0 && lc < static_cast<int>(cells.size())) {
                        QString combined;
                        appendCodepoint(combined, cells[lc].codepoint);
                        appendCodepoint(combined, cp);
                        const QString nfc = combined.normalized(QString::NormalizationForm_C);
                        const QList<uint> ucs4 = nfc.toUcs4();
                        if (ucs4.size() == 1) {
                            cells[lc].codepoint = static_cast<uint32_t>(ucs4.front());
                            lastPrintedCodepoint = cells[lc].codepoint;
                        }
                    }
                }
            }
            return;
        }

        if (pendingWrap) {
            if (modes.autoWrap) {
                TermLine& curLine = screens[static_cast<size_t>(a)][static_cast<size_t>(cursorRow)];
                curLine.isWrapped = true;
                curLine.wrappedWidth = cols;
                cursorCol = 0;
                index();
            }
            pendingWrap = false;
        }

        if (width == 2 && cursorCol == cols - 1) {
            if (modes.autoWrap) {
                writeCell(blankCell(), cursorRow, cursorCol);
                TermLine& curLine = screens[static_cast<size_t>(a)][static_cast<size_t>(cursorRow)];
                curLine.isWrapped = true;
                curLine.wrappedWidth = cols - 1;
                cursorCol = 0;
                index();
            } else {
                return;
            }
        }

        if (modes.insertMode) {
            insertBlanks(width);
        }

        writeCell(style.makeCell(cp, static_cast<uint8_t>(width)), cursorRow, cursorCol);
        if (width == 2 && cursorCol + 1 < cols) {
            writeCell(style.makeCell(U' ', 0), cursorRow, cursorCol + 1);
        }

        lastPrinted = {cursorRow, cursorCol};
        lastPrintedCodepoint = cp;

        const int nextCol = cursorCol + width;
        if (nextCol >= cols) {
            cursorCol = cols - 1;
            pendingWrap = true;
        } else {
            cursorCol = nextCol;
        }
    }

    void writeCell(const TermCell& cell, int row, int col) {
        const int a = activeScreen();
        if (row < 0 || row >= rows) {
            return;
        }
        auto& cells = screens[static_cast<size_t>(a)][static_cast<size_t>(row)].cells;
        if (col < 0 || col >= static_cast<int>(cells.size())) {
            return;
        }
        const TermCell old = cells[col];
        if (old.width == 0 && col > 0 && cell.width != 0) {
            TermCell repaired = cells[col - 1];
            repaired.codepoint = U' ';
            repaired.width = 1;
            cells[col - 1] = repaired;
        }
        if (old.width == 2 && col + 1 < static_cast<int>(cells.size()) && cell.width != 2) {
            TermCell repaired = old;
            repaired.codepoint = U' ';
            repaired.width = 1;
            cells[col + 1] = repaired;
        }
        cells[col] = cell;
    }

    // -------------------------------------------------------------------------
    // Cursor motion & scrolling
    // -------------------------------------------------------------------------

    void setCursor(int row, int col) noexcept {
        pendingWrap = false;
        lastPrinted.reset();
        const int minRow = modes.originMode ? scrollTop() : 0;
        const int maxRow = modes.originMode ? scrollBottom() : rows - 1;
        cursorRow = std::clamp(row, minRow, maxRow);
        cursorCol = std::clamp(col, 0, cols - 1);
    }

    void moveCursorUp(int n) noexcept {
        const int top = scrollTop();
        const int limit = (cursorRow >= top) ? top : 0;
        pendingWrap = false;
        lastPrinted.reset();
        cursorRow = std::clamp(std::max(limit, cursorRow - n), 0, rows - 1);
    }

    void moveCursorDown(int n) noexcept {
        const int bot = scrollBottom();
        const int limit = (cursorRow <= bot) ? bot : (rows - 1);
        pendingWrap = false;
        lastPrinted.reset();
        cursorRow = std::clamp(std::min(limit, cursorRow + n), 0, rows - 1);
    }

    void index() {
        pendingWrap = false;
        lastPrinted.reset();
        if (cursorRow == scrollBottom()) {
            scrollUp(1);
        } else if (cursorRow < rows - 1) {
            ++cursorRow;
        }
    }

    void reverseIndex() {
        pendingWrap = false;
        lastPrinted.reset();
        if (cursorRow == scrollTop()) {
            scrollDown(1);
        } else if (cursorRow > 0) {
            --cursorRow;
        }
    }

    void appendScrollback(TermLine&& line) {
        if (maxScrollback <= 0) {
            ++droppedScrollbackLines;
            return;
        }
        if (static_cast<int>(scrollback.size()) >= maxScrollback) {
            scrollback.pop_front();
            ++droppedScrollbackLines;
        }
        scrollback.push_back(std::move(line));
    }

    void scrollUp(int n) {
        const int a = activeScreen();
        const int top = scrollTop();
        const int bot = scrollBottom();
        const int regionHeight = bot - top + 1;
        const int amount = std::clamp(n, 0, regionHeight);
        if (amount <= 0) {
            return;
        }
        auto& grid = screens[static_cast<size_t>(a)];
        if (a == 0 && top == 0) {
            for (int i = 0; i < amount; ++i) {
                appendScrollback(std::move(grid[static_cast<size_t>(i)]));
            }
        }
        std::move(grid.begin() + top + amount,
                  grid.begin() + bot + 1,
                  grid.begin() + top);
        for (int r = bot - amount + 1; r <= bot; ++r) {
            grid[static_cast<size_t>(r)] = blankLine();
        }
        lastPrinted.reset();
    }

    void scrollDown(int n) {
        const int a = activeScreen();
        const int top = scrollTop();
        const int bot = scrollBottom();
        const int regionHeight = bot - top + 1;
        const int amount = std::clamp(n, 0, regionHeight);
        if (amount <= 0) {
            return;
        }
        auto& grid = screens[static_cast<size_t>(a)];
        std::move_backward(grid.begin() + top,
                           grid.begin() + bot + 1 - amount,
                           grid.begin() + bot + 1);
        for (int r = top; r < top + amount; ++r) {
            grid[static_cast<size_t>(r)] = blankLine();
        }
        lastPrinted.reset();
    }

    // -------------------------------------------------------------------------
    // Erase / Insert / Delete
    // -------------------------------------------------------------------------

    void eraseInDisplay(int mode) {
        const int a = activeScreen();
        auto& grid = screens[static_cast<size_t>(a)];
        switch (mode) {
        case 0:
            eraseInLine(0);
            for (int r = cursorRow + 1; r < rows; ++r) {
                grid[static_cast<size_t>(r)] = blankLine();
            }
            break;
        case 1:
            for (int r = 0; r < cursorRow; ++r) {
                grid[static_cast<size_t>(r)] = blankLine();
            }
            eraseInLine(1);
            break;
        case 2:
            for (int r = 0; r < rows; ++r) {
                grid[static_cast<size_t>(r)] = blankLine();
            }
            break;
        case 3:
            clearScrollbackInternal();
            break;
        default:
            break;
        }
        pendingWrap = false;
        lastPrinted.reset();
    }

    void eraseInLine(int mode) {
        const int a = activeScreen();
        TermLine& line = screens[static_cast<size_t>(a)][static_cast<size_t>(cursorRow)];
        const TermCell blank = blankCell();
        switch (mode) {
        case 0:
            if (cursorCol > 0 && cursorCol < cols && line.cells[cursorCol].width == 0) {
                line.cells[cursorCol - 1] = blank;
            }
            for (int c = cursorCol; c < cols; ++c) {
                line.cells[c] = blank;
            }
            line.isWrapped = false;
            line.wrappedWidth = 0;
            break;
        case 1: {
            const int end = std::min(cursorCol, cols - 1);
            if (end + 1 < cols && line.cells[end].width == 2) {
                line.cells[end + 1] = blank;
            }
            for (int c = 0; c <= end; ++c) {
                line.cells[c] = blank;
            }
            break;
        }
        case 2:
            line = blankLine();
            break;
        default:
            break;
        }
        pendingWrap = false;
        lastPrinted.reset();
    }

    void eraseChars(int n) {
        const int a = activeScreen();
        TermLine& line = screens[static_cast<size_t>(a)][static_cast<size_t>(cursorRow)];
        const int end = std::min(cols, cursorCol + std::max(0, n));
        if (cursorCol >= end) {
            return;
        }
        const TermCell blank = blankCell();
        if (cursorCol > 0 && line.cells[cursorCol].width == 0) {
            line.cells[cursorCol - 1] = blank;
        }
        if (end < cols && line.cells[end - 1].width == 2) {
            line.cells[end] = blank;
        }
        for (int c = cursorCol; c < end; ++c) {
            line.cells[c] = blank;
        }
        pendingWrap = false;
        lastPrinted.reset();
    }

    void insertBlanks(int n) {
        const int a = activeScreen();
        TermLine& line = screens[static_cast<size_t>(a)][static_cast<size_t>(cursorRow)];
        const int count = std::min(std::max(0, n), cols - cursorCol);
        if (count <= 0) {
            return;
        }
        const TermCell blank = blankCell();
        if (cursorCol > 0 && line.cells[cursorCol].width == 0) {
            line.cells[cursorCol - 1] = blank;
            line.cells[cursorCol] = blank;
        }
        for (int c = cols - 1; c >= cursorCol + count; --c) {
            line.cells[c] = line.cells[c - count];
        }
        for (int c = cursorCol; c < cursorCol + count; ++c) {
            line.cells[c] = blank;
        }
        if (line.cells[cols - 1].width == 2) {
            line.cells[cols - 1] = blank;
        }
        line.isWrapped = false;
        line.wrappedWidth = 0;
        pendingWrap = false;
        lastPrinted.reset();
    }

    void deleteChars(int n) {
        const int a = activeScreen();
        TermLine& line = screens[static_cast<size_t>(a)][static_cast<size_t>(cursorRow)];
        const int count = std::min(std::max(0, n), cols - cursorCol);
        if (count <= 0) {
            return;
        }
        const TermCell blank = blankCell();
        if (cursorCol > 0 && line.cells[cursorCol].width == 0) {
            line.cells[cursorCol - 1] = blank;
        }
        if (cursorCol + count < cols && line.cells[cursorCol + count].width == 0) {
            line.cells[cursorCol + count] = blank;
        }
        for (int c = cursorCol; c < cols - count; ++c) {
            line.cells[c] = line.cells[c + count];
        }
        for (int c = cols - count; c < cols; ++c) {
            line.cells[c] = blank;
        }
        line.isWrapped = false;
        line.wrappedWidth = 0;
        pendingWrap = false;
        lastPrinted.reset();
    }

    void insertLines(int n) {
        const int top = scrollTop();
        const int bot = scrollBottom();
        if (cursorRow < top || cursorRow > bot) {
            return;
        }
        const int amount = std::clamp(n, 0, bot - cursorRow + 1);
        if (amount <= 0) {
            return;
        }
        auto& grid = screens[static_cast<size_t>(activeScreen())];
        std::move_backward(grid.begin() + cursorRow,
                           grid.begin() + bot + 1 - amount,
                           grid.begin() + bot + 1);
        for (int r = cursorRow; r < cursorRow + amount; ++r) {
            grid[static_cast<size_t>(r)] = blankLine();
        }
        cursorCol = 0;
        pendingWrap = false;
        lastPrinted.reset();
    }

    void deleteLines(int n) {
        const int top = scrollTop();
        const int bot = scrollBottom();
        if (cursorRow < top || cursorRow > bot) {
            return;
        }
        const int amount = std::clamp(n, 0, bot - cursorRow + 1);
        if (amount <= 0) {
            return;
        }
        auto& grid = screens[static_cast<size_t>(activeScreen())];
        std::move(grid.begin() + cursorRow + amount,
                  grid.begin() + bot + 1,
                  grid.begin() + cursorRow);
        for (int r = bot - amount + 1; r <= bot; ++r) {
            grid[static_cast<size_t>(r)] = blankLine();
        }
        cursorCol = 0;
        pendingWrap = false;
        lastPrinted.reset();
    }

    // -------------------------------------------------------------------------
    // Cursor save/restore & primary/alternate buffer switch
    // -------------------------------------------------------------------------

    void saveCursor() noexcept {
        savedCursors[static_cast<size_t>(activeScreen())] = SavedCursor{
            cursorRow, cursorCol, style, pendingWrap, modes.originMode
        };
    }

    void restoreCursor() noexcept {
        const auto& saved = savedCursors[static_cast<size_t>(activeScreen())];
        if (!saved.has_value()) {
            setCursor(0, 0);
            return;
        }
        cursorRow = std::clamp(saved->row, 0, rows - 1);
        cursorCol = std::clamp(saved->col, 0, cols - 1);
        style = saved->style;
        pendingWrap = saved->pendingWrap;
        modes.originMode = saved->originMode;
        lastPrinted.reset();
    }

    void switchScreen(bool alternate, bool saveRestoreCursor, bool clearAlt) {
        if (alternate == modes.isAlternateScreen) {
            return;
        }
        if (alternate) {
            if (saveRestoreCursor) {
                savedCursors[0] = SavedCursor{
                    cursorRow, cursorCol, style, pendingWrap, modes.originMode
                };
            }
            modes.isAlternateScreen = true;
            scrollRegions[1] = {0, rows - 1};
            if (clearAlt) {
                for (int r = 0; r < rows; ++r) {
                    screens[1][static_cast<size_t>(r)] = blankLine();
                }
            }
        } else {
            modes.isAlternateScreen = false;
            if (saveRestoreCursor && savedCursors[0].has_value()) {
                const SavedCursor& saved = *savedCursors[0];
                cursorRow = std::clamp(saved.row, 0, rows - 1);
                cursorCol = std::clamp(saved.col, 0, cols - 1);
                style = saved.style;
                pendingWrap = saved.pendingWrap;
                modes.originMode = saved.originMode;
            }
        }
        lastPrinted.reset();
    }

    // -------------------------------------------------------------------------
    // Tab stop helpers
    // -------------------------------------------------------------------------

    int nextTabStop(int col) const noexcept {
        for (int c = col + 1; c < cols - 1; ++c) {
            if (tabStops[static_cast<size_t>(c)]) {
                return c;
            }
        }
        return cols - 1;
    }

    int previousTabStop(int col) const noexcept {
        for (int c = col - 1; c > 0; --c) {
            if (tabStops[static_cast<size_t>(c)]) {
                return c;
            }
        }
        return 0;
    }
};

// =============================================================================
// SpellTerminalEmulator public API
// =============================================================================

SpellTerminalEmulator::SpellTerminalEmulator(int cols, int rows, int maxScrollback)
    : m_impl(std::make_unique<Impl>(cols, rows, maxScrollback)) {}

SpellTerminalEmulator::~SpellTerminalEmulator() = default;

SpellTerminalEmulator::SpellTerminalEmulator(SpellTerminalEmulator&& other) noexcept
    : onResponseBytes(std::move(other.onResponseBytes))
    , m_impl(std::move(other.m_impl)) {}

SpellTerminalEmulator& SpellTerminalEmulator::operator=(SpellTerminalEmulator&& other) noexcept {
    if (this != &other) {
        m_impl = std::move(other.m_impl);
        onResponseBytes.swap(other.onResponseBytes);
        other.onResponseBytes = nullptr;
    }
    return *this;
}

void SpellTerminalEmulator::feed(const QByteArray& bytes) {
    if (!m_impl) {
        return;
    }
    m_impl->feed(bytes, onResponseBytes);
}

void SpellTerminalEmulator::resize(int cols, int rows) {
    if (!m_impl) {
        return;
    }
    m_impl->resize(cols, rows);
}

void SpellTerminalEmulator::clear() {
    if (!m_impl) {
        return;
    }
    m_impl->clear();
}

void SpellTerminalEmulator::clearScrollback() {
    if (!m_impl) {
        return;
    }
    m_impl->clearScrollbackInternal();
    ++m_impl->generation;
}

void SpellTerminalEmulator::reset() {
    if (!m_impl) {
        return;
    }
    m_impl->fullReset();
    ++m_impl->generation;
}

int SpellTerminalEmulator::cols() const {
    return m_impl ? m_impl->cols : 0;
}

int SpellTerminalEmulator::rows() const {
    return m_impl ? m_impl->rows : 0;
}

int SpellTerminalEmulator::scrollbackCount() const {
    return m_impl ? static_cast<int>(m_impl->scrollback.size()) : 0;
}

int SpellTerminalEmulator::maxScrollback() const {
    return m_impl ? m_impl->maxScrollback : 0;
}

int SpellTerminalEmulator::totalLineCount() const {
    return m_impl ? m_impl->totalLineCount() : 0;
}

int SpellTerminalEmulator::droppedScrollbackLines() const {
    return m_impl ? m_impl->droppedScrollbackLines : 0;
}

int SpellTerminalEmulator::cursorRow() const {
    return m_impl ? m_impl->cursorRow : 0;
}

int SpellTerminalEmulator::cursorCol() const {
    return m_impl ? m_impl->cursorCol : 0;
}

bool SpellTerminalEmulator::cursorVisible() const {
    return m_impl ? m_impl->modes.cursorVisible : true;
}

bool SpellTerminalEmulator::applicationCursorKeys() const {
    return m_impl ? m_impl->modes.applicationCursorKeys : false;
}

bool SpellTerminalEmulator::applicationKeypad() const {
    return m_impl ? m_impl->modes.applicationKeypad : false;
}

bool SpellTerminalEmulator::bracketedPasteMode() const {
    return m_impl ? m_impl->modes.bracketedPaste : false;
}

bool SpellTerminalEmulator::isAlternateScreen() const {
    return m_impl ? m_impl->modes.isAlternateScreen : false;
}

bool SpellTerminalEmulator::mouseReporting() const {
    return m_impl ? m_impl->modes.mouseReporting : false;
}

uint64_t SpellTerminalEmulator::bellCount() const {
    return m_impl ? m_impl->bellCount : 0;
}

uint64_t SpellTerminalEmulator::generation() const {
    return m_impl ? m_impl->generation : 0;
}

const QVector<TermCell>& SpellTerminalEmulator::lineAt(int historyOffsetFromTop) const {
    static const QVector<TermCell> kEmpty;
    if (!m_impl) {
        return kEmpty;
    }
    const TermLine* line = m_impl->termLineAt(historyOffsetFromTop);
    return line ? line->cells : kEmpty;
}

bool SpellTerminalEmulator::isLineWrapped(int historyOffsetFromTop) const {
    if (!m_impl) {
        return false;
    }
    const TermLine* line = m_impl->termLineAt(historyOffsetFromTop);
    return line ? line->isWrapped : false;
}

QString SpellTerminalEmulator::screenText() const {
    if (!m_impl) {
        return {};
    }
    const auto& screen = m_impl->screens[static_cast<size_t>(m_impl->activeScreen())];
    QString out;
    out.reserve(static_cast<qsizetype>(m_impl->rows) * (m_impl->cols + 1));
    for (int r = 0; r < m_impl->rows; ++r) {
        if (r > 0) {
            out.append(QChar(u'\n'));
        }
        out.append(screen[static_cast<size_t>(r)].visibleText(/*trimTrailing=*/true));
    }
    return out;
}

QString SpellTerminalEmulator::recentText(int maxLines) const {
    if (!m_impl || maxLines <= 0) {
        return {};
    }
    const int total = m_impl->totalLineCount();
    if (total <= 0) {
        return {};
    }

    // Walk backward from the bottom to skip trailing blank lines.
    int lastNonEmpty = total - 1;
    while (lastNonEmpty >= 0) {
        const TermLine* line = m_impl->termLineAt(lastNonEmpty);
        if (line && lineHasVisibleNonSpace(line->cells)) {
            break;
        }
        --lastNonEmpty;
    }
    if (lastNonEmpty < 0) {
        return {};
    }

    // Walk backward across soft-wrap chains to locate the starting row of up to maxLines logical lines.
    int startRow = lastNonEmpty;
    int logicalCount = 0;
    for (int idx = lastNonEmpty; idx >= 0; --idx) {
        const TermLine* prev = (idx > 0) ? m_impl->termLineAt(idx - 1) : nullptr;
        if (!prev || !prev->isWrapped) {
            startRow = idx;
            ++logicalCount;
            if (logicalCount >= maxLines) {
                break;
            }
        }
    }

    std::vector<QString> logical;
    logical.reserve(static_cast<size_t>(std::min(maxLines, lastNonEmpty - startRow + 1)));
    QString carry;
    for (int idx = startRow; idx <= lastNonEmpty; ++idx) {
        const TermLine* line = m_impl->termLineAt(idx);
        if (!line) {
            continue;
        }
        carry.append(line->visibleText(/*trimTrailing=*/!line->isWrapped));
        if (!line->isWrapped) {
            trimTrailingSpaces(carry);
            logical.push_back(std::move(carry));
            carry.clear();
        }
    }
    if (!carry.isEmpty()) {
        trimTrailingSpaces(carry);
        logical.push_back(std::move(carry));
    }

    while (!logical.empty() && logical.back().isEmpty()) {
        logical.pop_back();
    }
    if (logical.empty()) {
        return {};
    }

    const size_t first = (static_cast<int>(logical.size()) > maxLines)
        ? (logical.size() - static_cast<size_t>(maxLines))
        : 0u;
    QString result;
    for (size_t i = first; i < logical.size(); ++i) {
        if (i > first) {
            result.append(QChar(u'\n'));
        }
        result.append(logical[i]);
    }
    return result;
}

QString SpellTerminalEmulator::textRange(int r1, int c1, int r2, int c2) const {
    if (!m_impl) {
        return {};
    }
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        std::swap(r1, r2);
        std::swap(c1, c2);
    }

    const int total = m_impl->totalLineCount();
    if (total <= 0 || r2 < 0 || r1 >= total) {
        return {};
    }

    const int clampedStart = std::max(0, r1);
    const int clampedEnd = std::min(total - 1, r2);

    std::vector<QString> parts;
    parts.reserve(static_cast<size_t>(clampedEnd - clampedStart + 1));

    for (int row = clampedStart; row <= clampedEnd; ++row) {
        const TermLine* line = m_impl->termLineAt(row);
        if (!line) {
            continue;
        }
        const int cellCount = static_cast<int>(line->cells.size());
        const int lo = (row == r1) ? std::clamp(c1, 0, cellCount) : 0;
        int hi = (row == r2) ? std::clamp(c2, lo, cellCount) : cellCount;
        if (row != r2 && line->isWrapped && line->wrappedWidth > 0) {
            hi = std::min(hi, line->wrappedWidth);
        }

        QString segment;
        if (lo < hi) {
            segment.reserve(hi - lo);
            for (int c = lo; c < hi; ++c) {
                if (line->cells[c].width != 0) {
                    appendCodepoint(segment, line->cells[c].codepoint);
                }
            }
        }
        if ((row != r2 || hi >= cellCount) && !line->isWrapped) {
            trimTrailingSpaces(segment);
        }

        const TermLine* prev = (row > clampedStart) ? m_impl->termLineAt(row - 1) : nullptr;
        if (prev && prev->isWrapped && !parts.empty()) {
            parts.back().append(segment);
        } else {
            parts.push_back(std::move(segment));
        }
    }

    QString out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out.append(QChar(u'\n'));
        }
        out.append(parts[i]);
    }
    return out;
}

QString SpellTerminalEmulator::title() const {
    return m_impl ? m_impl->title : QString();
}

QString SpellTerminalEmulator::reportedWorkingDirectory() const {
    return m_impl ? m_impl->reportedWorkingDirectory : QString();
}

int SpellTerminalEmulator::displayWidth(uint32_t cp) noexcept {
    // Control characters and NUL have zero display width.
    if (cp == 0u || cp < 0x20u || (cp >= 0x7Fu && cp < 0xA0u)) {
        return 0;
    }
    if (cp < 0x0300u) {
        return 1;
    }
    if (cp > 0x10FFFFu) {
        return 1;
    }

    // Explicit zero-width format characters, variation selectors, and Hangul Jamo medial/final.
    if ((cp >= 0x1160u && cp <= 0x11FFu) ||
        (cp >= 0x200Bu && cp <= 0x200Fu) ||
        (cp >= 0x2028u && cp <= 0x202Eu) ||
        (cp >= 0x2060u && cp <= 0x206Fu) ||
        (cp >= 0xFE00u && cp <= 0xFE0Fu) ||
        cp == 0xFEFFu ||
        (cp >= 0xE0100u && cp <= 0xE01EFu)) {
        return 0;
    }

    // Unicode combining marks (nonspacing & enclosing).
    const QChar::Category cat = QChar::category(static_cast<char32_t>(cp));
    if (cat == QChar::Mark_NonSpacing || cat == QChar::Mark_Enclosing) {
        return 0;
    }

    // East Asian Wide / Fullwidth and Emoji Presentation ranges.
    if ((cp >= 0x1100u && cp <= 0x115Fu) ||
        (cp >= 0x231Au && cp <= 0x231Bu) ||
        (cp >= 0x2329u && cp <= 0x232Au) ||
        (cp >= 0x23E9u && cp <= 0x23ECu) ||
        cp == 0x23F0u || cp == 0x23F3u ||
        (cp >= 0x25FDu && cp <= 0x25FEu) ||
        (cp >= 0x2614u && cp <= 0x2615u) ||
        (cp >= 0x2648u && cp <= 0x2653u) ||
        cp == 0x267Fu || cp == 0x2693u || cp == 0x26A1u ||
        (cp >= 0x26AAu && cp <= 0x26ABu) ||
        (cp >= 0x26BDu && cp <= 0x26BEu) ||
        (cp >= 0x26C4u && cp <= 0x26C5u) ||
        cp == 0x26CEu || cp == 0x26D4u || cp == 0x26EAu ||
        (cp >= 0x26F2u && cp <= 0x26F3u) ||
        cp == 0x26F5u || cp == 0x26FAu || cp == 0x26FDu ||
        cp == 0x2705u ||
        (cp >= 0x270Au && cp <= 0x270Bu) ||
        cp == 0x2728u || cp == 0x274Cu || cp == 0x274Eu ||
        (cp >= 0x2753u && cp <= 0x2755u) ||
        cp == 0x2757u ||
        (cp >= 0x2795u && cp <= 0x2797u) ||
        cp == 0x27B0u || cp == 0x27BFu ||
        (cp >= 0x2B1Bu && cp <= 0x2B1Cu) ||
        cp == 0x2B50u || cp == 0x2B55u ||
        (cp >= 0x2E80u && cp <= 0x303Eu) ||
        (cp >= 0x3041u && cp <= 0x33FFu) ||
        (cp >= 0x3400u && cp <= 0x4DBFu) ||
        (cp >= 0x4E00u && cp <= 0x9FFFu) ||
        (cp >= 0xA000u && cp <= 0xA4CFu) ||
        (cp >= 0xA960u && cp <= 0xA97Cu) ||
        (cp >= 0xAC00u && cp <= 0xD7A3u) ||
        (cp >= 0xF900u && cp <= 0xFAFFu) ||
        (cp >= 0xFE10u && cp <= 0xFE19u) ||
        (cp >= 0xFE30u && cp <= 0xFE6Bu) ||
        (cp >= 0xFF00u && cp <= 0xFF60u) ||
        (cp >= 0xFFE0u && cp <= 0xFFE6u) ||
        (cp >= 0x16FE0u && cp <= 0x16FE4u) ||
        (cp >= 0x16FF0u && cp <= 0x16FF1u) ||
        (cp >= 0x17000u && cp <= 0x187F7u) ||
        (cp >= 0x18800u && cp <= 0x18CD5u) ||
        (cp >= 0x18D00u && cp <= 0x18D08u) ||
        (cp >= 0x1AFF0u && cp <= 0x1B2FBu) ||
        cp == 0x1F004u || cp == 0x1F0CFu || cp == 0x1F18Eu ||
        (cp >= 0x1F191u && cp <= 0x1F19Au) ||
        (cp >= 0x1F1E6u && cp <= 0x1F1FFu) ||
        (cp >= 0x1F200u && cp <= 0x1F2FFu) ||
        (cp >= 0x1F300u && cp <= 0x1F64Fu) ||
        (cp >= 0x1F680u && cp <= 0x1F6FFu) ||
        (cp >= 0x1F7E0u && cp <= 0x1F7EBu) ||
        cp == 0x1F7F0u ||
        (cp >= 0x1F900u && cp <= 0x1F9FFu) ||
        (cp >= 0x1FA70u && cp <= 0x1FAFFu) ||
        (cp >= 0x20000u && cp <= 0x2FFFDu) ||
        (cp >= 0x30000u && cp <= 0x3FFFDu)) {
        return 2;
    }

    return 1;
}

} // namespace spell
