#pragma once

#include "theme/SpellTheme.h"

#include <QColor>
#include <QFont>
#include <QString>
#include <Qsci/qscilexercustom.h>

#include <memory>

namespace spell {

/**
 * Custom QScintilla syntax lexer for languages without dedicated built-in
 * QScintilla2 lexers (Rust, Swift, Go, TOML, and TypeScript).
 *
 * Styles tokens using the active Spell IDE `CodePalette` across all 8 code themes.
 */
class SpellCustomLexer : public QsciLexerCustom {
    Q_OBJECT

public:
    enum class CustomLang {
        Rust,
        Swift,
        Go,
        Toml,
        TypeScript
    };

    enum Style : int {
        Default     = 0,
        Keyword     = 1,
        Type        = 2,
        Function    = 3,
        String      = 4,
        Number      = 5,
        Comment     = 6,
        MacroOrAttr = 7
    };

    explicit SpellCustomLexer(CustomLang lang = CustomLang::Rust, QObject* parent = nullptr);
    ~SpellCustomLexer() override;

    SpellCustomLexer(const SpellCustomLexer&) = delete;
    SpellCustomLexer& operator=(const SpellCustomLexer&) = delete;
    SpellCustomLexer(SpellCustomLexer&&) = delete;
    SpellCustomLexer& operator=(SpellCustomLexer&&) = delete;

    using QsciLexer::defaultColor;
    using QsciLexer::defaultPaper;
    using QsciLexer::defaultFont;

    /** Returns the custom language mode configured for this lexer. */
    CustomLang customLanguage() const;

    /** Applies foreground, background paper, and monospace font colors for all styles. */
    void applyCodePalette(const CodePalette& palette, const QFont& font);

    const char* language() const override;
    QString description(int style) const override;
    void styleText(int start, int end) override;

    QColor defaultColor(int style) const override;
    QColor defaultPaper(int style) const override;
    QFont defaultFont(int style) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
