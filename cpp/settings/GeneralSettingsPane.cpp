#include "settings/GeneralSettingsPane.h"

#include "theme/SpellTheme.h"

#include <QButtonGroup>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QFrame>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QRectF>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QStringList>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace spell {

namespace {

constexpr double kEditorFontMinPt   = 10.0;
constexpr double kEditorFontMaxPt   = 24.0;
constexpr double kTerminalFontMinPt = 9.0;
constexpr double kTerminalFontMaxPt = 24.0;
constexpr double kFontStepPt        = 0.5;

QString systemDefaultMonoFamily() {
    const QString fam = QFontDatabase::systemFont(QFontDatabase::FixedFont).family().trimmed();
    return fam.isEmpty() ? QStringLiteral("Monospace") : fam;
}

QIcon renderHiDpiIcon(int logicalW,
                      int logicalH,
                      const std::function<void(QPainter&, const QRectF&)>& drawFn) {
    QIcon icon;
    for (const qreal dpr : {1.0, 2.0}) {
        const int pw = std::max(1, qRound(logicalW * dpr));
        const int ph = std::max(1, qRound(logicalH * dpr));
        QImage img(pw, ph, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);

        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(dpr, dpr);
        drawFn(p, QRectF(0.0, 0.0, static_cast<qreal>(logicalW), static_cast<qreal>(logicalH)));
        p.end();

        QPixmap pm = QPixmap::fromImage(img);
        pm.setDevicePixelRatio(dpr);
        icon.addPixmap(pm);
    }
    return icon;
}

QIcon makeAppearanceIcon(Appearance mode, const QColor& frameBorder) {
    return renderHiDpiIcon(22, 16, [mode, frameBorder](QPainter& p, const QRectF& bounds) {
        const bool isLight = (mode == Appearance::Light);
        const QColor bg = isLight ? rgbHex(0xFFFFFF) : rgbHex(0x000000);
        const QColor fg = isLight ? rgbHex(0x000000) : rgbHex(0xFFFFFF);
        const QColor muted = withAlpha(fg, 0.25);

        const QRectF r = bounds.adjusted(0.75, 0.75, -0.75, -0.75);
        p.setPen(QPen(frameBorder, 1.0));
        p.setBrush(bg);
        p.drawRoundedRect(r, 3.5, 3.5);

        // Mini sidebar divider
        p.setPen(QPen(muted, 1.0));
        p.drawLine(QPointF(r.left() + 6.5, r.top() + 1.0), QPointF(r.left() + 6.5, r.bottom() - 1.0));

        // Mini content lines
        p.setPen(Qt::NoPen);
        p.setBrush(fg);
        p.drawRoundedRect(QRectF(r.left() + 9.0, r.top() + 4.0, 8.5, 2.0), 1.0, 1.0);
        p.setBrush(withAlpha(fg, 0.55));
        p.drawRoundedRect(QRectF(r.left() + 9.0, r.top() + 8.0, 6.0, 1.8), 0.9, 0.9);
        p.drawRoundedRect(QRectF(r.left() + 9.0, r.top() + 11.2, 7.2, 1.8), 0.9, 0.9);
    });
}

QIcon makeCodeThemeSwatchIcon(CodeThemeId id, Appearance app, const QColor& borderColor) {
    const CodePalette pal = codePaletteFor(id, app);
    return renderHiDpiIcon(24, 15, [pal, borderColor](QPainter& p, const QRectF& bounds) {
        const QRectF r = bounds.adjusted(0.75, 0.75, -0.75, -0.75);
        p.setPen(QPen(borderColor, 1.0));
        p.setBrush(pal.background);
        p.drawRoundedRect(r, 3.5, 3.5);

        const std::array<QColor, 4> dots = {
            pal.keyword, pal.function, pal.string, pal.type
        };
        const qreal dotD = 3.4;
        const qreal spacing = 1.6;
        const qreal totalW = 4.0 * dotD + 3.0 * spacing;
        const qreal startX = r.center().x() - totalW * 0.5;
        const qreal cy = r.center().y() - dotD * 0.5;

        p.setPen(Qt::NoPen);
        for (size_t i = 0; i < dots.size(); ++i) {
            p.setBrush(dots[i]);
            p.drawEllipse(QRectF(startX + static_cast<qreal>(i) * (dotD + spacing), cy, dotD, dotD));
        }
    });
}

QIcon makeTerminalThemeSwatchIcon(TerminalThemeId id,
                                  CodeThemeId codeId,
                                  Appearance app,
                                  const QColor& borderColor) {
    const TerminalPalette pal = terminalPaletteFor(id, codeId, app);
    return renderHiDpiIcon(24, 15, [pal, borderColor](QPainter& p, const QRectF& bounds) {
        const QRectF r = bounds.adjusted(0.75, 0.75, -0.75, -0.75);
        p.setPen(QPen(borderColor, 1.0));
        p.setBrush(pal.background);
        p.drawRoundedRect(r, 3.5, 3.5);

        // Prompt chevron + 3 ANSI dots
        p.setPen(QPen(pal.cursor, 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath chevron;
        chevron.moveTo(r.left() + 3.8, r.center().y() - 2.6);
        chevron.lineTo(r.left() + 6.4, r.center().y());
        chevron.lineTo(r.left() + 3.8, r.center().y() + 2.6);
        p.drawPath(chevron);

        const std::array<QColor, 3> dots = {
            pal.ansi[2], pal.ansi[4], pal.ansi[3]
        };
        const qreal dotD = 3.1;
        const qreal spacing = 1.6;
        const qreal startX = r.left() + 9.2;
        const qreal cy = r.center().y() - dotD * 0.5;

        p.setPen(Qt::NoPen);
        for (size_t i = 0; i < dots.size(); ++i) {
            p.setBrush(dots[i]);
            p.drawEllipse(QRectF(startX + static_cast<qreal>(i) * (dotD + spacing), cy, dotD, dotD));
        }
    });
}

QStringList discoverMonospaceFamilies(const QString& activeFamily) {
    const QStringList allInstalled = QFontDatabase::families();
    static const QStringList kPreferredMono = {
        QStringLiteral("SF Mono"),
        QStringLiteral("Menlo"),
        QStringLiteral("Monaco"),
        QStringLiteral("JetBrains Mono"),
        QStringLiteral("Fira Code"),
        QStringLiteral("Cascadia Code"),
        QStringLiteral("Cascadia Mono"),
        QStringLiteral("Consolas"),
        QStringLiteral("DejaVu Sans Mono"),
        QStringLiteral("Liberation Mono"),
        QStringLiteral("Ubuntu Mono"),
        QStringLiteral("Source Code Pro"),
        QStringLiteral("IBM Plex Mono"),
        QStringLiteral("Hack"),
        QStringLiteral("Inconsolata"),
        QStringLiteral("Iosevka"),
        QStringLiteral("Courier New"),
        QStringLiteral("Andale Mono")
    };

    QStringList result;
    auto addUnique = [&result](const QString& family) {
        const QString trimmed = family.trimmed();
        if (trimmed.isEmpty()) {
            return;
        }
        for (const QString& existing : result) {
            if (existing.compare(trimmed, Qt::CaseInsensitive) == 0) {
                return;
            }
        }
        result.append(trimmed);
    };

    for (const QString& fam : allInstalled) {
        if (fam.startsWith(QLatin1Char('.'))) {
            continue;
        }
        if (QFontDatabase::isFixedPitch(fam)) {
            addUnique(fam);
            continue;
        }
        for (const QString& preferred : kPreferredMono) {
            if (fam.compare(preferred, Qt::CaseInsensitive) == 0) {
                addUnique(fam);
                break;
            }
        }
    }

    addUnique(systemDefaultMonoFamily());
    addUnique(activeFamily);

    if (result.isEmpty()) {
        result.append(QStringLiteral("Monospace"));
    }

    std::sort(result.begin(), result.end(), [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return result;
}

/// Interactive 2-row live color swatch preview for the active `CodePalette`.
class CodePaletteSwatchBar : public QWidget {
public:
    explicit CodePaletteSwatchBar(QWidget* parent = nullptr)
        : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
    }

    void setPaletteData(const CodePalette& code, const UiPalette& ui) {
        m_code = code;
        m_ui = ui;
        m_entries = {
            {QStringLiteral("BG"),       QStringLiteral("Background"),   m_code.background},
            {QStringLiteral("Plain"),    QStringLiteral("Plain Text"),   m_code.plain},
            {QStringLiteral("Keyword"),  QStringLiteral("Keyword"),      m_code.keyword},
            {QStringLiteral("Function"), QStringLiteral("Function"),     m_code.function},
            {QStringLiteral("String"),   QStringLiteral("String"),       m_code.string},
            {QStringLiteral("Type"),     QStringLiteral("Type"),         m_code.type},
            {QStringLiteral("Number"),   QStringLiteral("Number"),       m_code.number},
            {QStringLiteral("Comment"),  QStringLiteral("Comment"),      m_code.comment},
            {QStringLiteral("Added"),    QStringLiteral("Diff Added"),   m_code.added},
            {QStringLiteral("Removed"),  QStringLiteral("Diff Removed"), m_code.removed}
        };
        update();
    }

    QSize sizeHint() const override { return QSize(420, 48); }
    QSize minimumSizeHint() const override { return QSize(240, 48); }

protected:
    bool event(QEvent* ev) override {
        if (ev->type() == QEvent::ToolTip) {
            const auto* helpEv = static_cast<QHelpEvent*>(ev);
            const int idx = indexAt(helpEv->pos());
            if (idx >= 0 && idx < m_entries.size()) {
                const Entry& e = m_entries[idx];
                const QString tip = QStringLiteral("%1  %2")
                    .arg(e.fullName, e.color.name(QColor::HexRgb).toUpper());
                QToolTip::showText(helpEv->globalPos(), tip, this);
            } else {
                QToolTip::hideText();
                ev->ignore();
            }
            return true;
        }
        return QWidget::event(ev);
    }

    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QColor borderColor = compositeOver(m_ui.subtleBorder, m_ui.background);
        const QColor labelColor = compositeOver(m_ui.textSecondary, m_ui.background);
        const QColor chipFill = compositeOver(m_ui.elevatedSurface, m_ui.background);

        QFont labelFont = font();
        labelFont.setPointSizeF(9.5);
        p.setFont(labelFont);

        const int count = m_entries.size();
        for (int i = 0; i < count; ++i) {
            const QRectF cell = cellRect(i);
            p.setPen(QPen(compositeOver(m_ui.divider, m_ui.background), 1.0));
            p.setBrush(chipFill);
            p.drawRoundedRect(cell.adjusted(0.5, 0.5, -0.5, -0.5), 4.5, 4.5);

            const qreal swatchSz = 10.0;
            const QRectF swatchRect(cell.left() + 6.0,
                                    cell.center().y() - swatchSz * 0.5,
                                    swatchSz,
                                    swatchSz);
            p.setPen(QPen(borderColor, 1.0));
            p.setBrush(m_entries[i].color);
            p.drawRoundedRect(swatchRect, 2.5, 2.5);

            const QRectF textRect(swatchRect.right() + 5.0,
                                  cell.top(),
                                  std::max<qreal>(0.0, cell.right() - swatchRect.right() - 8.0),
                                  cell.height());
            p.setPen(labelColor);
            const QString elided = p.fontMetrics().elidedText(
                m_entries[i].shortName, Qt::ElideRight, qRound(textRect.width()));
            p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
        }
    }

private:
    struct Entry {
        QString shortName;
        QString fullName;
        QColor color;
    };

    QRectF cellRect(int index) const {
        constexpr int kCols = 5;
        constexpr int kRows = 2;
        constexpr qreal kGapX = 6.0;
        constexpr qreal kGapY = 5.0;

        const int row = index / kCols;
        const int col = index % kCols;
        const qreal availW = std::max<qreal>(1.0, static_cast<qreal>(width()) - (kCols - 1) * kGapX);
        const qreal availH = std::max<qreal>(1.0, static_cast<qreal>(height()) - (kRows - 1) * kGapY);
        const qreal cw = availW / static_cast<qreal>(kCols);
        const qreal ch = availH / static_cast<qreal>(kRows);
        return QRectF(col * (cw + kGapX), row * (ch + kGapY), cw, ch);
    }

    int indexAt(const QPoint& pos) const {
        for (int i = 0; i < m_entries.size(); ++i) {
            if (cellRect(i).contains(pos)) {
                return i;
            }
        }
        return -1;
    }

    CodePalette m_code;
    UiPalette m_ui;
    QVector<Entry> m_entries;
};

/// Live syntax-highlighted code preview honoring active `CodePalette` and `editorFont()`.
class CodePreviewWidget : public QWidget {
public:
    explicit CodePreviewWidget(QWidget* parent = nullptr)
        : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setPreviewState(const CodePalette& code,
                         const UiPalette& ui,
                         const QFont& editorFont,
                         const QString& themeName) {
        m_code = code;
        m_ui = ui;
        m_font = editorFont;
        m_themeName = themeName;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override {
        const QFontMetrics fm(m_font);
        const int lineH = std::max(15, fm.lineSpacing() + 2);
        const int headerH = 24;
        const int padV = 16;
        const int lines = 7;
        return QSize(420, headerH + padV + lines * lineH);
    }

    QSize minimumSizeHint() const override {
        return QSize(240, sizeHint().height());
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const QColor borderColor = compositeOver(m_ui.subtleBorder, m_ui.background);

        QPainterPath clipPath;
        clipPath.addRoundedRect(outer, 7.0, 7.0);

        p.save();
        p.setClipPath(clipPath);

        // Background
        p.fillRect(outer, m_code.background);

        // Header bar
        constexpr qreal kHeaderH = 24.0;
        const QColor headerDivider = withAlpha(m_code.plain, 0.14);
        const QColor headerBg = withAlpha(m_code.plain, 0.04);
        p.fillRect(QRectF(outer.left(), outer.top(), outer.width(), kHeaderH), headerBg);
        p.setPen(QPen(headerDivider, 1.0));
        p.drawLine(QPointF(outer.left(), outer.top() + kHeaderH),
                   QPointF(outer.right(), outer.top() + kHeaderH));

        QFont headerFont = m_font;
        headerFont.setPointSizeF(9.5);
        p.setFont(headerFont);
        p.setPen(m_code.comment);
        const QString caption = QStringLiteral("%1  ·  %2  ·  %3 pt")
            .arg(m_themeName, m_font.family(), QString::number(m_font.pointSizeF(), 'f', 1));
        p.drawText(QRectF(outer.left() + 10.0, outer.top(), outer.width() - 68.0, kHeaderH),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(caption, Qt::ElideRight, std::max(40, width() - 72)));

        // Miniature syntax dots in header right
        const std::array<QColor, 5> dots = {
            m_code.keyword, m_code.function, m_code.string, m_code.type, m_code.added
        };
        p.setPen(Qt::NoPen);
        const qreal dotD = 5.5;
        const qreal dotGap = 4.0;
        const qreal dotsW = 5.0 * dotD + 4.0 * dotGap;
        qreal dx = outer.right() - 10.0 - dotsW;
        const qreal dy = outer.top() + (kHeaderH - dotD) * 0.5;
        for (const QColor& c : dots) {
            p.setBrush(c);
            p.drawEllipse(QRectF(dx, dy, dotD, dotD));
            dx += dotD + dotGap;
        }

        // Tokenized live code sample
        struct Span {
            QString text;
            QColor color;
            bool bold = false;
        };
        struct CodeLine {
            QVector<Span> spans;
            QColor rowTint;
        };

        const QVector<CodeLine> lines = {
            {
                {{QStringLiteral("// BeatrixLeslie IDE live syntax preview"), m_code.comment, false}},
                QColor()
            },
            {
                {
                    {QStringLiteral("struct "), m_code.keyword, true},
                    {QStringLiteral("Greeting"), m_code.type, false},
                    {QStringLiteral(" { "), m_code.plain, false},
                    {QStringLiteral("app: &str, "), m_code.plain, false},
                    {QStringLiteral("version: "), m_code.plain, false},
                    {QStringLiteral("u32"), m_code.type, false},
                    {QStringLiteral(" }"), m_code.plain, false}
                },
                QColor()
            },
            {
                {
                    {QStringLiteral("fn "), m_code.keyword, true},
                    {QStringLiteral("format_banner"), m_code.function, false},
                    {QStringLiteral("(g: &"), m_code.plain, false},
                    {QStringLiteral("Greeting"), m_code.type, false},
                    {QStringLiteral(") -> "), m_code.plain, false},
                    {QStringLiteral("String"), m_code.type, false},
                    {QStringLiteral(" {"), m_code.plain, false}
                },
                QColor()
            },
            {
                {
                    {QStringLiteral("    "), m_code.plain, false},
                    {QStringLiteral("format!"), m_code.function, false},
                    {QStringLiteral("("), m_code.plain, false},
                    {QStringLiteral("\"Hello, {} v{}\""), m_code.string, false},
                    {QStringLiteral(", g.app, "), m_code.plain, false},
                    {QStringLiteral("2026"), m_code.number, false},
                    {QStringLiteral(")"), m_code.plain, false}
                },
                QColor()
            },
            {
                {{QStringLiteral("}"), m_code.plain, false}},
                QColor()
            },
            {
                {{QStringLiteral("-   let name = \"Legacy\";"), m_code.removed, false}},
                withAlpha(m_code.removed, 0.13)
            },
            {
                {{QStringLiteral("+   let name = \"BeatrixLeslie\";"), m_code.added, false}},
                withAlpha(m_code.added, 0.13)
            }
        };

        p.setFont(m_font);
        const QFontMetrics fm(m_font);
        const int lineH = std::max(15, fm.lineSpacing() + 2);
        const qreal gutterW = static_cast<qreal>(fm.horizontalAdvance(QStringLiteral(" 9 "))) + 8.0;
        const qreal codeStartX = outer.left() + gutterW + 8.0;
        qreal y = outer.top() + kHeaderH + 8.0;

        // Subtle gutter separator
        p.setPen(QPen(withAlpha(m_code.plain, 0.10), 1.0));
        p.drawLine(QPointF(outer.left() + gutterW, outer.top() + kHeaderH),
                   QPointF(outer.left() + gutterW, outer.bottom()));

        QFont boldFont = m_font;
        boldFont.setBold(true);

        for (int i = 0; i < lines.size(); ++i) {
            const CodeLine& line = lines[i];
            if (line.rowTint.isValid()) {
                p.fillRect(QRectF(outer.left() + gutterW + 1.0,
                                  y,
                                  outer.width() - gutterW - 1.0,
                                  static_cast<qreal>(lineH)),
                           line.rowTint);
            }

            // Line number
            p.setFont(m_font);
            p.setPen(withAlpha(m_code.comment, 0.75));
            p.drawText(QRectF(outer.left() + 2.0, y, gutterW - 6.0, static_cast<qreal>(lineH)),
                       Qt::AlignVCenter | Qt::AlignRight,
                       QString::number(i + 1));

            // Code tokens
            qreal x = codeStartX;
            const qreal baseline = y + (lineH - fm.height()) * 0.5 + fm.ascent();
            for (const Span& span : line.spans) {
                p.setFont(span.bold ? boldFont : m_font);
                p.setPen(span.color);
                p.drawText(QPointF(x, baseline), span.text);
                x += QFontMetricsF(p.font()).horizontalAdvance(span.text);
            }
            y += lineH;
        }

        p.restore();

        // Crisp outer frame border
        p.setPen(QPen(borderColor, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(outer, 7.0, 7.0);
    }

private:
    CodePalette m_code;
    UiPalette m_ui;
    QFont m_font;
    QString m_themeName;
};

/// Live ANSI terminal preview honoring active `TerminalPalette` and `terminalFont()`.
class TerminalPreviewWidget : public QWidget {
public:
    explicit TerminalPreviewWidget(QWidget* parent = nullptr)
        : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
    }

    void setPreviewState(const TerminalPalette& term,
                         const UiPalette& ui,
                         const QFont& terminalFont,
                         const QString& themeName) {
        m_term = term;
        m_ui = ui;
        m_font = terminalFont;
        m_themeName = themeName;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override {
        const QFontMetrics fm(m_font);
        const int lineH = std::max(15, fm.lineSpacing() + 2);
        const int headerH = 24;
        const int ansiStripH = 24;
        const int padV = 16;
        const int lines = 4;
        return QSize(420, headerH + padV + lines * lineH + ansiStripH);
    }

    QSize minimumSizeHint() const override {
        return QSize(240, sizeHint().height());
    }

protected:
    bool event(QEvent* ev) override {
        if (ev->type() == QEvent::ToolTip) {
            const auto* helpEv = static_cast<QHelpEvent*>(ev);
            const int ansiIdx = ansiSwatchAt(helpEv->pos());
            if (ansiIdx >= 0 && ansiIdx < 16) {
                const QColor c = m_term.ansi[static_cast<size_t>(ansiIdx)];
                const QString tip = QStringLiteral("ANSI %1  %2")
                    .arg(ansiIdx)
                    .arg(c.name(QColor::HexRgb).toUpper());
                QToolTip::showText(helpEv->globalPos(), tip, this);
            } else {
                QToolTip::hideText();
                ev->ignore();
            }
            return true;
        }
        return QWidget::event(ev);
    }

    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const QColor borderColor = compositeOver(m_ui.subtleBorder, m_ui.background);

        QPainterPath clipPath;
        clipPath.addRoundedRect(outer, 7.0, 7.0);

        p.save();
        p.setClipPath(clipPath);

        // Background
        p.fillRect(outer, m_term.background);

        // Header bar
        constexpr qreal kHeaderH = 24.0;
        const QColor divider = withAlpha(m_term.foreground, 0.14);
        p.fillRect(QRectF(outer.left(), outer.top(), outer.width(), kHeaderH),
                   withAlpha(m_term.foreground, 0.04));
        p.setPen(QPen(divider, 1.0));
        p.drawLine(QPointF(outer.left(), outer.top() + kHeaderH),
                   QPointF(outer.right(), outer.top() + kHeaderH));

        QFont headerFont = m_font;
        headerFont.setPointSizeF(9.5);
        p.setFont(headerFont);
        p.setPen(withAlpha(m_term.foreground, 0.65));
        const QString caption = QStringLiteral("%1  ·  %2  ·  %3 pt")
            .arg(m_themeName, m_font.family(), QString::number(m_font.pointSizeF(), 'f', 1));
        p.drawText(QRectF(outer.left() + 10.0, outer.top(), outer.width() - 68.0, kHeaderH),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(caption, Qt::ElideRight, std::max(40, width() - 72)));

        p.drawText(QRectF(outer.right() - 56.0, outer.top(), 46.0, kHeaderH),
                   Qt::AlignVCenter | Qt::AlignRight,
                   QStringLiteral("ANSI 16"));

        // Terminal sample lines
        struct Span {
            QString text;
            QColor color;
            bool bold = false;
        };

        const QVector<QVector<Span>> lines = {
            {
                {QStringLiteral("user@spell"), m_term.ansi[2], false},
                {QStringLiteral(" ~/project "), m_term.ansi[4], false},
                {QStringLiteral("% "), m_term.foreground, false},
                {QStringLiteral("cargo test"), m_term.bold, true}
            },
            {
                {QStringLiteral("test result: ok. 59 passed; 0 failed"), m_term.ansi[2], false}
            },
            {
                {QStringLiteral("warning:"), m_term.ansi[3], true},
                {QStringLiteral(" unused variable "), m_term.foreground, false},
                {QStringLiteral("`x`"), m_term.ansi[6], false}
            },
            {
                {QStringLiteral("#spell "), m_term.cursor, true},
                {QStringLiteral("why did this fail?"), m_term.foreground, false}
            }
        };

        QFont boldFont = m_font;
        boldFont.setBold(true);

        p.setFont(m_font);
        const QFontMetrics fm(m_font);
        const int lineH = std::max(15, fm.lineSpacing() + 2);
        qreal y = outer.top() + kHeaderH + 8.0;

        for (int lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
            qreal x = outer.left() + 10.0;
            const qreal baseline = y + (lineH - fm.height()) * 0.5 + fm.ascent();

            for (const Span& span : lines[lineIdx]) {
                p.setFont(span.bold ? boldFont : m_font);
                const qreal spanW = QFontMetricsF(p.font()).horizontalAdvance(span.text);
                p.setPen(span.color);
                p.drawText(QPointF(x, baseline), span.text);
                x += spanW;
            }

            // Block cursor on the final #spell prompt line
            if (lineIdx == lines.size() - 1) {
                const qreal cursorW = std::max<qreal>(6.0, QFontMetricsF(m_font).horizontalAdvance(QLatin1Char(' ')));
                const qreal cursorH = std::max<qreal>(10.0, fm.height() - 1.0);
                const qreal cursorY = y + (lineH - cursorH) * 0.5;
                p.setPen(Qt::NoPen);
                p.setBrush(m_term.cursor);
                p.drawRoundedRect(QRectF(x + 1.0, cursorY, cursorW, cursorH), 1.5, 1.5);
            }

            y += lineH;
        }

        // Bottom 16-color ANSI palette strip
        const qreal stripTop = outer.bottom() - 24.0;
        p.setPen(QPen(divider, 1.0));
        p.drawLine(QPointF(outer.left(), stripTop), QPointF(outer.right(), stripTop));

        const QColor swatchBorder = withAlpha(m_term.foreground, 0.24);
        for (int i = 0; i < 16; ++i) {
            const QRectF r = ansiSwatchRect(i);
            p.setPen(QPen(swatchBorder, 0.8));
            p.setBrush(m_term.ansi[static_cast<size_t>(i)]);
            p.drawRoundedRect(r, 2.5, 2.5);
        }

        p.restore();

        // Outer border
        p.setPen(QPen(borderColor, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(outer, 7.0, 7.0);
    }

private:
    QRectF ansiSwatchRect(int index) const {
        const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal stripTop = outer.bottom() - 24.0;
        const qreal padX = 10.0;
        const qreal gap = 4.0;
        const qreal groupGap = 8.0;
        const qreal availW = std::max<qreal>(16.0, outer.width() - 2.0 * padX - 14.0 * gap - groupGap);
        const qreal sw = availW / 16.0;
        const qreal sh = 10.0;
        const qreal y = stripTop + (24.0 - sh) * 0.5;
        const qreal extra = (index >= 8) ? (groupGap - gap) : 0.0;
        const qreal x = outer.left() + padX + index * (sw + gap) + extra;
        return QRectF(x, y, sw, sh);
    }

    int ansiSwatchAt(const QPoint& pos) const {
        for (int i = 0; i < 16; ++i) {
            if (ansiSwatchRect(i).adjusted(-2.0, -4.0, 2.0, 4.0).contains(pos)) {
                return i;
            }
        }
        return -1;
    }

    TerminalPalette m_term;
    UiPalette m_ui;
    QFont m_font;
    QString m_themeName;
};

struct SettingsCard {
    QLabel* titleLabel = nullptr;
    QFrame* cardFrame = nullptr;
    QVBoxLayout* cardLayout = nullptr;
};

SettingsCard createSettingsCard(const QString& title, QWidget* parent, QVBoxLayout* rootLayout) {
    SettingsCard sc;

    auto* groupLayout = new QVBoxLayout();
    groupLayout->setContentsMargins(0, 0, 0, 0);
    groupLayout->setSpacing(6);

    sc.titleLabel = new QLabel(title.toUpper(), parent);
    QFont titleFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    titleFont.setPointSizeF(9.5);
    titleFont.setBold(true);
    sc.titleLabel->setFont(titleFont);
    groupLayout->addWidget(sc.titleLabel);

    sc.cardFrame = new QFrame(parent);
    sc.cardFrame->setObjectName(QStringLiteral("spellSettingsCard"));
    sc.cardFrame->setFrameShape(QFrame::NoFrame);
    sc.cardFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sc.cardLayout = new QVBoxLayout(sc.cardFrame);
    sc.cardLayout->setContentsMargins(14, 12, 14, 12);
    sc.cardLayout->setSpacing(10);
    groupLayout->addWidget(sc.cardFrame);

    rootLayout->addLayout(groupLayout);
    return sc;
}

QFrame* createDivider(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setObjectName(QStringLiteral("spellSettingsDivider"));
    line->setFixedHeight(1);
    line->setFrameShape(QFrame::NoFrame);
    return line;
}

QLabel* createRowLabel(const QString& text, QWidget* parent) {
    auto* lbl = new QLabel(text, parent);
    QFont f = lbl->font();
    f.setPointSizeF(12.0);
    lbl->setFont(f);
    return lbl;
}

} // namespace

struct GeneralSettingsPane::Impl {
    bool syncing = false;

    QVector<QLabel*> sectionTitles;
    QVector<QFrame*> cardFrames;
    QVector<QFrame*> dividers;
    QVector<QLabel*> rowLabels;
    QVector<QLabel*> mutedHints;

    // Appearance controls
    QButtonGroup* appearanceGroup = nullptr;
    QPushButton* darkButton = nullptr;
    QPushButton* whiteButton = nullptr;

    // Code Editor controls
    QComboBox* codeThemeCombo = nullptr;
    CodePaletteSwatchBar* codeSwatchBar = nullptr;
    QComboBox* fontFamilyCombo = nullptr;
    QPushButton* resetFontButton = nullptr;
    QSlider* editorFontSizeSlider = nullptr;
    QDoubleSpinBox* editorFontSizeSpin = nullptr;
    CodePreviewWidget* codePreview = nullptr;

    // Integrated Terminal controls
    QComboBox* terminalThemeCombo = nullptr;
    QSlider* terminalFontSizeSlider = nullptr;
    QDoubleSpinBox* terminalFontSizeSpin = nullptr;
    TerminalPreviewWidget* terminalPreview = nullptr;

    void syncControlsFromThemeManager() {
        syncing = true;
        const auto& tm = ThemeManager::instance();

        // Appearance buttons
        if (darkButton && whiteButton) {
            const QSignalBlocker b1(darkButton);
            const QSignalBlocker b2(whiteButton);
            const bool isLight = (tm.appearance() == Appearance::Light);
            darkButton->setChecked(!isLight);
            whiteButton->setChecked(isLight);
        }

        // Code theme dropdown
        if (codeThemeCombo) {
            const QSignalBlocker blocker(codeThemeCombo);
            const int target = static_cast<int>(tm.codeTheme());
            const int idx = codeThemeCombo->findData(target);
            if (idx >= 0) {
                codeThemeCombo->setCurrentIndex(idx);
            }
        }

        // Editor font family dropdown
        if (fontFamilyCombo) {
            const QSignalBlocker blocker(fontFamilyCombo);
            const QString activeFamily = tm.editorFontFamily();
            int idx = fontFamilyCombo->findText(activeFamily, Qt::MatchExactly);
            if (idx < 0) {
                idx = fontFamilyCombo->findText(activeFamily, Qt::MatchFixedString);
            }
            if (idx < 0 && !activeFamily.isEmpty()) {
                fontFamilyCombo->addItem(activeFamily);
                idx = fontFamilyCombo->count() - 1;
            }
            if (idx >= 0) {
                fontFamilyCombo->setCurrentIndex(idx);
            }
            if (resetFontButton) {
                const bool isSystemDefault =
                    activeFamily.compare(systemDefaultMonoFamily(), Qt::CaseInsensitive) == 0;
                resetFontButton->setEnabled(!isSystemDefault);
            }
        }

        // Editor font size (10..24 pt)
        const double edPt = std::clamp(tm.editorFontSize(), kEditorFontMinPt, kEditorFontMaxPt);
        if (editorFontSizeSlider) {
            const QSignalBlocker blocker(editorFontSizeSlider);
            editorFontSizeSlider->setValue(qRound(edPt / kFontStepPt));
        }
        if (editorFontSizeSpin) {
            const QSignalBlocker blocker(editorFontSizeSpin);
            editorFontSizeSpin->setValue(edPt);
        }

        // Terminal theme dropdown
        if (terminalThemeCombo) {
            const QSignalBlocker blocker(terminalThemeCombo);
            const int target = static_cast<int>(tm.terminalTheme());
            const int idx = terminalThemeCombo->findData(target);
            if (idx >= 0) {
                terminalThemeCombo->setCurrentIndex(idx);
            }
        }

        // Terminal font size (9..24 pt)
        const double termPt = std::clamp(tm.terminalFontSize(), kTerminalFontMinPt, kTerminalFontMaxPt);
        if (terminalFontSizeSlider) {
            const QSignalBlocker blocker(terminalFontSizeSlider);
            terminalFontSizeSlider->setValue(qRound(termPt / kFontStepPt));
        }
        if (terminalFontSizeSpin) {
            const QSignalBlocker blocker(terminalFontSizeSpin);
            terminalFontSizeSpin->setValue(termPt);
        }

        syncing = false;
    }

    void refreshDropdownIcons() {
        const auto& tm = ThemeManager::instance();
        const UiPalette u = tm.ui();
        const QColor border = compositeOver(u.subtleBorder, u.background);

        if (darkButton) {
            darkButton->setIcon(makeAppearanceIcon(Appearance::Dark, border));
            darkButton->setIconSize(QSize(22, 16));
        }
        if (whiteButton) {
            whiteButton->setIcon(makeAppearanceIcon(Appearance::Light, border));
            whiteButton->setIconSize(QSize(22, 16));
        }

        if (codeThemeCombo) {
            const QSignalBlocker blocker(codeThemeCombo);
            for (int i = 0; i < codeThemeCombo->count(); ++i) {
                const auto id = static_cast<CodeThemeId>(codeThemeCombo->itemData(i).toInt());
                codeThemeCombo->setItemIcon(i, makeCodeThemeSwatchIcon(id, tm.appearance(), border));
            }
        }

        if (terminalThemeCombo) {
            const QSignalBlocker blocker(terminalThemeCombo);
            for (int i = 0; i < terminalThemeCombo->count(); ++i) {
                const auto id = static_cast<TerminalThemeId>(terminalThemeCombo->itemData(i).toInt());
                terminalThemeCombo->setItemIcon(
                    i, makeTerminalThemeSwatchIcon(id, tm.codeTheme(), tm.appearance(), border));
            }
        }
    }
};

GeneralSettingsPane::GeneralSettingsPane(QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    setObjectName(QStringLiteral("generalSettingsPane"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(14);

    // =========================================================================
    // 1. Appearance (Dark / White)
    // =========================================================================
    SettingsCard appearanceCard = createSettingsCard(QStringLiteral("Appearance"), this, root);
    m_impl->sectionTitles.append(appearanceCard.titleLabel);
    m_impl->cardFrames.append(appearanceCard.cardFrame);

    {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(12);

        auto* label = createRowLabel(QStringLiteral("Interface mode"), appearanceCard.cardFrame);
        m_impl->rowLabels.append(label);
        row->addWidget(label);
        row->addStretch(1);

        m_impl->appearanceGroup = new QButtonGroup(this);
        m_impl->appearanceGroup->setExclusive(true);

        m_impl->darkButton = new QPushButton(QStringLiteral("Dark"), appearanceCard.cardFrame);
        m_impl->darkButton->setObjectName(QStringLiteral("appearanceDarkButton"));
        m_impl->darkButton->setCheckable(true);
        m_impl->darkButton->setCursor(Qt::PointingHandCursor);
        m_impl->darkButton->setMinimumWidth(96);
        m_impl->darkButton->setToolTip(QStringLiteral("Monochrome Dark interface (#000000)"));

        m_impl->whiteButton = new QPushButton(QStringLiteral("White"), appearanceCard.cardFrame);
        m_impl->whiteButton->setObjectName(QStringLiteral("appearanceWhiteButton"));
        m_impl->whiteButton->setCheckable(true);
        m_impl->whiteButton->setCursor(Qt::PointingHandCursor);
        m_impl->whiteButton->setMinimumWidth(96);
        m_impl->whiteButton->setToolTip(QStringLiteral("Monochrome White interface (#FFFFFF)"));

        m_impl->appearanceGroup->addButton(m_impl->darkButton, static_cast<int>(Appearance::Dark));
        m_impl->appearanceGroup->addButton(m_impl->whiteButton, static_cast<int>(Appearance::Light));

        row->addWidget(m_impl->darkButton);
        row->addWidget(m_impl->whiteButton);
        appearanceCard.cardLayout->addLayout(row);

        auto* hint = new QLabel(
            QStringLiteral("BeatrixLeslie IDE uses a high-contrast monochrome chrome (Dark #000000 / White #FFFFFF) "
                           "with green and red reserved for diffs and errors."),
            appearanceCard.cardFrame);
        hint->setWordWrap(true);
        QFont hintFont = hint->font();
        hintFont.setPointSizeF(10.5);
        hint->setFont(hintFont);
        m_impl->mutedHints.append(hint);
        appearanceCard.cardLayout->addWidget(hint);
    }

    // =========================================================================
    // 2. Code Editor (Code Theme + Swatches + Font Family + Font Size + Preview)
    // =========================================================================
    SettingsCard codeCard = createSettingsCard(QStringLiteral("Code Editor"), this, root);
    m_impl->sectionTitles.append(codeCard.titleLabel);
    m_impl->cardFrames.append(codeCard.cardFrame);

    {
        // Code Theme row
        auto* themeRow = new QHBoxLayout();
        themeRow->setContentsMargins(0, 0, 0, 0);
        themeRow->setSpacing(12);

        auto* themeLabel = createRowLabel(QStringLiteral("Code theme"), codeCard.cardFrame);
        m_impl->rowLabels.append(themeLabel);
        themeRow->addWidget(themeLabel);
        themeRow->addStretch(1);

        m_impl->codeThemeCombo = new QComboBox(codeCard.cardFrame);
        m_impl->codeThemeCombo->setObjectName(QStringLiteral("codeThemeCombo"));
        m_impl->codeThemeCombo->setMinimumWidth(220);
        m_impl->codeThemeCombo->setIconSize(QSize(24, 15));
        for (const CodeThemeId id : ThemeManager::allCodeThemes()) {
            m_impl->codeThemeCombo->addItem(ThemeManager::codeThemeName(id), static_cast<int>(id));
        }
        themeRow->addWidget(m_impl->codeThemeCombo);
        codeCard.cardLayout->addLayout(themeRow);

        // Live color swatch strip
        m_impl->codeSwatchBar = new CodePaletteSwatchBar(codeCard.cardFrame);
        m_impl->codeSwatchBar->setObjectName(QStringLiteral("codeThemeSwatchBar"));
        codeCard.cardLayout->addWidget(m_impl->codeSwatchBar);

        auto* div1 = createDivider(codeCard.cardFrame);
        m_impl->dividers.append(div1);
        codeCard.cardLayout->addWidget(div1);

        // Editor Font Family row
        auto* familyRow = new QHBoxLayout();
        familyRow->setContentsMargins(0, 0, 0, 0);
        familyRow->setSpacing(8);

        auto* familyLabel = createRowLabel(QStringLiteral("Editor font family"), codeCard.cardFrame);
        m_impl->rowLabels.append(familyLabel);
        familyRow->addWidget(familyLabel);
        familyRow->addStretch(1);

        m_impl->fontFamilyCombo = new QComboBox(codeCard.cardFrame);
        m_impl->fontFamilyCombo->setObjectName(QStringLiteral("editorFontFamilyCombo"));
        m_impl->fontFamilyCombo->setMinimumWidth(190);
        m_impl->fontFamilyCombo->setMaxVisibleItems(16);
        const QStringList monoFamilies =
            discoverMonospaceFamilies(ThemeManager::instance().editorFontFamily());
        for (const QString& fam : monoFamilies) {
            m_impl->fontFamilyCombo->addItem(fam);
        }
        familyRow->addWidget(m_impl->fontFamilyCombo);

        m_impl->resetFontButton = new QPushButton(QStringLiteral("Default"), codeCard.cardFrame);
        m_impl->resetFontButton->setObjectName(QStringLiteral("resetFontFamilyButton"));
        m_impl->resetFontButton->setCursor(Qt::PointingHandCursor);
        m_impl->resetFontButton->setToolTip(
            QStringLiteral("Reset to system default monospace font (%1)").arg(systemDefaultMonoFamily()));
        familyRow->addWidget(m_impl->resetFontButton);
        codeCard.cardLayout->addLayout(familyRow);

        // Editor Font Size row (10–24 pt)
        auto* edSizeRow = new QHBoxLayout();
        edSizeRow->setContentsMargins(0, 0, 0, 0);
        edSizeRow->setSpacing(10);

        auto* edSizeLabel = createRowLabel(QStringLiteral("Editor font size"), codeCard.cardFrame);
        m_impl->rowLabels.append(edSizeLabel);
        edSizeRow->addWidget(edSizeLabel);
        edSizeRow->addStretch(1);

        m_impl->editorFontSizeSlider = new QSlider(Qt::Horizontal, codeCard.cardFrame);
        m_impl->editorFontSizeSlider->setObjectName(QStringLiteral("editorFontSizeSlider"));
        m_impl->editorFontSizeSlider->setRange(
            qRound(kEditorFontMinPt / kFontStepPt),
            qRound(kEditorFontMaxPt / kFontStepPt));
        m_impl->editorFontSizeSlider->setSingleStep(1);
        m_impl->editorFontSizeSlider->setPageStep(2);
        m_impl->editorFontSizeSlider->setFixedWidth(160);
        edSizeRow->addWidget(m_impl->editorFontSizeSlider);

        m_impl->editorFontSizeSpin = new QDoubleSpinBox(codeCard.cardFrame);
        m_impl->editorFontSizeSpin->setObjectName(QStringLiteral("editorFontSizeSpin"));
        m_impl->editorFontSizeSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        m_impl->editorFontSizeSpin->setRange(kEditorFontMinPt, kEditorFontMaxPt);
        m_impl->editorFontSizeSpin->setSingleStep(kFontStepPt);
        m_impl->editorFontSizeSpin->setDecimals(1);
        m_impl->editorFontSizeSpin->setSuffix(QStringLiteral(" pt"));
        m_impl->editorFontSizeSpin->setFixedWidth(86);
        {
            QFont spinFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            spinFont.setPointSizeF(11.0);
            m_impl->editorFontSizeSpin->setFont(spinFont);
        }
        edSizeRow->addWidget(m_impl->editorFontSizeSpin);
        codeCard.cardLayout->addLayout(edSizeRow);

        // Live Code Preview
        m_impl->codePreview = new CodePreviewWidget(codeCard.cardFrame);
        m_impl->codePreview->setObjectName(QStringLiteral("codeThemePreview"));
        codeCard.cardLayout->addWidget(m_impl->codePreview);
    }

    // =========================================================================
    // 3. Integrated Terminal (Terminal Theme + Font Size + Live ANSI Preview)
    // =========================================================================
    SettingsCard terminalCard = createSettingsCard(QStringLiteral("Integrated Terminal"), this, root);
    m_impl->sectionTitles.append(terminalCard.titleLabel);
    m_impl->cardFrames.append(terminalCard.cardFrame);

    {
        // Terminal Theme row
        auto* termThemeRow = new QHBoxLayout();
        termThemeRow->setContentsMargins(0, 0, 0, 0);
        termThemeRow->setSpacing(12);

        auto* termThemeLabel = createRowLabel(QStringLiteral("Terminal theme"), terminalCard.cardFrame);
        m_impl->rowLabels.append(termThemeLabel);
        termThemeRow->addWidget(termThemeLabel);
        termThemeRow->addStretch(1);

        m_impl->terminalThemeCombo = new QComboBox(terminalCard.cardFrame);
        m_impl->terminalThemeCombo->setObjectName(QStringLiteral("terminalThemeCombo"));
        m_impl->terminalThemeCombo->setMinimumWidth(220);
        m_impl->terminalThemeCombo->setIconSize(QSize(24, 15));
        for (const TerminalThemeId id : ThemeManager::allTerminalThemes()) {
            m_impl->terminalThemeCombo->addItem(
                ThemeManager::terminalThemeName(id), static_cast<int>(id));
        }
        termThemeRow->addWidget(m_impl->terminalThemeCombo);
        terminalCard.cardLayout->addLayout(termThemeRow);

        // Terminal Font Size row (9–24 pt)
        auto* termSizeRow = new QHBoxLayout();
        termSizeRow->setContentsMargins(0, 0, 0, 0);
        termSizeRow->setSpacing(10);

        auto* termSizeLabel = createRowLabel(QStringLiteral("Terminal font size"), terminalCard.cardFrame);
        m_impl->rowLabels.append(termSizeLabel);
        termSizeRow->addWidget(termSizeLabel);
        termSizeRow->addStretch(1);

        m_impl->terminalFontSizeSlider = new QSlider(Qt::Horizontal, terminalCard.cardFrame);
        m_impl->terminalFontSizeSlider->setObjectName(QStringLiteral("terminalFontSizeSlider"));
        m_impl->terminalFontSizeSlider->setRange(
            qRound(kTerminalFontMinPt / kFontStepPt),
            qRound(kTerminalFontMaxPt / kFontStepPt));
        m_impl->terminalFontSizeSlider->setSingleStep(1);
        m_impl->terminalFontSizeSlider->setPageStep(2);
        m_impl->terminalFontSizeSlider->setFixedWidth(160);
        termSizeRow->addWidget(m_impl->terminalFontSizeSlider);

        m_impl->terminalFontSizeSpin = new QDoubleSpinBox(terminalCard.cardFrame);
        m_impl->terminalFontSizeSpin->setObjectName(QStringLiteral("terminalFontSizeSpin"));
        m_impl->terminalFontSizeSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        m_impl->terminalFontSizeSpin->setRange(kTerminalFontMinPt, kTerminalFontMaxPt);
        m_impl->terminalFontSizeSpin->setSingleStep(kFontStepPt);
        m_impl->terminalFontSizeSpin->setDecimals(1);
        m_impl->terminalFontSizeSpin->setSuffix(QStringLiteral(" pt"));
        m_impl->terminalFontSizeSpin->setFixedWidth(86);
        {
            QFont spinFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            spinFont.setPointSizeF(11.0);
            m_impl->terminalFontSizeSpin->setFont(spinFont);
        }
        termSizeRow->addWidget(m_impl->terminalFontSizeSpin);
        terminalCard.cardLayout->addLayout(termSizeRow);

        // Live ANSI Terminal Preview
        m_impl->terminalPreview = new TerminalPreviewWidget(terminalCard.cardFrame);
        m_impl->terminalPreview->setObjectName(QStringLiteral("terminalThemePreview"));
        terminalCard.cardLayout->addWidget(m_impl->terminalPreview);
    }

    root->addStretch(1);

    // =========================================================================
    // Signal Wiring -> Immediate Live Updates via ThemeManager::instance()
    // =========================================================================
    connect(m_impl->appearanceGroup, &QButtonGroup::idClicked, this, [this](int id) {
        if (m_impl->syncing) {
            return;
        }
        ThemeManager::instance().setAppearance(static_cast<Appearance>(id));
    });

    connect(m_impl->codeThemeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_impl->syncing || index < 0) {
            return;
        }
        const auto id = static_cast<CodeThemeId>(m_impl->codeThemeCombo->itemData(index).toInt());
        ThemeManager::instance().setCodeTheme(id);
    });

    connect(m_impl->fontFamilyCombo, &QComboBox::currentTextChanged, this, [this](const QString& fam) {
        if (m_impl->syncing || fam.trimmed().isEmpty()) {
            return;
        }
        ThemeManager::instance().setEditorFontFamily(fam);
    });

    connect(m_impl->resetFontButton, &QPushButton::clicked, this, [this]() {
        if (m_impl->syncing) {
            return;
        }
        ThemeManager::instance().setEditorFontFamily(QString());
        applyTheme();
    });

    connect(m_impl->editorFontSizeSlider, &QSlider::valueChanged, this, [this](int rawVal) {
        if (m_impl->syncing) {
            return;
        }
        const double pt = static_cast<double>(rawVal) * kFontStepPt;
        {
            const QSignalBlocker blocker(m_impl->editorFontSizeSpin);
            m_impl->editorFontSizeSpin->setValue(pt);
        }
        ThemeManager::instance().setEditorFontSize(pt);
    });

    connect(m_impl->editorFontSizeSpin, &QDoubleSpinBox::valueChanged, this, [this](double pt) {
        if (m_impl->syncing) {
            return;
        }
        {
            const QSignalBlocker blocker(m_impl->editorFontSizeSlider);
            m_impl->editorFontSizeSlider->setValue(qRound(pt / kFontStepPt));
        }
        ThemeManager::instance().setEditorFontSize(pt);
    });

    connect(m_impl->terminalThemeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_impl->syncing || index < 0) {
            return;
        }
        const auto id =
            static_cast<TerminalThemeId>(m_impl->terminalThemeCombo->itemData(index).toInt());
        ThemeManager::instance().setTerminalTheme(id);
    });

    connect(m_impl->terminalFontSizeSlider, &QSlider::valueChanged, this, [this](int rawVal) {
        if (m_impl->syncing) {
            return;
        }
        const double pt = static_cast<double>(rawVal) * kFontStepPt;
        {
            const QSignalBlocker blocker(m_impl->terminalFontSizeSpin);
            m_impl->terminalFontSizeSpin->setValue(pt);
        }
        ThemeManager::instance().setTerminalFontSize(pt);
    });

    connect(m_impl->terminalFontSizeSpin, &QDoubleSpinBox::valueChanged, this, [this](double pt) {
        if (m_impl->syncing) {
            return;
        }
        {
            const QSignalBlocker blocker(m_impl->terminalFontSizeSlider);
            m_impl->terminalFontSizeSlider->setValue(qRound(pt / kFontStepPt));
        }
        ThemeManager::instance().setTerminalFontSize(pt);
    });

    // Bidirectional synchronization whenever any pane or shortcut mutates ThemeManager
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });
    connect(&ThemeManager::instance(), &ThemeManager::terminalThemeChanged, this, [this]() {
        applyTheme();
    });

    applyTheme();
}

GeneralSettingsPane::~GeneralSettingsPane() = default;

void GeneralSettingsPane::applyTheme() {
    const auto& tm = ThemeManager::instance();
    const UiPalette u = tm.ui();
    const CodePalette code = tm.code();
    const TerminalPalette term = tm.terminal();

    const QColor solidBorder    = compositeOver(u.subtleBorder, u.background);
    const QColor solidDivider   = compositeOver(u.divider, u.background);
    const QColor solidElevated  = compositeOver(u.elevatedSurface, u.background);
    const QColor solidSelected  = compositeOver(u.selectedFill, u.background);
    const QColor solidSecondary = compositeOver(u.textSecondary, u.background);
    const QColor solidMuted     = compositeOver(u.textMuted, u.background);

    // Sync controls & repaint dropdown/button swatch icons
    m_impl->syncControlsFromThemeManager();
    m_impl->refreshDropdownIcons();

    // Card frames
    const QString cardQss = QStringLiteral(
        "QFrame#spellSettingsCard {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 8px;"
        "}"
    ).arg(qssColor(u.surface), qssColor(solidBorder));

    for (QFrame* card : m_impl->cardFrames) {
        if (card) {
            card->setStyleSheet(cardQss);
        }
    }

    // Hairline dividers
    const QString dividerQss = QStringLiteral(
        "QFrame#spellSettingsDivider { background-color: %1; border: none; }"
    ).arg(qssColor(solidDivider));
    for (QFrame* div : m_impl->dividers) {
        if (div) {
            div->setStyleSheet(dividerQss);
        }
    }

    // Section titles (uppercase muted monospace)
    const QString sectionTitleQss = QStringLiteral(
        "QLabel { color: %1; background: transparent; letter-spacing: 0.6px; }"
    ).arg(qssColor(solidMuted));
    for (QLabel* lbl : m_impl->sectionTitles) {
        if (lbl) {
            lbl->setStyleSheet(sectionTitleQss);
        }
    }

    // Row captions
    const QString rowLabelQss = QStringLiteral(
        "QLabel { color: %1; background: transparent; }"
    ).arg(qssColor(solidSecondary));
    for (QLabel* lbl : m_impl->rowLabels) {
        if (lbl) {
            lbl->setStyleSheet(rowLabelQss);
        }
    }

    // Muted helper descriptions
    const QString hintQss = QStringLiteral(
        "QLabel { color: %1; background: transparent; }"
    ).arg(qssColor(solidMuted));
    for (QLabel* lbl : m_impl->mutedHints) {
        if (lbl) {
            lbl->setStyleSheet(hintQss);
        }
    }

    // Monochrome Dark / White toggle buttons
    const QString toggleBtnQss = QStringLiteral(
        "QPushButton {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 6px;"
        "  padding: 5px 12px;"
        "  font-weight: 500;"
        "}"
        "QPushButton:hover {"
        "  background-color: %4;"
        "  color: %5;"
        "  border-color: %6;"
        "}"
        "QPushButton:checked {"
        "  background-color: %7;"
        "  color: %5;"
        "  border: 1px solid %6;"
        "  font-weight: 600;"
        "}"
    ).arg(qssColor(u.surface),
          qssColor(solidSecondary),
          qssColor(solidBorder),
          qssColor(solidElevated),
          qssColor(u.textPrimary),
          qssColor(u.selectedBorder),
          qssColor(solidSelected));

    if (m_impl->darkButton) {
        m_impl->darkButton->setStyleSheet(toggleBtnQss);
    }
    if (m_impl->whiteButton) {
        m_impl->whiteButton->setStyleSheet(toggleBtnQss);
    }

    // Monochrome sliders
    const QString sliderQss = QStringLiteral(
        "QSlider {"
        "  background: transparent;"
        "}"
        "QSlider::groove:horizontal {"
        "  height: 4px;"
        "  background-color: %1;"
        "  border-radius: 2px;"
        "}"
        "QSlider::sub-page:horizontal {"
        "  background-color: %2;"
        "  border-radius: 2px;"
        "}"
        "QSlider::handle:horizontal {"
        "  background-color: %2;"
        "  border: 2px solid %3;"
        "  width: 12px;"
        "  height: 12px;"
        "  margin: -5px 0;"
        "  border-radius: 7px;"
        "}"
        "QSlider::handle:horizontal:hover {"
        "  border-color: %4;"
        "}"
    ).arg(qssColor(solidDivider),
          qssColor(u.accent),
          qssColor(u.background),
          qssColor(u.selectedBorder));

    if (m_impl->editorFontSizeSlider) {
        m_impl->editorFontSizeSlider->setStyleSheet(sliderQss);
    }
    if (m_impl->terminalFontSizeSlider) {
        m_impl->terminalFontSizeSlider->setStyleSheet(sliderQss);
    }

    // Update live swatches & previews
    if (m_impl->codeSwatchBar) {
        m_impl->codeSwatchBar->setPaletteData(code, u);
    }
    if (m_impl->codePreview) {
        m_impl->codePreview->setPreviewState(
            code, u, tm.editorFont(), ThemeManager::codeThemeName(tm.codeTheme()));
    }
    if (m_impl->terminalPreview) {
        m_impl->terminalPreview->setPreviewState(
            term, u, tm.terminalFont(), ThemeManager::terminalThemeName(tm.terminalTheme()));
    }
}

} // namespace spell
