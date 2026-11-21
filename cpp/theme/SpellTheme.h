#pragma once

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QFont>
#include <QFontDatabase>
#include <QObject>
#include <QPalette>
#include <QSettings>
#include <QString>
#include <QStyleHints>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace spell {

enum class Appearance {
    Dark,
    Light
};

enum class CodeThemeId {
    OneDark,
    GitHubDark,
    Dracula,
    Monokai,
    SolarizedDark,
    GitHubLight,
    XcodeLight,
    SolarizedLight
};

enum class TerminalThemeId {
    MessageTerminal,
    ClassicDark,
    ClassicLight,
    FollowCodeTheme
};

inline QColor rgbHex(uint32_t rgb, double alpha = 1.0) {
    const int r = static_cast<int>((rgb >> 16) & 0xFFu);
    const int g = static_cast<int>((rgb >> 8) & 0xFFu);
    const int b = static_cast<int>(rgb & 0xFFu);
    const int a = static_cast<int>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255.0));
    return QColor(r, g, b, a);
}

inline QColor withAlpha(const QColor& color, double alpha) {
    QColor out(color);
    out.setAlpha(static_cast<int>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255.0)));
    return out;
}

inline QColor compositeOver(const QColor& fg, const QColor& bg) {
    const double a = fg.alphaF();
    const double inv = 1.0 - a;
    const int r = static_cast<int>(std::lround(fg.red() * a + bg.red() * inv));
    const int g = static_cast<int>(std::lround(fg.green() * a + bg.green() * inv));
    const int b = static_cast<int>(std::lround(fg.blue() * a + bg.blue() * inv));
    return QColor(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255), 255);
}

inline QString qssColor(const QColor& color) {
    if (color.alpha() >= 255) {
        return color.name(QColor::HexRgb);
    }
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

inline bool isLightCodeTheme(CodeThemeId id) {
    return id == CodeThemeId::GitHubLight
        || id == CodeThemeId::XcodeLight
        || id == CodeThemeId::SolarizedLight;
}

/// Semantic monochrome UI palette for Spell IDE.
/// Dark uses pure black (#000000) with white (#FFFFFF) foregrounds; Light is the exact inverse.
/// Green and red are reserved for diff additions/removals and danger/error states.
struct UiPalette {
    QColor background;
    QColor surface;
    QColor elevatedSurface;
    QColor accent;
    QColor selectedFill;
    QColor selectedBorder;
    QColor textPrimary;
    QColor textSecondary;
    QColor textMuted;
    QColor subtleBorder;
    QColor divider;
    QColor success;
    QColor warning;
    QColor danger;
    QColor info;
    QColor diffAdded;
    QColor diffRemoved;

    static UiPalette dark() {
        UiPalette p;
        p.background      = rgbHex(0x000000);
        p.surface         = rgbHex(0x000000);
        p.elevatedSurface = rgbHex(0xFFFFFF, 0.07);
        p.accent          = rgbHex(0xFFFFFF);
        p.selectedFill    = rgbHex(0xFFFFFF, 0.10);
        p.selectedBorder  = rgbHex(0xFFFFFF, 0.58);
        p.textPrimary     = rgbHex(0xFFFFFF);
        p.textSecondary   = rgbHex(0xFFFFFF, 0.72);
        p.textMuted       = rgbHex(0xFFFFFF, 0.48);
        p.subtleBorder    = rgbHex(0xFFFFFF, 0.38);
        p.divider         = p.subtleBorder;
        p.success         = rgbHex(0xFFFFFF);
        p.warning         = rgbHex(0xFFFFFF);
        p.danger          = rgbHex(0xF85149);
        p.info            = rgbHex(0xFFFFFF, 0.85);
        p.diffAdded       = rgbHex(0x3FB950);
        p.diffRemoved     = rgbHex(0xF85149);
        return p;
    }

    static UiPalette light() {
        UiPalette p;
        p.background      = rgbHex(0xFFFFFF);
        p.surface         = rgbHex(0xFFFFFF);
        p.elevatedSurface = rgbHex(0x000000, 0.05);
        p.accent          = rgbHex(0x000000);
        p.selectedFill    = rgbHex(0x000000, 0.10);
        p.selectedBorder  = rgbHex(0x000000, 0.58);
        p.textPrimary     = rgbHex(0x000000);
        p.textSecondary   = rgbHex(0x000000, 0.68);
        p.textMuted       = rgbHex(0x000000, 0.45);
        p.subtleBorder    = rgbHex(0x000000, 0.30);
        p.divider         = p.subtleBorder;
        p.success         = rgbHex(0x000000);
        p.warning         = rgbHex(0x000000);
        p.danger          = rgbHex(0xCF222E);
        p.info            = rgbHex(0x000000, 0.80);
        p.diffAdded       = rgbHex(0x1A7F37);
        p.diffRemoved     = rgbHex(0xCF222E);
        return p;
    }

    static UiPalette forAppearance(Appearance app) {
        return app == Appearance::Light ? light() : dark();
    }
};

/// Syntax colors for the code editor, chat code blocks, tool calls, and diffs.
struct CodePalette {
    bool hasCustomBackground = false;
    QColor background;
    QColor plain;
    QColor keyword;
    QColor function;
    QColor string;
    QColor type;
    QColor number;
    QColor comment;
    QColor added;
    QColor removed;
    QColor hunk;

    static CodePalette codePaletteFor(CodeThemeId id, Appearance app) {
        switch (id) {
        case CodeThemeId::OneDark:
            return CodePalette{
                false,
                app == Appearance::Dark ? rgbHex(0x000000) : rgbHex(0x282C34),
                rgbHex(0xABB2BF), rgbHex(0xC678DD), rgbHex(0x61AFEF), rgbHex(0x98C379),
                rgbHex(0xE5C07B), rgbHex(0xD19A66), rgbHex(0x5C6370),
                rgbHex(0x98C379), rgbHex(0xE06C75), rgbHex(0x61AFEF)
            };
        case CodeThemeId::GitHubDark:
            return CodePalette{
                true, rgbHex(0x0D1117),
                rgbHex(0xE6EDF3), rgbHex(0xFF7B72), rgbHex(0xD2A8FF), rgbHex(0xA5D6FF),
                rgbHex(0xFFA657), rgbHex(0x79C0FF), rgbHex(0x8B949E),
                rgbHex(0x3FB950), rgbHex(0xF85149), rgbHex(0x79C0FF)
            };
        case CodeThemeId::Dracula:
            return CodePalette{
                true, rgbHex(0x282A36),
                rgbHex(0xF8F8F2), rgbHex(0xFF79C6), rgbHex(0x50FA7B), rgbHex(0xF1FA8C),
                rgbHex(0x8BE9FD), rgbHex(0xBD93F9), rgbHex(0x6272A4),
                rgbHex(0x50FA7B), rgbHex(0xFF5555), rgbHex(0xBD93F9)
            };
        case CodeThemeId::Monokai:
            return CodePalette{
                true, rgbHex(0x272822),
                rgbHex(0xF8F8F2), rgbHex(0xF92672), rgbHex(0xA6E22E), rgbHex(0xE6DB74),
                rgbHex(0x66D9EF), rgbHex(0xAE81FF), rgbHex(0x75715E),
                rgbHex(0xA6E22E), rgbHex(0xF92672), rgbHex(0x66D9EF)
            };
        case CodeThemeId::SolarizedDark:
            return CodePalette{
                true, rgbHex(0x002B36),
                rgbHex(0x93A1A1), rgbHex(0x859900), rgbHex(0x268BD2), rgbHex(0x2AA198),
                rgbHex(0xB58900), rgbHex(0xD33682), rgbHex(0x586E75),
                rgbHex(0x859900), rgbHex(0xDC322F), rgbHex(0x268BD2)
            };
        case CodeThemeId::GitHubLight:
            return CodePalette{
                true, rgbHex(0xFFFFFF),
                rgbHex(0x1F2328), rgbHex(0xCF222E), rgbHex(0x8250DF), rgbHex(0x0A3069),
                rgbHex(0x953800), rgbHex(0x0550AE), rgbHex(0x6E7781),
                rgbHex(0x116329), rgbHex(0x82071E), rgbHex(0x0550AE)
            };
        case CodeThemeId::XcodeLight:
            return CodePalette{
                true, rgbHex(0xFFFFFF),
                rgbHex(0x000000), rgbHex(0xAD3DA4), rgbHex(0x4B21B0), rgbHex(0xD12F1B),
                rgbHex(0x703DAA), rgbHex(0x272AD8), rgbHex(0x707F8C),
                rgbHex(0x2E7D32), rgbHex(0xC62828), rgbHex(0x0F68A0)
            };
        case CodeThemeId::SolarizedLight:
            return CodePalette{
                true, rgbHex(0xFDF6E3),
                rgbHex(0x586E75), rgbHex(0x859900), rgbHex(0x268BD2), rgbHex(0x2AA198),
                rgbHex(0xB58900), rgbHex(0xD33682), rgbHex(0x93A1A1),
                rgbHex(0x859900), rgbHex(0xDC322F), rgbHex(0x268BD2)
            };
        }
        return codePaletteFor(CodeThemeId::OneDark, app);
    }
};

inline CodePalette codePaletteFor(CodeThemeId id, Appearance app) {
    return CodePalette::codePaletteFor(id, app);
}

/// Colors used by the integrated terminal renderer.
struct TerminalPalette {
    QColor background;
    QColor foreground;
    QColor bold;
    QColor cursor;
    QColor cursorText;
    QColor selection;
    std::array<QColor, 16> ansi;

    static std::array<QColor, 16> ansiFromHex(const std::array<uint32_t, 16>& hexValues) {
        std::array<QColor, 16> out{};
        for (size_t i = 0; i < 16; ++i) {
            out[i] = rgbHex(hexValues[i]);
        }
        return out;
    }

    static TerminalPalette terminalPaletteFor(TerminalThemeId id, CodeThemeId codeId, Appearance app) {
        static constexpr std::array<uint32_t, 16> kOneDarkAnsi = {
            0x26262Eu, 0xE06C75u, 0x98C379u, 0xE5C07Bu, 0x61AFEFu, 0xC678DDu, 0x56B6C2u, 0xADADADu,
            0x5C6370u, 0xEF8891u, 0xB5D99Cu, 0xF0D197u, 0x82C1F5u, 0xD898EAu, 0x7ACBD3u, 0xFFFFFFu
        };
        static constexpr std::array<uint32_t, 16> kClassicDarkAnsi = {
            0x000000u, 0xC23621u, 0x25BC24u, 0xADAD27u, 0x492EE1u, 0xD338D3u, 0x33BBC8u, 0xCBCCCDu,
            0x818383u, 0xFC391Fu, 0x31E722u, 0xEAEC23u, 0x5833FFu, 0xF935F8u, 0x14F0F0u, 0xE9EBEBu
        };
        static constexpr std::array<uint32_t, 16> kClassicLightAnsi = {
            0x000000u, 0x990000u, 0x00A600u, 0x999900u, 0x0000B2u, 0xB200B2u, 0x00A6B2u, 0xBFBFBFu,
            0x666666u, 0xE50000u, 0x00D900u, 0xC4A000u, 0x0000FFu, 0xE500E5u, 0x00C5C5u, 0x4D4D4Du
        };

        switch (id) {
        case TerminalThemeId::MessageTerminal:
            return TerminalPalette{
                rgbHex(0x0A0F0A),
                rgbHex(0xCFEBC0),
                rgbHex(0xE9FADF),
                rgbHex(0x98C379),
                rgbHex(0x0A0F0A),
                rgbHex(0x98C379, 0.30),
                ansiFromHex(kOneDarkAnsi)
            };
        case TerminalThemeId::ClassicDark:
            return TerminalPalette{
                rgbHex(0x000000),
                rgbHex(0xF2F2F2),
                rgbHex(0xFFFFFF),
                rgbHex(0xBFBFBF),
                rgbHex(0x000000),
                rgbHex(0x3A5A8C, 0.65),
                ansiFromHex(kClassicDarkAnsi)
            };
        case TerminalThemeId::ClassicLight:
            return TerminalPalette{
                rgbHex(0xFFFFFF),
                rgbHex(0x1D1D1F),
                rgbHex(0x000000),
                rgbHex(0x7F7F7F),
                rgbHex(0xFFFFFF),
                rgbHex(0xB3D7FF, 0.90),
                ansiFromHex(kClassicLightAnsi)
            };
        case TerminalThemeId::FollowCodeTheme: {
            const CodePalette code = CodePalette::codePaletteFor(codeId, app);
            const QColor bg = code.hasCustomBackground ? code.background : rgbHex(0x000000);
            auto ansi = ansiFromHex(kOneDarkAnsi);
            ansi[1] = code.removed;
            ansi[2] = code.added;
            ansi[3] = code.type;
            ansi[4] = code.function;
            ansi[5] = code.keyword;
            ansi[6] = code.string;
            if (isLightCodeTheme(codeId)) {
                ansi[0]  = rgbHex(0xD0D0D0);
                ansi[7]  = rgbHex(0x555555);
                ansi[15] = rgbHex(0x111111);
            }
            return TerminalPalette{
                bg,
                code.plain,
                code.plain,
                code.function,
                bg,
                withAlpha(code.function, 0.30),
                ansi
            };
        }
        }
        return terminalPaletteFor(TerminalThemeId::MessageTerminal, codeId, app);
    }
};

inline TerminalPalette terminalPaletteFor(TerminalThemeId id, CodeThemeId codeId, Appearance app) {
    return TerminalPalette::terminalPaletteFor(id, codeId, app);
}

/// Central theme & typography settings store persisted via QSettings("Spell", "SpellIDE").
class ThemeManager : public QObject {
    Q_OBJECT

public:
    static ThemeManager& instance() {
        static ThemeManager inst;
        return inst;
    }

    Appearance appearance() const { return m_appearance; }

    void setAppearance(Appearance a) {
        if (m_appearance == a) {
            return;
        }
        m_appearance = a;
        if (m_appearance == Appearance::Light && m_codeTheme == CodeThemeId::OneDark) {
            m_codeTheme = CodeThemeId::XcodeLight;
        } else if (m_appearance == Appearance::Dark && m_codeTheme == CodeThemeId::XcodeLight) {
            m_codeTheme = CodeThemeId::OneDark;
        }
        persist();
        if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
            applyToApplication(app);
        }
        emit themeChanged();
        if (m_terminalTheme == TerminalThemeId::FollowCodeTheme) {
            emit terminalThemeChanged();
        }
    }

    CodeThemeId codeTheme() const { return m_codeTheme; }

    void setCodeTheme(CodeThemeId id) {
        if (m_codeTheme == id) {
            return;
        }
        m_codeTheme = id;
        persist();
        emit themeChanged();
        if (m_terminalTheme == TerminalThemeId::FollowCodeTheme) {
            emit terminalThemeChanged();
        }
    }

    TerminalThemeId terminalTheme() const { return m_terminalTheme; }

    void setTerminalTheme(TerminalThemeId id) {
        if (m_terminalTheme == id) {
            return;
        }
        m_terminalTheme = id;
        persist();
        emit terminalThemeChanged();
    }

    double editorFontSize() const { return m_editorFontSize; }

    void setEditorFontSize(double pt) {
        const double clamped = std::clamp(pt, 9.0, 28.0);
        if (std::abs(m_editorFontSize - clamped) < 1e-6) {
            return;
        }
        m_editorFontSize = clamped;
        persist();
        emit themeChanged();
    }

    double terminalFontSize() const { return m_terminalFontSize; }

    void setTerminalFontSize(double pt) {
        const double clamped = std::clamp(pt, 9.0, 24.0);
        if (std::abs(m_terminalFontSize - clamped) < 1e-6) {
            return;
        }
        m_terminalFontSize = clamped;
        persist();
        emit terminalThemeChanged();
    }

    QString editorFontFamily() const {
        if (!m_editorFontFamily.isEmpty()) {
            return m_editorFontFamily;
        }
        return defaultMonoFontFamily();
    }

    void setEditorFontFamily(const QString& fam) {
        const QString trimmed = fam.trimmed();
        if (m_editorFontFamily == trimmed) {
            return;
        }
        m_editorFontFamily = trimmed;
        persist();
        emit themeChanged();
        emit terminalThemeChanged();
    }

    QFont editorFont() const {
        QFont font(editorFontFamily());
        font.setStyleHint(QFont::Monospace);
        font.setPointSizeF(m_editorFontSize);
        return font;
    }

    QFont terminalFont() const {
        QFont font(editorFontFamily());
        font.setStyleHint(QFont::Monospace);
        font.setPointSizeF(m_terminalFontSize);
        return font;
    }

    UiPalette ui() const {
        return UiPalette::forAppearance(m_appearance);
    }

    CodePalette code() const {
        return CodePalette::codePaletteFor(m_codeTheme, m_appearance);
    }

    TerminalPalette terminal() const {
        return TerminalPalette::terminalPaletteFor(m_terminalTheme, m_codeTheme, m_appearance);
    }

    QString buildApplicationStyleSheet() const {
        const UiPalette u = ui();
        const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
        const QColor solidSelected = compositeOver(u.selectedFill, u.background);
        const QColor solidPopup    = m_appearance == Appearance::Dark
                                         ? rgbHex(0x0D0D0D)
                                         : rgbHex(0xFAFAFA);
        const QColor solidBorder   = compositeOver(u.subtleBorder, u.background);
        const QColor solidDivider  = compositeOver(u.divider, u.background);
        const QColor solidMuted    = compositeOver(u.textMuted, u.background);
        const QColor solidSecondary = compositeOver(u.textSecondary, u.background);
        const QColor scrollHover   = compositeOver(withAlpha(u.accent, 0.34), u.background);
        const QColor dangerFill    = withAlpha(u.danger, 0.16);
        const QColor dangerBorder  = withAlpha(u.danger, 0.65);

        QString qss;
        qss.reserve(6144);

        qss += QStringLiteral(
            "QWidget {\n"
            "  background-color: %1;\n"
            "  color: %2;\n"
            "  selection-background-color: %3;\n"
            "  selection-color: %2;\n"
            "}\n"
            "QMainWindow, QDialog {\n"
            "  background-color: %1;\n"
            "  color: %2;\n"
            "}\n"
            "QLabel {\n"
            "  background-color: transparent;\n"
            "  color: %2;\n"
            "}\n"
            "QFrame[frameShape=\"4\"], QFrame[frameShape=\"5\"] {\n"
            "  color: %4;\n"
            "  background-color: %4;\n"
            "  border: none;\n"
            "}\n"
        ).arg(qssColor(u.background), qssColor(u.textPrimary),
              qssColor(solidSelected), qssColor(solidDivider));

        qss += QStringLiteral(
            "QSplitter {\n"
            "  background-color: %1;\n"
            "}\n"
            "QSplitter::handle {\n"
            "  background-color: %2;\n"
            "}\n"
            "QSplitter::handle:horizontal {\n"
            "  width: 1px;\n"
            "}\n"
            "QSplitter::handle:vertical {\n"
            "  height: 1px;\n"
            "}\n"
            "QSplitter::handle:hover, QSplitter::handle:pressed {\n"
            "  background-color: %3;\n"
            "}\n"
        ).arg(qssColor(u.background), qssColor(solidDivider), qssColor(u.selectedBorder));

        qss += QStringLiteral(
            "QMenuBar {\n"
            "  background-color: %1;\n"
            "  color: %2;\n"
            "  border-bottom: 1px solid %3;\n"
            "}\n"
            "QMenuBar::item {\n"
            "  background-color: transparent;\n"
            "  padding: 4px 10px;\n"
            "}\n"
            "QMenuBar::item:selected, QMenuBar::item:pressed {\n"
            "  background-color: %4;\n"
            "  border-radius: 4px;\n"
            "}\n"
            "QMenu {\n"
            "  background-color: %5;\n"
            "  color: %2;\n"
            "  border: 1px solid %6;\n"
            "  border-radius: 8px;\n"
            "  padding: 4px;\n"
            "}\n"
            "QMenu::item {\n"
            "  background-color: transparent;\n"
            "  padding: 5px 22px 5px 12px;\n"
            "  border-radius: 5px;\n"
            "}\n"
            "QMenu::item:selected {\n"
            "  background-color: %4;\n"
            "  color: %2;\n"
            "}\n"
            "QMenu::item:disabled {\n"
            "  color: %7;\n"
            "}\n"
            "QMenu::separator {\n"
            "  height: 1px;\n"
            "  background-color: %3;\n"
            "  margin: 4px 8px;\n"
            "}\n"
        ).arg(qssColor(u.surface), qssColor(u.textPrimary), qssColor(solidDivider),
              qssColor(solidSelected), qssColor(solidPopup), qssColor(solidBorder),
              qssColor(solidMuted));

        qss += QStringLiteral(
            "QToolBar {\n"
            "  background-color: %1;\n"
            "  border: none;\n"
            "  border-bottom: 1px solid %2;\n"
            "  spacing: 4px;\n"
            "  padding: 2px 6px;\n"
            "}\n"
            "QToolButton {\n"
            "  background-color: transparent;\n"
            "  color: %3;\n"
            "  border: 1px solid transparent;\n"
            "  border-radius: 6px;\n"
            "  padding: 4px 7px;\n"
            "}\n"
            "QToolButton:hover {\n"
            "  background-color: %4;\n"
            "  color: %5;\n"
            "}\n"
            "QToolButton:pressed, QToolButton:checked {\n"
            "  background-color: %6;\n"
            "  border: 1px solid %7;\n"
            "  color: %5;\n"
            "}\n"
            "QToolButton:disabled {\n"
            "  color: %8;\n"
            "}\n"
        ).arg(qssColor(u.surface), qssColor(solidDivider), qssColor(solidSecondary),
              qssColor(solidElevated), qssColor(u.textPrimary), qssColor(solidSelected),
              qssColor(u.selectedBorder), qssColor(solidMuted));

        qss += QStringLiteral(
            "QPushButton {\n"
            "  background-color: %1;\n"
            "  color: %2;\n"
            "  border: 1px solid %3;\n"
            "  border-radius: 6px;\n"
            "  padding: 5px 12px;\n"
            "  font-weight: 600;\n"
            "}\n"
            "QPushButton:hover {\n"
            "  background-color: %4;\n"
            "  border-color: %5;\n"
            "}\n"
            "QPushButton:pressed, QPushButton:checked {\n"
            "  background-color: %6;\n"
            "  border-color: %5;\n"
            "}\n"
            "QPushButton:disabled {\n"
            "  color: %7;\n"
            "  border-color: %8;\n"
            "  background-color: %1;\n"
            "}\n"
            "QPushButton[primary=\"true\"] {\n"
            "  background-color: %6;\n"
            "  border: 1px solid %5;\n"
            "  color: %2;\n"
            "}\n"
            "QPushButton[primary=\"true\"]:hover {\n"
            "  background-color: %9;\n"
            "}\n"
            "QPushButton[danger=\"true\"] {\n"
            "  background-color: %10;\n"
            "  border: 1px solid %11;\n"
            "  color: %12;\n"
            "}\n"
            "QPushButton[flat=\"true\"] {\n"
            "  background-color: transparent;\n"
            "  border: 1px solid transparent;\n"
            "}\n"
            "QPushButton[flat=\"true\"]:hover {\n"
            "  background-color: %4;\n"
            "}\n"
        ).arg(qssColor(u.surface), qssColor(u.textPrimary), qssColor(solidBorder),
              qssColor(solidElevated), qssColor(u.selectedBorder), qssColor(solidSelected),
              qssColor(solidMuted), qssColor(solidDivider),
              qssColor(withAlpha(u.accent, 0.18)),
              qssColor(dangerFill), qssColor(dangerBorder), qssColor(u.danger));

        qss += QStringLiteral(
            "QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox {\n"
            "  background-color: %1;\n"
            "  color: %2;\n"
            "  border: 1px solid %3;\n"
            "  border-radius: 6px;\n"
            "  padding: 5px 8px;\n"
            "  selection-background-color: %4;\n"
            "  selection-color: %2;\n"
            "}\n"
            "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, "
            "QSpinBox:focus, QDoubleSpinBox:focus {\n"
            "  border: 1px solid %5;\n"
            "}\n"
            "QLineEdit:disabled, QPlainTextEdit:disabled, QTextEdit:disabled,\n"
            "QSpinBox:disabled, QDoubleSpinBox:disabled {\n"
            "  color: %6;\n"
            "  border-color: %7;\n"
            "}\n"
            "QAbstractSpinBox::up-button, QAbstractSpinBox::down-button,\n"
            "QSpinBox::up-button, QSpinBox::down-button,\n"
            "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {\n"
            "  subcontrol-origin: padding;\n"
            "  width: 0px;\n"
            "  height: 0px;\n"
            "  border: none;\n"
            "  background: transparent;\n"
            "}\n"
            "QAbstractSpinBox::up-arrow, QAbstractSpinBox::down-arrow,\n"
            "QSpinBox::up-arrow, QSpinBox::down-arrow,\n"
            "QDoubleSpinBox::up-arrow, QDoubleSpinBox::down-arrow {\n"
            "  width: 0px;\n"
            "  height: 0px;\n"
            "  border: none;\n"
            "  image: none;\n"
            "}\n"
            "QSlider {\n"
            "  background: transparent;\n"
            "  border: none;\n"
            "}\n"
        ).arg(qssColor(u.surface), qssColor(u.textPrimary), qssColor(solidBorder),
              qssColor(solidSelected), qssColor(u.selectedBorder),
              qssColor(solidMuted), qssColor(solidDivider));

        qss += QStringLiteral(
            "QComboBox {\n"
            "  background-color: %1;\n"
            "  color: %2;\n"
            "  border: 1px solid %3;\n"
            "  border-radius: 6px;\n"
            "  padding: 4px 24px 4px 9px;\n"
            "}\n"
            "QComboBox QLineEdit {\n"
            "  background: transparent;\n"
            "  border: none;\n"
            "  padding: 0px;\n"
            "  margin: 0px;\n"
            "}\n"
            "QComboBox:hover, QComboBox:on {\n"
            "  background-color: %4;\n"
            "  border-color: %5;\n"
            "}\n"
            "QComboBox::drop-down {\n"
            "  subcontrol-origin: padding;\n"
            "  subcontrol-position: center right;\n"
            "  width: 20px;\n"
            "  border: none;\n"
            "}\n"
            "QComboBox QAbstractItemView {\n"
            "  background-color: %6;\n"
            "  color: %2;\n"
            "  border: 1px solid %3;\n"
            "  border-radius: 6px;\n"
            "  padding: 3px;\n"
            "  selection-background-color: %7;\n"
            "  selection-color: %2;\n"
            "  outline: 0;\n"
            "}\n"
        ).arg(qssColor(u.surface), qssColor(u.textPrimary), qssColor(solidBorder),
              qssColor(solidElevated), qssColor(u.selectedBorder),
              qssColor(solidPopup), qssColor(solidSelected));

        qss += QStringLiteral(
            "QTabWidget::pane {\n"
            "  background-color: %1;\n"
            "  border: none;\n"
            "  border-top: 1px solid %2;\n"
            "}\n"
            "QTabBar {\n"
            "  background-color: %1;\n"
            "}\n"
            "QTabBar::tab {\n"
            "  background-color: %1;\n"
            "  color: %3;\n"
            "  border: none;\n"
            "  border-right: 1px solid %2;\n"
            "  border-bottom: 2px solid transparent;\n"
            "  padding: 6px 12px;\n"
            "  min-height: 20px;\n"
            "}\n"
            "QTabBar::tab:hover {\n"
            "  background-color: %4;\n"
            "  color: %5;\n"
            "}\n"
            "QTabBar::tab:selected {\n"
            "  background-color: %6;\n"
            "  color: %5;\n"
            "  border-bottom: 2px solid %7;\n"
            "}\n"
        ).arg(qssColor(u.background), qssColor(solidDivider), qssColor(solidSecondary),
              qssColor(solidElevated), qssColor(u.textPrimary), qssColor(solidSelected),
              qssColor(u.accent));

        qss += QStringLiteral(
            "QTreeView, QListView, QTableView, QTreeWidget, QListWidget, QTableWidget {\n"
            "  background-color: %1;\n"
            "  alternate-background-color: %1;\n"
            "  color: %2;\n"
            "  border: none;\n"
            "  outline: 0;\n"
            "  gridline-color: %3;\n"
            "}\n"
            "QTreeView::item, QListView::item, QTableView::item {\n"
            "  padding: 3px 6px;\n"
            "  border-radius: 5px;\n"
            "}\n"
            "QTreeView::item:hover, QListView::item:hover, QTableView::item:hover {\n"
            "  background-color: %4;\n"
            "}\n"
            "QTreeView::item:selected, QListView::item:selected, QTableView::item:selected {\n"
            "  background-color: %5;\n"
            "  color: %2;\n"
            "}\n"
            "QHeaderView::section {\n"
            "  background-color: %1;\n"
            "  color: %6;\n"
            "  border: none;\n"
            "  border-bottom: 1px solid %3;\n"
            "  border-right: 1px solid %3;\n"
            "  padding: 5px 8px;\n"
            "  font-weight: 600;\n"
            "}\n"
        ).arg(qssColor(u.background), qssColor(u.textPrimary), qssColor(solidDivider),
              qssColor(solidElevated), qssColor(solidSelected), qssColor(solidSecondary));

        qss += QStringLiteral(
            "QScrollBar:vertical {\n"
            "  background-color: transparent;\n"
            "  width: 9px;\n"
            "  margin: 2px 1px 2px 1px;\n"
            "}\n"
            "QScrollBar::handle:vertical {\n"
            "  background-color: %1;\n"
            "  min-height: 24px;\n"
            "  border-radius: 3px;\n"
            "}\n"
            "QScrollBar::handle:vertical:hover {\n"
            "  background-color: %2;\n"
            "}\n"
            "QScrollBar:horizontal {\n"
            "  background-color: transparent;\n"
            "  height: 9px;\n"
            "  margin: 1px 2px 1px 2px;\n"
            "}\n"
            "QScrollBar::handle:horizontal {\n"
            "  background-color: %1;\n"
            "  min-width: 24px;\n"
            "  border-radius: 3px;\n"
            "}\n"
            "QScrollBar::handle:horizontal:hover {\n"
            "  background-color: %2;\n"
            "}\n"
            "QScrollBar::add-line, QScrollBar::sub-line,\n"
            "QScrollBar::add-page, QScrollBar::sub-page {\n"
            "  background: transparent;\n"
            "  border: none;\n"
            "  width: 0px;\n"
            "  height: 0px;\n"
            "}\n"
        ).arg(qssColor(solidBorder), qssColor(scrollHover));

        qss += QStringLiteral(
            "QCheckBox, QRadioButton {\n"
            "  background-color: transparent;\n"
            "  color: %1;\n"
            "  spacing: 6px;\n"
            "}\n"
            "QCheckBox:disabled, QRadioButton:disabled {\n"
            "  color: %5;\n"
            "}\n"
            "QCheckBox::indicator, QRadioButton::indicator {\n"
            "  width: 14px;\n"
            "  height: 14px;\n"
            "  background-color: transparent;\n"
            "  border: 1px solid %3;\n"
            "}\n"
            "QCheckBox::indicator {\n"
            "  border-radius: 4px;\n"
            "}\n"
            "QRadioButton::indicator {\n"
            "  border-radius: 8px;\n"
            "}\n"
            "QCheckBox::indicator:hover, QRadioButton::indicator:hover {\n"
            "  border-color: %4;\n"
            "}\n"
            "QCheckBox::indicator:checked, QRadioButton::indicator:checked {\n"
            "  border: 1px solid %1;\n"
            "  background: qradialgradient(cx:0.5, cy:0.5, radius:0.42, fx:0.5, fy:0.5, "
            "stop:0 %1, stop:0.65 %1, stop:0.72 %2, stop:1 %2);\n"
            "}\n"
            "QCheckBox::indicator:disabled, QRadioButton::indicator:disabled {\n"
            "  border-color: %6;\n"
            "}\n"
            "QCheckBox::indicator:checked:disabled, QRadioButton::indicator:checked:disabled {\n"
            "  border: 1px solid %5;\n"
            "  background: qradialgradient(cx:0.5, cy:0.5, radius:0.42, fx:0.5, fy:0.5, "
            "stop:0 %5, stop:0.65 %5, stop:0.72 %2, stop:1 %2);\n"
            "}\n"
        ).arg(qssColor(u.textPrimary), qssColor(u.background), qssColor(solidBorder),
              qssColor(u.selectedBorder), qssColor(solidMuted), qssColor(solidDivider));

        qss += QStringLiteral(
            "QGroupBox {\n"
            "  background-color: %2;\n"
            "  border: 1px solid %3;\n"
            "  border-radius: 8px;\n"
            "  margin-top: 12px;\n"
            "  padding: 10px;\n"
            "  font-weight: 600;\n"
            "}\n"
            "QGroupBox::title {\n"
            "  subcontrol-origin: margin;\n"
            "  subcontrol-position: top left;\n"
            "  left: 10px;\n"
            "  padding: 0 5px;\n"
            "  color: %5;\n"
            "}\n"
            "QProgressBar {\n"
            "  background-color: %6;\n"
            "  border: 1px solid %3;\n"
            "  border-radius: 4px;\n"
            "  text-align: center;\n"
            "  color: %1;\n"
            "  max-height: 8px;\n"
            "}\n"
            "QProgressBar::chunk {\n"
            "  background-color: %4;\n"
            "  border-radius: 3px;\n"
            "}\n"
            "QStatusBar {\n"
            "  background-color: %2;\n"
            "  color: %5;\n"
            "  border-top: 1px solid %7;\n"
            "}\n"
            "QStatusBar::item {\n"
            "  border: none;\n"
            "}\n"
            "QToolTip {\n"
            "  background-color: %8;\n"
            "  color: %1;\n"
            "  border: 1px solid %3;\n"
            "  border-radius: 6px;\n"
            "  padding: 5px 8px;\n"
            "}\n"
        ).arg(qssColor(u.textPrimary), qssColor(u.surface), qssColor(solidBorder),
              qssColor(u.accent), qssColor(solidSecondary), qssColor(solidElevated),
              qssColor(solidDivider), qssColor(solidPopup));

        return qss;
    }

    void applyToApplication(QApplication* app) const {
        if (!app) {
            return;
        }
        const UiPalette u = ui();
        const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
        const QColor solidSelected = compositeOver(u.selectedFill, u.background);
        const QColor solidMuted    = compositeOver(u.textMuted, u.background);
        const QColor solidPopup    = m_appearance == Appearance::Dark
                                         ? rgbHex(0x0D0D0D)
                                         : rgbHex(0xFAFAFA);

        QPalette pal;
        pal.setColor(QPalette::Window, u.background);
        pal.setColor(QPalette::WindowText, u.textPrimary);
        pal.setColor(QPalette::Base, u.surface);
        pal.setColor(QPalette::AlternateBase, solidElevated);
        pal.setColor(QPalette::ToolTipBase, solidPopup);
        pal.setColor(QPalette::ToolTipText, u.textPrimary);
        pal.setColor(QPalette::Text, u.textPrimary);
        pal.setColor(QPalette::Button, u.surface);
        pal.setColor(QPalette::ButtonText, u.textPrimary);
        pal.setColor(QPalette::BrightText, u.accent);
        pal.setColor(QPalette::Highlight, solidSelected);
        pal.setColor(QPalette::HighlightedText, u.textPrimary);
        pal.setColor(QPalette::Link, u.accent);
        pal.setColor(QPalette::LinkVisited, u.textSecondary);
        pal.setColor(QPalette::PlaceholderText, solidMuted);
        pal.setColor(QPalette::Mid, compositeOver(u.subtleBorder, u.background));
        pal.setColor(QPalette::Dark, compositeOver(u.divider, u.background));

        pal.setColor(QPalette::Disabled, QPalette::WindowText, solidMuted);
        pal.setColor(QPalette::Disabled, QPalette::Text, solidMuted);
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, solidMuted);
        pal.setColor(QPalette::Disabled, QPalette::HighlightedText, solidMuted);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (auto* hints = app->styleHints()) {
            hints->setColorScheme(m_appearance == Appearance::Light
                                      ? Qt::ColorScheme::Light
                                      : Qt::ColorScheme::Dark);
        }
#endif
        app->setPalette(pal);
        app->setStyleSheet(buildApplicationStyleSheet());
    }

    static QString appearanceName(Appearance a) {
        return a == Appearance::Light ? QStringLiteral("White") : QStringLiteral("Dark");
    }

    static QString codeThemeName(CodeThemeId id) {
        switch (id) {
        case CodeThemeId::OneDark:        return QStringLiteral("One Dark");
        case CodeThemeId::GitHubDark:     return QStringLiteral("GitHub Dark");
        case CodeThemeId::Dracula:        return QStringLiteral("Dracula");
        case CodeThemeId::Monokai:        return QStringLiteral("Monokai");
        case CodeThemeId::SolarizedDark:  return QStringLiteral("Solarized Dark");
        case CodeThemeId::GitHubLight:    return QStringLiteral("GitHub Light");
        case CodeThemeId::XcodeLight:     return QStringLiteral("Xcode Light");
        case CodeThemeId::SolarizedLight: return QStringLiteral("Solarized Light");
        }
        return QStringLiteral("One Dark");
    }

    static QString terminalThemeName(TerminalThemeId id) {
        switch (id) {
        case TerminalThemeId::MessageTerminal: return QStringLiteral("Message Terminal");
        case TerminalThemeId::ClassicDark:     return QStringLiteral("Classic Dark");
        case TerminalThemeId::ClassicLight:    return QStringLiteral("Classic Light");
        case TerminalThemeId::FollowCodeTheme: return QStringLiteral("Follow Code Theme");
        }
        return QStringLiteral("Message Terminal");
    }

    static QVector<CodeThemeId> allCodeThemes() {
        return {
            CodeThemeId::OneDark,
            CodeThemeId::GitHubDark,
            CodeThemeId::Dracula,
            CodeThemeId::Monokai,
            CodeThemeId::SolarizedDark,
            CodeThemeId::GitHubLight,
            CodeThemeId::XcodeLight,
            CodeThemeId::SolarizedLight
        };
    }

    static QVector<TerminalThemeId> allTerminalThemes() {
        return {
            TerminalThemeId::MessageTerminal,
            TerminalThemeId::ClassicDark,
            TerminalThemeId::ClassicLight,
            TerminalThemeId::FollowCodeTheme
        };
    }

signals:
    void themeChanged();
    void terminalThemeChanged();

private:
    explicit ThemeManager(QObject* parent = nullptr)
        : QObject(parent) {
        load();
    }

    ThemeManager(const ThemeManager&) = delete;
    ThemeManager& operator=(const ThemeManager&) = delete;

    static QString defaultMonoFontFamily() {
        if (QCoreApplication::instance() != nullptr) {
            const QString sys = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
            if (!sys.isEmpty()) {
                return sys;
            }
        }
        return QStringLiteral("Monospace");
    }

    static QString appearanceKey(Appearance a) {
        return a == Appearance::Light ? QStringLiteral("light") : QStringLiteral("dark");
    }

    static Appearance appearanceFromKey(const QString& key) {
        return key == QStringLiteral("light") ? Appearance::Light : Appearance::Dark;
    }

    static QString codeThemeKey(CodeThemeId id) {
        switch (id) {
        case CodeThemeId::OneDark:        return QStringLiteral("oneDark");
        case CodeThemeId::GitHubDark:     return QStringLiteral("githubDark");
        case CodeThemeId::Dracula:        return QStringLiteral("dracula");
        case CodeThemeId::Monokai:        return QStringLiteral("monokai");
        case CodeThemeId::SolarizedDark:  return QStringLiteral("solarizedDark");
        case CodeThemeId::GitHubLight:    return QStringLiteral("githubLight");
        case CodeThemeId::XcodeLight:     return QStringLiteral("xcodeLight");
        case CodeThemeId::SolarizedLight: return QStringLiteral("solarizedLight");
        }
        return QStringLiteral("oneDark");
    }

    static CodeThemeId codeThemeFromKey(const QString& key) {
        if (key == QStringLiteral("githubDark"))     return CodeThemeId::GitHubDark;
        if (key == QStringLiteral("dracula"))        return CodeThemeId::Dracula;
        if (key == QStringLiteral("monokai"))        return CodeThemeId::Monokai;
        if (key == QStringLiteral("solarizedDark"))  return CodeThemeId::SolarizedDark;
        if (key == QStringLiteral("githubLight"))    return CodeThemeId::GitHubLight;
        if (key == QStringLiteral("xcodeLight"))     return CodeThemeId::XcodeLight;
        if (key == QStringLiteral("solarizedLight")) return CodeThemeId::SolarizedLight;
        return CodeThemeId::OneDark;
    }

    static QString terminalThemeKey(TerminalThemeId id) {
        switch (id) {
        case TerminalThemeId::MessageTerminal: return QStringLiteral("messageTerminal");
        case TerminalThemeId::ClassicDark:     return QStringLiteral("classicDark");
        case TerminalThemeId::ClassicLight:    return QStringLiteral("classicLight");
        case TerminalThemeId::FollowCodeTheme: return QStringLiteral("followCodeTheme");
        }
        return QStringLiteral("messageTerminal");
    }

    static TerminalThemeId terminalThemeFromKey(const QString& key) {
        if (key == QStringLiteral("classicDark"))     return TerminalThemeId::ClassicDark;
        if (key == QStringLiteral("classicLight"))    return TerminalThemeId::ClassicLight;
        if (key == QStringLiteral("followCodeTheme")) return TerminalThemeId::FollowCodeTheme;
        return TerminalThemeId::MessageTerminal;
    }

    void load() {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("Spell"), QStringLiteral("SpellIDE"));
        m_appearance = appearanceFromKey(
            settings.value(QStringLiteral("spell.theme.appearance"), QStringLiteral("dark")).toString());
        m_codeTheme = codeThemeFromKey(
            settings.value(QStringLiteral("spell.theme.code"), QStringLiteral("oneDark")).toString());
        if (m_appearance == Appearance::Light && m_codeTheme == CodeThemeId::OneDark) {
            m_codeTheme = CodeThemeId::XcodeLight;
        }
        m_terminalTheme = terminalThemeFromKey(
            settings.value(QStringLiteral("spell.theme.terminal"), QStringLiteral("messageTerminal")).toString());

        const double edSize = settings.value(QStringLiteral("spell.theme.editorFontSize"), 13.0).toDouble();
        m_editorFontSize = (edSize >= 9.0 && edSize <= 28.0) ? edSize : 13.0;

        const double termSize = settings.value(QStringLiteral("spell.theme.terminalFontSize"), 12.5).toDouble();
        m_terminalFontSize = (termSize >= 9.0 && termSize <= 24.0) ? termSize : 12.5;

        m_editorFontFamily = settings.value(QStringLiteral("spell.theme.editorFontFamily")).toString().trimmed();
    }

    void persist() const {
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("Spell"), QStringLiteral("SpellIDE"));
        settings.setValue(QStringLiteral("spell.theme.appearance"), appearanceKey(m_appearance));
        settings.setValue(QStringLiteral("spell.theme.code"), codeThemeKey(m_codeTheme));
        settings.setValue(QStringLiteral("spell.theme.terminal"), terminalThemeKey(m_terminalTheme));
        settings.setValue(QStringLiteral("spell.theme.editorFontSize"), m_editorFontSize);
        settings.setValue(QStringLiteral("spell.theme.terminalFontSize"), m_terminalFontSize);
        settings.setValue(QStringLiteral("spell.theme.editorFontFamily"), m_editorFontFamily);
    }

    Appearance m_appearance = Appearance::Dark;
    CodeThemeId m_codeTheme = CodeThemeId::OneDark;
    TerminalThemeId m_terminalTheme = TerminalThemeId::MessageTerminal;
    double m_editorFontSize = 13.0;
    double m_terminalFontSize = 12.5;
    QString m_editorFontFamily;
};

} // namespace spell
