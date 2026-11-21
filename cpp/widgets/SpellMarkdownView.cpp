#include "widgets/SpellMarkdownView.h"

#include "theme/SpellIcons.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextFrameFormat>
#include <QTextOption>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace spell {
namespace {

// ============================================================================
// HTML & Color Helpers
// ============================================================================

void appendHtmlEscaped(QString& out, QStringView text) {
    for (QChar ch : text) {
        switch (ch.unicode()) {
        case '&':
            out += QLatin1StringView("&amp;");
            break;
        case '<':
            out += QLatin1StringView("&lt;");
            break;
        case '>':
            out += QLatin1StringView("&gt;");
            break;
        case '"':
            out += QLatin1StringView("&quot;");
            break;
        default:
            out += ch;
            break;
        }
    }
}

QString escapeHtml(QStringView text) {
    QString out;
    out.reserve(text.size() + text.size() / 8);
    appendHtmlEscaped(out, text);
    return out;
}

QString escapeHtmlAttr(QStringView text) {
    QString out;
    out.reserve(text.size() + 8);
    for (QChar ch : text) {
        switch (ch.unicode()) {
        case '&':
            out += QLatin1StringView("&amp;");
            break;
        case '<':
            out += QLatin1StringView("&lt;");
            break;
        case '>':
            out += QLatin1StringView("&gt;");
            break;
        case '"':
            out += QLatin1StringView("&quot;");
            break;
        case '\'':
            out += QLatin1StringView("&#39;");
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

QString normalizeLineEndings(const QString& input) {
    if (!input.contains(QLatin1Char('\r'))) {
        return input;
    }
    QString out;
    out.reserve(input.size());
    const qsizetype n = input.size();
    for (qsizetype i = 0; i < n; ++i) {
        if (input[i] == QLatin1Char('\r')) {
            out += QLatin1Char('\n');
            if (i + 1 < n && input[i + 1] == QLatin1Char('\n')) {
                ++i;
            }
        } else {
            out += input[i];
        }
    }
    return out;
}

double relativeLuminance(const QColor& c) {
    auto linearize = [](double channel) {
        return (channel <= 0.03928)
            ? (channel / 12.92)
            : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linearize(c.redF())
         + 0.7152 * linearize(c.greenF())
         + 0.0722 * linearize(c.blueF());
}

double contrastRatio(const QColor& a, const QColor& b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    const double hi = std::max(la, lb);
    const double lo = std::min(la, lb);
    return (hi + 0.05) / (lo + 0.05);
}

QColor pickContrastingColor(const QColor& preferred, const QColor& fallback, const QColor& bg) {
    const QColor solidPreferred = compositeOver(preferred, bg);
    if (contrastRatio(solidPreferred, bg) >= 4.5) {
        return solidPreferred;
    }
    return compositeOver(fallback, bg);
}

// ============================================================================
// Multi-Language Syntax Highlighter
// ============================================================================

enum class TokenKind {
    Plain,
    Keyword,
    Function,
    String,
    Type,
    Number,
    Comment,
    Added,
    Removed,
    Hunk
};

QColor colorForToken(TokenKind kind, const CodePalette& palette) {
    switch (kind) {
    case TokenKind::Plain:    return palette.plain;
    case TokenKind::Keyword:  return palette.keyword;
    case TokenKind::Function: return palette.function;
    case TokenKind::String:   return palette.string;
    case TokenKind::Type:     return palette.type;
    case TokenKind::Number:   return palette.number;
    case TokenKind::Comment:  return palette.comment;
    case TokenKind::Added:    return palette.added;
    case TokenKind::Removed:  return palette.removed;
    case TokenKind::Hunk:     return palette.hunk;
    }
    return palette.plain;
}

class HtmlTokenBuilder {
public:
    HtmlTokenBuilder(const CodePalette& palette, qsizetype estimatedSize)
        : m_palette(palette) {
        m_out.reserve(estimatedSize * 3);
    }

    void emitRun(QStringView slice, TokenKind kind) {
        if (slice.isEmpty()) {
            return;
        }
        // Split around newlines so each line's spans stay tidy and self-contained.
        qsizetype pos = 0;
        const qsizetype len = slice.size();
        while (pos < len) {
            const qsizetype nl = slice.indexOf(QLatin1Char('\n'), pos);
            if (nl < 0) {
                appendSegment(slice.mid(pos), kind);
                break;
            }
            if (nl > pos) {
                appendSegment(slice.mid(pos, nl - pos), kind);
            }
            flush();
            m_out += QLatin1Char('\n');
            pos = nl + 1;
        }
    }

    QString finish() {
        flush();
        return m_out;
    }

private:
    void appendSegment(QStringView text, TokenKind kind) {
        if (text.isEmpty()) {
            return;
        }
        bool allSpace = true;
        for (QChar ch : text) {
            if (ch != QLatin1Char(' ') && ch != QLatin1Char('\t')) {
                allSpace = false;
                break;
            }
        }
        if (allSpace && kind == TokenKind::Plain) {
            if (m_hasPending && m_pendingKind == TokenKind::Plain) {
                m_pending += text;
            } else {
                flush();
                m_out += text;
            }
            return;
        }

        if (m_hasPending && m_pendingKind == kind) {
            m_pending += text;
        } else {
            flush();
            m_hasPending = true;
            m_pendingKind = kind;
            m_pending = text.toString();
        }
    }

    void flush() {
        if (!m_hasPending || m_pending.isEmpty()) {
            m_hasPending = false;
            m_pending.clear();
            return;
        }
        const QColor col = colorForToken(m_pendingKind, m_palette);
        m_out += QLatin1StringView("<span style=\"color:");
        m_out += col.name(QColor::HexRgb);
        m_out += QLatin1StringView(";\">");
        appendHtmlEscaped(m_out, m_pending);
        m_out += QLatin1StringView("</span>");
        m_pending.clear();
        m_hasPending = false;
    }

    const CodePalette& m_palette;
    QString m_out;
    QString m_pending;
    TokenKind m_pendingKind = TokenKind::Plain;
    bool m_hasPending = false;
};

bool isHexDigit(QChar c) {
    const ushort u = c.unicode();
    return (u >= '0' && u <= '9')
        || (u >= 'a' && u <= 'f')
        || (u >= 'A' && u <= 'F');
}

bool isIdentStart(QChar c) {
    return c.isLetter() || c == QLatin1Char('_');
}

bool isIdentContinue(QChar c) {
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

bool isHashCommentLanguage(const QString& lang) {
    static const QSet<QString> kHashLangs = {
        QStringLiteral("python"), QStringLiteral("py"), QStringLiteral("python3"),
        QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"),
        QStringLiteral("shell"), QStringLiteral("fish"), QStringLiteral("ruby"),
        QStringLiteral("rb"), QStringLiteral("yaml"), QStringLiteral("yml"),
        QStringLiteral("toml"), QStringLiteral("ini"), QStringLiteral("make"),
        QStringLiteral("makefile"), QStringLiteral("dockerfile"), QStringLiteral("docker"),
        QStringLiteral("cmake"), QStringLiteral("perl"), QStringLiteral("pl"),
        QStringLiteral("r"), QStringLiteral("elixir"), QStringLiteral("ex"),
        QStringLiteral("nim"), QStringLiteral("powershell"), QStringLiteral("ps1"),
        QStringLiteral("conf"), QStringLiteral("cfg")
    };
    return kHashLangs.contains(lang);
}

bool isCppLikeLanguage(const QString& lang) {
    static const QSet<QString> kCppLangs = {
        QStringLiteral("c"), QStringLiteral("h"), QStringLiteral("cpp"),
        QStringLiteral("c++"), QStringLiteral("cc"), QStringLiteral("cxx"),
        QStringLiteral("hpp"), QStringLiteral("hh"), QStringLiteral("hxx"),
        QStringLiteral("metal"), QStringLiteral("objc"), QStringLiteral("objective-c"),
        QStringLiteral("m"), QStringLiteral("mm")
    };
    return kCppLangs.contains(lang);
}

bool isRustLanguage(const QString& lang) {
    return lang == QLatin1StringView("rs") || lang == QLatin1StringView("rust");
}

bool isPythonLanguage(const QString& lang) {
    return lang == QLatin1StringView("py")
        || lang == QLatin1StringView("python")
        || lang == QLatin1StringView("python3");
}

bool isShellLanguage(const QString& lang) {
    static const QSet<QString> kShellLangs = {
        QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"),
        QStringLiteral("shell"), QStringLiteral("fish")
    };
    return kShellLangs.contains(lang);
}

bool isJsonLanguage(const QString& lang) {
    return lang == QLatin1StringView("json") || lang == QLatin1StringView("jsonc");
}

bool isYamlLanguage(const QString& lang) {
    return lang == QLatin1StringView("yaml") || lang == QLatin1StringView("yml");
}

bool isTomlLanguage(const QString& lang) {
    return lang == QLatin1StringView("toml") || lang == QLatin1StringView("ini");
}

bool isSqlLanguage(const QString& lang) {
    return lang == QLatin1StringView("sql")
        || lang == QLatin1StringView("sqlite")
        || lang == QLatin1StringView("postgres")
        || lang == QLatin1StringView("postgresql")
        || lang == QLatin1StringView("mysql");
}

bool isMarkupLanguage(const QString& lang) {
    return lang == QLatin1StringView("html")
        || lang == QLatin1StringView("xml")
        || lang == QLatin1StringView("svg")
        || lang == QLatin1StringView("xhtml")
        || lang == QLatin1StringView("plist");
}

bool isDiffLanguage(const QString& lang) {
    return lang == QLatin1StringView("diff") || lang == QLatin1StringView("patch");
}

const QSet<QString>& commonKeywords() {
    static const QSet<QString> kKeywords = {
        // Shared / C / C++20 / Qt / Java / C#
        QStringLiteral("alignas"), QStringLiteral("alignof"), QStringLiteral("auto"),
        QStringLiteral("bool"), QStringLiteral("break"), QStringLiteral("case"),
        QStringLiteral("catch"), QStringLiteral("char"), QStringLiteral("char8_t"),
        QStringLiteral("char16_t"), QStringLiteral("char32_t"), QStringLiteral("class"),
        QStringLiteral("co_await"), QStringLiteral("co_return"), QStringLiteral("co_yield"),
        QStringLiteral("concept"), QStringLiteral("const"), QStringLiteral("consteval"),
        QStringLiteral("constexpr"), QStringLiteral("constinit"), QStringLiteral("const_cast"),
        QStringLiteral("continue"), QStringLiteral("decltype"), QStringLiteral("default"),
        QStringLiteral("delete"), QStringLiteral("do"), QStringLiteral("double"),
        QStringLiteral("dynamic_cast"), QStringLiteral("else"), QStringLiteral("emit"),
        QStringLiteral("enum"), QStringLiteral("explicit"), QStringLiteral("export"),
        QStringLiteral("extern"), QStringLiteral("false"), QStringLiteral("final"),
        QStringLiteral("float"), QStringLiteral("for"), QStringLiteral("friend"),
        QStringLiteral("goto"), QStringLiteral("if"), QStringLiteral("inline"),
        QStringLiteral("int"), QStringLiteral("long"), QStringLiteral("mutable"),
        QStringLiteral("namespace"), QStringLiteral("new"), QStringLiteral("noexcept"),
        QStringLiteral("null"), QStringLiteral("nullptr"), QStringLiteral("operator"),
        QStringLiteral("override"), QStringLiteral("private"), QStringLiteral("protected"),
        QStringLiteral("public"), QStringLiteral("register"),
        QStringLiteral("reinterpret_cast"), QStringLiteral("requires"), QStringLiteral("return"),
        QStringLiteral("short"), QStringLiteral("signals"), QStringLiteral("signed"),
        QStringLiteral("sizeof"), QStringLiteral("slots"), QStringLiteral("static"),
        QStringLiteral("static_assert"), QStringLiteral("static_cast"), QStringLiteral("struct"),
        QStringLiteral("switch"), QStringLiteral("template"), QStringLiteral("this"),
        QStringLiteral("thread_local"), QStringLiteral("throw"), QStringLiteral("throws"),
        QStringLiteral("true"), QStringLiteral("try"), QStringLiteral("typedef"),
        QStringLiteral("typeid"), QStringLiteral("typename"), QStringLiteral("union"),
        QStringLiteral("unsigned"), QStringLiteral("using"), QStringLiteral("virtual"),
        QStringLiteral("void"), QStringLiteral("volatile"), QStringLiteral("wchar_t"),
        QStringLiteral("while"),
        // Rust
        QStringLiteral("as"), QStringLiteral("async"), QStringLiteral("await"),
        QStringLiteral("crate"), QStringLiteral("dyn"), QStringLiteral("Err"),
        QStringLiteral("fn"), QStringLiteral("impl"), QStringLiteral("in"),
        QStringLiteral("let"), QStringLiteral("loop"), QStringLiteral("match"),
        QStringLiteral("mod"), QStringLiteral("move"), QStringLiteral("mut"),
        QStringLiteral("None"), QStringLiteral("Ok"), QStringLiteral("pub"),
        QStringLiteral("ref"), QStringLiteral("self"), QStringLiteral("Self"),
        QStringLiteral("Some"), QStringLiteral("super"), QStringLiteral("trait"),
        QStringLiteral("type"), QStringLiteral("unsafe"), QStringLiteral("use"),
        QStringLiteral("where"),
        // Swift / Kotlin / Go
        QStringLiteral("actor"), QStringLiteral("chan"), QStringLiteral("defer"),
        QStringLiteral("deinit"), QStringLiteral("extension"), QStringLiteral("fallthrough"),
        QStringLiteral("fileprivate"), QStringLiteral("func"), QStringLiteral("go"),
        QStringLiteral("guard"), QStringLiteral("import"), QStringLiteral("init"),
        QStringLiteral("inout"), QStringLiteral("internal"), QStringLiteral("iota"),
        QStringLiteral("mutating"), QStringLiteral("nil"), QStringLiteral("open"),
        QStringLiteral("package"), QStringLiteral("protocol"), QStringLiteral("range"),
        QStringLiteral("rethrows"), QStringLiteral("select"), QStringLiteral("some"),
        QStringLiteral("var"),
        // Python
        QStringLiteral("and"), QStringLiteral("assert"), QStringLiteral("def"),
        QStringLiteral("del"), QStringLiteral("elif"), QStringLiteral("except"),
        QStringLiteral("False"), QStringLiteral("finally"), QStringLiteral("from"),
        QStringLiteral("global"), QStringLiteral("is"), QStringLiteral("lambda"),
        QStringLiteral("nonlocal"), QStringLiteral("not"), QStringLiteral("or"),
        QStringLiteral("pass"), QStringLiteral("raise"), QStringLiteral("True"),
        QStringLiteral("with"), QStringLiteral("yield"),
        // JS / TS / Shell / Dockerfile
        QStringLiteral("debugger"), QStringLiteral("done"), QStringLiteral("echo"),
        QStringLiteral("esac"), QStringLiteral("extends"), QStringLiteral("fi"),
        QStringLiteral("function"), QStringLiteral("implements"), QStringLiteral("instanceof"),
        QStringLiteral("interface"), QStringLiteral("keyof"), QStringLiteral("local"),
        QStringLiteral("of"), QStringLiteral("readonly"), QStringLiteral("then"),
        QStringLiteral("typeof"), QStringLiteral("undefined"), QStringLiteral("until"),
        QStringLiteral("FROM"), QStringLiteral("RUN"), QStringLiteral("COPY"),
        QStringLiteral("ADD"), QStringLiteral("WORKDIR"), QStringLiteral("ENV"),
        QStringLiteral("CMD"), QStringLiteral("ENTRYPOINT"), QStringLiteral("EXPOSE"),
        QStringLiteral("ARG")
    };
    return kKeywords;
}

const QSet<QString>& builtinTypes() {
    static const QSet<QString> kTypes = {
        QStringLiteral("i8"), QStringLiteral("i16"), QStringLiteral("i32"),
        QStringLiteral("i64"), QStringLiteral("i128"), QStringLiteral("isize"),
        QStringLiteral("u8"), QStringLiteral("u16"), QStringLiteral("u32"),
        QStringLiteral("u64"), QStringLiteral("u128"), QStringLiteral("usize"),
        QStringLiteral("f32"), QStringLiteral("f64"), QStringLiteral("str"),
        QStringLiteral("size_t"), QStringLiteral("ssize_t"), QStringLiteral("ptrdiff_t"),
        QStringLiteral("int8_t"), QStringLiteral("int16_t"), QStringLiteral("int32_t"),
        QStringLiteral("int64_t"), QStringLiteral("uint8_t"), QStringLiteral("uint16_t"),
        QStringLiteral("uint32_t"), QStringLiteral("uint64_t"), QStringLiteral("intptr_t"),
        QStringLiteral("uintptr_t"), QStringLiteral("qsizetype"), QStringLiteral("qreal"),
        QStringLiteral("quint8"), QStringLiteral("quint16"), QStringLiteral("quint32"),
        QStringLiteral("quint64"), QStringLiteral("qint8"), QStringLiteral("qint16"),
        QStringLiteral("qint32"), QStringLiteral("qint64"),
        QStringLiteral("byte"), QStringLiteral("rune"), QStringLiteral("error"),
        QStringLiteral("float32"), QStringLiteral("float64"), QStringLiteral("int8"),
        QStringLiteral("int16"), QStringLiteral("int32"), QStringLiteral("int64"),
        QStringLiteral("uint"), QStringLiteral("uint8"), QStringLiteral("uint16"),
        QStringLiteral("uint32"), QStringLiteral("uint64"), QStringLiteral("uintptr"),
        QStringLiteral("string"), QStringLiteral("number"), QStringLiteral("boolean"),
        QStringLiteral("unknown"), QStringLiteral("never"), QStringLiteral("any")
    };
    return kTypes;
}

const QSet<QString>& sqlKeywords() {
    static const QSet<QString> kSql = {
        QStringLiteral("ADD"), QStringLiteral("ALL"), QStringLiteral("ALTER"),
        QStringLiteral("AND"), QStringLiteral("AS"), QStringLiteral("ASC"),
        QStringLiteral("BETWEEN"), QStringLiteral("BY"), QStringLiteral("CASE"),
        QStringLiteral("CHECK"), QStringLiteral("COLUMN"), QStringLiteral("CONSTRAINT"),
        QStringLiteral("CREATE"), QStringLiteral("CROSS"), QStringLiteral("DEFAULT"),
        QStringLiteral("DELETE"), QStringLiteral("DESC"), QStringLiteral("DISTINCT"),
        QStringLiteral("DROP"), QStringLiteral("ELSE"), QStringLiteral("END"),
        QStringLiteral("EXISTS"), QStringLiteral("FALSE"), QStringLiteral("FOREIGN"),
        QStringLiteral("FROM"), QStringLiteral("FULL"), QStringLiteral("GROUP"),
        QStringLiteral("HAVING"), QStringLiteral("IF"), QStringLiteral("IN"),
        QStringLiteral("INDEX"), QStringLiteral("INNER"), QStringLiteral("INSERT"),
        QStringLiteral("INTO"), QStringLiteral("IS"), QStringLiteral("JOIN"),
        QStringLiteral("KEY"), QStringLiteral("LEFT"), QStringLiteral("LIKE"),
        QStringLiteral("LIMIT"), QStringLiteral("NOT"), QStringLiteral("NULL"),
        QStringLiteral("OFFSET"), QStringLiteral("ON"), QStringLiteral("OR"),
        QStringLiteral("ORDER"), QStringLiteral("OUTER"), QStringLiteral("PRIMARY"),
        QStringLiteral("REFERENCES"), QStringLiteral("RETURNING"), QStringLiteral("RIGHT"),
        QStringLiteral("SELECT"), QStringLiteral("SET"), QStringLiteral("TABLE"),
        QStringLiteral("THEN"), QStringLiteral("TRUE"), QStringLiteral("UNION"),
        QStringLiteral("UNIQUE"), QStringLiteral("UPDATE"), QStringLiteral("VALUES"),
        QStringLiteral("VIEW"), QStringLiteral("WHEN"), QStringLiteral("WHERE"),
        QStringLiteral("WITH")
    };
    return kSql;
}

void highlightDiffCode(QStringView code, HtmlTokenBuilder& builder) {
    qsizetype pos = 0;
    const qsizetype n = code.size();
    while (pos <= n) {
        const qsizetype nl = code.indexOf(QLatin1Char('\n'), pos);
        const qsizetype lineEnd = (nl < 0) ? n : nl;
        const QStringView line = code.mid(pos, lineEnd - pos);

        TokenKind kind = TokenKind::Plain;
        if (line.startsWith(QLatin1StringView("@@"))
            || line.startsWith(QLatin1StringView("diff "))
            || line.startsWith(QLatin1StringView("index "))) {
            kind = TokenKind::Hunk;
        } else if (line.startsWith(QLatin1StringView("+++"))
                   || line.startsWith(QLatin1StringView("---"))) {
            kind = TokenKind::Comment;
        } else if (line.startsWith(QLatin1Char('+'))) {
            kind = TokenKind::Added;
        } else if (line.startsWith(QLatin1Char('-'))) {
            kind = TokenKind::Removed;
        }

        builder.emitRun(line, kind);
        if (nl < 0) {
            break;
        }
        builder.emitRun(QStringView(code.data() + nl, 1), TokenKind::Plain);
        pos = nl + 1;
    }
}

void highlightMarkupCode(QStringView code, HtmlTokenBuilder& builder) {
    const qsizetype n = code.size();
    qsizetype i = 0;
    while (i < n) {
        // <!-- comment -->
        if (code.mid(i).startsWith(QLatin1StringView("<!--"))) {
            qsizetype end = code.indexOf(QLatin1StringView("-->"), i + 4);
            end = (end < 0) ? n : (end + 3);
            builder.emitRun(code.mid(i, end - i), TokenKind::Comment);
            i = end;
            continue;
        }
        if (code[i] == QLatin1Char('<')) {
            qsizetype j = i + 1;
            if (j < n && (code[j] == QLatin1Char('/') || code[j] == QLatin1Char('!') || code[j] == QLatin1Char('?'))) {
                ++j;
            }
            builder.emitRun(code.mid(i, j - i), TokenKind::Plain);
            i = j;
            // Tag name
            qsizetype nameStart = i;
            while (i < n && (code[i].isLetterOrNumber() || code[i] == QLatin1Char('-')
                             || code[i] == QLatin1Char('_') || code[i] == QLatin1Char(':'))) {
                ++i;
            }
            if (i > nameStart) {
                builder.emitRun(code.mid(nameStart, i - nameStart), TokenKind::Keyword);
            }
            // Attributes until '>'
            while (i < n && code[i] != QLatin1Char('>')) {
                if (code[i].isSpace()) {
                    qsizetype ws = i;
                    while (i < n && code[i].isSpace()) {
                        ++i;
                    }
                    builder.emitRun(code.mid(ws, i - ws), TokenKind::Plain);
                } else if (code[i] == QLatin1Char('"') || code[i] == QLatin1Char('\'')) {
                    const QChar quote = code[i];
                    qsizetype s = i + 1;
                    while (s < n && code[s] != quote) {
                        ++s;
                    }
                    if (s < n) {
                        ++s;
                    }
                    builder.emitRun(code.mid(i, s - i), TokenKind::String);
                    i = s;
                } else if (isIdentStart(code[i])) {
                    qsizetype a = i + 1;
                    while (a < n && (isIdentContinue(code[a]) || code[a] == QLatin1Char('-') || code[a] == QLatin1Char(':'))) {
                        ++a;
                    }
                    builder.emitRun(code.mid(i, a - i), TokenKind::Type);
                    i = a;
                } else {
                    builder.emitRun(code.mid(i, 1), TokenKind::Plain);
                    ++i;
                }
            }
            if (i < n && code[i] == QLatin1Char('>')) {
                builder.emitRun(code.mid(i, 1), TokenKind::Plain);
                ++i;
            }
            continue;
        }
        builder.emitRun(code.mid(i, 1), TokenKind::Plain);
        ++i;
    }
}

void highlightJsonCode(QStringView code, HtmlTokenBuilder& builder) {
    const qsizetype n = code.size();
    qsizetype i = 0;
    while (i < n) {
        const QChar c = code[i];
        if (c == QLatin1Char('/') && i + 1 < n && code[i + 1] == QLatin1Char('/')) {
            qsizetype j = i + 2;
            while (j < n && code[j] != QLatin1Char('\n')) {
                ++j;
            }
            builder.emitRun(code.mid(i, j - i), TokenKind::Comment);
            i = j;
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < n && code[i + 1] == QLatin1Char('*')) {
            qsizetype j = i + 2;
            while (j + 1 < n && !(code[j] == QLatin1Char('*') && code[j + 1] == QLatin1Char('/'))) {
                ++j;
            }
            j = std::min(n, j + 2);
            builder.emitRun(code.mid(i, j - i), TokenKind::Comment);
            i = j;
            continue;
        }
        if (c == QLatin1Char('"')) {
            qsizetype j = i + 1;
            while (j < n && code[j] != QLatin1Char('"') && code[j] != QLatin1Char('\n')) {
                if (code[j] == QLatin1Char('\\') && j + 1 < n) {
                    ++j;
                }
                ++j;
            }
            if (j < n && code[j] == QLatin1Char('"')) {
                ++j;
            }
            qsizetype k = j;
            while (k < n && (code[k] == QLatin1Char(' ') || code[k] == QLatin1Char('\t'))) {
                ++k;
            }
            const bool isKey = (k < n && code[k] == QLatin1Char(':'));
            builder.emitRun(code.mid(i, j - i), isKey ? TokenKind::Function : TokenKind::String);
            i = j;
            continue;
        }
        if (c.isDigit() || (c == QLatin1Char('-') && i + 1 < n && code[i + 1].isDigit())) {
            qsizetype j = i + 1;
            while (j < n) {
                const QChar ch = code[j];
                if (ch.isDigit() || ch == QLatin1Char('.')
                    || ch == QLatin1Char('e') || ch == QLatin1Char('E')) {
                    ++j;
                } else if ((ch == QLatin1Char('+') || ch == QLatin1Char('-'))
                           && (code[j - 1] == QLatin1Char('e') || code[j - 1] == QLatin1Char('E'))) {
                    ++j;
                } else {
                    break;
                }
            }
            builder.emitRun(code.mid(i, j - i), TokenKind::Number);
            i = j;
            continue;
        }
        if (c.isLetter()) {
            qsizetype j = i + 1;
            while (j < n && code[j].isLetter()) {
                ++j;
            }
            const QStringView word = code.mid(i, j - i);
            const bool isLit = (word == QLatin1StringView("true")
                                || word == QLatin1StringView("false")
                                || word == QLatin1StringView("null"));
            builder.emitRun(word, isLit ? TokenKind::Keyword : TokenKind::Plain);
            i = j;
            continue;
        }
        builder.emitRun(code.mid(i, 1), TokenKind::Plain);
        ++i;
    }
}

void highlightYamlTomlCode(QStringView code, bool isToml, HtmlTokenBuilder& builder) {
    qsizetype pos = 0;
    const qsizetype n = code.size();
    while (pos <= n) {
        const qsizetype nl = code.indexOf(QLatin1Char('\n'), pos);
        const qsizetype lineEnd = (nl < 0) ? n : nl;
        const QStringView line = code.mid(pos, lineEnd - pos);
        const qsizetype len = line.size();
        qsizetype i = 0;

        while (i < len && (line[i] == QLatin1Char(' ') || line[i] == QLatin1Char('\t'))) {
            ++i;
        }
        if (i > 0) {
            builder.emitRun(line.mid(0, i), TokenKind::Plain);
        }

        // Full-line comment
        if (i < len && line[i] == QLatin1Char('#')) {
            builder.emitRun(line.mid(i), TokenKind::Comment);
            i = len;
        } else if (isToml && i < len && line[i] == QLatin1Char('[')) {
            // TOML section header [table] or [[array]]
            qsizetype close = line.indexOf(QLatin1Char(']'), i + 1);
            if (close >= 0) {
                if (close + 1 < len && line[close + 1] == QLatin1Char(']')) {
                    ++close;
                }
                builder.emitRun(line.mid(i, close - i + 1), TokenKind::Type);
                i = close + 1;
            }
        } else {
            // Optional YAML list dash
            if (!isToml && i + 1 < len && line[i] == QLatin1Char('-') && line[i + 1] == QLatin1Char(' ')) {
                builder.emitRun(line.mid(i, 2), TokenKind::Plain);
                i += 2;
                while (i < len && line[i] == QLatin1Char(' ')) {
                    builder.emitRun(line.mid(i, 1), TokenKind::Plain);
                    ++i;
                }
            }
            // Key before ':' (YAML) or '=' (TOML)
            const QChar sep = isToml ? QLatin1Char('=') : QLatin1Char(':');
            if (i < len && (isIdentStart(line[i]) || line[i] == QLatin1Char('"'))) {
                qsizetype kEnd = i;
                if (line[i] == QLatin1Char('"')) {
                    ++kEnd;
                    while (kEnd < len && line[kEnd] != QLatin1Char('"')) {
                        if (line[kEnd] == QLatin1Char('\\') && kEnd + 1 < len) {
                            ++kEnd;
                        }
                        ++kEnd;
                    }
                    if (kEnd < len) {
                        ++kEnd;
                    }
                } else {
                    while (kEnd < len && (isIdentContinue(line[kEnd]) || line[kEnd] == QLatin1Char('-')
                                          || line[kEnd] == QLatin1Char('.'))) {
                        ++kEnd;
                    }
                }
                qsizetype afterKey = kEnd;
                while (afterKey < len && (line[afterKey] == QLatin1Char(' ') || line[afterKey] == QLatin1Char('\t'))) {
                    ++afterKey;
                }
                if (afterKey < len && line[afterKey] == sep
                    && (isToml || afterKey + 1 == len || line[afterKey + 1].isSpace())) {
                    builder.emitRun(line.mid(i, kEnd - i), TokenKind::Function);
                    builder.emitRun(line.mid(kEnd, afterKey - kEnd + 1), TokenKind::Plain);
                    i = afterKey + 1;
                }
            }
        }

        // Value portion of the line
        while (i < len) {
            const QChar c = line[i];
            if (c == QLatin1Char('#')) {
                builder.emitRun(line.mid(i), TokenKind::Comment);
                break;
            }
            if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                qsizetype j = i + 1;
                while (j < len && line[j] != c) {
                    if (c == QLatin1Char('"') && line[j] == QLatin1Char('\\') && j + 1 < len) {
                        ++j;
                    }
                    ++j;
                }
                if (j < len) {
                    ++j;
                }
                builder.emitRun(line.mid(i, j - i), TokenKind::String);
                i = j;
                continue;
            }
            if (c.isDigit() || (c == QLatin1Char('-') && i + 1 < len && line[i + 1].isDigit()
                                && (i == 0 || !isIdentContinue(line[i - 1])))) {
                qsizetype j = i + 1;
                while (j < len && (isHexDigit(line[j]) || line[j] == QLatin1Char('.')
                                   || line[j] == QLatin1Char('_') || line[j] == QLatin1Char('x')
                                   || line[j] == QLatin1Char('X'))) {
                    ++j;
                }
                builder.emitRun(line.mid(i, j - i), TokenKind::Number);
                i = j;
                continue;
            }
            if (isIdentStart(c)) {
                qsizetype j = i + 1;
                while (j < len && (isIdentContinue(line[j]) || line[j] == QLatin1Char('-'))) {
                    ++j;
                }
                const QStringView word = line.mid(i, j - i);
                const bool isBoolOrNull = (word == QLatin1StringView("true")
                                           || word == QLatin1StringView("false")
                                           || word == QLatin1StringView("null")
                                           || word == QLatin1StringView("yes")
                                           || word == QLatin1StringView("no"));
                builder.emitRun(word, isBoolOrNull ? TokenKind::Keyword : TokenKind::Plain);
                i = j;
                continue;
            }
            builder.emitRun(line.mid(i, 1), TokenKind::Plain);
            ++i;
        }

        if (nl < 0) {
            break;
        }
        builder.emitRun(QStringView(code.data() + nl, 1), TokenKind::Plain);
        pos = nl + 1;
    }
}

void highlightGeneralCode(QStringView code, const QString& lang, HtmlTokenBuilder& builder) {
    const bool hashComments = isHashCommentLanguage(lang);
    const bool cppLike = isCppLikeLanguage(lang);
    const bool rustLang = isRustLanguage(lang);
    const bool pythonLang = isPythonLanguage(lang);
    const bool shellLang = isShellLanguage(lang);
    const bool sqlLang = isSqlLanguage(lang);
    const auto& kwSet = commonKeywords();
    const auto& typeSet = builtinTypes();
    const auto& sqlSet = sqlKeywords();

    const qsizetype n = code.size();
    qsizetype i = 0;

    while (i < n) {
        const QChar c = code[i];

        // Line comment: //
        if (!pythonLang && !shellLang && c == QLatin1Char('/') && i + 1 < n && code[i + 1] == QLatin1Char('/')) {
            qsizetype j = i + 2;
            while (j < n && code[j] != QLatin1Char('\n')) {
                ++j;
            }
            builder.emitRun(code.mid(i, j - i), TokenKind::Comment);
            i = j;
            continue;
        }

        // SQL line comment: --
        if (sqlLang && c == QLatin1Char('-') && i + 1 < n && code[i + 1] == QLatin1Char('-')) {
            qsizetype j = i + 2;
            while (j < n && code[j] != QLatin1Char('\n')) {
                ++j;
            }
            builder.emitRun(code.mid(i, j - i), TokenKind::Comment);
            i = j;
            continue;
        }

        // Block comment: /* ... */ (supports nesting in Rust)
        if (!pythonLang && !shellLang && c == QLatin1Char('/') && i + 1 < n && code[i + 1] == QLatin1Char('*')) {
            qsizetype j = i + 2;
            int depth = 1;
            while (j < n && depth > 0) {
                if (rustLang && j + 1 < n && code[j] == QLatin1Char('/') && code[j + 1] == QLatin1Char('*')) {
                    ++depth;
                    j += 2;
                } else if (j + 1 < n && code[j] == QLatin1Char('*') && code[j + 1] == QLatin1Char('/')) {
                    --depth;
                    j += 2;
                } else {
                    ++j;
                }
            }
            builder.emitRun(code.mid(i, j - i), TokenKind::Comment);
            i = j;
            continue;
        }

        // Hash comments (Python, Shell, Ruby, CMake, Makefile, Dockerfile, etc.)
        if (c == QLatin1Char('#') && hashComments) {
            qsizetype j = i + 1;
            while (j < n && code[j] != QLatin1Char('\n')) {
                ++j;
            }
            builder.emitRun(code.mid(i, j - i), TokenKind::Comment);
            i = j;
            continue;
        }

        // C/C++ preprocessor directives (#include, #define, #pragma, #if, ...)
        if (c == QLatin1Char('#') && (cppLike || lang.isEmpty())) {
            // Check that only horizontal whitespace precedes '#' on this line
            qsizetype ls = i;
            bool lineStart = true;
            while (ls > 0 && code[ls - 1] != QLatin1Char('\n')) {
                --ls;
                if (code[ls] != QLatin1Char(' ') && code[ls] != QLatin1Char('\t')) {
                    lineStart = false;
                    break;
                }
            }
            if (lineStart) {
                qsizetype j = i + 1;
                while (j < n && (code[j] == QLatin1Char(' ') || code[j] == QLatin1Char('\t'))) {
                    ++j;
                }
                const qsizetype kwStart = j;
                while (j < n && isIdentContinue(code[j])) {
                    ++j;
                }
                if (j > kwStart) {
                    const QStringView dir = code.mid(kwStart, j - kwStart);
                    builder.emitRun(code.mid(i, j - i), TokenKind::Keyword);
                    i = j;
                    if (dir == QLatin1StringView("include")) {
                        qsizetype wsEnd = i;
                        while (wsEnd < n && (code[wsEnd] == QLatin1Char(' ') || code[wsEnd] == QLatin1Char('\t'))) {
                            ++wsEnd;
                        }
                        if (wsEnd < n && code[wsEnd] == QLatin1Char('<')) {
                            builder.emitRun(code.mid(i, wsEnd - i), TokenKind::Plain);
                            qsizetype gt = wsEnd + 1;
                            while (gt < n && code[gt] != QLatin1Char('>') && code[gt] != QLatin1Char('\n')) {
                                ++gt;
                            }
                            if (gt < n && code[gt] == QLatin1Char('>')) {
                                ++gt;
                            }
                            builder.emitRun(code.mid(wsEnd, gt - wsEnd), TokenKind::String);
                            i = gt;
                        }
                    }
                    continue;
                }
            }
        }

        // Python triple-quoted strings ("""...""" or '''...''')
        if (pythonLang && (c == QLatin1Char('"') || c == QLatin1Char('\''))
            && i + 2 < n && code[i + 1] == c && code[i + 2] == c) {
            qsizetype j = i + 3;
            while (j < n) {
                if (code[j] == QLatin1Char('\\') && j + 1 < n) {
                    j += 2;
                    continue;
                }
                if (j + 2 < n && code[j] == c && code[j + 1] == c && code[j + 2] == c) {
                    j += 3;
                    break;
                }
                ++j;
            }
            builder.emitRun(code.mid(i, j - i), TokenKind::String);
            i = j;
            continue;
        }

        // Rust raw strings: r"..." or r#"..."# or br#"..."#
        if (rustLang && (c == QLatin1Char('r') || (c == QLatin1Char('b') && i + 1 < n && code[i + 1] == QLatin1Char('r')))) {
            qsizetype p = (c == QLatin1Char('b')) ? (i + 2) : (i + 1);
            qsizetype hashes = 0;
            while (p < n && code[p] == QLatin1Char('#')) {
                ++hashes;
                ++p;
            }
            if (p < n && code[p] == QLatin1Char('"')) {
                qsizetype j = p + 1;
                while (j < n) {
                    if (code[j] == QLatin1Char('"')) {
                        qsizetype h = 0;
                        while (h < hashes && j + 1 + h < n && code[j + 1 + h] == QLatin1Char('#')) {
                            ++h;
                        }
                        if (h == hashes) {
                            j = j + 1 + hashes;
                            break;
                        }
                    }
                    ++j;
                }
                builder.emitRun(code.mid(i, j - i), TokenKind::String);
                i = j;
                continue;
            }
        }

        // Python string prefixes (r"...", f'...', b"""...""", fr"...", etc.)
        if (pythonLang && (c == QLatin1Char('r') || c == QLatin1Char('R')
                           || c == QLatin1Char('f') || c == QLatin1Char('F')
                           || c == QLatin1Char('b') || c == QLatin1Char('B')
                           || c == QLatin1Char('u') || c == QLatin1Char('U'))) {
            qsizetype p = i + 1;
            if (p < n && (code[p] == QLatin1Char('r') || code[p] == QLatin1Char('R')
                          || code[p] == QLatin1Char('f') || code[p] == QLatin1Char('F')
                          || code[p] == QLatin1Char('b') || code[p] == QLatin1Char('B'))) {
                ++p;
            }
            if (p < n && (code[p] == QLatin1Char('"') || code[p] == QLatin1Char('\''))) {
                const QChar q = code[p];
                if (p + 2 < n && code[p + 1] == q && code[p + 2] == q) {
                    qsizetype j = p + 3;
                    while (j < n) {
                        if (code[j] == QLatin1Char('\\') && j + 1 < n) {
                            j += 2;
                            continue;
                        }
                        if (j + 2 < n && code[j] == q && code[j + 1] == q && code[j + 2] == q) {
                            j += 3;
                            break;
                        }
                        ++j;
                    }
                    builder.emitRun(code.mid(i, j - i), TokenKind::String);
                    i = j;
                    continue;
                }
                qsizetype j = p + 1;
                while (j < n && code[j] != q && code[j] != QLatin1Char('\n')) {
                    if (code[j] == QLatin1Char('\\') && j + 1 < n) {
                        ++j;
                    }
                    ++j;
                }
                if (j < n && code[j] == q) {
                    ++j;
                }
                builder.emitRun(code.mid(i, j - i), TokenKind::String);
                i = j;
                continue;
            }
        }

        // Rust lifetimes ('a, 'static, '_) vs char literals ('x', '\n', '\u{...}')
        if (rustLang && c == QLatin1Char('\'')) {
            if (i + 1 < n && code[i + 1] == QLatin1Char('\\')) {
                qsizetype j = i + 2;
                while (j < n && code[j] != QLatin1Char('\'') && code[j] != QLatin1Char('\n')) {
                    ++j;
                }
                if (j < n && code[j] == QLatin1Char('\'')) {
                    ++j;
                }
                builder.emitRun(code.mid(i, j - i), TokenKind::String);
                i = j;
                continue;
            }
            if (i + 2 < n && code[i + 2] == QLatin1Char('\'') && code[i + 1] != QLatin1Char('\n')) {
                builder.emitRun(code.mid(i, 3), TokenKind::String);
                i += 3;
                continue;
            }
            if (i + 1 < n && isIdentStart(code[i + 1])) {
                qsizetype j = i + 2;
                while (j < n && isIdentContinue(code[j])) {
                    ++j;
                }
                builder.emitRun(code.mid(i, j - i), TokenKind::Type);
                i = j;
                continue;
            }
            builder.emitRun(code.mid(i, 1), TokenKind::Plain);
            ++i;
            continue;
        }

        // Standard string / char / template literals
        if (c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`')) {
            const bool multiLine = (c == QLatin1Char('`'));
            qsizetype j = i + 1;
            while (j < n && code[j] != c && (multiLine || code[j] != QLatin1Char('\n'))) {
                if (code[j] == QLatin1Char('\\') && j + 1 < n) {
                    ++j;
                }
                ++j;
            }
            if (j < n && code[j] == c) {
                ++j;
            }
            builder.emitRun(code.mid(i, j - i), TokenKind::String);
            i = j;
            continue;
        }

        // Shell variables: $VAR or ${VAR}
        if (shellLang && c == QLatin1Char('$') && i + 1 < n) {
            if (code[i + 1] == QLatin1Char('{')) {
                qsizetype j = i + 2;
                while (j < n && code[j] != QLatin1Char('}') && code[j] != QLatin1Char('\n')) {
                    ++j;
                }
                if (j < n && code[j] == QLatin1Char('}')) {
                    ++j;
                }
                builder.emitRun(code.mid(i, j - i), TokenKind::Type);
                i = j;
                continue;
            }
            if (isIdentStart(code[i + 1]) || code[i + 1].isDigit()
                || code[i + 1] == QLatin1Char('?') || code[i + 1] == QLatin1Char('@')
                || code[i + 1] == QLatin1Char('#')) {
                qsizetype j = i + 2;
                while (j < n && isIdentContinue(code[j])) {
                    ++j;
                }
                builder.emitRun(code.mid(i, j - i), TokenKind::Type);
                i = j;
                continue;
            }
        }

        // Numbers (decimal, hex, binary, octal, floats, exponents, C++/Rust digit separators & suffixes)
        if (c.isDigit()) {
            qsizetype j = i + 1;
            bool isHex = false;
            if (c == QLatin1Char('0') && j < n && (code[j] == QLatin1Char('x') || code[j] == QLatin1Char('X'))) {
                isHex = true;
                ++j;
            } else if (c == QLatin1Char('0') && j < n
                       && (code[j] == QLatin1Char('b') || code[j] == QLatin1Char('B')
                           || code[j] == QLatin1Char('o') || code[j] == QLatin1Char('O'))) {
                ++j;
            }
            while (j < n) {
                const QChar ch = code[j];
                if (isHex ? isHexDigit(ch) : ch.isDigit()) {
                    ++j;
                } else if (ch == QLatin1Char('_')) {
                    ++j;
                } else if (ch == QLatin1Char('\'') && j + 1 < n && isHexDigit(code[j + 1])) {
                    ++j;
                } else if (!isHex && ch == QLatin1Char('.') && j + 1 < n && code[j + 1].isDigit()) {
                    ++j;
                } else if (!isHex && (ch == QLatin1Char('e') || ch == QLatin1Char('E'))
                           && j + 1 < n && (code[j + 1].isDigit() || code[j + 1] == QLatin1Char('+')
                                            || code[j + 1] == QLatin1Char('-'))) {
                    ++j;
                    if (j < n && (code[j] == QLatin1Char('+') || code[j] == QLatin1Char('-'))) {
                        ++j;
                    }
                } else {
                    break;
                }
            }
            // Numeric type suffix (e.g., u32, f64, ull, f, L)
            while (j < n && isIdentContinue(code[j])) {
                ++j;
            }
            builder.emitRun(code.mid(i, j - i), TokenKind::Number);
            i = j;
            continue;
        }

        // Identifiers, keywords, macros, decorators (@foo), functions, and UpperCamelCase types
        if (isIdentStart(c) || (c == QLatin1Char('@') && i + 1 < n && isIdentStart(code[i + 1]))) {
            qsizetype j = i + 1;
            while (j < n && isIdentContinue(code[j])) {
                ++j;
            }
            // Rust / Ruby bang suffix (e.g., println!, vec!, sort!)
            const bool allowBangSuffix = rustLang || lang.isEmpty()
                || lang == QLatin1StringView("rb") || lang == QLatin1StringView("ruby");
            if (allowBangSuffix && j < n && code[j] == QLatin1Char('!')
                && (j + 1 >= n || code[j + 1] != QLatin1Char('='))) {
                ++j;
            }
            const QStringView wordView = code.mid(i, j - i);
            const QString word = wordView.toString();

            qsizetype k = j;
            while (k < n && code[k] == QLatin1Char(' ')) {
                ++k;
            }

            TokenKind kind = TokenKind::Plain;
            if (word.startsWith(QLatin1Char('@'))) {
                kind = TokenKind::Keyword;
            } else if (sqlLang && sqlSet.contains(word.toUpper())) {
                kind = TokenKind::Keyword;
            } else if (kwSet.contains(word)) {
                kind = TokenKind::Keyword;
            } else if (typeSet.contains(word)) {
                kind = TokenKind::Type;
            } else if (word.endsWith(QLatin1Char('!')) || (k < n && code[k] == QLatin1Char('('))) {
                kind = TokenKind::Function;
            } else if (!word.isEmpty() && word[0].isUpper()) {
                kind = TokenKind::Type;
            }

            builder.emitRun(wordView, kind);
            i = j;
            continue;
        }

        builder.emitRun(code.mid(i, 1), TokenKind::Plain);
        ++i;
    }
}

// ============================================================================
// Markdown Prose-to-HTML Renderer
// ============================================================================

struct InlineRenderContext {
    QColor inlineCodeBg;
    QColor inlineCodeFg;
    QString monoFontFamily;
};

bool isAsciiPunctuation(QChar c) {
    const ushort u = c.unicode();
    return (u >= 33 && u <= 47) || (u >= 58 && u <= 64)
        || (u >= 91 && u <= 96) || (u >= 123 && u <= 126);
}

bool isSafeHref(const QString& href) {
    const QString trimmed = href.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    const QString lower = trimmed.toLower();
    if (lower.startsWith(QLatin1StringView("javascript:"))
        || lower.startsWith(QLatin1StringView("vbscript:"))
        || lower.startsWith(QLatin1StringView("data:"))) {
        return false;
    }
    return true;
}

bool canOpenUnderscore(const QString& text, qsizetype idx, qsizetype count) {
    if (idx + count >= text.size()) {
        return false;
    }
    if (idx > 0 && text[idx - 1].isLetterOrNumber()) {
        return false;
    }
    const QChar after = text[idx + count];
    return !after.isSpace() && after != QLatin1Char('_');
}

bool canCloseUnderscore(const QString& text, qsizetype idx, qsizetype count) {
    if (idx <= 0) {
        return false;
    }
    if (text[idx - 1].isSpace()) {
        return false;
    }
    if (idx + count < text.size() && text[idx + count].isLetterOrNumber()) {
        return false;
    }
    return true;
}

qsizetype findClosingDelimiter(const QString& text,
                               qsizetype start,
                               QChar delim,
                               qsizetype count,
                               bool underscoreRules) {
    const qsizetype n = text.size();
    qsizetype i = start;
    while (i + count <= n) {
        if (text[i] == QLatin1Char('\\') && i + 1 < n) {
            i += 2;
            continue;
        }
        // Skip inline code spans while searching for emphasis closing delimiter
        if (text[i] == QLatin1Char('`')) {
            qsizetype ticks = 0;
            while (i + ticks < n && text[i + ticks] == QLatin1Char('`')) {
                ++ticks;
            }
            qsizetype scan = i + ticks;
            bool closed = false;
            while (scan < n) {
                if (text[scan] == QLatin1Char('`')) {
                    qsizetype closeTicks = 0;
                    while (scan + closeTicks < n && text[scan + closeTicks] == QLatin1Char('`')) {
                        ++closeTicks;
                    }
                    if (closeTicks == ticks) {
                        i = scan + closeTicks;
                        closed = true;
                        break;
                    }
                    scan += closeTicks;
                } else {
                    ++scan;
                }
            }
            if (!closed) {
                i += ticks;
            }
            continue;
        }

        if (text[i] == delim) {
            qsizetype run = 0;
            while (i + run < n && text[i + run] == delim) {
                ++run;
            }
            if (run >= count) {
                const qsizetype closePos = i + (run - count);
                if (closePos > start && !text[closePos - 1].isSpace()) {
                    if (!underscoreRules || canCloseUnderscore(text, closePos, count)) {
                        return closePos;
                    }
                }
            }
            i += run;
            continue;
        }
        ++i;
    }
    return -1;
}

QString renderInlineMarkdown(const QString& text,
                             const InlineRenderContext& ctx,
                             bool insideLink = false,
                             int depth = 0) {
    if (text.isEmpty()) {
        return {};
    }
    if (depth > 16) {
        return escapeHtml(text);
    }

    QString out;
    out.reserve(text.size() + text.size() / 2);

    const qsizetype n = text.size();
    qsizetype i = 0;

    while (i < n) {
        const QChar c = text[i];

        // 1. Backslash escape
        if (c == QLatin1Char('\\') && i + 1 < n && isAsciiPunctuation(text[i + 1])) {
            appendHtmlEscaped(out, QStringView(text.data() + i + 1, 1));
            i += 2;
            continue;
        }

        // 2. Inline code span (`...` or ``...``)
        if (c == QLatin1Char('`')) {
            qsizetype ticks = 0;
            while (i + ticks < n && text[i + ticks] == QLatin1Char('`')) {
                ++ticks;
            }
            qsizetype scan = i + ticks;
            qsizetype closePos = -1;
            while (scan < n) {
                if (text[scan] == QLatin1Char('`')) {
                    qsizetype closeTicks = 0;
                    while (scan + closeTicks < n && text[scan + closeTicks] == QLatin1Char('`')) {
                        ++closeTicks;
                    }
                    if (closeTicks == ticks) {
                        closePos = scan;
                        break;
                    }
                    scan += closeTicks;
                } else {
                    ++scan;
                }
            }
            if (closePos >= 0) {
                QStringView inner = QStringView(text).mid(i + ticks, closePos - (i + ticks));
                if (inner.size() >= 2
                    && inner.front() == QLatin1Char(' ')
                    && inner.back() == QLatin1Char(' ')) {
                    bool allSpaces = true;
                    for (QChar ch : inner) {
                        if (ch != QLatin1Char(' ')) {
                            allSpaces = false;
                            break;
                        }
                    }
                    if (!allSpaces) {
                        inner = inner.mid(1, inner.size() - 2);
                    }
                }
                out += QStringLiteral(
                    "<code style=\"font-family:'%1', monospace; font-size:12px; "
                    "background-color:%2; color:%3;\">")
                    .arg(escapeHtmlAttr(ctx.monoFontFamily),
                         ctx.inlineCodeBg.name(QColor::HexRgb),
                         ctx.inlineCodeFg.name(QColor::HexRgb));
                appendHtmlEscaped(out, inner);
                out += QLatin1StringView("</code>");
                i = closePos + ticks;
                continue;
            }
            out += QStringView(text).mid(i, ticks);
            i += ticks;
            continue;
        }

        // 3. Markdown link [label](url)
        if (!insideLink && c == QLatin1Char('[')) {
            qsizetype bracketDepth = 1;
            qsizetype scan = i + 1;
            while (scan < n && bracketDepth > 0) {
                if (text[scan] == QLatin1Char('\\') && scan + 1 < n) {
                    scan += 2;
                    continue;
                }
                if (text[scan] == QLatin1Char('`')) {
                    qsizetype t = 0;
                    while (scan + t < n && text[scan + t] == QLatin1Char('`')) {
                        ++t;
                    }
                    qsizetype s2 = scan + t;
                    bool closed = false;
                    while (s2 < n) {
                        if (text[s2] == QLatin1Char('`')) {
                            qsizetype ct = 0;
                            while (s2 + ct < n && text[s2 + ct] == QLatin1Char('`')) {
                                ++ct;
                            }
                            if (ct == t) {
                                scan = s2 + ct;
                                closed = true;
                                break;
                            }
                            s2 += ct;
                        } else {
                            ++s2;
                        }
                    }
                    if (!closed) {
                        scan += t;
                    }
                    continue;
                }
                if (text[scan] == QLatin1Char('[')) {
                    ++bracketDepth;
                } else if (text[scan] == QLatin1Char(']')) {
                    --bracketDepth;
                    if (bracketDepth == 0) {
                        break;
                    }
                }
                ++scan;
            }
            if (bracketDepth == 0 && scan + 1 < n && text[scan + 1] == QLatin1Char('(')) {
                const qsizetype closeBracket = scan;
                qsizetype pScan = closeBracket + 2;
                int parenDepth = 1;
                while (pScan < n && parenDepth > 0) {
                    if (text[pScan] == QLatin1Char('\\') && pScan + 1 < n) {
                        pScan += 2;
                        continue;
                    }
                    if (text[pScan] == QLatin1Char('(')) {
                        ++parenDepth;
                    } else if (text[pScan] == QLatin1Char(')')) {
                        --parenDepth;
                        if (parenDepth == 0) {
                            break;
                        }
                    }
                    ++pScan;
                }
                if (parenDepth == 0) {
                    const QString label = text.mid(i + 1, closeBracket - i - 1);
                    QString dest = text.mid(closeBracket + 2, pScan - (closeBracket + 2)).trimmed();
                    // Strip optional title after whitespace (e.g., url "title")
                    const qsizetype spIdx = dest.indexOf(QLatin1Char(' '));
                    if (spIdx > 0) {
                        const QString after = dest.mid(spIdx + 1).trimmed();
                        if (after.startsWith(QLatin1Char('"')) || after.startsWith(QLatin1Char('\''))) {
                            dest = dest.left(spIdx).trimmed();
                        }
                    }
                    if (dest.startsWith(QLatin1Char('<')) && dest.endsWith(QLatin1Char('>')) && dest.size() >= 2) {
                        dest = dest.mid(1, dest.size() - 2).trimmed();
                    }
                    if (isSafeHref(dest)) {
                        out += QLatin1StringView("<a href=\"");
                        out += escapeHtmlAttr(dest);
                        out += QLatin1StringView("\">");
                        out += renderInlineMarkdown(label, ctx, true, depth + 1);
                        out += QLatin1StringView("</a>");
                        i = pScan + 1;
                        continue;
                    }
                }
            }
        }

        // 4. Autolink <https://...> / <http://...> / <mailto:...>
        if (!insideLink && c == QLatin1Char('<')) {
            const QStringView tail = QStringView(text).mid(i + 1);
            if (tail.startsWith(QLatin1StringView("https://"), Qt::CaseInsensitive)
                || tail.startsWith(QLatin1StringView("http://"), Qt::CaseInsensitive)
                || tail.startsWith(QLatin1StringView("mailto:"), Qt::CaseInsensitive)) {
                const qsizetype gt = text.indexOf(QLatin1Char('>'), i + 1);
                if (gt > i + 1) {
                    const QString url = text.mid(i + 1, gt - i - 1);
                    bool hasWs = false;
                    for (QChar ch : url) {
                        if (ch.isSpace()) {
                            hasWs = true;
                            break;
                        }
                    }
                    if (!hasWs && isSafeHref(url)) {
                        out += QLatin1StringView("<a href=\"");
                        out += escapeHtmlAttr(url);
                        out += QLatin1StringView("\">");
                        appendHtmlEscaped(out, url);
                        out += QLatin1StringView("</a>");
                        i = gt + 1;
                        continue;
                    }
                }
            }
        }

        // 5. Bare http:// or https:// URL
        if (!insideLink && (c == QLatin1Char('h') || c == QLatin1Char('H'))
            && (i == 0 || !text[i - 1].isLetterOrNumber())) {
            const QStringView tail = QStringView(text).mid(i);
            if (tail.startsWith(QLatin1StringView("https://"), Qt::CaseInsensitive)
                || tail.startsWith(QLatin1StringView("http://"), Qt::CaseInsensitive)) {
                qsizetype j = i;
                while (j < n && !text[j].isSpace()
                       && text[j] != QLatin1Char('<') && text[j] != QLatin1Char('>')
                       && text[j] != QLatin1Char('"') && text[j] != QLatin1Char('\'')) {
                    ++j;
                }
                while (j > i) {
                    const QChar last = text[j - 1];
                    if (last == QLatin1Char('.') || last == QLatin1Char(',')
                        || last == QLatin1Char(';') || last == QLatin1Char(':')
                        || last == QLatin1Char('!') || last == QLatin1Char('?')) {
                        --j;
                    } else if (last == QLatin1Char(')')) {
                        const QStringView sub = QStringView(text).mid(i, j - i);
                        if (sub.count(QLatin1Char(')')) > sub.count(QLatin1Char('('))) {
                            --j;
                        } else {
                            break;
                        }
                    } else if (last == QLatin1Char(']') || last == QLatin1Char('}')) {
                        --j;
                    } else {
                        break;
                    }
                }
                if (j > i + 8) {
                    const QString url = text.mid(i, j - i);
                    out += QLatin1StringView("<a href=\"");
                    out += escapeHtmlAttr(url);
                    out += QLatin1StringView("\">");
                    appendHtmlEscaped(out, url);
                    out += QLatin1StringView("</a>");
                    i = j;
                    continue;
                }
            }
        }

        // 6. Strikethrough ~~text~~
        if (c == QLatin1Char('~') && i + 1 < n && text[i + 1] == QLatin1Char('~')
            && i + 2 < n && !text[i + 2].isSpace()) {
            const qsizetype close = findClosingDelimiter(text, i + 2, QLatin1Char('~'), 2, false);
            if (close >= 0) {
                out += QLatin1StringView("<s>");
                out += renderInlineMarkdown(text.mid(i + 2, close - (i + 2)), ctx, insideLink, depth + 1);
                out += QLatin1StringView("</s>");
                i = close + 2;
                continue;
            }
        }

        // 7. Bold + Italic (***text*** or ___text___)
        if ((c == QLatin1Char('*') || c == QLatin1Char('_'))
            && i + 2 < n && text[i + 1] == c && text[i + 2] == c) {
            const bool uRules = (c == QLatin1Char('_'));
            const bool canOpen = uRules
                ? canOpenUnderscore(text, i, 3)
                : (i + 3 < n && !text[i + 3].isSpace());
            if (canOpen) {
                const qsizetype close = findClosingDelimiter(text, i + 3, c, 3, uRules);
                if (close >= 0) {
                    out += QLatin1StringView("<strong><em>");
                    out += renderInlineMarkdown(text.mid(i + 3, close - (i + 3)), ctx, insideLink, depth + 1);
                    out += QLatin1StringView("</em></strong>");
                    i = close + 3;
                    continue;
                }
            }
        }

        // 8. Bold (**text** or __text__)
        if ((c == QLatin1Char('*') || c == QLatin1Char('_')) && i + 1 < n && text[i + 1] == c) {
            const bool uRules = (c == QLatin1Char('_'));
            const bool canOpen = uRules
                ? canOpenUnderscore(text, i, 2)
                : (i + 2 < n && !text[i + 2].isSpace());
            if (canOpen) {
                const qsizetype close = findClosingDelimiter(text, i + 2, c, 2, uRules);
                if (close >= 0) {
                    out += QLatin1StringView("<strong>");
                    out += renderInlineMarkdown(text.mid(i + 2, close - (i + 2)), ctx, insideLink, depth + 1);
                    out += QLatin1StringView("</strong>");
                    i = close + 2;
                    continue;
                }
            }
        }

        // 9. Italic (*text* or _text_)
        if (c == QLatin1Char('*') || c == QLatin1Char('_')) {
            const bool uRules = (c == QLatin1Char('_'));
            const bool canOpen = uRules
                ? canOpenUnderscore(text, i, 1)
                : (i + 1 < n && !text[i + 1].isSpace() && text[i + 1] != QLatin1Char('*'));
            if (canOpen) {
                const qsizetype close = findClosingDelimiter(text, i + 1, c, 1, uRules);
                if (close >= 0) {
                    out += QLatin1StringView("<em>");
                    out += renderInlineMarkdown(text.mid(i + 1, close - (i + 1)), ctx, insideLink, depth + 1);
                    out += QLatin1StringView("</em>");
                    i = close + 1;
                    continue;
                }
            }
        }

        // 10. Plain character
        appendHtmlEscaped(out, QStringView(text.data() + i, 1));
        ++i;
    }

    return out;
}

bool isHorizontalRule(const QString& line) {
    const QString trimmed = line.trimmed();
    if (trimmed.size() < 3) {
        return false;
    }
    const QChar marker = trimmed[0];
    if (marker != QLatin1Char('-') && marker != QLatin1Char('*') && marker != QLatin1Char('_')) {
        return false;
    }
    int count = 0;
    for (QChar ch : trimmed) {
        if (ch == marker) {
            ++count;
        } else if (!ch.isSpace()) {
            return false;
        }
    }
    return count >= 3;
}

bool matchAtxHeading(const QString& line, int* outLevel, QString* outText) {
    qsizetype i = 0;
    const qsizetype n = line.size();
    while (i < n && i < 3 && line[i] == QLatin1Char(' ')) {
        ++i;
    }
    qsizetype hashes = 0;
    while (i + hashes < n && line[i + hashes] == QLatin1Char('#') && hashes < 7) {
        ++hashes;
    }
    if (hashes < 1 || hashes > 6) {
        return false;
    }
    if (i + hashes < n && line[i + hashes] != QLatin1Char(' ') && line[i + hashes] != QLatin1Char('\t')) {
        return false;
    }
    QString content = line.mid(i + hashes).trimmed();
    // Strip optional closing ATX hashes preceded by whitespace
    qsizetype end = content.size();
    while (end > 0 && content[end - 1] == QLatin1Char('#')) {
        --end;
    }
    if (end < content.size() && (end == 0 || content[end - 1].isSpace())) {
        content = content.left(end).trimmed();
    }
    if (outLevel) {
        *outLevel = static_cast<int>(hashes);
    }
    if (outText) {
        *outText = content;
    }
    return true;
}

struct ListMatch {
    bool matched = false;
    bool ordered = false;
    int indent = 0;
    int startNumber = 1;
    QString text;
};

ListMatch matchListItem(const QString& line) {
    ListMatch res;
    qsizetype i = 0;
    const qsizetype n = line.size();
    int indent = 0;
    while (i < n && (line[i] == QLatin1Char(' ') || line[i] == QLatin1Char('\t'))) {
        indent += (line[i] == QLatin1Char('\t')) ? 4 : 1;
        ++i;
    }
    if (i >= n) {
        return res;
    }
    // Bullet list: -, *, +
    if ((line[i] == QLatin1Char('-') || line[i] == QLatin1Char('*') || line[i] == QLatin1Char('+'))
        && i + 1 < n && (line[i + 1] == QLatin1Char(' ') || line[i + 1] == QLatin1Char('\t'))) {
        res.matched = true;
        res.ordered = false;
        res.indent = indent;
        res.text = line.mid(i + 2).trimmed();
        return res;
    }
    // Ordered list: 1. or 1)
    if (line[i].isDigit()) {
        qsizetype d = i;
        while (d < n && line[d].isDigit() && (d - i) < 9) {
            ++d;
        }
        if (d < n && (line[d] == QLatin1Char('.') || line[d] == QLatin1Char(')'))
            && d + 1 < n && (line[d + 1] == QLatin1Char(' ') || line[d + 1] == QLatin1Char('\t'))) {
            res.matched = true;
            res.ordered = true;
            res.indent = indent;
            res.startNumber = line.mid(i, d - i).toInt();
            res.text = line.mid(d + 2).trimmed();
            return res;
        }
    }
    return res;
}

bool isBlockquoteLine(const QString& line, QString* outStripped = nullptr) {
    qsizetype i = 0;
    const qsizetype n = line.size();
    while (i < n && i < 3 && line[i] == QLatin1Char(' ')) {
        ++i;
    }
    if (i < n && line[i] == QLatin1Char('>')) {
        ++i;
        if (i < n && line[i] == QLatin1Char(' ')) {
            ++i;
        }
        if (outStripped) {
            *outStripped = line.mid(i);
        }
        return true;
    }
    return false;
}

QStringList splitTableCells(const QString& line) {
    QString trimmed = line.trimmed();
    if (trimmed.startsWith(QLatin1Char('|'))) {
        trimmed.remove(0, 1);
    }
    if (trimmed.endsWith(QLatin1Char('|')) && !trimmed.endsWith(QLatin1StringView("\\|"))) {
        trimmed.chop(1);
    }
    QStringList cells;
    QString cur;
    const qsizetype n = trimmed.size();
    for (qsizetype i = 0; i < n; ++i) {
        if (trimmed[i] == QLatin1Char('\\') && i + 1 < n && trimmed[i + 1] == QLatin1Char('|')) {
            cur += QLatin1Char('|');
            ++i;
        } else if (trimmed[i] == QLatin1Char('|')) {
            cells.append(cur.trimmed());
            cur.clear();
        } else {
            cur += trimmed[i];
        }
    }
    cells.append(cur.trimmed());
    return cells;
}

bool isTableDelimiterRow(const QString& line, QStringList* outAlignments = nullptr) {
    if (!line.contains(QLatin1Char('|'))) {
        return false;
    }
    const QStringList cells = splitTableCells(line);
    if (cells.isEmpty()) {
        return false;
    }
    QStringList aligns;
    aligns.reserve(cells.size());
    for (const QString& cell : cells) {
        const QString c = cell.trimmed();
        if (c.isEmpty()) {
            return false;
        }
        const bool leftColon = c.startsWith(QLatin1Char(':'));
        const bool rightColon = c.endsWith(QLatin1Char(':'));
        qsizetype start = leftColon ? 1 : 0;
        qsizetype end = c.size() - (rightColon ? 1 : 0);
        if (end - start < 1) {
            return false;
        }
        for (qsizetype k = start; k < end; ++k) {
            if (c[k] != QLatin1Char('-')) {
                return false;
            }
        }
        if (leftColon && rightColon) {
            aligns.append(QStringLiteral("center"));
        } else if (rightColon) {
            aligns.append(QStringLiteral("right"));
        } else {
            aligns.append(QStringLiteral("left"));
        }
    }
    if (outAlignments) {
        *outAlignments = aligns;
    }
    return true;
}

bool stripTaskListCheckbox(const QString& text, QString* outCheckboxHtml, QString* outRest) {
    if (text.startsWith(QLatin1StringView("[ ] "))) {
        if (outCheckboxHtml) {
            *outCheckboxHtml = QStringLiteral("&#9744; ");
        }
        if (outRest) {
            *outRest = text.mid(4);
        }
        return true;
    }
    if (text.startsWith(QLatin1StringView("[x] "), Qt::CaseInsensitive)) {
        if (outCheckboxHtml) {
            *outCheckboxHtml = QStringLiteral("&#9745; ");
        }
        if (outRest) {
            *outRest = text.mid(4);
        }
        return true;
    }
    return false;
}

QString renderBlocksToHtml(const QStringList& lines,
                           const InlineRenderContext& ctx,
                           const UiPalette& ui,
                           int quoteDepth = 0) {
    const QColor solidDivider = compositeOver(ui.divider, ui.background);
    const QColor solidBorder = compositeOver(ui.subtleBorder, ui.background);
    const QColor solidElevated = compositeOver(ui.elevatedSurface, ui.background);
    const QColor solidSecondary = compositeOver(ui.textSecondary, ui.background);

    QStringList blocksHtml;
    const qsizetype total = lines.size();
    qsizetype i = 0;

    while (i < total) {
        if (lines[i].trimmed().isEmpty()) {
            ++i;
            continue;
        }

        // 1. ATX Heading
        int headingLevel = 0;
        QString headingText;
        if (matchAtxHeading(lines[i], &headingLevel, &headingText)) {
            static constexpr const char* kFontSizes[6] = {
                "18px", "16px", "14.5px", "13.5px", "13px", "12.5px"
            };
            const QString colorHex = (headingLevel == 6)
                ? solidSecondary.name(QColor::HexRgb)
                : ui.textPrimary.name(QColor::HexRgb);
            const QString rendered = renderInlineMarkdown(headingText, ctx);
            blocksHtml.append(QStringLiteral(
                "<h%1 style=\"margin-top:%2px; margin-bottom:4px; font-size:%3; "
                "font-weight:600; color:%4;\">%5</h%1>")
                .arg(headingLevel)
                .arg(blocksHtml.isEmpty() ? 0 : (headingLevel <= 2 ? 10 : 7))
                .arg(QLatin1StringView(kFontSizes[headingLevel - 1]),
                     colorHex,
                     rendered));
            ++i;
            continue;
        }

        // 2. Horizontal Rule
        if (isHorizontalRule(lines[i])) {
            blocksHtml.append(QStringLiteral(
                "<hr style=\"background-color:%1; height:1px; border:none; margin-top:6px; margin-bottom:6px;\"/>")
                .arg(solidDivider.name(QColor::HexRgb)));
            ++i;
            continue;
        }

        // 3. Blockquote
        QString stripped;
        if (isBlockquoteLine(lines[i], &stripped) && quoteDepth < 6) {
            QStringList quoteLines;
            while (i < total && isBlockquoteLine(lines[i], &stripped)) {
                quoteLines.append(stripped);
                ++i;
            }
            const QString innerHtml = renderBlocksToHtml(quoteLines, ctx, ui, quoteDepth + 1);
            blocksHtml.append(QStringLiteral(
                "<blockquote style=\"margin-top:%1px; margin-bottom:6px; margin-left:12px; "
                "margin-right:0px; color:%2;\">%3</blockquote>")
                .arg(blocksHtml.isEmpty() ? 0 : 4)
                .arg(solidSecondary.name(QColor::HexRgb), innerHtml));
            continue;
        }

        // 4. GFM Pipe Table
        QStringList aligns;
        if (i + 1 < total
            && lines[i].contains(QLatin1Char('|'))
            && isTableDelimiterRow(lines[i + 1], &aligns)) {
            const QStringList headerCells = splitTableCells(lines[i]);
            i += 2;
            QVector<QStringList> bodyRows;
            while (i < total && !lines[i].trimmed().isEmpty() && lines[i].contains(QLatin1Char('|'))) {
                bodyRows.append(splitTableCells(lines[i]));
                ++i;
            }

            QString tbl;
            tbl += QStringLiteral(
                "<table cellspacing=\"0\" cellpadding=\"4\" style=\"margin-top:%1px; margin-bottom:6px; "
                "border-collapse:collapse; border:1px solid %2;\">")
                .arg(blocksHtml.isEmpty() ? 0 : 4)
                .arg(solidBorder.name(QColor::HexRgb));

            tbl += QLatin1StringView("<thead><tr>");
            const qsizetype cols = std::max(headerCells.size(), aligns.size());
            for (qsizetype c = 0; c < cols; ++c) {
                const QString align = (c < aligns.size()) ? aligns[c] : QStringLiteral("left");
                const QString cellText = (c < headerCells.size()) ? headerCells[c] : QString();
                tbl += QStringLiteral(
                    "<th align=\"%1\" style=\"background-color:%2; border:1px solid %3; "
                    "font-weight:600; text-align:%1;\">%4</th>")
                    .arg(align,
                         solidElevated.name(QColor::HexRgb),
                         solidBorder.name(QColor::HexRgb),
                         renderInlineMarkdown(cellText, ctx));
            }
            tbl += QLatin1StringView("</tr></thead>");

            if (!bodyRows.isEmpty()) {
                tbl += QLatin1StringView("<tbody>");
                for (const QStringList& row : bodyRows) {
                    tbl += QLatin1StringView("<tr>");
                    for (qsizetype c = 0; c < cols; ++c) {
                        const QString align = (c < aligns.size()) ? aligns[c] : QStringLiteral("left");
                        const QString cellText = (c < row.size()) ? row[c] : QString();
                        tbl += QStringLiteral(
                            "<td align=\"%1\" style=\"border:1px solid %2; text-align:%1;\">%3</td>")
                            .arg(align,
                                 solidBorder.name(QColor::HexRgb),
                                 renderInlineMarkdown(cellText, ctx));
                    }
                    tbl += QLatin1StringView("</tr>");
                }
                tbl += QLatin1StringView("</tbody>");
            }
            tbl += QLatin1StringView("</table>");
            blocksHtml.append(tbl);
            continue;
        }

        // 5. Bullet / Numbered List (supports nesting and continuation lines)
        if (const ListMatch firstItem = matchListItem(lines[i]); firstItem.matched) {
            struct ListFrame {
                bool ordered = false;
                int indent = 0;
            };
            QVector<ListFrame> stack;
            QString listHtml;

            auto openList = [&](bool ordered, int startNum, bool topLevel) {
                const int topMargin = (topLevel && !blocksHtml.isEmpty()) ? 3 : 0;
                const int bottomMargin = topLevel ? 6 : 2;
                if (ordered) {
                    if (startNum > 1) {
                        listHtml += QStringLiteral(
                            "<ol start=\"%1\" style=\"margin-top:%2px; margin-bottom:%3px; "
                            "margin-left:0px; -qt-list-indent:1;\">")
                            .arg(startNum)
                            .arg(topMargin)
                            .arg(bottomMargin);
                    } else {
                        listHtml += QStringLiteral(
                            "<ol style=\"margin-top:%1px; margin-bottom:%2px; "
                            "margin-left:0px; -qt-list-indent:1;\">")
                            .arg(topMargin)
                            .arg(bottomMargin);
                    }
                } else {
                    listHtml += QStringLiteral(
                        "<ul style=\"margin-top:%1px; margin-bottom:%2px; "
                        "margin-left:0px; -qt-list-indent:1;\">")
                        .arg(topMargin)
                        .arg(bottomMargin);
                }
            };

            auto closeList = [&](bool ordered) {
                listHtml += ordered ? QLatin1StringView("</li></ol>") : QLatin1StringView("</li></ul>");
            };

            while (i < total) {
                if (lines[i].trimmed().isEmpty()) {
                    // Keep list open across a single blank line if followed by a matching list item
                    qsizetype nextNonBlank = i + 1;
                    while (nextNonBlank < total && lines[nextNonBlank].trimmed().isEmpty()) {
                        ++nextNonBlank;
                    }
                    if (nextNonBlank < total) {
                        const ListMatch peek = matchListItem(lines[nextNonBlank]);
                        if (peek.matched && !stack.isEmpty()
                            && peek.ordered == stack.first().ordered
                            && peek.indent <= stack.last().indent + 4) {
                            i = nextNonBlank;
                            continue;
                        }
                    }
                    break;
                }

                const ListMatch item = matchListItem(lines[i]);
                if (item.matched) {
                    while (!stack.isEmpty() && item.indent < stack.last().indent) {
                        const bool wasOrdered = stack.last().ordered;
                        stack.removeLast();
                        closeList(wasOrdered);
                    }
                    if (!stack.isEmpty() && item.indent == stack.last().indent
                        && item.ordered != stack.last().ordered) {
                        const bool wasOrdered = stack.last().ordered;
                        stack.removeLast();
                        closeList(wasOrdered);
                    }

                    if (stack.isEmpty() || item.indent > stack.last().indent) {
                        const bool topLevel = stack.isEmpty();
                        openList(item.ordered, item.startNumber, topLevel);
                        stack.append(ListFrame{item.ordered, item.indent});
                    } else {
                        listHtml += QLatin1StringView("</li>");
                    }

                    listHtml += QLatin1StringView("<li style=\"margin-top:1px; margin-bottom:2px;\">");
                    QString checkboxHtml;
                    QString restText;
                    if (!item.ordered && stripTaskListCheckbox(item.text, &checkboxHtml, &restText)) {
                        listHtml += checkboxHtml;
                        listHtml += renderInlineMarkdown(restText, ctx);
                    } else {
                        listHtml += renderInlineMarkdown(item.text, ctx);
                    }
                    ++i;
                    continue;
                }

                // Indented continuation line inside the current list item
                if (!stack.isEmpty()
                    && (lines[i].startsWith(QLatin1Char(' ')) || lines[i].startsWith(QLatin1Char('\t')))
                    && !matchAtxHeading(lines[i], nullptr, nullptr)
                    && !isHorizontalRule(lines[i])
                    && !isBlockquoteLine(lines[i])) {
                    listHtml += QLatin1StringView("<br/>");
                    listHtml += renderInlineMarkdown(lines[i].trimmed(), ctx);
                    ++i;
                    continue;
                }

                break;
            }

            while (!stack.isEmpty()) {
                const bool wasOrdered = stack.last().ordered;
                stack.removeLast();
                closeList(wasOrdered);
            }
            blocksHtml.append(listHtml);
            continue;
        }

        // 6. Paragraph
        QStringList paraLines;
        while (i < total && !lines[i].trimmed().isEmpty()) {
            if (!paraLines.isEmpty()) {
                if (matchAtxHeading(lines[i], nullptr, nullptr)
                    || isHorizontalRule(lines[i])
                    || isBlockquoteLine(lines[i])
                    || matchListItem(lines[i]).matched) {
                    break;
                }
                if (i + 1 < total && lines[i].contains(QLatin1Char('|'))
                    && isTableDelimiterRow(lines[i + 1])) {
                    break;
                }
            }
            paraLines.append(lines[i]);
            ++i;
        }

        QString paraInner;
        for (qsizetype p = 0; p < paraLines.size(); ++p) {
            if (p > 0) {
                paraInner += QLatin1StringView("<br/>");
            }
            paraInner += renderInlineMarkdown(paraLines[p], ctx);
        }
        const int topMargin = blocksHtml.isEmpty() ? 0 : 4;
        blocksHtml.append(QStringLiteral(
            "<p style=\"margin-top:%1px; margin-bottom:6px;\">%2</p>")
            .arg(topMargin)
            .arg(paraInner));
    }

    // Remove bottom margin on the final block's opening tag to prevent trailing vertical gap
    if (!blocksHtml.isEmpty()) {
        QString& last = blocksHtml.last();
        const qsizetype tagEnd = last.indexOf(QLatin1Char('>'));
        if (tagEnd > 0) {
            const qsizetype mb = last.indexOf(QLatin1StringView("margin-bottom:6px;"));
            if (mb >= 0 && mb < tagEnd) {
                last.replace(mb, 18, QLatin1StringView("margin-bottom:0px;"));
            } else {
                const qsizetype mb4 = last.indexOf(QLatin1StringView("margin-bottom:4px;"));
                if (mb4 >= 0 && mb4 < tagEnd) {
                    last.replace(mb4, 18, QLatin1StringView("margin-bottom:0px;"));
                }
            }
        }
    }

    return blocksHtml.join(QString());
}

} // namespace

// ============================================================================
// SpellCodeBlockWidget
// ============================================================================

SpellCodeBlockWidget::SpellCodeBlockWidget(QWidget* parent)
    : QFrame(parent) {
    setupUi();
    applyTheme();
}

SpellCodeBlockWidget::SpellCodeBlockWidget(const QString& language,
                                           const QString& code,
                                           QWidget* parent)
    : SpellCodeBlockWidget(parent) {
    setCode(language, code);
}

void SpellCodeBlockWidget::setupUi() {
    setObjectName(QStringLiteral("SpellCodeBlockWidget"));
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Plain);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMinimumWidth(1);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("SpellCodeBlockHeader"));
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 5, 8, 5);
    headerLayout->setSpacing(6);

    m_languageLabel = new QLabel(QStringLiteral("code"), header);
    m_languageLabel->setObjectName(QStringLiteral("SpellCodeBlockLanguageLabel"));
    m_languageLabel->setTextFormat(Qt::PlainText);
    m_languageLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_languageLabel->setMinimumWidth(0);
    headerLayout->addWidget(m_languageLabel);

    headerLayout->addStretch(1);

    m_copyButton = new QPushButton(QStringLiteral("Copy"), header);
    m_copyButton->setObjectName(QStringLiteral("SpellCodeBlockCopyButton"));
    m_copyButton->setCursor(Qt::PointingHandCursor);
    m_copyButton->setFlat(true);
    m_copyButton->setFocusPolicy(Qt::TabFocus);
    m_copyButton->setToolTip(QStringLiteral("Copy code to clipboard"));
    m_copyButton->setIconSize(QSize(12, 12));
    m_copyButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(m_copyButton, &QPushButton::clicked, this, &SpellCodeBlockWidget::copyToClipboard);
    headerLayout->addWidget(m_copyButton);

    rootLayout->addWidget(header);

    m_codeView = new QTextBrowser(this);
    m_codeView->setObjectName(QStringLiteral("SpellCodeBlockCodeView"));
    m_codeView->setFrameShape(QFrame::NoFrame);
    m_codeView->setReadOnly(true);
    m_codeView->setOpenLinks(false);
    m_codeView->setOpenExternalLinks(false);
    m_codeView->setUndoRedoEnabled(false);
    m_codeView->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_codeView->setLineWrapMode(QTextEdit::NoWrap);
    m_codeView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_codeView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_codeView->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    m_codeView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_codeView->setMinimumWidth(1);
    if (QTextDocument* doc = m_codeView->document()) {
        doc->setDocumentMargin(0.0);
    }
    if (QScrollBar* hBar = m_codeView->horizontalScrollBar()) {
        connect(hBar, &QScrollBar::rangeChanged, this, [this](int, int) {
            updateCodeViewHeight();
        });
    }

    rootLayout->addWidget(m_codeView);

    m_copyResetTimer = new QTimer(this);
    m_copyResetTimer->setSingleShot(true);
    m_copyResetTimer->setInterval(1200);
    connect(m_copyResetTimer, &QTimer::timeout, this, [this]() {
        m_showingCopied = false;
        updateCopyButtonAppearance();
    });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SpellCodeBlockWidget::applyTheme);
}

void SpellCodeBlockWidget::setCode(const QString& language, const QString& code) {
    const QString trimmedLang = language.trimmed();
    if (m_language == trimmedLang && m_code == code) {
        return;
    }
    m_language = trimmedLang;
    m_code = code;

    if (m_languageLabel) {
        m_languageLabel->setText(m_language.isEmpty() ? QStringLiteral("code") : m_language);
    }
    renderHighlightedCode();
}

QString SpellCodeBlockWidget::highlightToHtml(const QString& code,
                                              const QString& language,
                                              const CodePalette& palette) {
    if (code.isEmpty()) {
        return {};
    }

    const QString normalized = normalizeLineEndings(code);
    const QString lang = language.trimmed().toLower();
    HtmlTokenBuilder builder(palette, normalized.size());

    if (isDiffLanguage(lang)) {
        highlightDiffCode(normalized, builder);
    } else if (isMarkupLanguage(lang)) {
        highlightMarkupCode(normalized, builder);
    } else if (isJsonLanguage(lang)) {
        highlightJsonCode(normalized, builder);
    } else if (isYamlLanguage(lang) || isTomlLanguage(lang)) {
        highlightYamlTomlCode(normalized, isTomlLanguage(lang), builder);
    } else {
        highlightGeneralCode(normalized, lang, builder);
    }

    return builder.finish();
}

void SpellCodeBlockWidget::copyToClipboard() {
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(m_code, QClipboard::Clipboard);
    }
    m_showingCopied = true;
    updateCopyButtonAppearance();
    if (m_copyResetTimer) {
        m_copyResetTimer->start(1200);
    }
    emit copied();
}

void SpellCodeBlockWidget::applyTheme() {
    const auto& tm = ThemeManager::instance();
    const UiPalette ui = tm.ui();
    const CodePalette code = tm.code();

    const QColor codeBg = code.background;
    const bool darkCodeBg = relativeLuminance(codeBg) < 0.25;
    const QColor headerBg = darkCodeBg
        ? compositeOver(rgbHex(0xFFFFFF, 0.04), codeBg)
        : compositeOver(rgbHex(0x000000, 0.03), codeBg);
    const QColor headerDivider = darkCodeBg
        ? compositeOver(rgbHex(0xFFFFFF, 0.10), codeBg)
        : compositeOver(rgbHex(0x000000, 0.10), codeBg);
    const QColor outerBorder = compositeOver(ui.subtleBorder, ui.background);
    const QColor copyHoverBg = darkCodeBg
        ? rgbHex(0xFFFFFF, 0.10)
        : rgbHex(0x000000, 0.08);
    const QColor selectionBg = darkCodeBg
        ? compositeOver(rgbHex(0xFFFFFF, 0.16), codeBg)
        : compositeOver(rgbHex(0x000000, 0.12), codeBg);

    if (m_languageLabel) {
        QFont langFont(tm.editorFontFamily());
        langFont.setStyleHint(QFont::Monospace);
        langFont.setPointSizeF(9.5);
        langFont.setWeight(QFont::DemiBold);
        m_languageLabel->setFont(langFont);
    }

    if (m_copyButton) {
        QFont btnFont = m_copyButton->font();
        btnFont.setPointSizeF(9.5);
        btnFont.setWeight(QFont::Medium);
        m_copyButton->setFont(btnFont);
    }

    const QString qss = QStringLiteral(
        "QFrame#SpellCodeBlockWidget {\n"
        "  background-color: %1;\n"
        "  border: 1px solid %2;\n"
        "  border-radius: 8px;\n"
        "}\n"
        "QWidget#SpellCodeBlockHeader {\n"
        "  background-color: %3;\n"
        "  border: none;\n"
        "  border-bottom: 1px solid %4;\n"
        "  border-top-left-radius: 7px;\n"
        "  border-top-right-radius: 7px;\n"
        "}\n"
        "QLabel#SpellCodeBlockLanguageLabel {\n"
        "  background-color: transparent;\n"
        "  color: %5;\n"
        "  border: none;\n"
        "}\n"
        "QPushButton#SpellCodeBlockCopyButton {\n"
        "  background-color: transparent;\n"
        "  color: %5;\n"
        "  border: none;\n"
        "  border-radius: 4px;\n"
        "  padding: 2px 6px;\n"
        "  font-weight: 500;\n"
        "}\n"
        "QPushButton#SpellCodeBlockCopyButton[copied=\"true\"] {\n"
        "  color: %7;\n"
        "}\n"
        "QPushButton#SpellCodeBlockCopyButton:hover {\n"
        "  background-color: %6;\n"
        "  color: %7;\n"
        "}\n"
        "QTextBrowser#SpellCodeBlockCodeView {\n"
        "  background-color: %1;\n"
        "  color: %7;\n"
        "  border: none;\n"
        "  padding: 0px;\n"
        "  border-bottom-left-radius: 7px;\n"
        "  border-bottom-right-radius: 7px;\n"
        "  selection-background-color: %8;\n"
        "  selection-color: %7;\n"
        "}\n")
        .arg(qssColor(codeBg),
             qssColor(outerBorder),
             qssColor(headerBg),
             qssColor(headerDivider),
             qssColor(code.comment),
             qssColor(copyHoverBg),
             qssColor(code.plain),
             qssColor(selectionBg));

    setStyleSheet(qss);
    updateCopyButtonAppearance();
    renderHighlightedCode();
}

void SpellCodeBlockWidget::renderHighlightedCode() {
    if (!m_codeView) {
        return;
    }

    const auto& tm = ThemeManager::instance();
    const CodePalette palette = tm.code();
    const QString monoFamily = tm.editorFontFamily();
    const double ptSize = std::max(10.0, tm.editorFontSize() - 1.0);

    QFont codeFont(monoFamily);
    codeFont.setStyleHint(QFont::Monospace);
    codeFont.setFixedPitch(true);
    codeFont.setPointSizeF(ptSize);
    m_codeView->setFont(codeFont);

    QTextDocument* doc = m_codeView->document();
    if (doc) {
        doc->setDefaultFont(codeFont);
        QTextOption opt = doc->defaultTextOption();
        opt.setWrapMode(QTextOption::NoWrap);
        opt.setTabStopDistance(QFontMetricsF(codeFont).horizontalAdvance(QLatin1Char(' ')) * 4.0);
        doc->setDefaultTextOption(opt);
    }

    QString highlighted = highlightToHtml(m_code, m_language, palette);
    if (highlighted.startsWith(QLatin1Char('\n'))) {
        // HTML parsers drop a single newline directly after <pre>.
        highlighted.prepend(QLatin1Char('\n'));
    }

    const QString html = QStringLiteral(
        "<html><body style=\"margin:0; padding:0; background-color:transparent; color:%1;\">"
        "<pre style=\"margin:0; padding:0; font-family:'%2', monospace; font-size:%3pt; "
        "white-space:pre; color:%1;\">%4</pre>"
        "</body></html>")
        .arg(palette.plain.name(QColor::HexRgb),
             escapeHtmlAttr(monoFamily),
             QString::number(ptSize, 'f', 1),
             highlighted);

    m_codeView->setHtml(html);

    if (doc) {
        if (QTextFrame* rootFrame = doc->rootFrame()) {
            QTextFrameFormat fmt = rootFrame->frameFormat();
            fmt.setTopMargin(7.0);
            fmt.setBottomMargin(9.0);
            fmt.setLeftMargin(10.0);
            fmt.setRightMargin(10.0);
            rootFrame->setFrameFormat(fmt);
        }
    }

    updateCodeViewHeight();
}

void SpellCodeBlockWidget::updateCodeViewHeight() {
    if (!m_codeView) {
        return;
    }
    QTextDocument* doc = m_codeView->document();
    if (!doc) {
        return;
    }

    doc->setTextWidth(-1.0);
    const qreal docHeight = doc->size().height();
    const QFontMetrics fm(m_codeView->font());
    const int minHeight = fm.lineSpacing() + 18;

    int hBarHeight = 0;
    if (const QScrollBar* hBar = m_codeView->horizontalScrollBar()) {
        if (hBar->isVisible() || hBar->maximum() > hBar->minimum()) {
            hBarHeight = std::max(hBar->sizeHint().height(), 11);
        }
    }

    const int targetHeight = std::max(minHeight, static_cast<int>(std::ceil(docHeight)) + 2) + hBarHeight;
    if (m_codeView->minimumHeight() != targetHeight || m_codeView->maximumHeight() != targetHeight) {
        m_codeView->setFixedHeight(targetHeight);
        updateGeometry();
    }
}

void SpellCodeBlockWidget::updateCopyButtonAppearance() {
    if (!m_copyButton) {
        return;
    }
    const CodePalette code = ThemeManager::instance().code();
    const QColor fg = m_showingCopied ? code.plain : code.comment;

    m_copyButton->setText(m_showingCopied ? QStringLiteral("Copied") : QStringLiteral("Copy"));
    m_copyButton->setIcon(spell::icons::vectorIcon(
        m_showingCopied ? QStringLiteral("check") : QStringLiteral("copy"), fg, 12));

    m_copyButton->setProperty("copied", m_showingCopied);
    if (QStyle* st = m_copyButton->style()) {
        st->unpolish(m_copyButton);
        st->polish(m_copyButton);
    }
}

// ============================================================================
// SpellMarkdownView
// ============================================================================

SpellMarkdownView::SpellMarkdownView(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("SpellMarkdownView"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMinimumWidth(1);
    setAttribute(Qt::WA_StyledBackground, false);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(8);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SpellMarkdownView::applyTheme);
}

void SpellMarkdownView::setMarkdown(const QString& md, const QStringList& projectRoots) {
    const bool rootsChanged = (m_projectRoots != projectRoots);
    if (m_markdown == md && !rootsChanged) {
        return;
    }
    m_markdown = md;
    m_projectRoots = projectRoots;
    syncBlocks(rootsChanged);
}

void SpellMarkdownView::clear() {
    m_markdown.clear();
    m_projectRoots.clear();
    for (BlockEntry& entry : m_blocks) {
        if (entry.proseLabel) {
            m_layout->removeWidget(entry.proseLabel);
            entry.proseLabel->hide();
            entry.proseLabel->deleteLater();
        }
        if (entry.codeBlock) {
            m_layout->removeWidget(entry.codeBlock);
            entry.codeBlock->hide();
            entry.codeBlock->deleteLater();
        }
    }
    m_blocks.clear();
    updateGeometry();
}

QVector<SpellMarkdownView::Segment> SpellMarkdownView::parseSegments(const QString& markdown) {
    QVector<Segment> segments;
    if (markdown.isEmpty()) {
        return segments;
    }

    const QString normalized = normalizeLineEndings(markdown);
    const QStringList lines = normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    QStringList proseLines;
    QStringList codeLines;
    QString language;
    bool inCode = false;
    QChar fenceChar = QLatin1Char('`');
    qsizetype fenceLen = 3;

    auto flushProse = [&]() {
        // Trim leading and trailing blank lines around code blocks while keeping inner formatting
        qsizetype first = 0;
        while (first < proseLines.size() && proseLines[first].trimmed().isEmpty()) {
            ++first;
        }
        qsizetype last = proseLines.size();
        while (last > first && proseLines[last - 1].trimmed().isEmpty()) {
            --last;
        }
        if (first < last) {
            QStringList slice;
            slice.reserve(last - first);
            for (qsizetype k = first; k < last; ++k) {
                slice.append(proseLines[k]);
            }
            Segment seg;
            seg.kind = Segment::Kind::Prose;
            seg.content = slice.join(QLatin1Char('\n'));
            segments.append(std::move(seg));
        }
        proseLines.clear();
    };

    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();

        if (!inCode) {
            if (trimmed.startsWith(QLatin1StringView("```"))
                || trimmed.startsWith(QLatin1StringView("~~~"))) {
                const QChar fc = trimmed[0];
                qsizetype fl = 0;
                while (fl < trimmed.size() && trimmed[fl] == fc) {
                    ++fl;
                }
                const QString info = trimmed.mid(fl).trimmed();
                // CommonMark: backtick fence info strings cannot contain backticks
                // (avoids mistaking single-line ```inline``` for a fenced code block).
                if (fc == QLatin1Char('`') && info.contains(QLatin1Char('`'))) {
                    proseLines.append(line);
                    continue;
                }

                flushProse();
                inCode = true;
                fenceChar = fc;
                fenceLen = fl;
                // Take the first whitespace-delimited token as the language identifier
                qsizetype tokEnd = 0;
                while (tokEnd < info.size() && !info[tokEnd].isSpace()) {
                    ++tokEnd;
                }
                language = info.left(tokEnd).toLower();
                codeLines.clear();
                continue;
            }
            proseLines.append(line);
        } else {
            // Closing fence: at least fenceLen repetitions of fenceChar and nothing else
            bool isClose = false;
            if (trimmed.size() >= fenceLen && trimmed[0] == fenceChar) {
                isClose = true;
                for (QChar ch : trimmed) {
                    if (ch != fenceChar) {
                        isClose = false;
                        break;
                    }
                }
            }
            if (isClose) {
                Segment seg;
                seg.kind = Segment::Kind::Code;
                seg.language = language;
                seg.content = codeLines.join(QLatin1Char('\n'));
                segments.append(std::move(seg));
                codeLines.clear();
                language.clear();
                inCode = false;
                continue;
            }
            codeLines.append(line);
        }
    }

    if (inCode) {
        // Unterminated trailing fence (e.g., while streaming): still render as a code block.
        Segment seg;
        seg.kind = Segment::Kind::Code;
        seg.language = language;
        seg.content = codeLines.join(QLatin1Char('\n'));
        segments.append(std::move(seg));
    } else {
        flushProse();
    }

    return segments;
}

QString SpellMarkdownView::renderProseToHtml(const QString& prose,
                                             const QStringList& projectRoots,
                                             const UiPalette& ui,
                                             const CodePalette& code,
                                             const QString& monoFontFamily) {
    if (prose.trimmed().isEmpty()) {
        return {};
    }

    const QString normalized = normalizeLineEndings(prose);
    const QStringList lines = normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    const QColor inlineBg = compositeOver(ui.selectedFill, ui.background);
    const QColor inlineFg = pickContrastingColor(code.plain, ui.textPrimary, inlineBg);
    const QColor textPrimary = compositeOver(ui.textPrimary, ui.background);
    const QColor linkColor = compositeOver(ui.accent, ui.background);

    InlineRenderContext ctx;
    ctx.inlineCodeBg = inlineBg;
    ctx.inlineCodeFg = inlineFg;
    ctx.monoFontFamily = monoFontFamily.isEmpty() ? QStringLiteral("Monospace") : monoFontFamily;

    const QString bodyHtml = renderBlocksToHtml(lines, ctx, ui);
    const QString linkedBody = linkifyHtml(bodyHtml, projectRoots);

    return QStringLiteral(
        "<html><head><style type=\"text/css\">"
        "body { color:%1; font-size:13px; margin:0; padding:0; }"
        "a { color:%2; text-decoration:underline; }"
        "p { line-height:135%; }"
        "li { line-height:135%; }"
        "</style></head><body>%3</body></html>")
        .arg(textPrimary.name(QColor::HexRgb),
             linkColor.name(QColor::HexRgb),
             linkedBody);
}

void SpellMarkdownView::applyTheme() {
    for (BlockEntry& entry : m_blocks) {
        if (entry.kind == Segment::Kind::Prose && entry.proseLabel) {
            updateProseLabel(entry.proseLabel, entry.content);
        } else if (entry.kind == Segment::Kind::Code && entry.codeBlock) {
            entry.codeBlock->applyTheme();
        }
    }
    updateGeometry();
}

void SpellMarkdownView::handleLinkActivated(const QString& href) {
    const QString trimmed = href.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    const QUrl url(trimmed);
    QString filePath;
    int line = 0;
    if (parseSpellFileUrl(url, &filePath, &line)) {
        emit fileCitationClicked(filePath, line);
        return;
    }

    if (url.isLocalFile()) {
        const QString localPath = url.toLocalFile();
        if (!localPath.isEmpty()) {
            int localLine = 0;
            const QString frag = url.fragment();
            if (frag.startsWith(QLatin1Char('L'), Qt::CaseInsensitive)) {
                const QString numStr = frag.mid(1).section(QLatin1Char('-'), 0, 0);
                bool ok = false;
                const int parsed = numStr.toInt(&ok);
                if (ok && parsed > 0) {
                    localLine = parsed;
                }
            }
            emit fileCitationClicked(localPath, localLine);
            return;
        }
    }

    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1StringView("http")
        || scheme == QLatin1StringView("https")
        || scheme == QLatin1StringView("mailto")) {
        QDesktopServices::openUrl(url);
    }
}

void SpellMarkdownView::syncBlocks(bool forceRestyle) {
    const QVector<Segment> segments = parseSegments(m_markdown);
    const qsizetype newCount = segments.size();

    // Remove surplus trailing blocks
    while (m_blocks.size() > newCount) {
        BlockEntry entry = m_blocks.takeLast();
        if (entry.proseLabel) {
            m_layout->removeWidget(entry.proseLabel);
            entry.proseLabel->hide();
            entry.proseLabel->deleteLater();
        }
        if (entry.codeBlock) {
            m_layout->removeWidget(entry.codeBlock);
            entry.codeBlock->hide();
            entry.codeBlock->deleteLater();
        }
    }

    // Update or replace overlapping blocks in place
    const qsizetype commonCount = std::min(m_blocks.size(), newCount);
    for (qsizetype i = 0; i < commonCount; ++i) {
        const Segment& seg = segments[i];
        BlockEntry& entry = m_blocks[i];

        if (entry.kind == seg.kind) {
            if (seg.kind == Segment::Kind::Prose) {
                if (forceRestyle || entry.content != seg.content) {
                    entry.content = seg.content;
                    updateProseLabel(entry.proseLabel, entry.content);
                }
            } else {
                const bool contentChanged = (entry.language != seg.language || entry.content != seg.content);
                if (contentChanged) {
                    entry.language = seg.language;
                    entry.content = seg.content;
                    entry.codeBlock->setCode(entry.language, entry.content);
                }
                if (forceRestyle) {
                    entry.codeBlock->applyTheme();
                }
            }
        } else {
            // Segment kind changed at this index: swap child widget in place
            if (entry.proseLabel) {
                m_layout->removeWidget(entry.proseLabel);
                entry.proseLabel->hide();
                entry.proseLabel->deleteLater();
                entry.proseLabel = nullptr;
            }
            if (entry.codeBlock) {
                m_layout->removeWidget(entry.codeBlock);
                entry.codeBlock->hide();
                entry.codeBlock->deleteLater();
                entry.codeBlock = nullptr;
            }

            entry.kind = seg.kind;
            entry.language = seg.language;
            entry.content = seg.content;

            if (seg.kind == Segment::Kind::Prose) {
                entry.proseLabel = createProseLabel();
                updateProseLabel(entry.proseLabel, entry.content);
                m_layout->insertWidget(static_cast<int>(i), entry.proseLabel);
            } else {
                entry.codeBlock = new SpellCodeBlockWidget(entry.language, entry.content, this);
                m_layout->insertWidget(static_cast<int>(i), entry.codeBlock);
            }
        }
    }

    // Append new trailing blocks
    for (qsizetype i = commonCount; i < newCount; ++i) {
        const Segment& seg = segments[i];
        BlockEntry entry;
        entry.kind = seg.kind;
        entry.language = seg.language;
        entry.content = seg.content;

        if (seg.kind == Segment::Kind::Prose) {
            entry.proseLabel = createProseLabel();
            updateProseLabel(entry.proseLabel, entry.content);
            m_layout->addWidget(entry.proseLabel);
        } else {
            entry.codeBlock = new SpellCodeBlockWidget(entry.language, entry.content, this);
            m_layout->addWidget(entry.codeBlock);
        }
        m_blocks.append(entry);
    }

    updateGeometry();
}

QLabel* SpellMarkdownView::createProseLabel() {
    auto* label = new QLabel(this);
    label->setObjectName(QStringLiteral("SpellMarkdownProseLabel"));
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(false);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    label->setMinimumWidth(1);
    label->setStyleSheet(QStringLiteral(
        "QLabel#SpellMarkdownProseLabel { background-color: transparent; border: none; padding: 0px; }"));
    connect(label, &QLabel::linkActivated, this, &SpellMarkdownView::handleLinkActivated);
    return label;
}

void SpellMarkdownView::updateProseLabel(QLabel* label, const QString& prose) {
    if (!label) {
        return;
    }
    const auto& tm = ThemeManager::instance();
    const UiPalette ui = tm.ui();
    const CodePalette code = tm.code();
    const QString monoFamily = tm.editorFontFamily();

    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, ui.textPrimary);
    pal.setColor(QPalette::Text, ui.textPrimary);
    pal.setColor(QPalette::Link, ui.accent);
    pal.setColor(QPalette::LinkVisited, ui.textSecondary);
    label->setPalette(pal);

    label->setText(renderProseToHtml(prose, m_projectRoots, ui, code, monoFamily));
    label->updateGeometry();
}

} // namespace spell
