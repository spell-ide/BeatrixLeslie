#include "panes/CodeEditorPane.h"

#include "panes/EditorDocument.h"
#include "panes/RustCustomLexer.h"
#include "shell/WindowChrome.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <QAbstractButton>
#include <QColor>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMetaMethod>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <QWheelEvent>

#include <Qsci/qscilexer.h>
#include <Qsci/qscilexerbash.h>
#include <Qsci/qscilexercmake.h>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexercss.h>
#include <Qsci/qscilexerdiff.h>
#include <Qsci/qscilexerhtml.h>
#include <Qsci/qscilexerjava.h>
#include <Qsci/qscilexerjavascript.h>
#include <Qsci/qscilexerjson.h>
#include <Qsci/qscilexermarkdown.h>
#include <Qsci/qscilexerpython.h>
#include <Qsci/qscilexerruby.h>
#include <Qsci/qscilexersql.h>
#include <Qsci/qscilexerxml.h>
#include <Qsci/qscilexeryaml.h>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <utility>

namespace spell {

namespace {

constexpr int kTopBarHeight = 34;
constexpr int kTabPadH = 12;
constexpr int kTabSpacing = 6;
constexpr int kTabSlotSize = 12;
constexpr int kTabDividerWidth = 1;
constexpr int kTabMaxWidth = 220;
constexpr double kTabDotRadius = 3.0; // 6px solid diameter
constexpr int kReloadDebounceMs = 120;
constexpr qsizetype kMatchCountCharLimit = 512 * 1024;

/// Lightweight RAII boolean guard to suppress re-entrant Qt/Scintilla signals.
class ScopedBoolGuard {
public:
    explicit ScopedBoolGuard(bool& flag)
        : m_flag(flag), m_previous(flag) {
        m_flag = true;
    }

    ~ScopedBoolGuard() {
        m_flag = m_previous;
    }

    ScopedBoolGuard(const ScopedBoolGuard&) = delete;
    ScopedBoolGuard& operator=(const ScopedBoolGuard&) = delete;

private:
    bool& m_flag;
    bool m_previous;
};

/// Plain QObject event-filter forwarder (intentionally no Q_OBJECT macro for clean automoc).
class EventFilterRelay : public QObject {
public:
    using Callback = std::function<bool(QObject*, QEvent*)>;

    EventFilterRelay(Callback callback, QObject* parent = nullptr)
        : QObject(parent), m_callback(std::move(callback)) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (m_callback && m_callback(watched, event)) {
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    Callback m_callback;
};

QString canonicalDocumentKey(const QString& filePath) {
    if (filePath.isEmpty()) {
        return {};
    }
    const QFileInfo fi(filePath);
    const QString canonical = fi.canonicalFilePath();
    if (!canonical.isEmpty()) {
        return canonical;
    }
    return QDir::cleanPath(fi.absoluteFilePath());
}

/// Minimalist flat file tab matching BeatrixLeslie IDE's ConversationTab pixel-for-pixel
/// (intentionally no Q_OBJECT macro for clean automoc).
class FileTabWidget final : public QWidget {
public:
    using Callback = std::function<void()>;

    FileTabWidget(const QString& title,
                  Callback onSelect,
                  Callback onClose,
                  QWidget* parent = nullptr)
        : QWidget(parent)
        , m_title(title)
        , m_onSelect(std::move(onSelect))
        , m_onClose(std::move(onClose)) {
        setFixedHeight(kTopBarHeight);
        setMaximumWidth(kTabMaxWidth);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover, true);
        setCursor(Qt::PointingHandCursor);
    }

    void setTitle(const QString& title) {
        if (m_title == title) {
            return;
        }
        m_title = title;
        updateGeometry();
        update();
    }

    void setActive(bool active) {
        if (m_active == active) {
            return;
        }
        m_active = active;
        update();
    }

    void setDirty(bool dirty) {
        if (m_dirty == dirty) {
            return;
        }
        m_dirty = dirty;
        update();
    }

    void applyTheme() {
        updateGeometry();
        update();
    }

    QSize sizeHint() const override {
        QFont measureFont = QGuiApplication::font();
        measureFont.setPointSizeF(11.5);
        measureFont.setWeight(QFont::DemiBold);
        const QFontMetricsF fm(measureFont, this);
        const int textWidth = static_cast<int>(std::ceil(fm.horizontalAdvance(m_title))) + 4;
        const int chromeWidth = (2 * kTabPadH) + kTabSpacing + kTabSlotSize + kTabDividerWidth;
        const int preferredWidth = chromeWidth + std::max(8, textWidth);
        return QSize(std::min(kTabMaxWidth, preferredWidth), kTopBarHeight);
    }

    QSize minimumSizeHint() const override {
        return sizeHint();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);

        const UiPalette u = ThemeManager::instance().ui();
        const QColor dividerCol = compositeOver(u.divider, u.background);

        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        const int contentW = std::max(0, width() - kTabDividerWidth);
        QColor tabBg = u.background;
        if (m_active) {
            tabBg = compositeOver(u.elevatedSurface, u.background);
            p.fillRect(QRect(0, 0, contentW, height()), tabBg);
        } else if (m_hovered) {
            const QColor hoverTint = withAlpha(u.elevatedSurface, u.elevatedSurface.alphaF() * 0.55);
            tabBg = compositeOver(hoverTint, u.background);
            p.fillRect(QRect(0, 0, contentW, height()), tabBg);
        }

        if (m_active) {
            p.fillRect(QRect(0, height() - 1, contentW, 1), u.textPrimary);
        }

        // Crisp 1px right vertical hairline divider
        p.fillRect(QRect(width() - kTabDividerWidth, 0, kTabDividerWidth, height()), dividerCol);

        // Filename label (11.5pt semibold active / regular inactive)
        QFont font = QGuiApplication::font();
        font.setPointSizeF(11.5);
        font.setWeight(m_active ? QFont::DemiBold : QFont::Normal);
        p.setFont(font);
        const QColor textCol = m_active ? u.textPrimary : compositeOver(u.textSecondary, tabBg);
        p.setPen(textCol);

        const QRect slot = slotRect();
        const int textW = std::max(0, slot.left() - kTabSpacing - kTabPadH);
        const QRect textRect(kTabPadH, 0, textW, height());
        const QFontMetrics fm(font);
        const QString elided = fm.elidedText(m_title, Qt::ElideRight, textW);
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, elided);

        // Right 12x12 indicator / close slot
        const QPointF center(slot.x() + (kTabSlotSize * 0.5),
                             slot.y() + (kTabSlotSize * 0.5));
        if (showsDirtyDot()) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(u.textPrimary);
            p.drawEllipse(center, kTabDotRadius, kTabDotRadius);
        } else if (showsCloseButton()) {
            p.setRenderHint(QPainter::Antialiasing, true);
            if (m_closeHovered) {
                p.setPen(Qt::NoPen);
                p.setBrush(compositeOver(u.selectedFill, tabBg));
                p.drawRoundedRect(QRectF(center.x() - 7.5, center.y() - 7.5, 15.0, 15.0), 3.0, 3.0);
            }
            const QColor xColor = m_closeHovered ? u.textPrimary : compositeOver(u.textMuted, tabBg);
            QPen xPen(xColor, 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(xPen);
            p.setBrush(Qt::NoBrush);
            constexpr qreal arm = 3.0;
            p.drawLine(QPointF(center.x() - arm, center.y() - arm),
                       QPointF(center.x() + arm, center.y() + arm));
            p.drawLine(QPointF(center.x() + arm, center.y() - arm),
                       QPointF(center.x() - arm, center.y() + arm));
        }
    }

    void enterEvent(QEnterEvent* event) override {
        m_hovered = true;
        m_closeHovered = event && closeHitRect().contains(event->position().toPoint());
        update();
        QWidget::enterEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (event) {
            const bool nowCloseHovered = closeHitRect().contains(event->position().toPoint());
            if (nowCloseHovered != m_closeHovered) {
                m_closeHovered = nowCloseHovered;
                update();
            }
        }
        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        m_hovered = false;
        m_closeHovered = false;
        m_closePressed = false;
        update();
        QWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (!event) {
            QWidget::mousePressEvent(event);
            return;
        }
        if (event->button() == Qt::LeftButton) {
            const QPoint pos = event->position().toPoint();
            if (showsCloseButton() && closeHitRect().contains(pos)) {
                m_closePressed = true;
                event->accept();
                return;
            }
            m_closePressed = false;
            if (m_onSelect) {
                m_onSelect();
            }
            event->accept();
            return;
        }
        if (event->button() == Qt::MiddleButton) {
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!event) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        if (event->button() == Qt::LeftButton && m_closePressed) {
            const bool inside = closeHitRect().contains(event->position().toPoint());
            m_closePressed = false;
            if (inside && m_onClose) {
                m_onClose();
            }
            event->accept();
            return;
        }
        if (event->button() == Qt::MiddleButton) {
            if (rect().contains(event->position().toPoint()) && m_onClose) {
                m_onClose();
            }
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QRect slotRect() const {
        const int x = width() - kTabDividerWidth - kTabPadH - kTabSlotSize;
        const int y = (height() - kTabSlotSize) / 2;
        return QRect(x, y, kTabSlotSize, kTabSlotSize);
    }

    QRect closeHitRect() const {
        return slotRect().adjusted(-3, -3, 3, 3);
    }

    bool showsDirtyDot() const {
        return m_dirty && !m_hovered;
    }

    bool showsCloseButton() const {
        return !showsDirtyDot() && (m_hovered || m_active);
    }

    QString m_title;
    Callback m_onSelect;
    Callback m_onClose;
    bool m_active = false;
    bool m_dirty = false;
    bool m_hovered = false;
    bool m_closeHovered = false;
    bool m_closePressed = false;
};

/// Frameless horizontal scroll strip for flat file tabs (hidden scrollbars, smooth wheel scroll).
class FileTabScrollArea final : public QScrollArea {
public:
    explicit FileTabScrollArea(QWidget* parent = nullptr)
        : QScrollArea(parent) {
        setFrameShape(QFrame::NoFrame);
        setFixedHeight(kTopBarHeight);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setWidgetResizable(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void wheelEvent(QWheelEvent* event) override {
        if (!event) {
            return;
        }
        QScrollBar* hBar = horizontalScrollBar();
        if (!hBar || hBar->maximum() <= hBar->minimum()) {
            event->ignore();
            return;
        }
        const QPoint pixelDelta = event->pixelDelta();
        const QPoint angleDelta = event->angleDelta();
        int delta = 0;
        if (!pixelDelta.isNull()) {
            delta = (std::abs(pixelDelta.x()) >= std::abs(pixelDelta.y()))
                        ? pixelDelta.x()
                        : pixelDelta.y();
        } else if (!angleDelta.isNull()) {
            delta = (std::abs(angleDelta.x()) >= std::abs(angleDelta.y()))
                        ? angleDelta.x()
                        : angleDelta.y();
            delta /= 2;
        }
        if (delta != 0) {
            hBar->setValue(hBar->value() - delta);
            event->accept();
            return;
        }
        QScrollArea::wheelEvent(event);
    }
};

/// Computes the UTF-16 character offset inside `text` corresponding to 0-based (line, col).
qsizetype charOffsetForLineIndex(const QString& text, int targetLine, int targetCol) {
    if (targetLine <= 0) {
        return std::min<qsizetype>(std::max(0, targetCol), text.size());
    }
    int line = 0;
    qsizetype idx = 0;
    const qsizetype len = text.size();
    while (idx < len && line < targetLine) {
        if (text[idx] == QLatin1Char('\n')) {
            ++line;
        } else if (text[idx] == QLatin1Char('\r')) {
            if (idx + 1 < len && text[idx + 1] == QLatin1Char('\n')) {
                ++idx;
            }
            ++line;
        }
        ++idx;
    }
    return std::min<qsizetype>(idx + std::max(0, targetCol), len);
}

/// Creates the appropriate QScintilla lexer for a normalized language key, or nullptr for plain text.
QsciLexer* createLexerForLanguage(const QString& rawLang, QObject* parent) {
    const QString lang = rawLang.trimmed().toLower();

    // Custom Spell IDE lexers (languages without built-in QScintilla2 lexers)
    if (lang == QLatin1String("rust") || lang == QLatin1String("rs")) {
        return new SpellCustomLexer(SpellCustomLexer::CustomLang::Rust, parent);
    }
    if (lang == QLatin1String("swift")) {
        return new SpellCustomLexer(SpellCustomLexer::CustomLang::Swift, parent);
    }
    if (lang == QLatin1String("go") || lang == QLatin1String("golang")) {
        return new SpellCustomLexer(SpellCustomLexer::CustomLang::Go, parent);
    }
    if (lang == QLatin1String("toml")) {
        return new SpellCustomLexer(SpellCustomLexer::CustomLang::Toml, parent);
    }
    if (lang == QLatin1String("ts") || lang == QLatin1String("typescript")) {
        return new SpellCustomLexer(SpellCustomLexer::CustomLang::TypeScript, parent);
    }

    // Built-in QScintilla2 lexers
    if (lang == QLatin1String("cpp") || lang == QLatin1String("c") ||
        lang == QLatin1String("h") || lang == QLatin1String("c++") ||
        lang == QLatin1String("cxx") || lang == QLatin1String("hpp")) {
        return new QsciLexerCPP(parent);
    }
    if (lang == QLatin1String("python") || lang == QLatin1String("py")) {
        return new QsciLexerPython(parent);
    }
    if (lang == QLatin1String("js") || lang == QLatin1String("javascript")) {
        return new QsciLexerJavaScript(parent);
    }
    if (lang == QLatin1String("json")) {
        return new QsciLexerJSON(parent);
    }
    if (lang == QLatin1String("yaml") || lang == QLatin1String("yml")) {
        return new QsciLexerYAML(parent);
    }
    if (lang == QLatin1String("markdown") || lang == QLatin1String("md")) {
        return new QsciLexerMarkdown(parent);
    }
    if (lang == QLatin1String("bash") || lang == QLatin1String("sh") ||
        lang == QLatin1String("shell") || lang == QLatin1String("zsh")) {
        return new QsciLexerBash(parent);
    }
    if (lang == QLatin1String("html") || lang == QLatin1String("htm")) {
        return new QsciLexerHTML(parent);
    }
    if (lang == QLatin1String("css")) {
        return new QsciLexerCSS(parent);
    }
    if (lang == QLatin1String("sql")) {
        return new QsciLexerSQL(parent);
    }
    if (lang == QLatin1String("cmake")) {
        return new QsciLexerCMake(parent);
    }
    if (lang == QLatin1String("diff") || lang == QLatin1String("patch")) {
        return new QsciLexerDiff(parent);
    }
    if (lang == QLatin1String("xml")) {
        return new QsciLexerXML(parent);
    }
    if (lang == QLatin1String("ruby") || lang == QLatin1String("rb")) {
        return new QsciLexerRuby(parent);
    }
    if (lang == QLatin1String("java")) {
        return new QsciLexerJava(parent);
    }

    return nullptr;
}

/// Applies the active Spell IDE `CodePalette` and editor font across any built-in or custom lexer.
void applyPaletteToLexer(QsciLexer* lexer, const CodePalette& pal, const QFont& font) {
    if (!lexer) {
        return;
    }

    if (auto* custom = qobject_cast<SpellCustomLexer*>(lexer)) {
        custom->applyCodePalette(pal, font);
        return;
    }

    QFont italicFont(font);
    italicFont.setItalic(true);

    QFont boldFont(font);
    boldFont.setBold(true);

    const QColor mutedInactive = compositeOver(withAlpha(pal.comment, 0.72), pal.background);

    // Reset all styles first so no style retains hardcoded light backgrounds or default fonts.
    lexer->setDefaultFont(font);
    lexer->setDefaultColor(pal.plain);
    lexer->setDefaultPaper(pal.background);
    lexer->setFont(font, -1);
    lexer->setColor(pal.plain, -1);
    lexer->setPaper(pal.background, -1);
    lexer->setEolFill(false, -1);

    auto setStyles = [lexer](const QColor& col, std::initializer_list<int> styles) {
        for (int s : styles) {
            lexer->setColor(col, s);
        }
    };
    auto setStylesWithFont = [lexer](const QColor& col, const QFont& f, std::initializer_list<int> styles) {
        for (int s : styles) {
            lexer->setColor(col, s);
            lexer->setFont(f, s);
        }
    };

    // Note: QsciLexerJavaScript and QsciLexerJava inherit from QsciLexerCPP
    // and share its style enum values.
    if (qobject_cast<QsciLexerCPP*>(lexer) != nullptr) {
        using S = QsciLexerCPP;
        setStylesWithFont(pal.comment, italicFont, {
            S::Comment, S::CommentLine, S::CommentDoc, S::CommentLineDoc,
            S::PreProcessorComment, S::PreProcessorCommentLineDoc
        });
        setStyles(pal.keyword, {
            S::Keyword, S::PreProcessor, S::CommentDocKeyword
        });
        setStyles(pal.type, {
            S::KeywordSet2, S::GlobalClass
        });
        setStylesWithFont(pal.type, boldFont, {S::TaskMarker});
        setStyles(pal.number, {
            S::Number, S::UserLiteral, S::EscapeSequence
        });
        setStyles(pal.string, {
            S::DoubleQuotedString, S::SingleQuotedString, S::VerbatimString,
            S::RawString, S::TripleQuotedVerbatimString, S::HashQuotedString,
            S::UUID, S::Regex
        });
        setStyles(pal.removed, {
            S::UnclosedString, S::CommentDocKeywordError
        });
        setStyles(pal.plain, {
            S::Default, S::Operator, S::Identifier
        });
        // Dimmed inactive preprocessor branch styles (style + 64)
        for (int base = S::Default; base <= S::EscapeSequence; ++base) {
            lexer->setColor(mutedInactive, base + 64);
            lexer->setPaper(pal.background, base + 64);
            lexer->setFont(font, base + 64);
        }
        return;
    }

    if (qobject_cast<QsciLexerPython*>(lexer) != nullptr) {
        using S = QsciLexerPython;
        setStylesWithFont(pal.comment, italicFont, {S::Comment, S::CommentBlock});
        setStyles(pal.number, {S::Number});
        setStyles(pal.keyword, {S::Keyword});
        setStyles(pal.type, {S::ClassName, S::HighlightedIdentifier, S::Decorator});
        setStyles(pal.function, {S::FunctionMethodName});
        setStyles(pal.string, {
            S::DoubleQuotedString, S::SingleQuotedString,
            S::TripleSingleQuotedString, S::TripleDoubleQuotedString,
            S::DoubleQuotedFString, S::SingleQuotedFString,
            S::TripleSingleQuotedFString, S::TripleDoubleQuotedFString
        });
        setStyles(pal.removed, {S::UnclosedString});
        setStyles(pal.plain, {S::Default, S::Operator, S::Identifier});
        return;
    }

    if (qobject_cast<QsciLexerJSON*>(lexer) != nullptr) {
        using S = QsciLexerJSON;
        setStyles(pal.plain, {S::Default, S::Operator});
        setStyles(pal.number, {S::Number});
        setStyles(pal.string, {S::String, S::IRI, S::IRICompact});
        setStyles(pal.function, {S::Property});
        setStyles(pal.keyword, {S::Keyword, S::KeywordLD});
        setStyles(pal.type, {S::EscapeSequence});
        setStylesWithFont(pal.comment, italicFont, {S::CommentLine, S::CommentBlock});
        setStyles(pal.removed, {S::UnclosedString, S::Error});
        return;
    }

    if (qobject_cast<QsciLexerYAML*>(lexer) != nullptr) {
        using S = QsciLexerYAML;
        setStyles(pal.plain, {S::Default, S::Operator});
        setStylesWithFont(pal.comment, italicFont, {S::Comment});
        setStyles(pal.function, {S::Identifier});
        setStyles(pal.keyword, {S::Keyword});
        setStylesWithFont(pal.keyword, boldFont, {S::DocumentDelimiter});
        setStyles(pal.number, {S::Number});
        setStyles(pal.type, {S::Reference});
        setStyles(pal.string, {S::TextBlockMarker});
        setStyles(pal.removed, {S::SyntaxErrorMarker});
        return;
    }

    if (qobject_cast<QsciLexerMarkdown*>(lexer) != nullptr) {
        using S = QsciLexerMarkdown;
        setStyles(pal.plain, {S::Default, S::Prechar});
        setStylesWithFont(pal.function, boldFont, {
            S::Header1, S::Header2, S::Header3, S::Header4, S::Header5, S::Header6
        });
        setStylesWithFont(pal.plain, boldFont, {
            S::StrongEmphasisAsterisks, S::StrongEmphasisUnderscores
        });
        setStylesWithFont(pal.plain, italicFont, {
            S::EmphasisAsterisks, S::EmphasisUnderscores
        });
        setStyles(pal.string, {S::CodeBackticks, S::CodeDoubleBackticks, S::CodeBlock});
        setStyles(pal.type, {S::Link});
        setStyles(pal.keyword, {S::UnorderedListItem, S::OrderedListItem});
        setStylesWithFont(pal.comment, italicFont, {S::BlockQuote});
        setStyles(pal.comment, {S::HorizontalRule, S::Special, S::StrikeOut});
        return;
    }

    if (qobject_cast<QsciLexerBash*>(lexer) != nullptr) {
        using S = QsciLexerBash;
        setStyles(pal.plain, {S::Default, S::Operator, S::Identifier});
        setStylesWithFont(pal.comment, italicFont, {S::Comment});
        setStyles(pal.number, {S::Number});
        setStyles(pal.keyword, {S::Keyword, S::HereDocumentDelimiter});
        setStyles(pal.string, {
            S::DoubleQuotedString, S::SingleQuotedString, S::SingleQuotedHereDocument
        });
        setStyles(pal.type, {S::Scalar, S::ParameterExpansion});
        setStyles(pal.function, {S::Backticks});
        setStyles(pal.removed, {S::Error});
        return;
    }

    // Note: QsciLexerXML inherits from QsciLexerHTML and shares its style enum values.
    if (qobject_cast<QsciLexerHTML*>(lexer) != nullptr) {
        using S = QsciLexerHTML;
        setStyles(pal.plain, {
            S::Default, S::OtherInTag, S::SGMLDefault, S::SGMLBlockDefault,
            S::JavaScriptDefault, S::JavaScriptWord, S::JavaScriptSymbol,
            S::ASPJavaScriptDefault, S::ASPJavaScriptWord, S::ASPJavaScriptSymbol,
            S::VBScriptDefault, S::VBScriptIdentifier,
            S::ASPVBScriptDefault, S::ASPVBScriptIdentifier,
            S::PythonDefault, S::PythonOperator, S::PythonIdentifier,
            S::ASPPythonDefault, S::ASPPythonOperator, S::ASPPythonIdentifier,
            S::PHPDefault, S::PHPOperator
        });
        setStyles(pal.keyword, {
            S::Tag, S::UnknownTag, S::XMLTagEnd, S::XMLStart, S::XMLEnd,
            S::Script, S::ASPAtStart, S::ASPStart, S::PHPStart, S::SGMLCommand,
            S::JavaScriptStart, S::JavaScriptKeyword,
            S::ASPJavaScriptStart, S::ASPJavaScriptKeyword,
            S::VBScriptStart, S::VBScriptKeyword,
            S::ASPVBScriptStart, S::ASPVBScriptKeyword,
            S::PythonStart, S::PythonKeyword,
            S::ASPPythonStart, S::ASPPythonKeyword,
            S::PHPKeyword
        });
        setStyles(pal.type, {
            S::Attribute, S::UnknownAttribute, S::SGMLParameter,
            S::PythonClassName, S::ASPPythonClassName,
            S::PHPVariable, S::PHPDoubleQuotedVariable
        });
        setStyles(pal.function, {
            S::Entity, S::SGMLEntity, S::SGMLSpecial,
            S::PythonFunctionMethodName, S::ASPPythonFunctionMethodName
        });
        setStyles(pal.number, {
            S::HTMLNumber, S::JavaScriptNumber, S::ASPJavaScriptNumber,
            S::VBScriptNumber, S::ASPVBScriptNumber, S::PythonNumber,
            S::ASPPythonNumber, S::PHPNumber
        });
        setStyles(pal.string, {
            S::HTMLDoubleQuotedString, S::HTMLSingleQuotedString, S::HTMLValue,
            S::SGMLDoubleQuotedString, S::SGMLSingleQuotedString, S::CDATA,
            S::JavaScriptDoubleQuotedString, S::JavaScriptSingleQuotedString, S::JavaScriptRegex,
            S::ASPJavaScriptDoubleQuotedString, S::ASPJavaScriptSingleQuotedString, S::ASPJavaScriptRegex,
            S::VBScriptString, S::ASPVBScriptString,
            S::PythonDoubleQuotedString, S::PythonSingleQuotedString,
            S::PythonTripleSingleQuotedString, S::PythonTripleDoubleQuotedString,
            S::ASPPythonDoubleQuotedString, S::ASPPythonSingleQuotedString,
            S::ASPPythonTripleSingleQuotedString, S::ASPPythonTripleDoubleQuotedString,
            S::PHPDoubleQuotedString, S::PHPSingleQuotedString
        });
        setStylesWithFont(pal.comment, italicFont, {
            S::HTMLComment, S::ASPXCComment, S::SGMLComment, S::SGMLParameterComment,
            S::JavaScriptComment, S::JavaScriptCommentLine, S::JavaScriptCommentDoc,
            S::ASPJavaScriptComment, S::ASPJavaScriptCommentLine, S::ASPJavaScriptCommentDoc,
            S::VBScriptComment, S::ASPVBScriptComment,
            S::PythonComment, S::ASPPythonComment,
            S::PHPComment, S::PHPCommentLine
        });
        setStyles(pal.removed, {
            S::SGMLError, S::JavaScriptUnclosedString, S::ASPJavaScriptUnclosedString,
            S::VBScriptUnclosedString, S::ASPVBScriptUnclosedString
        });
        return;
    }

    if (qobject_cast<QsciLexerCSS*>(lexer) != nullptr) {
        using S = QsciLexerCSS;
        setStyles(pal.plain, {S::Default, S::Operator});
        setStyles(pal.keyword, {S::Tag, S::Important, S::AtRule, S::MediaRule});
        setStyles(pal.type, {S::ClassSelector, S::IDSelector, S::Attribute, S::Variable});
        setStyles(pal.function, {
            S::CSS1Property, S::CSS2Property, S::CSS3Property,
            S::ExtendedCSSProperty, S::UnknownProperty,
            S::PseudoClass, S::UnknownPseudoClass, S::PseudoElement,
            S::ExtendedPseudoClass, S::ExtendedPseudoElement
        });
        setStyles(pal.number, {S::Value});
        setStyles(pal.string, {S::DoubleQuotedString, S::SingleQuotedString});
        setStylesWithFont(pal.comment, italicFont, {S::Comment});
        return;
    }

    if (qobject_cast<QsciLexerSQL*>(lexer) != nullptr) {
        using S = QsciLexerSQL;
        setStyles(pal.plain, {S::Default, S::Operator, S::Identifier, S::QuotedOperator});
        setStylesWithFont(pal.comment, italicFont, {
            S::Comment, S::CommentLine, S::CommentDoc, S::PlusComment, S::CommentLineHash
        });
        setStyles(pal.keyword, {
            S::Keyword, S::PlusKeyword, S::PlusPrompt, S::CommentDocKeyword
        });
        setStyles(pal.type, {
            S::KeywordSet5, S::KeywordSet6, S::KeywordSet7, S::KeywordSet8
        });
        setStyles(pal.number, {S::Number});
        setStyles(pal.string, {S::DoubleQuotedString, S::SingleQuotedString});
        setStyles(pal.function, {S::QuotedIdentifier});
        setStyles(pal.removed, {S::CommentDocKeywordError});
        return;
    }

    if (qobject_cast<QsciLexerCMake*>(lexer) != nullptr) {
        using S = QsciLexerCMake;
        setStyles(pal.plain, {S::Default});
        setStylesWithFont(pal.comment, italicFont, {S::Comment});
        setStyles(pal.string, {S::String, S::StringLeftQuote, S::StringRightQuote});
        setStyles(pal.function, {S::Function});
        setStyles(pal.type, {S::Variable, S::StringVariable});
        setStyles(pal.keyword, {
            S::Label, S::KeywordSet3, S::BlockWhile, S::BlockForeach,
            S::BlockIf, S::BlockMacro
        });
        setStyles(pal.number, {S::Number});
        return;
    }

    if (qobject_cast<QsciLexerDiff*>(lexer) != nullptr) {
        using S = QsciLexerDiff;
        const QColor addedBg = compositeOver(withAlpha(pal.added, 0.13), pal.background);
        const QColor removedBg = compositeOver(withAlpha(pal.removed, 0.13), pal.background);
        const QColor hunkBg = compositeOver(withAlpha(pal.hunk, 0.12), pal.background);

        setStyles(pal.plain, {S::Default});
        setStylesWithFont(pal.comment, italicFont, {S::Comment});
        setStylesWithFont(pal.type, boldFont, {S::Command, S::Header});

        setStyles(pal.hunk, {S::Position, S::LineChanged});
        lexer->setPaper(hunkBg, S::Position);
        lexer->setEolFill(true, S::Position);

        for (int s : {S::LineAdded, S::AddingPatchAdded, S::RemovingPatchAdded}) {
            lexer->setColor(pal.added, s);
            lexer->setPaper(addedBg, s);
            lexer->setEolFill(true, s);
        }
        for (int s : {S::LineRemoved, S::AddingPatchRemoved, S::RemovingPatchRemoved}) {
            lexer->setColor(pal.removed, s);
            lexer->setPaper(removedBg, s);
            lexer->setEolFill(true, s);
        }
        return;
    }

    if (qobject_cast<QsciLexerRuby*>(lexer) != nullptr) {
        using S = QsciLexerRuby;
        setStyles(pal.plain, {
            S::Default, S::Operator, S::Identifier, S::DataSection,
            S::Stdin, S::Stdout, S::Stderr
        });
        setStyles(pal.removed, {S::Error});
        setStylesWithFont(pal.comment, italicFont, {S::Comment, S::POD});
        setStyles(pal.number, {S::Number});
        setStyles(pal.keyword, {S::Keyword, S::DemotedKeyword});
        setStyles(pal.type, {
            S::ClassName, S::ModuleName, S::Global,
            S::InstanceVariable, S::ClassVariable, S::Symbol
        });
        setStyles(pal.function, {S::FunctionMethodName});
        setStyles(pal.string, {
            S::DoubleQuotedString, S::SingleQuotedString, S::Regex, S::Backticks,
            S::HereDocumentDelimiter, S::HereDocument,
            S::PercentStringq, S::PercentStringQ, S::PercentStringx,
            S::PercentStringr, S::PercentStringw
        });
        return;
    }
}

} // namespace

struct CodeEditorPane::Impl {
    CodeEditorPane* owner = nullptr;

    // Document storage (1-to-1 ordered correspondence with tabWidgets)
    QVector<EditorDocument*> documents;
    QVector<FileTabWidget*> tabWidgets;
    QHash<EditorDocument*, int> savedFirstVisibleLines;
    int activeIndex = -1;
    QString lastEmittedFilePath;

    // Re-entrancy suppression flag
    bool suppressEditorSignals = false;

    // Top flat file tab bar (34px frameless horizontal scroll strip + 1px hairline)
    FileTabScrollArea* tabScrollArea = nullptr;
    QWidget* tabStripContent = nullptr;
    QHBoxLayout* tabStripLayout = nullptr;
    SpellHairline* topHairline = nullptr;

    // Collapsible Find & Replace bar
    QWidget* findBar = nullptr;
    QWidget* findRowsWidget = nullptr;
    QLineEdit* findEdit = nullptr;
    QLabel* matchCountLabel = nullptr;
    SpellChipButton* caseBtn = nullptr;
    SpellChipButton* wordBtn = nullptr;
    SpellChipButton* regexBtn = nullptr;
    SpellIconButton* prevBtn = nullptr;
    SpellIconButton* nextBtn = nullptr;
    SpellIconButton* closeFindBtn = nullptr;
    QLineEdit* replaceEdit = nullptr;
    QPushButton* replaceBtn = nullptr;
    QPushButton* replaceAllBtn = nullptr;
    SpellHairline* findHairline = nullptr;
    bool findMatchActive = false;

    // Main content stack
    QStackedWidget* contentStack = nullptr;

    // Page 0: Minimal empty state ("Open File…" chip button only)
    QWidget* emptyPage = nullptr;
    SpellChipButton* emptyOpenBtn = nullptr;

    // Page 1: Unavailable / binary file banner
    QWidget* unavailablePage = nullptr;
    QLabel* unavailableIconLabel = nullptr;
    QLabel* unavailableFileLabel = nullptr;
    QLabel* unavailableReasonLabel = nullptr;
    QLabel* unavailablePathLabel = nullptr;

    // Page 2: Code editor + inline error banner
    QWidget* editorPage = nullptr;
    QWidget* errorBanner = nullptr;
    QLabel* errorLabel = nullptr;
    SpellIconButton* dismissErrorBtn = nullptr;
    QsciScintilla* editor = nullptr;
    QPointer<QsciLexer> activeLexer;

    // External file change monitoring
    QFileSystemWatcher* fileWatcher = nullptr;
    QTimer* reloadDebounceTimer = nullptr;
    QSet<QString> pendingReloadPaths;
    QSet<QString> selfSavingPaths;

    int indexOfDocument(const EditorDocument* doc) const {
        for (int i = 0; i < documents.size(); ++i) {
            if (documents[i] == doc) {
                return i;
            }
        }
        return -1;
    }

    int indexOfFilePath(const QString& filePath) const {
        if (filePath.isEmpty()) {
            return -1;
        }
        const QString targetKey = canonicalDocumentKey(filePath);
        for (int i = 0; i < documents.size(); ++i) {
            const EditorDocument* doc = documents[i];
            if (!doc) {
                continue;
            }
            if (doc->filePath() == filePath || doc->id() == targetKey ||
                canonicalDocumentKey(doc->filePath()) == targetKey) {
                return i;
            }
        }
        return -1;
    }

    EditorDocument* activeDocument() const {
        if (activeIndex >= 0 && activeIndex < documents.size()) {
            return documents[activeIndex];
        }
        return nullptr;
    }

    void watchPath(const QString& filePath) {
        if (!fileWatcher || filePath.isEmpty()) {
            return;
        }
        if (QFileInfo::exists(filePath) && !fileWatcher->files().contains(filePath)) {
            fileWatcher->addPath(filePath);
        }
    }

    void unwatchPathIfUnused(const QString& filePath, const EditorDocument* excluding = nullptr) {
        if (!fileWatcher || filePath.isEmpty()) {
            return;
        }
        for (const EditorDocument* doc : documents) {
            if (doc && doc != excluding && doc->filePath() == filePath) {
                return;
            }
        }
        if (fileWatcher->files().contains(filePath)) {
            fileWatcher->removePath(filePath);
        }
    }

    void updateLineNumberMarginWidth() {
        if (!editor) {
            return;
        }
        const int lineCount = std::max(1, editor->lines());
        const int digits = std::max(3, static_cast<int>(QString::number(lineCount).size()));
        editor->setMarginWidth(0, QString(digits + 2, QLatin1Char('9')));
    }

    void applyEditorChromeColors(const CodePalette& pal, const QFont& font) {
        if (!editor) {
            return;
        }

        const bool lightEditor = isLightCodeTheme(ThemeManager::instance().codeTheme());
        const QColor caretLineBg = compositeOver(
            withAlpha(lightEditor ? rgbHex(0x000000) : rgbHex(0xFFFFFF), lightEditor ? 0.045 : 0.06),
            pal.background);
        const QColor selectionBg = compositeOver(withAlpha(pal.function, 0.28), pal.background);
        const QColor matchBraceBg = compositeOver(withAlpha(pal.function, 0.24), pal.background);
        const QColor badBraceBg = compositeOver(withAlpha(pal.removed, 0.24), pal.background);
        const QColor indentGuideFg = compositeOver(withAlpha(pal.comment, 0.35), pal.background);
        const QColor foldMarkerBox = compositeOver(withAlpha(pal.comment, 0.85), pal.background);

        QFont marginFont(font);
        marginFont.setPointSizeF(std::max(9.0, font.pointSizeF() - 1.5));

        editor->setColor(pal.plain);
        editor->setPaper(pal.background);
        editor->setFont(font);

        editor->setMarginsFont(marginFont);
        editor->setMarginsBackgroundColor(pal.background);
        editor->setMarginsForegroundColor(pal.comment);

        editor->setFoldMarginColors(pal.background, pal.background);
        for (int marker : {
                 QsciScintillaBase::SC_MARKNUM_FOLDER,
                 QsciScintillaBase::SC_MARKNUM_FOLDEROPEN,
                 QsciScintillaBase::SC_MARKNUM_FOLDERSUB,
                 QsciScintillaBase::SC_MARKNUM_FOLDERTAIL,
                 QsciScintillaBase::SC_MARKNUM_FOLDEREND,
                 QsciScintillaBase::SC_MARKNUM_FOLDEROPENMID,
                 QsciScintillaBase::SC_MARKNUM_FOLDERMIDTAIL
             }) {
            editor->SendScintilla(QsciScintillaBase::SCI_MARKERSETFORE,
                                  static_cast<unsigned long>(marker), pal.background);
            editor->SendScintilla(QsciScintillaBase::SCI_MARKERSETBACK,
                                  static_cast<unsigned long>(marker), foldMarkerBox);
        }

        editor->setCaretForegroundColor(pal.function);
        editor->setCaretLineVisible(true);
        editor->setCaretLineBackgroundColor(caretLineBg);

        editor->setSelectionBackgroundColor(selectionBg);
        editor->resetSelectionForegroundColor();

        editor->setMatchedBraceBackgroundColor(matchBraceBg);
        editor->setMatchedBraceForegroundColor(pal.function);
        editor->setUnmatchedBraceBackgroundColor(badBraceBg);
        editor->setUnmatchedBraceForegroundColor(pal.removed);

        editor->setIndentationGuidesBackgroundColor(pal.background);
        editor->setIndentationGuidesForegroundColor(indentGuideFg);

        const QColor scrollHandle = compositeOver(withAlpha(pal.comment, 0.45), pal.background);
        const QColor scrollHover = compositeOver(withAlpha(pal.plain, 0.45), pal.background);
        editor->setStyleSheet(QStringLiteral(
            "QsciScintilla {\n"
            "  background-color: %1;\n"
            "  border: none;\n"
            "}\n"
            "QsciScintilla QScrollBar:vertical {\n"
            "  background-color: %1;\n"
            "  width: 9px;\n"
            "  margin: 2px 1px 2px 1px;\n"
            "}\n"
            "QsciScintilla QScrollBar::handle:vertical {\n"
            "  background-color: %2;\n"
            "  min-height: 24px;\n"
            "  border-radius: 3px;\n"
            "}\n"
            "QsciScintilla QScrollBar::handle:vertical:hover {\n"
            "  background-color: %3;\n"
            "}\n"
            "QsciScintilla QScrollBar:horizontal {\n"
            "  background-color: %1;\n"
            "  height: 9px;\n"
            "  margin: 1px 2px 1px 2px;\n"
            "}\n"
            "QsciScintilla QScrollBar::handle:horizontal {\n"
            "  background-color: %2;\n"
            "  min-width: 24px;\n"
            "  border-radius: 3px;\n"
            "}\n"
            "QsciScintilla QScrollBar::handle:horizontal:hover {\n"
            "  background-color: %3;\n"
            "}\n"
            "QsciScintilla QScrollBar::add-line, QsciScintilla QScrollBar::sub-line,\n"
            "QsciScintilla QScrollBar::add-page, QsciScintilla QScrollBar::sub-page {\n"
            "  background: transparent;\n"
            "  border: none;\n"
            "  width: 0px;\n"
            "  height: 0px;\n"
            "}\n"
        ).arg(qssColor(pal.background), qssColor(scrollHandle), qssColor(scrollHover)));

        updateLineNumberMarginWidth();
    }

    void attachLexerForDocument(const EditorDocument* doc) {
        if (!editor) {
            return;
        }
        const CodePalette codePal = ThemeManager::instance().code();
        const QFont font = ThemeManager::instance().editorFont();

        QsciLexer* oldLexer = activeLexer.data();
        QsciLexer* newLexer = (doc && doc->isAvailable())
                                  ? createLexerForLanguage(doc->language(), editor)
                                  : nullptr;

        if (newLexer) {
            applyPaletteToLexer(newLexer, codePal, font);
        }

        editor->setLexer(newLexer);
        activeLexer = newLexer;

        if (oldLexer && oldLexer != newLexer) {
            delete oldLexer;
        }

        applyEditorChromeColors(codePal, font);
        if (newLexer) {
            editor->recolor();
        }
    }

    void saveActiveCursorAndScroll() {
        EditorDocument* doc = activeDocument();
        if (!doc || !doc->isAvailable() || !editor) {
            return;
        }
        int line = 0;
        int col = 0;
        editor->getCursorPosition(&line, &col);
        doc->setCursorPosition(line, col);

        const int firstVisible = static_cast<int>(
            editor->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));
        savedFirstVisibleLines.insert(doc, std::max(0, firstVisible));
    }

    void updateTabIndicator(int index) {
        if (index < 0 || index >= documents.size() || index >= tabWidgets.size()) {
            return;
        }
        const EditorDocument* doc = documents[index];
        FileTabWidget* tab = tabWidgets[index];
        if (!doc || !tab) {
            return;
        }
        tab->setTitle(doc->fileName());
        tab->setDirty(doc->isDirty());
        tab->setActive(index == activeIndex);

        QString tip = doc->filePath();
        if (doc->isDirty()) {
            tip += QStringLiteral(" (Unsaved)");
        }
        if (doc->isReadOnly()) {
            tip += QStringLiteral(" [Read-Only]");
        }
        tab->setToolTip(tip);
    }

    void updateTabIndicatorForDocument(const EditorDocument* doc) {
        const int idx = indexOfDocument(doc);
        if (idx >= 0) {
            updateTabIndicator(idx);
        }
    }

    void updateAllTabActiveStates() {
        for (int i = 0; i < tabWidgets.size(); ++i) {
            if (FileTabWidget* tab = tabWidgets[i]) {
                tab->setActive(i == activeIndex);
            }
        }
    }

    void ensureActiveTabVisible() {
        if (!tabScrollArea || activeIndex < 0 || activeIndex >= tabWidgets.size()) {
            return;
        }
        auto scrollToActive = [this]() {
            if (!tabScrollArea || activeIndex < 0 || activeIndex >= tabWidgets.size()) {
                return;
            }
            if (tabStripLayout) {
                tabStripLayout->activate();
            }
            if (tabStripContent) {
                const int vpWidth = tabScrollArea->viewport() ? tabScrollArea->viewport()->width() : 0;
                const int minWidth = tabStripContent->minimumSizeHint().width();
                tabStripContent->resize(std::max(vpWidth, minWidth), kTopBarHeight);
            }
            if (FileTabWidget* activeTab = tabWidgets[activeIndex]) {
                tabScrollArea->ensureWidgetVisible(activeTab, 16, 0);
            }
        };
        scrollToActive();
        if (owner) {
            QTimer::singleShot(0, owner, scrollToActive);
        }
    }

    void selectDocumentAtIndex(int index) {
        if (index < 0 || index >= documents.size() || index == activeIndex) {
            return;
        }
        saveActiveCursorAndScroll();
        activeIndex = index;
        updateAllTabActiveStates();
        ensureActiveTabVisible();
        loadActiveDocumentIntoEditor();
    }

    FileTabWidget* createTabWidget(EditorDocument* doc) {
        QPointer<EditorDocument> guard(doc);
        auto* tab = new FileTabWidget(
            doc ? doc->fileName() : QString(),
            [this, guard]() {
                if (guard) {
                    selectDocumentAtIndex(indexOfDocument(guard.data()));
                }
            },
            [this, guard]() {
                if (guard) {
                    closeDocumentAtIndex(indexOfDocument(guard.data()));
                }
            },
            tabStripContent);
        return tab;
    }

    void updateHeaderAndErrorState() {
        EditorDocument* doc = activeDocument();
        const bool hasDoc = (doc != nullptr);
        const bool available = hasDoc && doc->isAvailable();
        const bool writable = available && !doc->isReadOnly();

        if (replaceBtn) {
            replaceBtn->setEnabled(writable);
        }
        if (replaceAllBtn) {
            replaceAllBtn->setEnabled(writable);
        }

        if (errorBanner && errorLabel) {
            const QString err = hasDoc ? doc->lastError().trimmed() : QString();
            errorLabel->setText(err);
            errorBanner->setVisible(available && !err.isEmpty());
        }
    }

    void loadActiveDocumentIntoEditor() {
        EditorDocument* doc = activeDocument();
        if (!doc) {
            {
                ScopedBoolGuard guard(suppressEditorSignals);
                if (editor) {
                    editor->cancelFind();
                    editor->setReadOnly(false);
                    editor->setText(QString());
                    editor->setModified(false);
                    QsciLexer* oldLexer = activeLexer.data();
                    editor->setLexer(nullptr);
                    activeLexer = nullptr;
                    delete oldLexer;
                }
            }
            if (contentStack && emptyPage) {
                contentStack->setCurrentWidget(emptyPage);
            }
            updateHeaderAndErrorState();
            updateMatchCount();
            emitCurrentFileChangedIfNeeded(QString());
            return;
        }

        if (!doc->isAvailable()) {
            if (unavailableFileLabel) {
                unavailableFileLabel->setText(doc->fileName());
            }
            if (unavailableReasonLabel) {
                unavailableReasonLabel->setText(doc->unavailableReason());
            }
            if (unavailablePathLabel) {
                unavailablePathLabel->setText(doc->filePath());
            }
            if (contentStack && unavailablePage) {
                contentStack->setCurrentWidget(unavailablePage);
            }
            updateHeaderAndErrorState();
            updateMatchCount();
            emitCurrentFileChangedIfNeeded(doc->filePath());
            return;
        }

        {
            ScopedBoolGuard guard(suppressEditorSignals);
            editor->cancelFind();
            editor->setReadOnly(false);
            editor->setText(doc->text());
            editor->setModified(doc->isDirty());
            attachLexerForDocument(doc);
            editor->setReadOnly(doc->isReadOnly());

            const int maxLine = std::max(0, editor->lines() - 1);
            const int targetLine = std::clamp(doc->cursorLine(), 0, maxLine);
            const int lineLen = static_cast<int>(editor->text(targetLine).length());
            const int targetCol = std::clamp(doc->cursorCol(), 0, std::max(0, lineLen));
            editor->setCursorPosition(targetLine, targetCol);

            if (savedFirstVisibleLines.contains(doc)) {
                const int firstVisible = std::clamp(savedFirstVisibleLines.value(doc), 0, maxLine);
                editor->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                                      static_cast<unsigned long>(firstVisible), 0L);
            } else {
                editor->ensureLineVisible(targetLine);
            }
            updateLineNumberMarginWidth();
        }

        if (contentStack && editorPage) {
            contentStack->setCurrentWidget(editorPage);
        }
        updateHeaderAndErrorState();
        updateMatchCount();
        emitCurrentFileChangedIfNeeded(doc->filePath());
    }

    void syncActiveDocumentReloaded(EditorDocument* doc, const QString& newText) {
        if (!doc || doc != activeDocument() || !editor) {
            return;
        }

        if (!doc->isAvailable()) {
            loadActiveDocumentIntoEditor();
            return;
        }

        int oldLine = 0;
        int oldCol = 0;
        editor->getCursorPosition(&oldLine, &oldCol);
        const int oldFirstVisible = static_cast<int>(
            editor->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE));

        {
            ScopedBoolGuard guard(suppressEditorSignals);
            editor->cancelFind();
            editor->setReadOnly(false);
            editor->setText(newText);
            editor->setModified(false);
            editor->setReadOnly(doc->isReadOnly());

            const int maxLine = std::max(0, editor->lines() - 1);
            const int clampedLine = std::clamp(oldLine, 0, maxLine);
            const int maxCol = static_cast<int>(editor->text(clampedLine).length());
            const int clampedCol = std::clamp(oldCol, 0, std::max(0, maxCol));
            editor->setCursorPosition(clampedLine, clampedCol);
            doc->setCursorPosition(clampedLine, clampedCol);

            const int clampedFirst = std::clamp(oldFirstVisible, 0, maxLine);
            editor->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                                  static_cast<unsigned long>(clampedFirst), 0L);
            updateLineNumberMarginWidth();
        }

        if (contentStack && editorPage && contentStack->currentWidget() != editorPage) {
            attachLexerForDocument(doc);
            contentStack->setCurrentWidget(editorPage);
        }
        updateHeaderAndErrorState();
        updateTabIndicatorForDocument(doc);
        updateMatchCount();
        emit owner->statusMessage(QStringLiteral("Reloaded %1 from disk").arg(doc->fileName()));
    }

    void jumpToLine(int oneBasedLine) {
        EditorDocument* doc = activeDocument();
        if (!doc || !doc->isAvailable() || !editor || oneBasedLine <= 0) {
            return;
        }
        const int maxLine = std::max(0, editor->lines() - 1);
        const int targetLine = std::clamp(oneBasedLine - 1, 0, maxLine);

        editor->ensureLineVisible(targetLine);
        editor->setCursorPosition(targetLine, 0);
        editor->ensureCursorVisible();

        const int linesOnScreen = static_cast<int>(
            editor->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN));
        if (linesOnScreen > 2) {
            const int firstVisible = std::max(0, targetLine - (linesOnScreen / 3));
            editor->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                                  static_cast<unsigned long>(firstVisible), 0L);
        }

        doc->setCursorPosition(targetLine, 0);
    }

    void emitCurrentFileChangedIfNeeded(const QString& newPath) {
        if (lastEmittedFilePath != newPath) {
            lastEmittedFilePath = newPath;
            emit owner->currentFileChanged(newPath);
        }
    }

    void closeDocumentAtIndex(int index) {
        if (index < 0 || index >= documents.size()) {
            return;
        }

        EditorDocument* doc = documents[index];
        const bool wasActive = (index == activeIndex);
        if (!wasActive) {
            saveActiveCursorAndScroll();
        }

        const QString closingPath = doc ? doc->filePath() : QString();
        unwatchPathIfUnused(closingPath, doc);
        pendingReloadPaths.remove(closingPath);
        savedFirstVisibleLines.remove(doc);

        if (doc) {
            QObject::disconnect(doc, nullptr, owner, nullptr);
        }

        documents.removeAt(index);
        if (index < tabWidgets.size()) {
            FileTabWidget* tab = tabWidgets.takeAt(index);
            if (tab) {
                if (tabStripLayout) {
                    tabStripLayout->removeWidget(tab);
                }
                tab->hide();
                tab->deleteLater();
            }
        }

        if (documents.isEmpty()) {
            activeIndex = -1;
        } else if (wasActive) {
            activeIndex = std::clamp(index, 0, static_cast<int>(documents.size()) - 1);
        } else if (index < activeIndex) {
            --activeIndex;
        }

        updateAllTabActiveStates();
        if (tabStripContent) {
            tabStripContent->updateGeometry();
            if (documents.isEmpty() && tabScrollArea) {
                if (QScrollBar* hBar = tabScrollArea->horizontalScrollBar()) {
                    hBar->setValue(0);
                }
                const int vpWidth = tabScrollArea->viewport() ? tabScrollArea->viewport()->width() : 0;
                tabStripContent->resize(vpWidth, kTopBarHeight);
            }
        }

        if (doc) {
            doc->deleteLater();
        }

        if (wasActive || documents.isEmpty()) {
            loadActiveDocumentIntoEditor();
        } else {
            updateHeaderAndErrorState();
        }

        ensureActiveTabVisible();
        emit owner->openDocumentsChanged();
    }

    // -------------------------------------------------------------------------
    // Find & Replace helpers
    // -------------------------------------------------------------------------

    QRegularExpression buildFindRegex(const QString& query, bool* outValid) const {
        if (outValid) {
            *outValid = true;
        }
        if (query.isEmpty()) {
            return {};
        }

        const bool useRegex = regexBtn && regexBtn->isChecked();
        const bool wholeWord = wordBtn && wordBtn->isChecked();
        const bool caseSens = caseBtn && caseBtn->isChecked();

        QString pattern = useRegex ? query : QRegularExpression::escape(query);
        if (wholeWord) {
            pattern = QStringLiteral("\\b(?:%1)\\b").arg(pattern);
        }

        QRegularExpression::PatternOptions opts = QRegularExpression::UseUnicodePropertiesOption;
        if (!caseSens) {
            opts |= QRegularExpression::CaseInsensitiveOption;
        }
        QRegularExpression rx(pattern, opts);
        if (!rx.isValid() && outValid) {
            *outValid = false;
        }
        return rx;
    }

    void updateMatchCount() {
        if (!matchCountLabel) {
            return;
        }
        const UiPalette u = ThemeManager::instance().ui();
        const QColor mutedCol = compositeOver(u.textMuted, u.background);

        const EditorDocument* doc = activeDocument();
        const QString query = findEdit ? findEdit->text() : QString();
        if (!doc || !doc->isAvailable() || !editor || query.isEmpty()) {
            matchCountLabel->setText(QString());
            matchCountLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                               .arg(qssColor(mutedCol)));
            return;
        }

        bool valid = true;
        const QRegularExpression rx = buildFindRegex(query, &valid);
        if (!valid) {
            matchCountLabel->setText(QStringLiteral("Invalid regex"));
            matchCountLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                               .arg(qssColor(u.danger)));
            return;
        }

        const QString text = editor->text();
        if (text.size() > kMatchCountCharLimit) {
            matchCountLabel->setText(findMatchActive ? QStringLiteral("Found") : QStringLiteral("Large file"));
            matchCountLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                               .arg(qssColor(mutedCol)));
            return;
        }

        qsizetype selStart = -1;
        qsizetype selEnd = -1;
        if (editor->hasSelectedText()) {
            int l1 = 0, i1 = 0, l2 = 0, i2 = 0;
            editor->getSelection(&l1, &i1, &l2, &i2);
            selStart = charOffsetForLineIndex(text, l1, i1);
            selEnd = charOffsetForLineIndex(text, l2, i2);
        }

        int total = 0;
        int currentOrdinal = 0;
        QRegularExpressionMatchIterator it = rx.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            ++total;
            if (selStart >= 0 && m.capturedStart() == selStart && m.capturedEnd() == selEnd) {
                currentOrdinal = total;
            }
        }

        if (total == 0) {
            matchCountLabel->setText(QStringLiteral("No matches"));
            matchCountLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                               .arg(qssColor(u.danger)));
        } else if (currentOrdinal > 0) {
            matchCountLabel->setText(QStringLiteral("%1 / %2").arg(currentOrdinal).arg(total));
            matchCountLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                               .arg(qssColor(mutedCol)));
        } else {
            matchCountLabel->setText(QStringLiteral("%1 match%2")
                                         .arg(total)
                                         .arg(total == 1 ? QString() : QStringLiteral("es")));
            matchCountLabel->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                               .arg(qssColor(mutedCol)));
        }
    }

    bool findMatch(bool forward, bool incremental = false) {
        const EditorDocument* doc = activeDocument();
        if (!doc || !doc->isAvailable() || !editor || !findEdit) {
            findMatchActive = false;
            updateMatchCount();
            return false;
        }

        const QString query = findEdit->text();
        if (query.isEmpty()) {
            editor->cancelFind();
            findMatchActive = false;
            updateMatchCount();
            return false;
        }

        const bool useRegex = regexBtn && regexBtn->isChecked();
        const bool caseSens = caseBtn && caseBtn->isChecked();
        const bool wholeWord = wordBtn && wordBtn->isChecked();

        if (useRegex) {
            bool valid = true;
            buildFindRegex(query, &valid);
            if (!valid) {
                findMatchActive = false;
                updateMatchCount();
                return false;
            }
        }

        int startLine = 0;
        int startIndex = 0;
        if (editor->hasSelectedText()) {
            int l1 = 0, i1 = 0, l2 = 0, i2 = 0;
            editor->getSelection(&l1, &i1, &l2, &i2);
            if (forward) {
                startLine = incremental ? l1 : l2;
                startIndex = incremental ? i1 : i2;
            } else {
                startLine = l1;
                startIndex = i1;
            }
        } else {
            editor->getCursorPosition(&startLine, &startIndex);
        }

        // When searching backward from (0, 0), start from the end of the file so wrap always succeeds.
        if (!forward && startLine == 0 && startIndex == 0) {
            startLine = std::max(0, editor->lines() - 1);
            startIndex = static_cast<int>(editor->text(startLine).length());
        }

        const bool found = editor->findFirst(
            query,
            useRegex,
            caseSens,
            wholeWord,
            /*wrap=*/true,
            forward,
            startLine,
            startIndex,
            /*show=*/true,
            /*posix=*/false,
            /*cxx11=*/useRegex);

        findMatchActive = found;
        updateMatchCount();
        return found;
    }

    bool selectionMatchesCurrentQuery() const {
        if (!editor || !findEdit || !editor->hasSelectedText()) {
            return false;
        }
        const QString query = findEdit->text();
        if (query.isEmpty()) {
            return false;
        }
        bool valid = true;
        const QRegularExpression rx = buildFindRegex(query, &valid);
        if (!valid) {
            return false;
        }
        const QString selected = editor->selectedText();
        const QRegularExpressionMatch m = rx.match(selected);
        return m.hasMatch() && m.capturedStart() == 0 && m.capturedLength() == selected.length();
    }

    void replaceCurrentMatch() {
        EditorDocument* doc = activeDocument();
        if (!doc || !doc->isAvailable() || doc->isReadOnly() || !editor || !findEdit || !replaceEdit) {
            return;
        }
        const QString query = findEdit->text();
        if (query.isEmpty()) {
            return;
        }

        if (!selectionMatchesCurrentQuery()) {
            findMatch(/*forward=*/true, /*incremental=*/false);
            return;
        }

        const QString replacement = replaceEdit->text();
        const bool useRegex = regexBtn && regexBtn->isChecked();
        if (useRegex && !findMatchActive) {
            int l1 = 0, i1 = 0, l2 = 0, i2 = 0;
            editor->getSelection(&l1, &i1, &l2, &i2);
            const bool caseSens = caseBtn && caseBtn->isChecked();
            const bool wholeWord = wordBtn && wordBtn->isChecked();
            findMatchActive = editor->findFirst(
                query,
                true,
                caseSens,
                wholeWord,
                /*wrap=*/false,
                /*forward=*/true,
                l1,
                i1,
                /*show=*/false,
                /*posix=*/false,
                /*cxx11=*/true);
        }

        if (findMatchActive) {
            editor->replace(replacement);
        } else {
            editor->replaceSelectedText(replacement);
        }
        findMatchActive = false;
        findMatch(/*forward=*/true, /*incremental=*/false);
    }

    void replaceAllMatches() {
        EditorDocument* doc = activeDocument();
        if (!doc || !doc->isAvailable() || doc->isReadOnly() || !editor || !findEdit || !replaceEdit) {
            return;
        }
        const QString query = findEdit->text();
        if (query.isEmpty()) {
            return;
        }

        const bool useRegex = regexBtn && regexBtn->isChecked();
        const bool caseSens = caseBtn && caseBtn->isChecked();
        const bool wholeWord = wordBtn && wordBtn->isChecked();
        if (useRegex) {
            bool valid = true;
            buildFindRegex(query, &valid);
            if (!valid) {
                updateMatchCount();
                return;
            }
        }

        const QString replacement = replaceEdit->text();
        int searchLine = 0;
        int searchIndex = 0;
        int replacedCount = 0;

        editor->beginUndoAction();
        while (true) {
            const bool found = editor->findFirst(
                query,
                useRegex,
                caseSens,
                wholeWord,
                /*wrap=*/false,
                /*forward=*/true,
                searchLine,
                searchIndex,
                /*show=*/false,
                /*posix=*/false,
                /*cxx11=*/useRegex);
            if (!found) {
                break;
            }

            int l1 = 0, i1 = 0, l2 = 0, i2 = 0;
            editor->getSelection(&l1, &i1, &l2, &i2);
            const bool zeroWidth = (l1 == l2 && i1 == i2);

            editor->replace(replacement);
            ++replacedCount;

            editor->getCursorPosition(&searchLine, &searchIndex);
            if (zeroWidth) {
                const int lineLen = static_cast<int>(editor->text(searchLine).length());
                if (searchIndex < lineLen) {
                    ++searchIndex;
                } else if (searchLine + 1 < editor->lines()) {
                    ++searchLine;
                    searchIndex = 0;
                } else {
                    break;
                }
            }
        }
        editor->endUndoAction();

        editor->cancelFind();
        findMatchActive = false;
        updateMatchCount();
        emit owner->statusMessage(
            replacedCount > 0
                ? QStringLiteral("Replaced %1 occurrence%2")
                      .arg(replacedCount)
                      .arg(replacedCount == 1 ? QString() : QStringLiteral("s"))
                : QStringLiteral("No matches replaced"));
    }

    void hideFindBar() {
        if (findBar) {
            findBar->hide();
        }
        if (editor) {
            editor->cancelFind();
            if (const EditorDocument* doc = activeDocument(); doc && doc->isAvailable()) {
                editor->setFocus();
            }
        }
        findMatchActive = false;
    }

    void processPendingDiskReloads() {
        if (pendingReloadPaths.isEmpty()) {
            return;
        }
        const QSet<QString> paths = std::exchange(pendingReloadPaths, {});
        for (const QString& path : paths) {
            const int idx = indexOfFilePath(path);
            if (idx < 0 || idx >= documents.size()) {
                continue;
            }
            EditorDocument* doc = documents[idx];
            if (!doc) {
                continue;
            }
            // Re-watch file after atomic save/replace (which swaps the inode on POSIX/macOS).
            watchPath(doc->filePath());

            if (!doc->isDirty()) {
                doc->reloadIfChangedOnDisk();
            } else if (doc == activeDocument()) {
                emit owner->statusMessage(
                    QStringLiteral("%1 changed on disk (unsaved edits kept)").arg(doc->fileName()));
            }
        }
    }
};

// =============================================================================
// CodeEditorPane
// =============================================================================

CodeEditorPane::CodeEditorPane(QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // -------------------------------------------------------------------------
    // Top FileTabBar: pure flat 34px horizontal scroll strip + 1px hairline
    // -------------------------------------------------------------------------
    m_impl->tabScrollArea = new FileTabScrollArea(this);
    m_impl->tabScrollArea->setObjectName(QStringLiteral("spellFileTabBar"));
    m_impl->tabScrollArea->setAttribute(Qt::WA_StyledBackground, true);

    m_impl->tabStripContent = new QWidget(m_impl->tabScrollArea);
    m_impl->tabStripContent->setObjectName(QStringLiteral("spellFileTabStrip"));
    m_impl->tabStripContent->setAttribute(Qt::WA_StyledBackground, true);
    m_impl->tabStripContent->setFixedHeight(kTopBarHeight);
    m_impl->tabStripContent->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);

    m_impl->tabStripLayout = new QHBoxLayout(m_impl->tabStripContent);
    m_impl->tabStripLayout->setContentsMargins(0, 0, 0, 0);
    m_impl->tabStripLayout->setSpacing(0);
    m_impl->tabStripLayout->addStretch(1);

    m_impl->tabScrollArea->setWidget(m_impl->tabStripContent);
    installWindowDragRegion(m_impl->tabStripContent);
    if (QWidget* vp = m_impl->tabScrollArea->viewport()) {
        installWindowDragRegion(vp);
    }
    rootLayout->addWidget(m_impl->tabScrollArea);

    m_impl->topHairline = new SpellHairline(Qt::Horizontal, this);
    rootLayout->addWidget(m_impl->topHairline);

    // -------------------------------------------------------------------------
    // Collapsible Find & Replace bar
    // -------------------------------------------------------------------------
    m_impl->findBar = new QWidget(this);
    m_impl->findBar->setObjectName(QStringLiteral("spellEditorFindBar"));
    m_impl->findBar->setAttribute(Qt::WA_StyledBackground, true);
    m_impl->findBar->setVisible(false);

    auto* findRootLayout = new QVBoxLayout(m_impl->findBar);
    findRootLayout->setContentsMargins(0, 0, 0, 0);
    findRootLayout->setSpacing(0);

    m_impl->findRowsWidget = new QWidget(m_impl->findBar);
    m_impl->findRowsWidget->setObjectName(QStringLiteral("spellEditorFindRows"));
    m_impl->findRowsWidget->setAttribute(Qt::WA_StyledBackground, true);
    auto* findRowsWidget = m_impl->findRowsWidget;
    auto* findRowsLayout = new QVBoxLayout(findRowsWidget);
    findRowsLayout->setContentsMargins(10, 6, 10, 6);
    findRowsLayout->setSpacing(5);

    // Row 1: Find input + match count + Aa/W/.* + Prev/Next + Close
    auto* findRow = new QHBoxLayout();
    findRow->setContentsMargins(0, 0, 0, 0);
    findRow->setSpacing(5);

    m_impl->findEdit = new QLineEdit(findRowsWidget);
    m_impl->findEdit->setPlaceholderText(QStringLiteral("Find..."));
    m_impl->findEdit->setClearButtonEnabled(true);
    m_impl->findEdit->setFixedHeight(26);
    findRow->addWidget(m_impl->findEdit, 1);

    m_impl->matchCountLabel = new QLabel(findRowsWidget);
    m_impl->matchCountLabel->setMinimumWidth(72);
    m_impl->matchCountLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    findRow->addWidget(m_impl->matchCountLabel);

    m_impl->caseBtn = new SpellChipButton(QStringLiteral("Aa"), BadgeTone::Accent, findRowsWidget);
    m_impl->caseBtn->setToolTip(QStringLiteral("Match Case"));
    m_impl->caseBtn->setFixedHeight(24);
    findRow->addWidget(m_impl->caseBtn);

    m_impl->wordBtn = new SpellChipButton(QStringLiteral("W"), BadgeTone::Accent, findRowsWidget);
    m_impl->wordBtn->setToolTip(QStringLiteral("Whole Word"));
    m_impl->wordBtn->setFixedHeight(24);
    findRow->addWidget(m_impl->wordBtn);

    m_impl->regexBtn = new SpellChipButton(QStringLiteral(".*"), BadgeTone::Accent, findRowsWidget);
    m_impl->regexBtn->setToolTip(QStringLiteral("Regular Expression"));
    m_impl->regexBtn->setFixedHeight(24);
    findRow->addWidget(m_impl->regexBtn);

    m_impl->prevBtn = new SpellIconButton(
        QStringLiteral("chevron-up"),
        QStringLiteral("Find Previous (Shift+Enter)"),
        BadgeTone::Neutral,
        findRowsWidget);
    findRow->addWidget(m_impl->prevBtn);

    m_impl->nextBtn = new SpellIconButton(
        QStringLiteral("chevron-down"),
        QStringLiteral("Find Next (Enter)"),
        BadgeTone::Neutral,
        findRowsWidget);
    findRow->addWidget(m_impl->nextBtn);

    m_impl->closeFindBtn = new SpellIconButton(
        QStringLiteral("close"),
        QStringLiteral("Close Find Bar (Esc)"),
        BadgeTone::Neutral,
        findRowsWidget);
    findRow->addWidget(m_impl->closeFindBtn);

    findRowsLayout->addLayout(findRow);

    // Row 2: Replace input + Replace + Replace All
    auto* replaceRow = new QHBoxLayout();
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->setSpacing(5);

    m_impl->replaceEdit = new QLineEdit(findRowsWidget);
    m_impl->replaceEdit->setPlaceholderText(QStringLiteral("Replace..."));
    m_impl->replaceEdit->setClearButtonEnabled(true);
    m_impl->replaceEdit->setFixedHeight(26);
    replaceRow->addWidget(m_impl->replaceEdit, 1);

    m_impl->replaceBtn = new QPushButton(QStringLiteral("Replace"), findRowsWidget);
    m_impl->replaceBtn->setObjectName(QStringLiteral("spellReplaceBtn"));
    m_impl->replaceBtn->setFixedHeight(26);
    m_impl->replaceBtn->setCursor(Qt::PointingHandCursor);
    replaceRow->addWidget(m_impl->replaceBtn);

    m_impl->replaceAllBtn = new QPushButton(QStringLiteral("Replace All"), findRowsWidget);
    m_impl->replaceAllBtn->setObjectName(QStringLiteral("spellReplaceAllBtn"));
    m_impl->replaceAllBtn->setFixedHeight(26);
    m_impl->replaceAllBtn->setCursor(Qt::PointingHandCursor);
    replaceRow->addWidget(m_impl->replaceAllBtn);

    findRowsLayout->addLayout(replaceRow);
    findRootLayout->addWidget(findRowsWidget);

    m_impl->findHairline = new SpellHairline(Qt::Horizontal, m_impl->findBar);
    findRootLayout->addWidget(m_impl->findHairline);

    rootLayout->addWidget(m_impl->findBar);

    // Keyboard filter for Find & Replace bar (Esc closes; Enter/Shift+Enter navigates)
    auto* findKeyFilter = new EventFilterRelay(
        [this](QObject* watched, QEvent* event) -> bool {
            if (event->type() != QEvent::KeyPress) {
                return false;
            }
            const auto* ke = static_cast<const QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Escape) {
                m_impl->hideFindBar();
                return true;
            }
            if (watched == m_impl->findEdit &&
                (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)) {
                const bool backward = ke->modifiers().testFlag(Qt::ShiftModifier);
                m_impl->findMatch(!backward, /*incremental=*/false);
                return true;
            }
            if (watched == m_impl->replaceEdit &&
                (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)) {
                m_impl->replaceCurrentMatch();
                return true;
            }
            return false;
        },
        m_impl->findBar);
    m_impl->findBar->installEventFilter(findKeyFilter);
    m_impl->findEdit->installEventFilter(findKeyFilter);
    m_impl->replaceEdit->installEventFilter(findKeyFilter);

    // -------------------------------------------------------------------------
    // Main content stack: Empty State / Unavailable Banner / QsciScintilla
    // -------------------------------------------------------------------------
    m_impl->contentStack = new QStackedWidget(this);

    // Page 0: Minimal EmptyEditorView (monochrome "Open File…" chip button only)
    m_impl->emptyPage = new QWidget(m_impl->contentStack);
    m_impl->emptyPage->setObjectName(QStringLiteral("spellEditorEmptyPage"));
    m_impl->emptyPage->setAttribute(Qt::WA_StyledBackground, true);
    auto* emptyLayout = new QVBoxLayout(m_impl->emptyPage);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    emptyLayout->setSpacing(10);
    emptyLayout->addStretch(1);

    const QString openShortcut = QKeySequence(QKeySequence::Open).toString(QKeySequence::NativeText);
    const QString openButtonLabel = openShortcut.isEmpty()
        ? QStringLiteral("Open File\u2026")
        : QStringLiteral("Open File\u2026  %1").arg(openShortcut);

    m_impl->emptyOpenBtn = new SpellChipButton(
        openButtonLabel,
        QStringLiteral("file"),
        BadgeTone::Accent,
        m_impl->emptyPage);
    m_impl->emptyOpenBtn->setObjectName(QStringLiteral("spellEditorEmptyOpenButton"));
    m_impl->emptyOpenBtn->setCheckable(false);
    m_impl->emptyOpenBtn->setAutoDefault(false);
    m_impl->emptyOpenBtn->setDefault(false);
    m_impl->emptyOpenBtn->setToolTip(openButtonLabel);
    emptyLayout->addWidget(m_impl->emptyOpenBtn, 0, Qt::AlignHCenter);

    emptyLayout->addStretch(1);
    m_impl->contentStack->addWidget(m_impl->emptyPage);

    // Page 1: Unavailable / binary / oversized file banner
    m_impl->unavailablePage = new QWidget(m_impl->contentStack);
    m_impl->unavailablePage->setObjectName(QStringLiteral("spellEditorUnavailablePage"));
    m_impl->unavailablePage->setAttribute(Qt::WA_StyledBackground, true);
    auto* unavailLayout = new QVBoxLayout(m_impl->unavailablePage);
    unavailLayout->setContentsMargins(32, 32, 32, 32);
    unavailLayout->setSpacing(8);
    unavailLayout->addStretch(1);

    m_impl->unavailableIconLabel = new QLabel(m_impl->unavailablePage);
    m_impl->unavailableIconLabel->setAlignment(Qt::AlignCenter);
    unavailLayout->addWidget(m_impl->unavailableIconLabel, 0, Qt::AlignHCenter);

    m_impl->unavailableFileLabel = new QLabel(m_impl->unavailablePage);
    m_impl->unavailableFileLabel->setAlignment(Qt::AlignCenter);
    unavailLayout->addWidget(m_impl->unavailableFileLabel, 0, Qt::AlignHCenter);

    m_impl->unavailableReasonLabel = new QLabel(m_impl->unavailablePage);
    m_impl->unavailableReasonLabel->setAlignment(Qt::AlignCenter);
    m_impl->unavailableReasonLabel->setWordWrap(true);
    unavailLayout->addWidget(m_impl->unavailableReasonLabel, 0, Qt::AlignHCenter);

    m_impl->unavailablePathLabel = new QLabel(m_impl->unavailablePage);
    m_impl->unavailablePathLabel->setAlignment(Qt::AlignCenter);
    m_impl->unavailablePathLabel->setWordWrap(true);
    m_impl->unavailablePathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    unavailLayout->addWidget(m_impl->unavailablePathLabel, 0, Qt::AlignHCenter);

    unavailLayout->addStretch(1);
    m_impl->contentStack->addWidget(m_impl->unavailablePage);

    // Page 2: Editor page (inline error banner + QsciScintilla)
    m_impl->editorPage = new QWidget(m_impl->contentStack);
    auto* editorPageLayout = new QVBoxLayout(m_impl->editorPage);
    editorPageLayout->setContentsMargins(0, 0, 0, 0);
    editorPageLayout->setSpacing(0);

    m_impl->errorBanner = new QWidget(m_impl->editorPage);
    m_impl->errorBanner->setObjectName(QStringLiteral("spellEditorErrorBanner"));
    m_impl->errorBanner->setAttribute(Qt::WA_StyledBackground, true);
    m_impl->errorBanner->setVisible(false);
    auto* errLayout = new QHBoxLayout(m_impl->errorBanner);
    errLayout->setContentsMargins(10, 5, 8, 5);
    errLayout->setSpacing(8);

    m_impl->errorLabel = new QLabel(m_impl->errorBanner);
    m_impl->errorLabel->setWordWrap(true);
    errLayout->addWidget(m_impl->errorLabel, 1);

    m_impl->dismissErrorBtn = new SpellIconButton(
        QStringLiteral("close"),
        QStringLiteral("Dismiss"),
        BadgeTone::Danger,
        m_impl->errorBanner);
    errLayout->addWidget(m_impl->dismissErrorBtn);

    editorPageLayout->addWidget(m_impl->errorBanner);

    m_impl->editor = new QsciScintilla(m_impl->editorPage);
    m_impl->editor->setFrameShape(QFrame::NoFrame);
    m_impl->editor->setUtf8(true);
    m_impl->editor->setEolVisibility(false);
    m_impl->editor->setWrapMode(QsciScintilla::WrapNone);
    m_impl->editor->setAutoIndent(true);
    m_impl->editor->setTabIndents(true);
    m_impl->editor->setBackspaceUnindents(true);
    m_impl->editor->setTabWidth(4);
    m_impl->editor->setIndentationWidth(4);
    m_impl->editor->setIndentationsUseTabs(false);
    m_impl->editor->setIndentationGuides(true);
    m_impl->editor->setBraceMatching(QsciScintilla::SloppyBraceMatch);
    m_impl->editor->setCaretLineVisible(true);
    m_impl->editor->setCaretWidth(2);
    m_impl->editor->setMarginType(0, QsciScintilla::NumberMargin);
    m_impl->editor->setMarginLineNumbers(0, true);
    m_impl->editor->setMarginWidth(1, 0);
    m_impl->editor->setFolding(QsciScintilla::BoxedTreeFoldStyle, 2);
    m_impl->editor->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTH, 1UL, 0L);
    m_impl->editor->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTHTRACKING, 1UL, 0L);
    m_impl->editor->SendScintilla(QsciScintillaBase::SCI_SETMARGINLEFT, 0UL, 6L);
    m_impl->editor->SendScintilla(QsciScintillaBase::SCI_SETEXTRAASCENT, 1UL, 0L);
    m_impl->editor->SendScintilla(QsciScintillaBase::SCI_SETEXTRADESCENT, 1UL, 0L);

    editorPageLayout->addWidget(m_impl->editor, 1);
    m_impl->contentStack->addWidget(m_impl->editorPage);
    m_impl->contentStack->setCurrentWidget(m_impl->emptyPage);

    rootLayout->addWidget(m_impl->contentStack, 1);

    // -------------------------------------------------------------------------
    // External file watcher + debounce timer
    // -------------------------------------------------------------------------
    m_impl->fileWatcher = new QFileSystemWatcher(this);
    m_impl->reloadDebounceTimer = new QTimer(this);
    m_impl->reloadDebounceTimer->setSingleShot(true);
    m_impl->reloadDebounceTimer->setInterval(kReloadDebounceMs);

    connect(m_impl->fileWatcher, &QFileSystemWatcher::fileChanged, this,
            [this](const QString& path) {
                if (m_impl->selfSavingPaths.contains(path)) {
                    return;
                }
                m_impl->pendingReloadPaths.insert(path);
                m_impl->reloadDebounceTimer->start();
            });
    connect(m_impl->reloadDebounceTimer, &QTimer::timeout, this, [this]() {
        m_impl->processPendingDiskReloads();
    });

    // -------------------------------------------------------------------------
    // Empty-state & banner action connections
    // -------------------------------------------------------------------------
    connect(m_impl->emptyOpenBtn, &QPushButton::clicked, this, [this]() {
        const bool hasListener = isSignalConnected(
            QMetaMethod::fromSignal(&CodeEditorPane::openFileDialogRequested));
        emit openFileDialogRequested();
        if (!hasListener) {
            const QString selected = QFileDialog::getOpenFileName(
                this,
                tr("Open File"),
                QString());
            if (!selected.isEmpty()) {
                openFile(QDir::cleanPath(selected));
            }
        }
    });

    connect(m_impl->dismissErrorBtn, &QToolButton::clicked, this, [this]() {
        if (m_impl->errorBanner) {
            m_impl->errorBanner->hide();
        }
    });

    // -------------------------------------------------------------------------
    // Find & Replace connections
    // -------------------------------------------------------------------------
    connect(m_impl->findEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        m_impl->findMatch(/*forward=*/true, /*incremental=*/true);
    });
    connect(m_impl->caseBtn, &QAbstractButton::toggled, this, [this](bool) {
        m_impl->findMatch(/*forward=*/true, /*incremental=*/true);
    });
    connect(m_impl->wordBtn, &QAbstractButton::toggled, this, [this](bool) {
        m_impl->findMatch(/*forward=*/true, /*incremental=*/true);
    });
    connect(m_impl->regexBtn, &QAbstractButton::toggled, this, [this](bool) {
        m_impl->findMatch(/*forward=*/true, /*incremental=*/true);
    });
    connect(m_impl->nextBtn, &QToolButton::clicked, this, [this]() {
        m_impl->findMatch(/*forward=*/true, /*incremental=*/false);
    });
    connect(m_impl->prevBtn, &QToolButton::clicked, this, [this]() {
        m_impl->findMatch(/*forward=*/false, /*incremental=*/false);
    });
    connect(m_impl->closeFindBtn, &QToolButton::clicked, this, [this]() {
        m_impl->hideFindBar();
    });
    connect(m_impl->replaceBtn, &QPushButton::clicked, this, [this]() {
        m_impl->replaceCurrentMatch();
    });
    connect(m_impl->replaceAllBtn, &QPushButton::clicked, this, [this]() {
        m_impl->replaceAllMatches();
    });

    // -------------------------------------------------------------------------
    // QsciScintilla editor connections
    // -------------------------------------------------------------------------
    connect(m_impl->editor, &QsciScintilla::textChanged, this, [this]() {
        if (m_impl->suppressEditorSignals) {
            return;
        }
        if (EditorDocument* doc = m_impl->activeDocument()) {
            if (doc->isAvailable() && !doc->isReadOnly()) {
                doc->setTextFromEditor(m_impl->editor->text());
                m_impl->updateTabIndicatorForDocument(doc);
                m_impl->updateHeaderAndErrorState();
            }
        }
        if (m_impl->findBar && m_impl->findBar->isVisible()) {
            m_impl->updateMatchCount();
        }
    });

    connect(m_impl->editor, &QsciScintilla::cursorPositionChanged, this,
            [this](int line, int index) {
                if (m_impl->suppressEditorSignals) {
                    return;
                }
                if (EditorDocument* doc = m_impl->activeDocument()) {
                    doc->setCursorPosition(line, index);
                }
            });

    connect(m_impl->editor, &QsciScintilla::linesChanged, this, [this]() {
        m_impl->updateLineNumberMarginWidth();
    });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &CodeEditorPane::applyTheme);

    applyTheme();
}

CodeEditorPane::~CodeEditorPane() {
    if (m_impl && m_impl->reloadDebounceTimer) {
        m_impl->reloadDebounceTimer->stop();
    }
    if (m_impl && m_impl->editor) {
        QsciLexer* oldLexer = m_impl->activeLexer.data();
        m_impl->editor->setLexer(nullptr);
        m_impl->activeLexer = nullptr;
        delete oldLexer;
    }
}

QString CodeEditorPane::currentFilePath() const {
    if (const EditorDocument* doc = currentDocument()) {
        return doc->filePath();
    }
    return {};
}

int CodeEditorPane::openDocumentCount() const {
    return static_cast<int>(m_impl->documents.size());
}

QsciScintilla* CodeEditorPane::scintilla() const {
    return m_impl->editor;
}

QStringList CodeEditorPane::openFilePaths() const {
    QStringList paths;
    paths.reserve(m_impl->documents.size());
    for (const EditorDocument* doc : m_impl->documents) {
        if (doc) {
            paths.append(doc->filePath());
        }
    }
    return paths;
}

EditorDocument* CodeEditorPane::currentDocument() const {
    return m_impl->activeDocument();
}

bool CodeEditorPane::hasUnsavedChanges() const {
    return std::any_of(m_impl->documents.cbegin(), m_impl->documents.cend(),
                       [](const EditorDocument* doc) { return doc && doc->isDirty(); });
}

void CodeEditorPane::openFile(const QString& filePath, int jumpToLine, bool readOnly) {
    const QString trimmed = filePath.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    const int existingIdx = m_impl->indexOfFilePath(trimmed);
    if (existingIdx >= 0) {
        EditorDocument* existing = m_impl->documents[existingIdx];
        if (existing) {
            existing->setReadOnly(readOnly);
            if (!existing->isDirty()) {
                existing->reloadIfChangedOnDisk();
            }
            m_impl->watchPath(existing->filePath());
        }

        if (existingIdx != m_impl->activeIndex) {
            m_impl->saveActiveCursorAndScroll();
            m_impl->activeIndex = existingIdx;
            m_impl->updateAllTabActiveStates();
            m_impl->ensureActiveTabVisible();
            m_impl->loadActiveDocumentIntoEditor();
        } else {
            if (existing && m_impl->editor) {
                ScopedBoolGuard guard(m_impl->suppressEditorSignals);
                m_impl->editor->setReadOnly(existing->isReadOnly() || !existing->isAvailable());
            }
            m_impl->updateHeaderAndErrorState();
            m_impl->updateTabIndicator(existingIdx);
            m_impl->ensureActiveTabVisible();
        }

        if (jumpToLine > 0) {
            m_impl->jumpToLine(jumpToLine);
        }
        if (existing && existing->isAvailable() && m_impl->editor) {
            m_impl->editor->setFocus();
        }
        return;
    }

    m_impl->saveActiveCursorAndScroll();

    auto* doc = new EditorDocument(trimmed, readOnly, this);
    doc->loadFromDisk();

    connect(doc, &EditorDocument::dirtyChanged, this, [this, doc](bool) {
        m_impl->updateTabIndicatorForDocument(doc);
        if (doc == m_impl->activeDocument()) {
            m_impl->updateHeaderAndErrorState();
        }
    });

    connect(doc, &EditorDocument::reloadedFromDisk, this,
            [this, doc](const QString& newText) {
                m_impl->syncActiveDocumentReloaded(doc, newText);
            });

    connect(doc, &EditorDocument::errorChanged, this,
            [this, doc](const QString& err) {
                if (doc == m_impl->activeDocument()) {
                    m_impl->updateHeaderAndErrorState();
                }
                if (!err.trimmed().isEmpty()) {
                    emit statusMessage(err);
                }
            });

    connect(doc, &EditorDocument::readOnlyChanged, this,
            [this, doc](bool) {
                m_impl->updateTabIndicatorForDocument(doc);
                if (doc == m_impl->activeDocument()) {
                    if (m_impl->editor) {
                        ScopedBoolGuard guard(m_impl->suppressEditorSignals);
                        m_impl->editor->setReadOnly(doc->isReadOnly() || !doc->isAvailable());
                    }
                    m_impl->updateHeaderAndErrorState();
                }
            });

    const int newIdx = static_cast<int>(m_impl->documents.size());
    m_impl->documents.append(doc);
    FileTabWidget* tab = m_impl->createTabWidget(doc);
    m_impl->tabWidgets.append(tab);
    if (m_impl->tabStripLayout) {
        m_impl->tabStripLayout->insertWidget(newIdx, tab);
    }
    m_impl->activeIndex = newIdx;
    m_impl->updateAllTabActiveStates();
    m_impl->updateTabIndicator(newIdx);
    m_impl->ensureActiveTabVisible();

    m_impl->watchPath(doc->filePath());
    m_impl->loadActiveDocumentIntoEditor();

    if (jumpToLine > 0) {
        m_impl->jumpToLine(jumpToLine);
    }
    if (doc->isAvailable() && m_impl->editor) {
        m_impl->editor->setFocus();
    }

    emit openDocumentsChanged();
}

void CodeEditorPane::closeFile(const QString& filePath) {
    const int idx = m_impl->indexOfFilePath(filePath);
    if (idx >= 0) {
        m_impl->closeDocumentAtIndex(idx);
    }
}

void CodeEditorPane::saveCurrentFile() {
    EditorDocument* doc = m_impl->activeDocument();
    if (!doc) {
        return;
    }
    if (!doc->isAvailable()) {
        emit statusMessage(
            QStringLiteral("Cannot save %1: %2").arg(doc->fileName(), doc->unavailableReason()));
        return;
    }
    if (doc->isReadOnly()) {
        emit statusMessage(QStringLiteral("%1 is read-only").arg(doc->fileName()));
        return;
    }

    // Ensure latest buffer text is synchronized prior to save.
    if (m_impl->editor) {
        doc->setTextFromEditor(m_impl->editor->text());
    }
    if (!doc->isDirty()) {
        return;
    }

    const QString path = doc->filePath();
    m_impl->selfSavingPaths.insert(path);
    if (m_impl->fileWatcher && m_impl->fileWatcher->files().contains(path)) {
        m_impl->fileWatcher->removePath(path);
    }

    const bool ok = doc->saveToDisk();

    m_impl->watchPath(path);
    m_impl->selfSavingPaths.remove(path);

    if (ok) {
        if (m_impl->editor) {
            m_impl->editor->setModified(false);
        }
        m_impl->updateTabIndicatorForDocument(doc);
        m_impl->updateHeaderAndErrorState();
        emit statusMessage(QStringLiteral("Saved %1").arg(doc->fileName()));
    } else {
        m_impl->updateHeaderAndErrorState();
        emit statusMessage(
            QStringLiteral("Failed to save %1: %2").arg(doc->fileName(), doc->lastError()));
    }
}

void CodeEditorPane::saveAllFiles() {
    if (EditorDocument* active = m_impl->activeDocument()) {
        if (active->isAvailable() && !active->isReadOnly() && m_impl->editor) {
            active->setTextFromEditor(m_impl->editor->text());
        }
    }

    int savedCount = 0;
    int failedCount = 0;

    for (EditorDocument* doc : m_impl->documents) {
        if (!doc || !doc->isDirty() || doc->isReadOnly() || !doc->isAvailable()) {
            continue;
        }
        const QString path = doc->filePath();
        m_impl->selfSavingPaths.insert(path);
        if (m_impl->fileWatcher && m_impl->fileWatcher->files().contains(path)) {
            m_impl->fileWatcher->removePath(path);
        }

        const bool ok = doc->saveToDisk();

        m_impl->watchPath(path);
        m_impl->selfSavingPaths.remove(path);

        if (ok) {
            ++savedCount;
            if (doc == m_impl->activeDocument() && m_impl->editor) {
                m_impl->editor->setModified(false);
            }
        } else {
            ++failedCount;
        }
        m_impl->updateTabIndicatorForDocument(doc);
    }

    m_impl->updateHeaderAndErrorState();

    if (failedCount > 0) {
        emit statusMessage(
            QStringLiteral("Saved %1 file%2 (%3 failed)")
                .arg(savedCount)
                .arg(savedCount == 1 ? QString() : QStringLiteral("s"))
                .arg(failedCount));
    } else if (savedCount > 0) {
        emit statusMessage(
            QStringLiteral("Saved %1 file%2")
                .arg(savedCount)
                .arg(savedCount == 1 ? QString() : QStringLiteral("s")));
    } else {
        emit statusMessage(QStringLiteral("All open files are up to date"));
    }
}

void CodeEditorPane::showFindBar() {
    if (!m_impl->findBar || !m_impl->findEdit) {
        return;
    }

    m_impl->findBar->setVisible(true);

    if (const EditorDocument* doc = m_impl->activeDocument();
        doc && doc->isAvailable() && m_impl->editor && m_impl->editor->hasSelectedText()) {
        const QString sel = m_impl->editor->selectedText();
        if (!sel.isEmpty() && !sel.contains(QLatin1Char('\n')) &&
            !sel.contains(QLatin1Char('\r')) && sel.size() <= 200) {
            QSignalBlocker blocker(m_impl->findEdit);
            m_impl->findEdit->setText(sel);
        }
    }

    m_impl->findEdit->setFocus();
    m_impl->findEdit->selectAll();
    m_impl->updateHeaderAndErrorState();
    m_impl->updateMatchCount();
}

void CodeEditorPane::reloadOpenDocumentsIfChanged() {
    for (EditorDocument* doc : m_impl->documents) {
        if (!doc) {
            continue;
        }
        m_impl->watchPath(doc->filePath());
        if (!doc->isDirty()) {
            doc->reloadIfChangedOnDisk();
        }
    }
}

void CodeEditorPane::applyTheme() {
    const UiPalette u = ThemeManager::instance().ui();
    const CodePalette c = ThemeManager::instance().code();
    const QFont edFont = ThemeManager::instance().editorFont();

    const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
    const QColor solidSelected = compositeOver(u.selectedFill, u.background);
    const QColor solidDivider = compositeOver(u.divider, u.background);
    const QColor solidBorder = compositeOver(u.subtleBorder, u.background);
    const QColor solidMuted = compositeOver(u.textMuted, u.background);

    // Top flat FileTabBar styling (background ui.background, frameless scroll strip)
    if (m_impl->tabScrollArea) {
        m_impl->tabScrollArea->setStyleSheet(QStringLiteral(
            "#spellFileTabBar, #spellFileTabBar > QWidget, #spellFileTabStrip {\n"
            "  background-color: %1;\n"
            "  border: none;\n"
            "}\n"
        ).arg(qssColor(u.background)));

        QPalette barPal = m_impl->tabScrollArea->palette();
        barPal.setColor(QPalette::Window, u.background);
        barPal.setColor(QPalette::Base, u.background);
        m_impl->tabScrollArea->setPalette(barPal);
        if (QWidget* vp = m_impl->tabScrollArea->viewport()) {
            vp->setAutoFillBackground(true);
            vp->setPalette(barPal);
        }
        if (m_impl->tabStripContent) {
            m_impl->tabStripContent->setAutoFillBackground(true);
            m_impl->tabStripContent->setPalette(barPal);
        }
    }

    for (int i = 0; i < m_impl->tabWidgets.size(); ++i) {
        m_impl->updateTabIndicator(i);
        if (FileTabWidget* tab = m_impl->tabWidgets[i]) {
            tab->applyTheme();
        }
    }

    m_impl->topHairline->applyTheme();

    QFont monoStatus(ThemeManager::instance().editorFontFamily());
    monoStatus.setStyleHint(QFont::Monospace);
    monoStatus.setPointSizeF(10.5);

    // Find & Replace bar styling (ID-scoped so SpellChipButton/SpellIconButton keep their own QSS)
    m_impl->findBar->setStyleSheet(QStringLiteral(
        "#spellEditorFindBar, #spellEditorFindRows { background-color: %1; }\n"
        "#spellEditorFindRows QLineEdit {\n"
        "  background-color: %2;\n"
        "  color: %3;\n"
        "  border: 1px solid %4;\n"
        "  border-radius: 5px;\n"
        "  padding: 2px 7px;\n"
        "  font-size: 12px;\n"
        "}\n"
        "#spellEditorFindRows QLineEdit:focus {\n"
        "  border: 1px solid %5;\n"
        "}\n"
        "#spellReplaceBtn, #spellReplaceAllBtn {\n"
        "  background-color: %2;\n"
        "  color: %3;\n"
        "  border: 1px solid %4;\n"
        "  border-radius: 5px;\n"
        "  padding: 2px 10px;\n"
        "  font-size: 11.5px;\n"
        "  font-weight: 600;\n"
        "}\n"
        "#spellReplaceBtn:hover, #spellReplaceAllBtn:hover {\n"
        "  background-color: %6;\n"
        "  border-color: %5;\n"
        "}\n"
        "#spellReplaceBtn:disabled, #spellReplaceAllBtn:disabled {\n"
        "  color: %7;\n"
        "  border-color: %8;\n"
        "}\n"
    ).arg(qssColor(solidElevated),
          qssColor(u.surface),
          qssColor(u.textPrimary),
          qssColor(solidBorder),
          qssColor(u.selectedBorder),
          qssColor(solidSelected),
          qssColor(solidMuted),
          qssColor(solidDivider)));

    m_impl->matchCountLabel->setFont(monoStatus);
    m_impl->caseBtn->applyTheme();
    m_impl->wordBtn->applyTheme();
    m_impl->regexBtn->applyTheme();
    m_impl->prevBtn->applyTheme();
    m_impl->nextBtn->applyTheme();
    m_impl->closeFindBtn->applyTheme();
    m_impl->findHairline->applyTheme();
    m_impl->updateMatchCount();

    // Inline error banner styling
    const QColor dangerBg = compositeOver(withAlpha(u.danger, 0.14), c.background);
    const QColor dangerBorder = compositeOver(withAlpha(u.danger, 0.55), c.background);
    m_impl->errorBanner->setStyleSheet(QStringLiteral(
        "#spellEditorErrorBanner {\n"
        "  background-color: %1;\n"
        "  border-bottom: 1px solid %2;\n"
        "}\n"
        "#spellEditorErrorBanner QLabel {\n"
        "  color: %3;\n"
        "  background: transparent;\n"
        "  font-size: 11.5px;\n"
        "}\n"
    ).arg(qssColor(dangerBg), qssColor(dangerBorder), qssColor(u.danger)));
    m_impl->dismissErrorBtn->applyTheme();

    // Minimal EmptyEditorView (monochrome "Open File…" chip button only)
    m_impl->emptyPage->setStyleSheet(
        QStringLiteral("#spellEditorEmptyPage { background-color: %1; }").arg(qssColor(c.background)));
    if (m_impl->emptyOpenBtn) {
        m_impl->emptyOpenBtn->applyTheme();
    }

    // Unavailable / binary file banner page
    const QColor unavailHintTint = compositeOver(withAlpha(c.comment, 0.72), c.background);
    m_impl->unavailablePage->setStyleSheet(
        QStringLiteral("#spellEditorUnavailablePage { background-color: %1; }").arg(qssColor(c.background)));
    m_impl->unavailableIconLabel->setPixmap(
        icons::vectorIcon(QStringLiteral("file"), c.comment, 36).pixmap(36, 36));
    m_impl->unavailableFileLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px; font-weight: 600; background: transparent;")
            .arg(qssColor(c.plain)));
    m_impl->unavailableReasonLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 12.5px; background: transparent;")
            .arg(qssColor(c.comment)));
    m_impl->unavailablePathLabel->setFont(monoStatus);
    m_impl->unavailablePathLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px; background: transparent;")
            .arg(qssColor(unavailHintTint)));

    // Re-style active QsciLexer and QsciScintilla widget across all 8 code themes
    if (m_impl->activeLexer) {
        applyPaletteToLexer(m_impl->activeLexer.data(), c, edFont);
    }
    m_impl->applyEditorChromeColors(c, edFont);
    if (m_impl->activeLexer && m_impl->editor) {
        m_impl->editor->recolor();
    }

    m_impl->updateHeaderAndErrorState();
}

} // namespace spell
