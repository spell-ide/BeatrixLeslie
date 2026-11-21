#pragma once

#include "core/SpellCitationLinkifier.h"
#include "theme/SpellTheme.h"

#include <QFrame>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QLabel;
class QPushButton;
class QTextBrowser;
class QTimer;
class QVBoxLayout;

namespace spell {

/**
 * Fenced code block card displaying a language tag, a **Copy** button with transient
 * `"Copied"` feedback, and a syntax-highlighted code surface colored with
 * `ThemeManager::instance().code()`.
 */
class SpellCodeBlockWidget : public QFrame {
    Q_OBJECT

public:
    explicit SpellCodeBlockWidget(QWidget* parent = nullptr);
    SpellCodeBlockWidget(const QString& language,
                         const QString& code,
                         QWidget* parent = nullptr);
    ~SpellCodeBlockWidget() override = default;

    QString language() const { return m_language; }
    QString code() const { return m_code; }

    void setCode(const QString& language, const QString& code);

    /**
     * Highlights multi-language source code (C/C++, Rust, Python, JS/TS, Go, shell,
     * JSON, YAML/TOML, etc.) into inline-styled HTML spans using `palette`.
     */
    static QString highlightToHtml(const QString& code,
                                   const QString& language,
                                   const CodePalette& palette);

public slots:
    void copyToClipboard();
    void applyTheme();

signals:
    void copied();

private:
    void setupUi();
    void renderHighlightedCode();
    void updateCodeViewHeight();
    void updateCopyButtonAppearance();

    QString m_language;
    QString m_code;
    bool m_showingCopied = false;

    QLabel* m_languageLabel = nullptr;
    QPushButton* m_copyButton = nullptr;
    QTextBrowser* m_codeView = nullptr;
    QTimer* m_copyResetTimer = nullptr;
};

/**
 * Markdown renderer widget for chat messages and documentation panels.
 *
 * Splits markdown into alternating prose and fenced code block segments:
 * - Prose blocks render headings (`#`..`######`), paragraphs, inline code, emphasis,
 *   bullet/numbered lists, blockquotes, links, and clickable `spellfile://` file
 *   citations via `SpellCitationLinkifier`.
 * - Fenced code blocks render as `SpellCodeBlockWidget` cards syntax-highlighted
 *   with `ThemeManager::instance().code()`, complete with language label and Copy button.
 *
 * Incremental streaming updates via `setMarkdown(...)` diff existing segments and
 * reuse unchanged child widgets in place to avoid flicker and preserve selection.
 */
class SpellMarkdownView : public QWidget {
    Q_OBJECT

public:
    struct Segment {
        enum class Kind {
            Prose,
            Code
        };

        Kind kind = Kind::Prose;
        QString language;
        QString content;

        bool operator==(const Segment&) const = default;
    };

    explicit SpellMarkdownView(QWidget* parent = nullptr);
    ~SpellMarkdownView() override = default;

    QString markdown() const { return m_markdown; }
    QStringList projectRoots() const { return m_projectRoots; }
    int blockCount() const { return static_cast<int>(m_blocks.size()); }

    /**
     * Updates the rendered markdown incrementally, resolving relative file citations
     * against `projectRoots`.
     */
    void setMarkdown(const QString& md, const QStringList& projectRoots = {});
    void clear();

    /**
     * Parses raw markdown into alternating `Prose` and fenced `Code` segments.
     * Unterminated trailing fences (common during live streaming) still emit a `Code` segment.
     */
    static QVector<Segment> parseSegments(const QString& markdown);

    /**
     * Converts a markdown prose segment into themed Qt rich-text HTML with clickable
     * `spellfile://` links injected via `spell::linkifyHtml`.
     */
    static QString renderProseToHtml(const QString& prose,
                                     const QStringList& projectRoots,
                                     const UiPalette& ui,
                                     const CodePalette& code,
                                     const QString& monoFontFamily);

public slots:
    void applyTheme();

signals:
    void fileCitationClicked(const QString& filePath, int line);

private slots:
    void handleLinkActivated(const QString& href);

private:
    struct BlockEntry {
        Segment::Kind kind = Segment::Kind::Prose;
        QString language;
        QString content;
        QLabel* proseLabel = nullptr;
        SpellCodeBlockWidget* codeBlock = nullptr;
    };

    void syncBlocks(bool forceRestyle);
    QLabel* createProseLabel();
    void updateProseLabel(QLabel* label, const QString& prose);

    QVBoxLayout* m_layout = nullptr;
    QString m_markdown;
    QStringList m_projectRoots;
    QVector<BlockEntry> m_blocks;
};

} // namespace spell
