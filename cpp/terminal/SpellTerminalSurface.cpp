#include "SpellTerminalSurface.h"
#include "shell/WindowChrome.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHideEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QOperatingSystemVersion>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace spell {

namespace {

constexpr qreal kInsetLeft = 10.0;
constexpr qreal kInsetRight = 14.0;
constexpr qreal kInsetTop = 8.0;
constexpr qreal kInsetBottom = 8.0;
constexpr int kScrollBarWidth = 8;
constexpr int kScrollBarMargin = 2;
constexpr int kResizeDebounceMs = 40;
constexpr int kCursorBlinkMs = 600;
constexpr int kDefaultCols = 80;
constexpr int kDefaultRows = 24;
constexpr int kDefaultMaxScrollback = 4000;

struct GridPoint {
    int64_t absLine = 0;
    int col = 0;

    bool operator==(const GridPoint&) const = default;
    bool operator<(const GridPoint& other) const noexcept {
        return absLine != other.absLine ? absLine < other.absLine : col < other.col;
    }
};

enum class SelectionMode {
    Char,
    Word,
    Line
};

struct CellRenderStyle {
    TermColor fg;
    TermColor bg;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool inverse = false;
    bool dim = false;
    bool strikethrough = false;

    bool operator==(const CellRenderStyle&) const = default;
};

CellRenderStyle styleOfCell(const TermCell& cell) noexcept {
    return CellRenderStyle{
        cell.fg,
        cell.bg,
        cell.bold,
        cell.italic,
        cell.underline,
        cell.inverse,
        cell.dim,
        cell.strikethrough
    };
}

struct ResolvedCellColors {
    QColor fg;
    QColor bg;
};

ResolvedCellColors resolveCellColors(const CellRenderStyle& style, const TerminalPalette& palette) {
    QColor fg = (style.bold && style.fg.kind == TermColor::DefaultFg)
        ? palette.bold
        : SpellTerminalSurface::resolveColor(style.fg, palette, true);
    QColor bg = SpellTerminalSurface::resolveColor(style.bg, palette, false);
    if (style.inverse) {
        std::swap(fg, bg);
    }
    if (style.dim) {
        fg.setAlphaF(static_cast<float>(fg.alphaF() * 0.55));
    }
    return {fg, bg};
}

QString codepointToQString(uint32_t cp) {
    if (cp <= 0xFFFFu) {
        return QString(QChar(static_cast<char16_t>(cp)));
    }
    const char32_t ucs4 = static_cast<char32_t>(cp);
    return QString::fromUcs4(&ucs4, 1);
}

bool isWordCodepoint(uint32_t cp) {
    if (cp <= 0xFFFFu) {
        const QChar ch(static_cast<char16_t>(cp));
        if (ch.isLetterOrNumber()) {
            return true;
        }
        switch (cp) {
        case U'_':
        case U'-':
        case U'.':
        case U'/':
        case U'~':
        case U':':
        case U'@':
        case U'%':
        case U'+':
            return true;
        default:
            return false;
        }
    }
    return QChar::isLetterOrNumber(static_cast<char32_t>(cp));
}

bool isMacSwappedControlMeta() {
    return QOperatingSystemVersion::currentType() == QOperatingSystemVersion::MacOS
        && !QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
}

std::optional<char> controlByteForQtKey(int qtKey, const QString& text) {
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        return static_cast<char>(qtKey - Qt::Key_A + 1);
    }
    switch (qtKey) {
    case Qt::Key_At:
    case Qt::Key_Space:
    case Qt::Key_2:
        return '\x00';
    case Qt::Key_BracketLeft:
    case Qt::Key_3:
        return '\x1b';
    case Qt::Key_Backslash:
    case Qt::Key_4:
        return '\x1c';
    case Qt::Key_BracketRight:
    case Qt::Key_5:
        return '\x1d';
    case Qt::Key_AsciiCircum:
    case Qt::Key_6:
        return '\x1e';
    case Qt::Key_Underscore:
    case Qt::Key_Minus:
    case Qt::Key_7:
    case Qt::Key_Slash:
        return '\x1f';
    case Qt::Key_8:
    case Qt::Key_Question:
        return '\x7f';
    default:
        break;
    }

    if (text.size() == 1) {
        const char16_t u = text.at(0).unicode();
        if (u >= u'a' && u <= u'z') {
            return static_cast<char>(u - u'a' + 1);
        }
        if (u >= u'A' && u <= u'Z') {
            return static_cast<char>(u - u'A' + 1);
        }
        if (u == u'@' || u == u' ' || u == u'2') {
            return '\x00';
        }
        if (u == u'[' || u == u'3') {
            return '\x1b';
        }
        if (u == u'\\' || u == u'4') {
            return '\x1c';
        }
        if (u == u']' || u == u'5') {
            return '\x1d';
        }
        if (u == u'^' || u == u'6') {
            return '\x1e';
        }
        if (u == u'_' || u == u'-' || u == u'7' || u == u'/') {
            return '\x1f';
        }
        if (u == u'8' || u == u'?') {
            return '\x7f';
        }
        if (u <= 0x1Fu || u == 0x7Fu) {
            return static_cast<char>(u);
        }
    }

    return std::nullopt;
}

bool isNavigationOrSpecialKey(int qtKey) noexcept {
    switch (qtKey) {
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
    case Qt::Key_Escape:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Delete:
    case Qt::Key_Insert:
        return true;
    default:
        return qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F12;
    }
}

bool isModifierOnlyKey(int qtKey) noexcept {
    switch (qtKey) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Meta:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_Hyper_L:
    case Qt::Key_Hyper_R:
    case Qt::Key_Direction_L:
    case Qt::Key_Direction_R:
        return true;
    default:
        return false;
    }
}

QString resolveExistingDirectory(const QString& requested, const QString& fallbackDir) {
    auto tryPath = [](const QString& raw) -> QString {
        if (raw.trimmed().isEmpty()) {
            return QString();
        }
        QString expanded = raw.trimmed();
        if (expanded == QStringLiteral("~")) {
            expanded = QDir::homePath();
        } else if (expanded.startsWith(QStringLiteral("~/"))) {
            expanded = QDir::homePath() + expanded.mid(1);
        }
        const QFileInfo info(expanded);
        if (!info.exists()) {
            return QString();
        }
        if (info.isDir()) {
            const QString canonical = info.canonicalFilePath();
            return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
        }
        return info.absolutePath();
    };

    if (const QString resolved = tryPath(requested); !resolved.isEmpty()) {
        return resolved;
    }
    if (const QString resolved = tryPath(fallbackDir); !resolved.isEmpty()) {
        return resolved;
    }
    return QDir::homePath();
}

/**
 * Tracks the exact command typed on the current shell input line so `#spell <instruction>`
 * can be intercepted cleanly on Enter without executing `#spell` in the shell.
 */
struct ShellLineTracker {
    QString line;
    bool reliable = true;

    void reset() {
        line.clear();
        reliable = true;
    }

    void invalidate() {
        reliable = false;
    }

    void recordText(const QString& text) {
        if (reliable) {
            line.append(text);
        }
    }

    void recordBackspace(bool unmodified) {
        if (unmodified && reliable && !line.isEmpty()) {
            line.chop(1);
        } else {
            reliable = false;
        }
    }

    void recordControl(int qtKey, const QString& text) {
        QChar ch;
        if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
            ch = QChar(static_cast<char16_t>(qtKey - Qt::Key_A + 'a'));
        } else if (text.size() == 1) {
            ch = text.at(0).toLower();
        }
        if (ch == QLatin1Char('u') || ch == QLatin1Char('c')) {
            reset();
        } else {
            reliable = false;
        }
    }

    void recordPaste(const QString& text) {
        if (text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r'))) {
            reliable = false;
        } else if (reliable) {
            line.append(text);
        }
    }

    std::optional<QString> spellInstruction() const {
        if (!reliable) {
            return std::nullopt;
        }
        const QString trimmed = line.trimmed();
        const QString lower = trimmed.toLower();
        if (lower == QStringLiteral("#spell") || lower.startsWith(QStringLiteral("#spell "))) {
            return trimmed.mid(6).trimmed();
        }
        return std::nullopt;
    }
};

} // namespace

// =============================================================================
// SpellTerminalSurface::Impl
// =============================================================================

struct SpellTerminalSurface::Impl {
    SpellTerminalSurface* owner = nullptr;
    std::unique_ptr<SpellPtyProcess> pty;
    std::unique_ptr<SpellTerminalEmulator> emulator;

    TerminalPalette palette;
    QFont regularFont;
    QFont boldFont;
    QFont italicFont;
    QFont boldItalicFont;

    qreal cellWidth = 8.0;
    qreal cellHeight = 16.0;
    qreal baselineOffset = 12.0;
    qreal underlineOffset = 14.0;
    qreal strikeOffset = 8.0;
    bool boldMatchesCellWidth = true;
    bool italicMatchesCellWidth = true;
    bool boldItalicMatchesCellWidth = true;

    int reportedCols = kDefaultCols;
    int reportedRows = kDefaultRows;
    int pendingCols = kDefaultCols;
    int pendingRows = kDefaultRows;
    bool hasPendingResize = false;

    int scrollOffset = 0;
    qreal scrollAccumulator = 0.0;
    bool updatingScrollBar = false;

    QString workingDir;
    QString terminalTitle;
    bool optionAsMeta = true;

    bool cursorBlinkOn = true;
    QTimer* blinkTimer = nullptr;
    QTimer* resizeTimer = nullptr;
    QScrollBar* scrollBar = nullptr;

    // Selection state (stored in absolute line coordinates so scrollback eviction keeps anchors stable)
    GridPoint selectionAnchor{};
    GridPoint selectionHead{};
    GridPoint wordAnchorStart{};
    GridPoint wordAnchorEnd{};
    bool hasSelectionBounds = false;
    bool selecting = false;
    SelectionMode selectionMode = SelectionMode::Char;
    QElapsedTimer lastDoubleClickTimer;
    QPoint lastDoubleClickPos;

    // IME preedit composition state
    QString preeditText;
    int preeditCursorPos = 0;

    // `#spell` command line interception tracker
    ShellLineTracker lineTracker;

    explicit Impl(SpellTerminalSurface* q)
        : owner(q)
        , pty(std::make_unique<SpellPtyProcess>(q))
        , emulator(std::make_unique<SpellTerminalEmulator>(kDefaultCols, kDefaultRows, kDefaultMaxScrollback))
        , palette(ThemeManager::instance().terminal()) {}

    int64_t baseDropped() const {
        return emulator->isAlternateScreen()
            ? 0
            : static_cast<int64_t>(emulator->droppedScrollbackLines());
    }

    int topHistoryIndex() const {
        if (emulator->isAlternateScreen()) {
            return 0;
        }
        const int sb = emulator->scrollbackCount();
        const int clamped = std::clamp(scrollOffset, 0, sb);
        return sb - clamped;
    }

    int cursorViewportRow() const {
        if (emulator->isAlternateScreen()) {
            return emulator->cursorRow();
        }
        return emulator->cursorRow() + scrollOffset;
    }

    void clampScrollOffset() {
        if (emulator->isAlternateScreen()) {
            scrollOffset = 0;
        } else {
            scrollOffset = std::clamp(scrollOffset, 0, emulator->scrollbackCount());
        }
    }

    void recomputeFontMetrics() {
        regularFont = ThemeManager::instance().terminalFont();
        regularFont.setStyleHint(QFont::Monospace);
        regularFont.setFixedPitch(true);
        regularFont.setKerning(false);

        boldFont = regularFont;
        boldFont.setBold(true);

        italicFont = regularFont;
        italicFont.setItalic(true);

        boldItalicFont = boldFont;
        boldItalicFont.setItalic(true);

        const QFontMetricsF fm(regularFont);
        qreal adv = fm.horizontalAdvance(QLatin1Char('M'));
        if (adv < 4.0) {
            adv = std::max<qreal>(4.0, fm.averageCharWidth());
        }
        cellWidth = std::max<qreal>(4.0, adv);

        const qreal rawHeight = std::ceil(fm.ascent() + fm.descent() + std::max<qreal>(0.0, fm.leading()));
        cellHeight = std::max<qreal>(10.0, std::ceil(rawHeight * 1.18));
        baselineOffset = (cellHeight - (fm.ascent() + fm.descent())) / 2.0 + fm.ascent();
        underlineOffset = std::min(cellHeight - 1.0, baselineOffset + std::max<qreal>(1.5, fm.underlinePos()));
        strikeOffset = baselineOffset - fm.ascent() * 0.34;

        boldMatchesCellWidth = std::abs(QFontMetricsF(boldFont).horizontalAdvance(QLatin1Char('M')) - cellWidth) < 0.05;
        italicMatchesCellWidth = std::abs(QFontMetricsF(italicFont).horizontalAdvance(QLatin1Char('M')) - cellWidth) < 0.05;
        boldItalicMatchesCellWidth = std::abs(QFontMetricsF(boldItalicFont).horizontalAdvance(QLatin1Char('M')) - cellWidth) < 0.05;
    }

    bool fontMatchesCellWidth(bool bold, bool italic) const noexcept {
        if (bold && italic) return boldItalicMatchesCellWidth;
        if (bold) return boldMatchesCellWidth;
        if (italic) return italicMatchesCellWidth;
        return true;
    }

    const QFont& fontForStyle(bool bold, bool italic) const noexcept {
        if (bold && italic) return boldItalicFont;
        if (bold) return boldFont;
        if (italic) return italicFont;
        return regularFont;
    }

    void styleScrollBar() {
        if (!scrollBar) {
            return;
        }
        const QColor handle = withAlpha(palette.foreground, 0.28);
        const QColor handleHover = withAlpha(palette.foreground, 0.48);
        scrollBar->setStyleSheet(QStringLiteral(
            "QScrollBar:vertical {"
            "  background: transparent;"
            "  width: %1px;"
            "  margin: 0px;"
            "}"
            "QScrollBar::handle:vertical {"
            "  background: %2;"
            "  min-height: 24px;"
            "  border-radius: 4px;"
            "}"
            "QScrollBar::handle:vertical:hover {"
            "  background: %3;"
            "}"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
            "  background: transparent;"
            "  border: none;"
            "  width: 0px;"
            "  height: 0px;"
            "}"
        ).arg(kScrollBarWidth).arg(qssColor(handle), qssColor(handleHover)));
    }

    void updateScrollBarGeometry() {
        if (!scrollBar) {
            return;
        }
        const int x = std::max(0, owner->width() - kScrollBarWidth - kScrollBarMargin);
        const int h = std::max(0, owner->height() - kScrollBarMargin * 2);
        scrollBar->setGeometry(x, kScrollBarMargin, kScrollBarWidth, h);
    }

    void updateScrollBar() {
        if (!scrollBar) {
            return;
        }
        clampScrollOffset();
        const int sb = emulator->isAlternateScreen() ? 0 : emulator->scrollbackCount();
        updatingScrollBar = true;
        if (sb <= 0) {
            scrollBar->setRange(0, 0);
            scrollBar->setValue(0);
            scrollBar->hide();
        } else {
            scrollBar->setRange(0, sb);
            scrollBar->setPageStep(std::max(1, emulator->rows()));
            scrollBar->setSingleStep(1);
            scrollBar->setValue(sb - scrollOffset);
            updateScrollBarGeometry();
            scrollBar->show();
        }
        updatingScrollBar = false;
    }

    std::pair<int, int> computeGridDimensions() const {
        const qreal availW = static_cast<qreal>(owner->width()) - kInsetLeft - kInsetRight;
        const qreal availH = static_cast<qreal>(owner->height()) - kInsetTop - kInsetBottom;
        if (availW <= 0.0 || availH <= 0.0 || cellWidth <= 0.0 || cellHeight <= 0.0) {
            return {reportedCols, reportedRows};
        }
        const int cols = std::max(2, static_cast<int>(std::floor(availW / cellWidth)));
        const int rows = std::max(2, static_cast<int>(std::floor(availH / cellHeight)));
        return {cols, rows};
    }

    void applyGridSize(int cols, int rows) {
        cols = std::max(2, cols);
        rows = std::max(2, rows);
        if (cols == reportedCols && rows == reportedRows) {
            return;
        }
        reportedCols = cols;
        reportedRows = rows;
        emulator->resize(cols, rows);
        clampScrollOffset();
        updateScrollBar();
        if (pty && pty->isRunning()) {
            pty->resize(cols, rows);
        }
        emit owner->gridSizeChanged(cols, rows);
        owner->update();
    }

    void scheduleGridResize(bool immediate) {
        const auto [cols, rows] = computeGridDimensions();
        if (cols == reportedCols && rows == reportedRows) {
            if (resizeTimer) {
                resizeTimer->stop();
            }
            hasPendingResize = false;
            return;
        }
        if (immediate) {
            if (resizeTimer) {
                resizeTimer->stop();
            }
            hasPendingResize = false;
            applyGridSize(cols, rows);
            return;
        }
        pendingCols = cols;
        pendingRows = rows;
        hasPendingResize = true;
        if (resizeTimer) {
            resizeTimer->start(kResizeDebounceMs);
        }
    }

    QRect cursorPaintRect() const {
        const int visRows = emulator->rows();
        const int visCols = emulator->cols();
        const int curRow = cursorViewportRow();
        if (curRow < 0 || curRow >= visRows || visCols <= 0) {
            return QRect();
        }
        const int curCol = std::clamp(emulator->cursorCol(), 0, visCols - 1);
        const qreal x = kInsetLeft + curCol * cellWidth;
        const qreal y = kInsetTop + curRow * cellHeight;
        return QRectF(x - 1.0, y - 1.0, cellWidth * 2.0 + 2.0, cellHeight + 2.0).toAlignedRect();
    }

    std::optional<std::pair<GridPoint, GridPoint>> normalizedSelection() const {
        if (!hasSelectionBounds || selectionAnchor == selectionHead) {
            return std::nullopt;
        }
        if (selectionAnchor < selectionHead) {
            return std::make_pair(selectionAnchor, selectionHead);
        }
        return std::make_pair(selectionHead, selectionAnchor);
    }

    GridPoint gridPointForPos(const QPointF& pos, bool roundColumn) const {
        const int visRows = std::max(1, emulator->rows());
        const int visCols = std::max(1, emulator->cols());
        const int row = std::clamp(
            static_cast<int>(std::floor((pos.y() - kInsetTop) / std::max<qreal>(1.0, cellHeight))),
            0, visRows - 1);
        const qreal colF = (pos.x() - kInsetLeft) / std::max<qreal>(1.0, cellWidth);
        const int col = std::clamp(
            roundColumn ? static_cast<int>(std::lround(colF))
                        : static_cast<int>(std::floor(colF)),
            0, visCols);
        return GridPoint{baseDropped() + topHistoryIndex() + row, col};
    }

    std::optional<std::pair<GridPoint, GridPoint>> wordBoundsAt(const GridPoint& pt) const {
        const int histIdx = static_cast<int>(pt.absLine - baseDropped());
        if (histIdx < 0 || histIdx >= emulator->totalLineCount()) {
            return std::nullopt;
        }
        const QVector<TermCell>& cells = emulator->lineAt(histIdx);
        if (cells.isEmpty()) {
            return std::nullopt;
        }
        int idx = std::clamp(pt.col, 0, static_cast<int>(cells.size()) - 1);
        if (cells[idx].width == 0 && idx > 0) {
            --idx;
        }
        if (isWordCodepoint(cells[idx].codepoint)) {
            int lo = idx;
            int hi = idx;
            while (lo > 0) {
                int prev = lo - 1;
                if (cells[prev].width == 0 && prev > 0) {
                    --prev;
                }
                if (!isWordCodepoint(cells[prev].codepoint)) {
                    break;
                }
                lo = prev;
            }
            while (hi + 1 < cells.size()) {
                int next = hi + 1;
                if (cells[next].width == 0) {
                    hi = next;
                    continue;
                }
                if (!isWordCodepoint(cells[next].codepoint)) {
                    break;
                }
                hi = next;
            }
            if (cells[hi].width == 2 && hi + 1 < cells.size() && cells[hi + 1].width == 0) {
                ++hi;
            }
            return std::make_pair(GridPoint{pt.absLine, lo}, GridPoint{pt.absLine, hi + 1});
        }
        if (cells[idx].codepoint > U' ') {
            const int endCol = (cells[idx].width == 2) ? idx + 2 : idx + 1;
            return std::make_pair(
                GridPoint{pt.absLine, idx},
                GridPoint{pt.absLine, std::min(endCol, static_cast<int>(cells.size()))});
        }
        return std::nullopt;
    }

    void selectWordAt(const GridPoint& pt) {
        if (const auto bounds = wordBoundsAt(pt)) {
            wordAnchorStart = bounds->first;
            wordAnchorEnd = bounds->second;
            selectionAnchor = wordAnchorStart;
            selectionHead = wordAnchorEnd;
            hasSelectionBounds = true;
        } else {
            hasSelectionBounds = false;
        }
    }

    void drawRun(QPainter& painter,
                 const QString& text,
                 const CellRenderStyle& style,
                 int startCol,
                 int spanCols,
                 qreal rowTopY) const {
        if (text.isEmpty() && !style.underline && !style.strikethrough) {
            return;
        }
        bool hasVisibleGlyph = false;
        for (const QChar ch : text) {
            if (ch != QLatin1Char(' ')) {
                hasVisibleGlyph = true;
                break;
            }
        }
        if (!hasVisibleGlyph && !style.underline && !style.strikethrough) {
            return;
        }

        const ResolvedCellColors colors = resolveCellColors(style, palette);
        const qreal x = kInsetLeft + startCol * cellWidth;
        const qreal baselineY = rowTopY + baselineOffset;

        painter.setPen(colors.fg);
        if (hasVisibleGlyph) {
            painter.setFont(fontForStyle(style.bold, style.italic));
            if (spanCols == static_cast<int>(text.size())
                && fontMatchesCellWidth(style.bold, style.italic)) {
                painter.drawText(QPointF(x, baselineY), text);
            } else {
                for (qsizetype i = 0; i < text.size(); ++i) {
                    const QChar ch = text.at(i);
                    if (ch != QLatin1Char(' ')) {
                        painter.drawText(QPointF(x + static_cast<qreal>(i) * cellWidth, baselineY),
                                         QString(ch));
                    }
                }
            }
        }

        if (style.underline || style.strikethrough) {
            QPen linePen(colors.fg, 1.0);
            linePen.setCosmetic(false);
            painter.setPen(linePen);
            const qreal xEnd = x + static_cast<qreal>(spanCols) * cellWidth;
            if (style.underline) {
                const qreal uY = rowTopY + underlineOffset;
                painter.drawLine(QPointF(x, uY), QPointF(xEnd, uY));
            }
            if (style.strikethrough) {
                const qreal sY = rowTopY + strikeOffset;
                painter.drawLine(QPointF(x, sY), QPointF(xEnd, sY));
            }
        }
    }

    void drawSingleCellGlyph(QPainter& painter,
                             const TermCell& cell,
                             int col,
                             qreal rowTopY) const {
        const CellRenderStyle style = styleOfCell(cell);
        const int spanCols = (cell.width == 2) ? 2 : 1;
        const bool isSpace = (cell.codepoint <= U' ');
        if (isSpace && !style.underline && !style.strikethrough) {
            return;
        }

        const ResolvedCellColors colors = resolveCellColors(style, palette);
        const qreal x = kInsetLeft + col * cellWidth;
        const qreal baselineY = rowTopY + baselineOffset;

        painter.setPen(colors.fg);
        if (!isSpace) {
            painter.setFont(fontForStyle(style.bold, style.italic));
            painter.drawText(QPointF(x, baselineY), codepointToQString(cell.codepoint));
        }

        if (style.underline || style.strikethrough) {
            QPen linePen(colors.fg, 1.0);
            linePen.setCosmetic(false);
            painter.setPen(linePen);
            const qreal xEnd = x + static_cast<qreal>(spanCols) * cellWidth;
            if (style.underline) {
                const qreal uY = rowTopY + underlineOffset;
                painter.drawLine(QPointF(x, uY), QPointF(xEnd, uY));
            }
            if (style.strikethrough) {
                const qreal sY = rowTopY + strikeOffset;
                painter.drawLine(QPointF(x, sY), QPointF(xEnd, sY));
            }
        }
    }

    void drawCursor(QPainter& painter) const {
        if (!emulator->cursorVisible() || !pty || !pty->isRunning()) {
            return;
        }
        const int visRows = emulator->rows();
        const int visCols = emulator->cols();
        const int curRow = cursorViewportRow();
        if (curRow < 0 || curRow >= visRows || visCols <= 0) {
            return;
        }

        const int curCol = std::clamp(emulator->cursorCol(), 0, visCols - 1);
        const int histIdx = topHistoryIndex() + curRow;
        const QVector<TermCell>& cells = emulator->lineAt(histIdx);
        const TermCell cell = (curCol < cells.size()) ? cells[curCol] : TermCell::blank();
        const int span = (cell.width == 2) ? 2 : 1;

        const QRectF rect(
            kInsetLeft + curCol * cellWidth,
            kInsetTop + curRow * cellHeight,
            static_cast<qreal>(span) * cellWidth,
            cellHeight);

        const bool focused = owner->hasFocus() && owner->isActiveWindow();
        if (focused) {
            if (!cursorBlinkOn) {
                return;
            }
            painter.fillRect(rect, palette.cursor);
            if (cell.codepoint > U' ') {
                painter.setFont(fontForStyle(cell.bold, cell.italic));
                painter.setPen(palette.cursorText);
                painter.drawText(QPointF(rect.left(), rect.top() + baselineOffset),
                                 codepointToQString(cell.codepoint));
            }
        } else {
            QPen outlinePen(withAlpha(palette.cursor, 0.8), 1.0);
            painter.setPen(outlinePen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }

    void drawPreedit(QPainter& painter) const {
        const int visRows = emulator->rows();
        const int visCols = emulator->cols();
        const int curRow = std::clamp(cursorViewportRow(), 0, std::max(0, visRows - 1));
        const int curCol = std::clamp(emulator->cursorCol(), 0, std::max(0, visCols - 1));
        const qreal x = kInsetLeft + curCol * cellWidth;
        const qreal y = kInsetTop + curRow * cellHeight;

        const QFontMetricsF fm(regularFont);
        const qreal textWidth = std::max(cellWidth, fm.horizontalAdvance(preeditText));
        const QRectF bgRect(x, y, textWidth + 2.0, cellHeight);

        painter.fillRect(bgRect, palette.background);
        painter.setFont(regularFont);
        painter.setPen(palette.foreground);
        painter.drawText(QPointF(x, y + baselineOffset), preeditText);

        QPen underlinePen(palette.cursor, 1.0);
        painter.setPen(underlinePen);
        painter.drawLine(QPointF(x, y + underlineOffset), QPointF(x + textWidth, y + underlineOffset));

        const int clampedPos = std::clamp(preeditCursorPos, 0, static_cast<int>(preeditText.size()));
        const qreal caretX = x + fm.horizontalAdvance(preeditText.left(clampedPos));
        painter.fillRect(QRectF(caretX, y + 1.0, 1.5, std::max<qreal>(2.0, cellHeight - 2.0)),
                         palette.cursor);
    }
};

// =============================================================================
// SpellTerminalSurface — Construction & Destruction
// =============================================================================

SpellTerminalSurface::SpellTerminalSurface(QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>(this)) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    setCursor(Qt::IBeamCursor);
    setContextMenuPolicy(Qt::DefaultContextMenu);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Integrated vertical scrollback scrollbar
    m_impl->scrollBar = new QScrollBar(Qt::Vertical, this);
    m_impl->scrollBar->setFocusPolicy(Qt::NoFocus);
    m_impl->scrollBar->setCursor(Qt::ArrowCursor);
    m_impl->scrollBar->hide();
    connect(m_impl->scrollBar, &QScrollBar::valueChanged, this, [this](int value) {
        if (m_impl->updatingScrollBar || m_impl->emulator->isAlternateScreen()) {
            return;
        }
        const int sb = m_impl->emulator->scrollbackCount();
        const int nextOffset = std::clamp(sb - value, 0, sb);
        if (nextOffset != m_impl->scrollOffset) {
            m_impl->scrollOffset = nextOffset;
            update();
        }
    });

    // ~40 ms debounced resize timer so pane drags never spam SIGWINCH
    m_impl->resizeTimer = new QTimer(this);
    m_impl->resizeTimer->setSingleShot(true);
    m_impl->resizeTimer->setInterval(kResizeDebounceMs);
    connect(m_impl->resizeTimer, &QTimer::timeout, this, [this]() {
        if (!m_impl->hasPendingResize) {
            return;
        }
        m_impl->hasPendingResize = false;
        m_impl->applyGridSize(m_impl->pendingCols, m_impl->pendingRows);
    });

    // Focused cursor blink timer
    m_impl->blinkTimer = new QTimer(this);
    m_impl->blinkTimer->setInterval(kCursorBlinkMs);
    connect(m_impl->blinkTimer, &QTimer::timeout, this, [this]() {
        if (!hasFocus() || !isActiveWindow() || !isVisible() || !isShellRunning()
            || !m_impl->emulator->cursorVisible() || !m_impl->preeditText.isEmpty()) {
            return;
        }
        m_impl->cursorBlinkOn = !m_impl->cursorBlinkOn;
        const QRect r = m_impl->cursorPaintRect();
        if (r.isValid()) {
            update(r);
        } else {
            update();
        }
    });

    // Wire emulator device query responses (DSR/DA/XTWINOPS) directly back to the PTY
    m_impl->emulator->onResponseBytes = [this](const QByteArray& reply) {
        if (m_impl->pty && m_impl->pty->isRunning() && !reply.isEmpty()) {
            m_impl->pty->writeData(reply);
        }
    };

    // Wire PTY output stream into the VT emulator
    connect(m_impl->pty.get(), &SpellPtyProcess::dataReceived, this, [this](const QByteArray& chunk) {
        if (chunk.isEmpty()) {
            return;
        }
        auto& emu = *m_impl->emulator;
        const bool wasAlt = emu.isAlternateScreen();
        const int64_t prevTotalHist =
            static_cast<int64_t>(emu.droppedScrollbackLines()) + emu.scrollbackCount();
        const uint64_t prevBell = emu.bellCount();
        const QString prevTitle = emu.title();
        const QString prevReportedCwd = emu.reportedWorkingDirectory();

        emu.feed(chunk);

        const bool isAlt = emu.isAlternateScreen();
        if (wasAlt != isAlt) {
            clearSelection();
            if (isAlt) {
                m_impl->scrollOffset = 0;
            }
        } else if (!isAlt && m_impl->scrollOffset > 0) {
            const int64_t nowTotalHist =
                static_cast<int64_t>(emu.droppedScrollbackLines()) + emu.scrollbackCount();
            const int64_t delta = nowTotalHist - prevTotalHist;
            if (delta > 0) {
                m_impl->scrollOffset = std::min(
                    emu.scrollbackCount(),
                    m_impl->scrollOffset + static_cast<int>(delta));
            } else {
                m_impl->scrollOffset = std::min(m_impl->scrollOffset, emu.scrollbackCount());
            }
        }

        m_impl->updateScrollBar();

        const QString newTitle = emu.title();
        if (newTitle != prevTitle) {
            m_impl->terminalTitle = newTitle;
            emit titleChanged(newTitle);
        }

        const QString newReportedCwd = emu.reportedWorkingDirectory();
        if (!newReportedCwd.isEmpty()
            && newReportedCwd != prevReportedCwd
            && newReportedCwd != m_impl->workingDir) {
            m_impl->workingDir = newReportedCwd;
            emit workingDirectoryChanged(newReportedCwd);
        }

        if (emu.bellCount() != prevBell) {
            emit bellRung();
        }

        m_impl->cursorBlinkOn = true;
        if (m_impl->blinkTimer->isActive()) {
            m_impl->blinkTimer->start();
        }
        update();
    });

    // Wire PTY process exit
    connect(m_impl->pty.get(), &SpellPtyProcess::processExited, this, [this](int exitCode) {
        m_impl->lineTracker.reset();
        const QString notice = QStringLiteral(
            "\r\n\x1b[2m[process exited with status %1 — press Enter to restart]\x1b[0m\r\n")
            .arg(exitCode);
        m_impl->emulator->feed(notice.toUtf8());
        m_impl->updateScrollBar();
        update();
        emit shellExited(exitCode);
    });

    // Wire PTY startup/runtime errors into the terminal buffer for visibility
    connect(m_impl->pty.get(), &SpellPtyProcess::errorOccurred, this, [this](const QString& message) {
        if (message.isEmpty()) {
            return;
        }
        const QString notice = QStringLiteral("\r\n\x1b[31m[terminal error: %1]\x1b[0m\r\n").arg(message);
        m_impl->emulator->feed(notice.toUtf8());
        m_impl->updateScrollBar();
        update();
    });

    // React live to terminal theme and typography updates
    connect(&ThemeManager::instance(), &ThemeManager::terminalThemeChanged,
            this, &SpellTerminalSurface::applyTheme);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SpellTerminalSurface::applyTheme);

    applyTheme();
}

SpellTerminalSurface::~SpellTerminalSurface() {
    if (m_impl) {
        if (m_impl->resizeTimer) {
            m_impl->resizeTimer->stop();
        }
        if (m_impl->blinkTimer) {
            m_impl->blinkTimer->stop();
        }
        if (m_impl->emulator) {
            m_impl->emulator->onResponseBytes = nullptr;
        }
        if (m_impl->pty) {
            m_impl->pty->disconnect(this);
            if (m_impl->pty->isRunning()) {
                m_impl->pty->stop();
            }
        }
    }
}

// =============================================================================
// Shell Lifecycle & I/O
// =============================================================================

void SpellTerminalSurface::startShell(const QString& cwd) {
    if (isShellRunning()) {
        return;
    }

    // Sync grid size to current widget layout before spawning the PTY
    if (width() > static_cast<int>(kInsetLeft + kInsetRight)
        && height() > static_cast<int>(kInsetTop + kInsetBottom)) {
        m_impl->scheduleGridResize(true);
    }

    const QString targetDir = resolveExistingDirectory(cwd, m_impl->workingDir);
    const QString prevDir = m_impl->workingDir;

    const bool started = m_impl->pty->start(
        targetDir,
        QString(),
        m_impl->reportedCols,
        m_impl->reportedRows);

    if (!started) {
        return;
    }

    m_impl->lineTracker.reset();
    m_impl->scrollOffset = 0;
    m_impl->cursorBlinkOn = true;

    const QString actualDir = m_impl->pty->workingDirectory().isEmpty()
        ? targetDir
        : m_impl->pty->workingDirectory();
    m_impl->workingDir = actualDir;

    m_impl->updateScrollBar();
    if (hasFocus() && isVisible()) {
        m_impl->blinkTimer->start();
    }
    update();

    if (actualDir != prevDir) {
        emit workingDirectoryChanged(actualDir);
    }
    emit shellStarted(actualDir);
}

void SpellTerminalSurface::ensureShellRunning(const QString& cwd) {
    if (!isShellRunning()) {
        startShell(cwd);
    }
}

void SpellTerminalSurface::restartShell(const QString& cwd) {
    const QString targetDir = cwd.trimmed().isEmpty() ? m_impl->workingDir : cwd;
    stopShell();

    m_impl->emulator->reset();
    m_impl->scrollOffset = 0;
    m_impl->scrollAccumulator = 0.0;
    m_impl->lineTracker.reset();
    m_impl->preeditText.clear();
    m_impl->preeditCursorPos = 0;
    clearSelection();

    if (!m_impl->terminalTitle.isEmpty()) {
        m_impl->terminalTitle.clear();
        emit titleChanged(QString());
    }

    m_impl->updateScrollBar();
    update();

    startShell(targetDir);
}

void SpellTerminalSurface::stopShell() {
    if (m_impl->pty && m_impl->pty->isRunning()) {
        m_impl->pty->stop();
    }
    m_impl->lineTracker.reset();
    update();
}

void SpellTerminalSurface::sendCommandText(const QString& text, bool pressEnter) {
    if (!isShellRunning()) {
        return;
    }
    m_impl->lineTracker.reset();
    QByteArray payload = text.toUtf8();
    if (pressEnter) {
        payload.append('\r');
    }
    if (!payload.isEmpty()) {
        m_impl->pty->writeData(payload);
    }
    scrollToBottom();
}

void SpellTerminalSurface::sendRawBytes(const QByteArray& bytes) {
    if (!isShellRunning() || bytes.isEmpty()) {
        return;
    }
    m_impl->lineTracker.reset();
    m_impl->pty->writeData(bytes);
}

void SpellTerminalSurface::sendInterrupt() {
    if (!isShellRunning()) {
        return;
    }
    m_impl->lineTracker.reset();
    m_impl->pty->sendInterrupt();
    scrollToBottom();
}

void SpellTerminalSurface::pasteText(const QString& text) {
    if (!isShellRunning() || text.isEmpty()) {
        return;
    }
    clearSelection();
    if (m_impl->emulator->isAlternateScreen()) {
        m_impl->lineTracker.invalidate();
    } else {
        m_impl->lineTracker.recordPaste(text);
    }
    const QByteArray payload = encodeBracketedPaste(text, m_impl->emulator->bracketedPasteMode());
    if (!payload.isEmpty()) {
        m_impl->pty->writeData(payload);
    }
    scrollToBottom();
    m_impl->cursorBlinkOn = true;
    update();
}

// =============================================================================
// Inspection, Geometry & Selection
// =============================================================================

QString SpellTerminalSurface::recentOutputText(int maxLines) const {
    return m_impl->emulator->recentText(maxLines);
}

QString SpellTerminalSurface::selectedText() const {
    const auto norm = m_impl->normalizedSelection();
    if (!norm) {
        return QString();
    }
    const int total = m_impl->emulator->totalLineCount();
    if (total <= 0) {
        return QString();
    }
    const int64_t dropped = m_impl->baseDropped();
    int64_t r1 = norm->first.absLine - dropped;
    int c1 = norm->first.col;
    int64_t r2 = norm->second.absLine - dropped;
    int c2 = norm->second.col;

    if (r2 < 0 || r1 >= total) {
        return QString();
    }
    if (r1 < 0) {
        r1 = 0;
        c1 = 0;
    }
    if (r2 >= total) {
        r2 = total - 1;
        c2 = m_impl->emulator->cols();
    }
    return m_impl->emulator->textRange(static_cast<int>(r1), c1, static_cast<int>(r2), c2);
}

bool SpellTerminalSurface::hasSelection() const {
    const auto norm = m_impl->normalizedSelection();
    if (!norm) {
        return false;
    }
    const int total = m_impl->emulator->totalLineCount();
    if (total <= 0) {
        return false;
    }
    const int64_t dropped = m_impl->baseDropped();
    const int64_t r1 = norm->first.absLine - dropped;
    const int64_t r2 = norm->second.absLine - dropped;
    return r2 >= 0 && r1 < total;
}

bool SpellTerminalSurface::isShellRunning() const {
    return m_impl->pty && m_impl->pty->isRunning();
}

QString SpellTerminalSurface::workingDirectory() const {
    if (!m_impl->workingDir.isEmpty()) {
        return m_impl->workingDir;
    }
    return m_impl->pty ? m_impl->pty->workingDirectory() : QString();
}

QString SpellTerminalSurface::terminalTitle() const {
    return m_impl->terminalTitle;
}

int SpellTerminalSurface::gridCols() const {
    return m_impl->reportedCols;
}

int SpellTerminalSurface::gridRows() const {
    return m_impl->reportedRows;
}

int SpellTerminalSurface::scrollOffset() const {
    return m_impl->emulator->isAlternateScreen() ? 0 : m_impl->scrollOffset;
}

void SpellTerminalSurface::scrollByLines(int lines) {
    if (lines == 0) {
        return;
    }
    if (m_impl->emulator->isAlternateScreen()) {
        if (!isShellRunning()) {
            return;
        }
        const int key = (lines > 0) ? Qt::Key_Up : Qt::Key_Down;
        const QByteArray seq = encodeKeyEvent(
            key,
            Qt::NoModifier,
            QString(),
            m_impl->emulator->applicationCursorKeys(),
            m_impl->optionAsMeta);
        if (!seq.isEmpty()) {
            const int count = std::min(std::abs(lines), 20);
            QByteArray batch;
            batch.reserve(seq.size() * count);
            for (int i = 0; i < count; ++i) {
                batch.append(seq);
            }
            m_impl->pty->writeData(batch);
        }
        return;
    }

    const int maxOffset = m_impl->emulator->scrollbackCount();
    const int nextOffset = std::clamp(m_impl->scrollOffset + lines, 0, maxOffset);
    if (nextOffset != m_impl->scrollOffset) {
        m_impl->scrollOffset = nextOffset;
        m_impl->updateScrollBar();
        update();
    }
}

void SpellTerminalSurface::setOptionAsMeta(bool enabled) {
    m_impl->optionAsMeta = enabled;
}

bool SpellTerminalSurface::optionAsMeta() const {
    return m_impl->optionAsMeta;
}

SpellPtyProcess* SpellTerminalSurface::ptyProcess() const {
    return m_impl->pty.get();
}

const SpellTerminalEmulator* SpellTerminalSurface::emulator() const {
    return m_impl->emulator.get();
}

QSize SpellTerminalSurface::sizeHint() const {
    const int w = static_cast<int>(std::ceil(kDefaultCols * m_impl->cellWidth + kInsetLeft + kInsetRight));
    const int h = static_cast<int>(std::ceil(kDefaultRows * m_impl->cellHeight + kInsetTop + kInsetBottom));
    return QSize(std::max(320, w), std::max(180, h));
}

QSize SpellTerminalSurface::minimumSizeHint() const {
    return QSize(200, 100);
}

// =============================================================================
// Pure Static Color & VT Key Encoding Helpers
// =============================================================================

QColor SpellTerminalSurface::resolveColor(const TermColor& color,
                                          const TerminalPalette& palette,
                                          bool isForeground) {
    switch (color.kind) {
    case TermColor::DefaultFg:
        return palette.foreground;
    case TermColor::DefaultBg:
        return palette.background;
    case TermColor::Indexed: {
        const uint8_t idx = color.index;
        if (idx < 16u) {
            return palette.ansi[idx];
        }
        if (idx < 232u) {
            static constexpr int kCubeSteps[6] = {0, 95, 135, 175, 215, 255};
            const int v = static_cast<int>(idx) - 16;
            const int r = kCubeSteps[v / 36];
            const int g = kCubeSteps[(v / 6) % 6];
            const int b = kCubeSteps[v % 6];
            return QColor(r, g, b);
        }
        const int gray = 8 + (static_cast<int>(idx) - 232) * 10;
        return QColor(gray, gray, gray);
    }
    case TermColor::Rgb:
        return QColor(color.r, color.g, color.b);
    }
    return isForeground ? palette.foreground : palette.background;
}

QByteArray SpellTerminalSurface::encodeKeyEvent(int qtKey,
                                                Qt::KeyboardModifiers modifiers,
                                                const QString& text,
                                                bool applicationCursorKeys,
                                                bool optionAsMeta) {
    if (isModifierOnlyKey(qtKey)) {
        return QByteArray();
    }

    const bool hasShift = modifiers.testFlag(Qt::ShiftModifier);
    const bool hasAlt = modifiers.testFlag(Qt::AltModifier);
    const bool hasCtrl = modifiers.testFlag(Qt::ControlModifier);

    // xterm CSI modifier parameter: 1 + shift(1) + alt(2) + ctrl(4)
    int modParam = 1;
    if (hasShift) modParam += 1;
    if (hasAlt)   modParam += 2;
    if (hasCtrl)  modParam += 4;

    auto encodeCursor = [&](char finalChar) -> QByteArray {
        if (modParam > 1) {
            return QByteArray("\x1b[1;") + QByteArray::number(modParam) + finalChar;
        }
        return applicationCursorKeys
            ? QByteArray("\x1bO") + finalChar
            : QByteArray("\x1b[") + finalChar;
    };

    auto encodeTilde = [&](int code) -> QByteArray {
        if (modParam > 1) {
            return QByteArray("\x1b[") + QByteArray::number(code) + ';'
                + QByteArray::number(modParam) + '~';
        }
        return QByteArray("\x1b[") + QByteArray::number(code) + '~';
    };

    auto encodeF1ToF4 = [&](char finalChar) -> QByteArray {
        if (modParam > 1) {
            return QByteArray("\x1b[1;") + QByteArray::number(modParam) + finalChar;
        }
        return QByteArray("\x1bO") + finalChar;
    };

    switch (qtKey) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return (hasAlt && optionAsMeta) ? QByteArray("\x1b\r", 2) : QByteArray("\r", 1);
    case Qt::Key_Tab:
        if (hasShift) {
            return QByteArray("\x1b[Z");
        }
        return (hasAlt && optionAsMeta) ? QByteArray("\x1b\t", 2) : QByteArray("\t", 1);
    case Qt::Key_Backtab:
        return QByteArray("\x1b[Z");
    case Qt::Key_Backspace:
        if (hasCtrl) {
            return QByteArray("\x08", 1);
        }
        return (hasAlt && optionAsMeta) ? QByteArray("\x1b\x7f", 2) : QByteArray("\x7f", 1);
    case Qt::Key_Escape:
        return QByteArray("\x1b", 1);
    case Qt::Key_Up:
        return encodeCursor('A');
    case Qt::Key_Down:
        return encodeCursor('B');
    case Qt::Key_Right:
        return encodeCursor('C');
    case Qt::Key_Left:
        return encodeCursor('D');
    case Qt::Key_Home:
        return encodeCursor('H');
    case Qt::Key_End:
        return encodeCursor('F');
    case Qt::Key_Insert:
        return encodeTilde(2);
    case Qt::Key_Delete:
        return encodeTilde(3);
    case Qt::Key_PageUp:
        return encodeTilde(5);
    case Qt::Key_PageDown:
        return encodeTilde(6);
    case Qt::Key_F1:
        return encodeF1ToF4('P');
    case Qt::Key_F2:
        return encodeF1ToF4('Q');
    case Qt::Key_F3:
        return encodeF1ToF4('R');
    case Qt::Key_F4:
        return encodeF1ToF4('S');
    case Qt::Key_F5:
        return encodeTilde(15);
    case Qt::Key_F6:
        return encodeTilde(17);
    case Qt::Key_F7:
        return encodeTilde(18);
    case Qt::Key_F8:
        return encodeTilde(19);
    case Qt::Key_F9:
        return encodeTilde(20);
    case Qt::Key_F10:
        return encodeTilde(21);
    case Qt::Key_F11:
        return encodeTilde(23);
    case Qt::Key_F12:
        return encodeTilde(24);
    default:
        break;
    }

    if (hasCtrl) {
        if (const auto ctrl = controlByteForQtKey(qtKey, text)) {
            QByteArray out;
            if (hasAlt && optionAsMeta) {
                out.append('\x1b');
            }
            out.append(*ctrl);
            return out;
        }
        return QByteArray();
    }

    if (hasAlt && optionAsMeta) {
        QByteArray payload;
        if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
            const char ch = static_cast<char>(hasShift ? qtKey : (qtKey - Qt::Key_A + 'a'));
            payload.append(ch);
        } else if (text.size() == 1 && text.at(0).unicode() >= 0x20 && text.at(0).unicode() <= 0x7E) {
            payload = text.toUtf8();
        } else if (qtKey >= 0x20 && qtKey <= 0x7E) {
            payload.append(static_cast<char>(qtKey));
        } else if (!text.isEmpty()) {
            payload = text.toUtf8();
        }
        if (payload.isEmpty()) {
            return QByteArray();
        }
        QByteArray out("\x1b", 1);
        out.append(payload);
        return out;
    }

    if (!text.isEmpty()) {
        return text.toUtf8();
    }

    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        const char ch = static_cast<char>(hasShift ? qtKey : (qtKey - Qt::Key_A + 'a'));
        return QByteArray(1, ch);
    }
    if (qtKey >= 0x20 && qtKey <= 0x7E) {
        return QByteArray(1, static_cast<char>(qtKey));
    }

    return QByteArray();
}

QByteArray SpellTerminalSurface::encodeBracketedPaste(const QString& text, bool bracketedPasteMode) {
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
    normalized.replace(QLatin1Char('\n'), QLatin1Char('\r'));

    if (bracketedPasteMode) {
        const QString endMarker = QStringLiteral("\x1b[201~");
        const QString startMarker = QStringLiteral("\x1b[200~");
        while (normalized.contains(endMarker) || normalized.contains(startMarker)) {
            normalized.remove(endMarker);
            normalized.remove(startMarker);
        }
        QByteArray out("\x1b[200~");
        out.append(normalized.toUtf8());
        out.append("\x1b[201~");
        return out;
    }

    return normalized.toUtf8();
}

// =============================================================================
// Public Slots
// =============================================================================

void SpellTerminalSurface::clearBuffer() {
    m_impl->emulator->clear();
    m_impl->scrollOffset = 0;
    m_impl->scrollAccumulator = 0.0;
    clearSelection();
    m_impl->updateScrollBar();
    update();
}

void SpellTerminalSurface::applyTheme() {
    m_impl->palette = ThemeManager::instance().terminal();
    m_impl->recomputeFontMetrics();
    m_impl->styleScrollBar();
    m_impl->updateScrollBarGeometry();

    if (width() > static_cast<int>(kInsetLeft + kInsetRight)
        && height() > static_cast<int>(kInsetTop + kInsetBottom)) {
        m_impl->scheduleGridResize(true);
    }
    updateGeometry();
    update();
}

void SpellTerminalSurface::copySelection() const {
    const QString text = selectedText();
    if (text.isEmpty()) {
        return;
    }
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text, QClipboard::Clipboard);
    }
}

void SpellTerminalSurface::pasteFromClipboard() {
    if (const QClipboard* clipboard = QGuiApplication::clipboard()) {
        const QString text = clipboard->text(QClipboard::Clipboard);
        if (!text.isEmpty()) {
            pasteText(text);
        }
    }
}

void SpellTerminalSurface::selectAll() {
    const int total = m_impl->emulator->totalLineCount();
    const int cols = m_impl->emulator->cols();
    if (total <= 0 || cols <= 0) {
        return;
    }
    const int64_t dropped = m_impl->baseDropped();
    m_impl->selectionAnchor = GridPoint{dropped, 0};
    m_impl->selectionHead = GridPoint{dropped + total - 1, cols};
    m_impl->hasSelectionBounds = true;
    update();
}

void SpellTerminalSurface::clearSelection() {
    if (m_impl->hasSelectionBounds || m_impl->selecting) {
        m_impl->hasSelectionBounds = false;
        m_impl->selecting = false;
        update();
    }
}

void SpellTerminalSurface::scrollToBottom() {
    if (m_impl->scrollOffset != 0) {
        m_impl->scrollOffset = 0;
        m_impl->updateScrollBar();
        update();
    }
}

// =============================================================================
// Protected Event Overrides
// =============================================================================

bool SpellTerminalSurface::event(QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            keyPressEvent(ke);
            return true;
        }
    } else if (event->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(event);
        const int key = ke->key();
        const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;
        const bool macSwapped = isMacSwappedControlMeta();

        if (isTerminalWindowControlShortcut(key, mods)) {
            // Let the pop-out window's Close/Minimize shortcuts fire.
            return QWidget::event(event);
        }
        if (macSwapped) {
            if (mods == Qt::ControlModifier
                && (key == Qt::Key_C || key == Qt::Key_V || key == Qt::Key_A || key == Qt::Key_K)) {
                ke->accept();
                return true;
            }
            if (mods.testFlag(Qt::MetaModifier) && !mods.testFlag(Qt::ControlModifier)) {
                ke->accept();
                return true;
            }
            if (!mods.testFlag(Qt::ControlModifier)
                && (isNavigationOrSpecialKey(key) || key == Qt::Key_Backspace)) {
                ke->accept();
                return true;
            }
        } else {
            if (mods.testFlag(Qt::ControlModifier) && !mods.testFlag(Qt::MetaModifier)) {
                ke->accept();
                return true;
            }
            if (isNavigationOrSpecialKey(key) || key == Qt::Key_Backspace) {
                ke->accept();
                return true;
            }
        }
    } else if (event->type() == QEvent::WindowActivate) {
        m_impl->cursorBlinkOn = true;
        if (hasFocus() && isVisible() && m_impl->blinkTimer) {
            m_impl->blinkTimer->start();
        }
        update();
    } else if (event->type() == QEvent::WindowDeactivate) {
        if (m_impl->blinkTimer) {
            m_impl->blinkTimer->stop();
        }
        m_impl->cursorBlinkOn = true;
        update();
    }
    return QWidget::event(event);
}

void SpellTerminalSurface::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRect dirtyRect = event->rect();
    const TerminalPalette& pal = m_impl->palette;
    painter.fillRect(dirtyRect, pal.background);

    const auto& emu = *m_impl->emulator;
    const int visRows = emu.rows();
    const int visCols = emu.cols();
    const int topHist = m_impl->topHistoryIndex();
    const int64_t dropped = m_impl->baseDropped();
    const auto selection = m_impl->normalizedSelection();

    for (int row = 0; row < visRows; ++row) {
        const qreal y = kInsetTop + row * m_impl->cellHeight;
        const QRectF rowRect(0.0, y, static_cast<qreal>(width()), m_impl->cellHeight);
        if (!rowRect.intersects(QRectF(dirtyRect))) {
            continue;
        }

        const int histIdx = topHist + row;
        const QVector<TermCell>& cells = emu.lineAt(histIdx);
        const int cellCount = static_cast<int>(cells.size());
        const int64_t absLine = dropped + histIdx;

        // Pass 1: Coalesced non-default cell backgrounds (including inverse video)
        int bgRunStart = -1;
        QColor bgRunColor;
        auto flushBgRun = [&](int endCol) {
            if (bgRunStart >= 0 && endCol > bgRunStart) {
                painter.fillRect(
                    QRectF(kInsetLeft + bgRunStart * m_impl->cellWidth,
                           y,
                           static_cast<qreal>(endCol - bgRunStart) * m_impl->cellWidth,
                           m_impl->cellHeight),
                    bgRunColor);
            }
            bgRunStart = -1;
        };

        for (int c = 0; c < cellCount; ++c) {
            const TermCell& cell = cells[c];
            if (cell.width == 0 && c > 0 && cells[c - 1].width == 2) {
                continue;
            }
            const QColor bg = resolveCellColors(styleOfCell(cell), pal).bg;
            if (bg == pal.background) {
                flushBgRun(c);
            } else if (bgRunStart < 0 || bg != bgRunColor) {
                flushBgRun(c);
                bgRunStart = c;
                bgRunColor = bg;
            }
        }
        flushBgRun(cellCount);

        // Pass 2: Selection highlight overlay
        if (selection && absLine >= selection->first.absLine && absLine <= selection->second.absLine) {
            const int startCol = std::clamp(
                (absLine == selection->first.absLine) ? selection->first.col : 0,
                0, visCols);
            const int endCol = std::clamp(
                (absLine == selection->second.absLine) ? selection->second.col : visCols,
                0, visCols);
            if (endCol > startCol) {
                painter.fillRect(
                    QRectF(kInsetLeft + startCol * m_impl->cellWidth,
                           y,
                           static_cast<qreal>(endCol - startCol) * m_impl->cellWidth,
                           m_impl->cellHeight),
                    pal.selection);
            }
        }

        // Pass 3: Glyphs, underline & strikethrough
        // Coalesce contiguous single-width printable ASCII runs with identical style;
        // draw non-ASCII and wide glyphs per cell so fallback/wide fonts stay locked to the grid.
        int runStart = 0;
        QString runText;
        CellRenderStyle runStyle{};
        bool hasRun = false;

        auto flushTextRun = [&]() {
            if (hasRun && !runText.isEmpty()) {
                m_impl->drawRun(painter, runText, runStyle, runStart, static_cast<int>(runText.size()), y);
            }
            runText.clear();
            hasRun = false;
        };

        for (int c = 0; c < cellCount; ++c) {
            const TermCell& cell = cells[c];
            if (cell.width == 0) {
                continue;
            }
            const bool isAsciiSingleWidth =
                cell.width == 1 && cell.codepoint >= 0x20u && cell.codepoint <= 0x7Eu;
            if (isAsciiSingleWidth) {
                const CellRenderStyle st = styleOfCell(cell);
                if (hasRun && st == runStyle && runStart + static_cast<int>(runText.size()) == c) {
                    runText.append(QChar(static_cast<char16_t>(cell.codepoint)));
                } else {
                    flushTextRun();
                    runStart = c;
                    runStyle = st;
                    runText = QString(QChar(static_cast<char16_t>(cell.codepoint)));
                    hasRun = true;
                }
            } else {
                flushTextRun();
                m_impl->drawSingleCellGlyph(painter, cell, c, y);
            }
        }
        flushTextRun();
    }

    // Pass 4: Cursor block / outline or IME composition overlay
    if (m_impl->preeditText.isEmpty()) {
        m_impl->drawCursor(painter);
    } else {
        m_impl->drawPreedit(painter);
    }
}

void SpellTerminalSurface::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    m_impl->updateScrollBarGeometry();
    m_impl->scheduleGridResize(false);
    update();
}

void SpellTerminalSurface::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    const bool macSwapped = isMacSwappedControlMeta();

    // Clipboard & terminal buffer shortcuts
    if (macSwapped) {
        if (mods == Qt::ControlModifier) {
            if (key == Qt::Key_C) {
                if (hasSelection()) {
                    copySelection();
                }
                event->accept();
                return;
            }
            if (key == Qt::Key_V) {
                pasteFromClipboard();
                event->accept();
                return;
            }
            if (key == Qt::Key_A) {
                selectAll();
                event->accept();
                return;
            }
            if (key == Qt::Key_K) {
                clearBuffer();
                event->accept();
                return;
            }
        }
        if (mods.testFlag(Qt::ControlModifier)) {
            event->ignore();
            return;
        }
    } else {
        if (mods == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_C) {
            copySelection();
            event->accept();
            return;
        }
        if (mods == Qt::ControlModifier && key == Qt::Key_C && !selectedText().isEmpty()) {
            copySelection();
            event->accept();
            return;
        }
        if ((mods == Qt::ControlModifier || mods == (Qt::ControlModifier | Qt::ShiftModifier))
            && key == Qt::Key_V) {
            pasteFromClipboard();
            event->accept();
            return;
        }
        if (mods == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_A) {
            selectAll();
            event->accept();
            return;
        }
        if (mods == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_K) {
            clearBuffer();
            event->accept();
            return;
        }
        if (mods == Qt::ControlModifier && key == Qt::Key_Insert && hasSelection()) {
            copySelection();
            event->accept();
            return;
        }
        if (mods == Qt::ShiftModifier && key == Qt::Key_Insert) {
            pasteFromClipboard();
            event->accept();
            return;
        }
    }

    // Shift + paging keys scroll scrollback on the primary screen
    if (mods == Qt::ShiftModifier && !m_impl->emulator->isAlternateScreen()) {
        switch (key) {
        case Qt::Key_PageUp:
            scrollByLines(std::max(1, gridRows() - 1));
            event->accept();
            return;
        case Qt::Key_PageDown:
            scrollByLines(-std::max(1, gridRows() - 1));
            event->accept();
            return;
        case Qt::Key_Home:
            scrollByLines(m_impl->emulator->scrollbackCount());
            event->accept();
            return;
        case Qt::Key_End:
            scrollToBottom();
            event->accept();
            return;
        default:
            break;
        }
    }

    // Stopped shell: pressing Enter restarts it cleanly
    if (!isShellRunning()) {
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && mods == Qt::NoModifier) {
            restartShell();
        }
        event->accept();
        return;
    }

    // Translate OS modifiers into xterm terminal modifiers
    Qt::KeyboardModifiers termMods = Qt::NoModifier;
    if (mods.testFlag(Qt::ShiftModifier)) {
        termMods |= Qt::ShiftModifier;
    }
    if (mods.testFlag(Qt::AltModifier)) {
        termMods |= Qt::AltModifier;
    }
    if (macSwapped) {
        if (mods.testFlag(Qt::MetaModifier)) {
            termMods |= Qt::ControlModifier;
        }
    } else {
        if (mods.testFlag(Qt::ControlModifier)) {
            termMods |= Qt::ControlModifier;
        }
    }

    // Intercept `#spell <instruction>` typed on the primary shell input line
    if ((key == Qt::Key_Return || key == Qt::Key_Enter)
        && termMods == Qt::NoModifier
        && !m_impl->emulator->isAlternateScreen()) {
        if (const auto instruction = m_impl->lineTracker.spellInstruction()) {
            clearSelection();
            // Ctrl-U clears the typed `#spell ...` line in the shell instead of executing it
            m_impl->pty->writeData(QByteArray("\x15", 1));
            m_impl->lineTracker.reset();
            scrollToBottom();
            emit spellHandOffRequested(*instruction, recentOutputText(40), workingDirectory());
            event->accept();
            return;
        }
    }

    const QByteArray encoded = encodeKeyEvent(
        key,
        termMods,
        event->text(),
        m_impl->emulator->applicationCursorKeys(),
        m_impl->optionAsMeta);

    if (encoded.isEmpty()) {
        QWidget::keyPressEvent(event);
        return;
    }

    clearSelection();

    // Update `#spell` line tracker
    if (m_impl->emulator->isAlternateScreen()) {
        m_impl->lineTracker.invalidate();
    } else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        m_impl->lineTracker.reset();
    } else if (key == Qt::Key_Backspace) {
        m_impl->lineTracker.recordBackspace(termMods == Qt::NoModifier);
    } else if (termMods.testFlag(Qt::ControlModifier)) {
        m_impl->lineTracker.recordControl(key, event->text());
    } else if (termMods.testFlag(Qt::AltModifier) && m_impl->optionAsMeta) {
        m_impl->lineTracker.invalidate();
    } else if (isNavigationOrSpecialKey(key)) {
        m_impl->lineTracker.invalidate();
    } else {
        m_impl->lineTracker.recordText(QString::fromUtf8(encoded));
    }

    m_impl->pty->writeData(encoded);
    scrollToBottom();
    m_impl->cursorBlinkOn = true;
    if (m_impl->blinkTimer && m_impl->blinkTimer->isActive()) {
        m_impl->blinkTimer->start();
    }
    update();
    event->accept();
}

void SpellTerminalSurface::inputMethodEvent(QInputMethodEvent* event) {
    const QString commit = event->commitString();
    const QString preedit = event->preeditString();

    m_impl->preeditText = preedit;
    m_impl->preeditCursorPos = static_cast<int>(preedit.size());
    for (const QInputMethodEvent::Attribute& attr : event->attributes()) {
        if (attr.type == QInputMethodEvent::Cursor) {
            m_impl->preeditCursorPos = std::clamp(attr.start, 0, static_cast<int>(preedit.size()));
            break;
        }
    }

    if (!preedit.isEmpty()) {
        scrollToBottom();
    }

    if (!commit.isEmpty() && isShellRunning()) {
        clearSelection();
        if (m_impl->emulator->isAlternateScreen()) {
            m_impl->lineTracker.invalidate();
        } else {
            m_impl->lineTracker.recordText(commit);
        }
        m_impl->pty->writeData(commit.toUtf8());
        scrollToBottom();
        m_impl->cursorBlinkOn = true;
    }

    update();
    event->accept();
}

QVariant SpellTerminalSurface::inputMethodQuery(Qt::InputMethodQuery query) const {
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle: {
        const int visRows = std::max(1, m_impl->emulator->rows());
        const int visCols = std::max(1, m_impl->emulator->cols());
        const int curRow = std::clamp(m_impl->cursorViewportRow(), 0, visRows - 1);
        const int curCol = std::clamp(m_impl->emulator->cursorCol(), 0, visCols - 1);
        return QRect(
            static_cast<int>(std::floor(kInsetLeft + curCol * m_impl->cellWidth)),
            static_cast<int>(std::floor(kInsetTop + curRow * m_impl->cellHeight)),
            std::max(1, static_cast<int>(std::ceil(m_impl->cellWidth))),
            std::max(1, static_cast<int>(std::ceil(m_impl->cellHeight))));
    }
    case Qt::ImFont:
        return m_impl->regularFont;
    case Qt::ImCursorPosition:
        return m_impl->preeditCursorPos;
    case Qt::ImSurroundingText:
        return QString();
    case Qt::ImCurrentSelection:
        return selectedText();
    default:
        return QWidget::inputMethodQuery(query);
    }
}

void SpellTerminalSurface::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    setFocus(Qt::MouseFocusReason);

    const QPoint posInt = event->position().toPoint();
    const bool isTripleClick = m_impl->lastDoubleClickTimer.isValid()
        && m_impl->lastDoubleClickTimer.elapsed() <= QApplication::doubleClickInterval()
        && (posInt - m_impl->lastDoubleClickPos).manhattanLength() <= QApplication::startDragDistance();

    if (isTripleClick) {
        m_impl->lastDoubleClickTimer.invalidate();
        const GridPoint pt = m_impl->gridPointForPos(event->position(), false);
        m_impl->selectionAnchor = GridPoint{pt.absLine, 0};
        m_impl->selectionHead = GridPoint{pt.absLine, m_impl->emulator->cols()};
        m_impl->hasSelectionBounds = true;
        m_impl->selectionMode = SelectionMode::Line;
        m_impl->selecting = true;
    } else if (event->modifiers().testFlag(Qt::ShiftModifier) && m_impl->hasSelectionBounds) {
        m_impl->selectionHead = m_impl->gridPointForPos(event->position(), true);
        m_impl->selectionMode = SelectionMode::Char;
        m_impl->selecting = true;
    } else {
        const GridPoint pt = m_impl->gridPointForPos(event->position(), true);
        m_impl->selectionAnchor = pt;
        m_impl->selectionHead = pt;
        m_impl->hasSelectionBounds = true;
        m_impl->selectionMode = SelectionMode::Char;
        m_impl->selecting = true;
    }

    update();
    event->accept();
}

void SpellTerminalSurface::mouseMoveEvent(QMouseEvent* event) {
    if (!m_impl->selecting || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPointF pos = event->position();
    if (pos.y() < 0.0) {
        scrollByLines(1);
    } else if (pos.y() > static_cast<qreal>(height())) {
        scrollByLines(-1);
    }

    if (m_impl->selectionMode == SelectionMode::Line) {
        const GridPoint pt = m_impl->gridPointForPos(pos, false);
        const int cols = m_impl->emulator->cols();
        if (pt.absLine >= m_impl->selectionAnchor.absLine) {
            m_impl->selectionAnchor.col = 0;
            m_impl->selectionHead = GridPoint{pt.absLine, cols};
        } else {
            m_impl->selectionAnchor.col = cols;
            m_impl->selectionHead = GridPoint{pt.absLine, 0};
        }
    } else if (m_impl->selectionMode == SelectionMode::Word) {
        const GridPoint pt = m_impl->gridPointForPos(pos, false);
        const auto curWord = m_impl->wordBoundsAt(pt);
        if (pt < m_impl->wordAnchorStart) {
            m_impl->selectionAnchor = m_impl->wordAnchorEnd;
            m_impl->selectionHead = curWord ? curWord->first : pt;
        } else if (m_impl->wordAnchorEnd < pt || pt == m_impl->wordAnchorEnd) {
            m_impl->selectionAnchor = m_impl->wordAnchorStart;
            m_impl->selectionHead = curWord ? curWord->second : m_impl->gridPointForPos(pos, true);
        } else {
            m_impl->selectionAnchor = m_impl->wordAnchorStart;
            m_impl->selectionHead = m_impl->wordAnchorEnd;
        }
    } else {
        m_impl->selectionHead = m_impl->gridPointForPos(pos, true);
    }

    update();
    event->accept();
}

void SpellTerminalSurface::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_impl->selecting) {
        m_impl->selecting = false;
        if (m_impl->selectionAnchor == m_impl->selectionHead) {
            clearSelection();
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void SpellTerminalSurface::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    setFocus(Qt::MouseFocusReason);
    m_impl->lastDoubleClickTimer.start();
    m_impl->lastDoubleClickPos = event->position().toPoint();

    const GridPoint pt = m_impl->gridPointForPos(event->position(), false);
    m_impl->selectWordAt(pt);
    m_impl->selectionMode = SelectionMode::Word;
    m_impl->selecting = m_impl->hasSelectionBounds;
    update();
    event->accept();
}

void SpellTerminalSurface::wheelEvent(QWheelEvent* event) {
    if (event->phase() == Qt::ScrollBegin || event->phase() == Qt::ScrollEnd) {
        m_impl->scrollAccumulator = 0.0;
    }

    qreal deltaLines = 0.0;
    const QPoint pixelDelta = event->pixelDelta();
    const QPoint angleDelta = event->angleDelta();
    if (!pixelDelta.isNull()) {
        deltaLines = static_cast<qreal>(pixelDelta.y()) / std::max<qreal>(1.0, m_impl->cellHeight);
    } else if (!angleDelta.isNull()) {
        deltaLines = (static_cast<qreal>(angleDelta.y()) / 120.0) * 3.0;
    }

    if (std::abs(deltaLines) > 0.0) {
        m_impl->scrollAccumulator += deltaLines;
        const int wholeLines = static_cast<int>(std::trunc(m_impl->scrollAccumulator));
        if (wholeLines != 0) {
            m_impl->scrollAccumulator -= static_cast<qreal>(wholeLines);
            scrollByLines(wholeLines);
        }
        event->accept();
        return;
    }

    QWidget::wheelEvent(event);
}

void SpellTerminalSurface::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    m_impl->cursorBlinkOn = true;
    if (isVisible() && m_impl->blinkTimer) {
        m_impl->blinkTimer->start();
    }
    update();
}

void SpellTerminalSurface::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    if (m_impl->blinkTimer) {
        m_impl->blinkTimer->stop();
    }
    m_impl->cursorBlinkOn = true;
    update();
}

void SpellTerminalSurface::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);

    const QString sel = selectedText();
    const bool hasTextSelection = !sel.isEmpty();

    QAction* copyAct = menu.addAction(QStringLiteral("Copy"));
    copyAct->setEnabled(hasTextSelection);
    connect(copyAct, &QAction::triggered, this, &SpellTerminalSurface::copySelection);

    QAction* pasteAct = menu.addAction(QStringLiteral("Paste"));
    const QClipboard* clipboard = QGuiApplication::clipboard();
    pasteAct->setEnabled(isShellRunning() && clipboard && !clipboard->text(QClipboard::Clipboard).isEmpty());
    connect(pasteAct, &QAction::triggered, this, &SpellTerminalSurface::pasteFromClipboard);

    QAction* selectAllAct = menu.addAction(QStringLiteral("Select All"));
    connect(selectAllAct, &QAction::triggered, this, &SpellTerminalSurface::selectAll);

    menu.addSeparator();

    QAction* askAct = menu.addAction(QStringLiteral("Ask BeatrixLeslie About Selection"));
    askAct->setEnabled(hasTextSelection);
    connect(askAct, &QAction::triggered, this, [this, sel]() {
        emit spellHandOffRequested(
            QStringLiteral("Explain or help with this terminal selection:\n") + sel,
            recentOutputText(80),
            workingDirectory());
    });

    menu.addSeparator();

    QAction* clearAct = menu.addAction(QStringLiteral("Clear Buffer"));
    connect(clearAct, &QAction::triggered, this, &SpellTerminalSurface::clearBuffer);

    QAction* restartAct = menu.addAction(QStringLiteral("Restart Shell"));
    connect(restartAct, &QAction::triggered, this, [this]() {
        restartShell();
    });

    menu.exec(event->globalPos());
    event->accept();
}

void SpellTerminalSurface::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_impl->updateScrollBarGeometry();
    m_impl->updateScrollBar();
    if (width() > static_cast<int>(kInsetLeft + kInsetRight)
        && height() > static_cast<int>(kInsetTop + kInsetBottom)) {
        m_impl->scheduleGridResize(true);
    }
    m_impl->cursorBlinkOn = true;
    if (hasFocus() && m_impl->blinkTimer) {
        m_impl->blinkTimer->start();
    }
    update();
}

void SpellTerminalSurface::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_impl->blinkTimer) {
        m_impl->blinkTimer->stop();
    }
    if (m_impl->resizeTimer) {
        m_impl->resizeTimer->stop();
    }
    m_impl->hasPendingResize = false;
}

} // namespace spell
