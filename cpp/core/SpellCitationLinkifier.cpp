#include "core/SpellCitationLinkifier.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringView>
#include <QUrlQuery>
#include <QtGlobal>

#include <algorithm>
#include <utility>

namespace spell {
namespace {

QString normalizeSeparators(const QString& rawPath) {
    QString normalized = rawPath;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return normalized;
}

bool isWindowsDrivePath(const QString& path) {
    return path.size() >= 3 && path.at(0).isLetter() && path.at(1) == QLatin1Char(':')
           && (path.at(2) == QLatin1Char('/') || path.at(2) == QLatin1Char('\\'));
}

bool isResearchCachePath(const QString& normalizedPath) {
    if (normalizedPath.endsWith(QLatin1Char('/'))) {
        return false;
    }
    return normalizedPath.startsWith(QLatin1String(".spell/research_cache/"))
           || normalizedPath.contains(QLatin1String("/.spell/research_cache/"));
}

bool isKnownFileExtension(const QString& extLower) {
    static const QSet<QString> kExtensions = {
        // C / C++ / Objective-C
        QStringLiteral("c"), QStringLiteral("cc"), QStringLiteral("cpp"), QStringLiteral("cxx"),
        QStringLiteral("h"), QStringLiteral("hh"), QStringLiteral("hpp"), QStringLiteral("hxx"),
        QStringLiteral("inl"), QStringLiteral("ipp"), QStringLiteral("tpp"),
        QStringLiteral("m"), QStringLiteral("mm"), QStringLiteral("pch"), QStringLiteral("def"),
        QStringLiteral("rc"),
        // Systems / compiled
        QStringLiteral("rs"), QStringLiteral("swift"), QStringLiteral("go"), QStringLiteral("mod"),
        QStringLiteral("sum"), QStringLiteral("zig"), QStringLiteral("nim"), QStringLiteral("d"),
        // Scripting
        QStringLiteral("py"), QStringLiteral("pyi"), QStringLiteral("pyw"), QStringLiteral("pyx"),
        QStringLiteral("pxd"), QStringLiteral("rb"), QStringLiteral("rake"), QStringLiteral("gemspec"),
        QStringLiteral("pl"), QStringLiteral("pm"), QStringLiteral("php"), QStringLiteral("lua"),
        // JS / TS / Web
        QStringLiteral("js"), QStringLiteral("jsx"), QStringLiteral("mjs"), QStringLiteral("cjs"),
        QStringLiteral("ts"), QStringLiteral("tsx"), QStringLiteral("mts"), QStringLiteral("cts"),
        QStringLiteral("vue"), QStringLiteral("svelte"), QStringLiteral("astro"),
        QStringLiteral("html"), QStringLiteral("htm"), QStringLiteral("xhtml"),
        QStringLiteral("css"), QStringLiteral("scss"), QStringLiteral("sass"), QStringLiteral("less"),
        QStringLiteral("styl"), QStringLiteral("wasm"),
        // JVM / .NET / Mobile
        QStringLiteral("java"), QStringLiteral("kt"), QStringLiteral("kts"), QStringLiteral("scala"),
        QStringLiteral("groovy"), QStringLiteral("gradle"), QStringLiteral("clj"), QStringLiteral("cljs"),
        QStringLiteral("cljc"), QStringLiteral("cs"), QStringLiteral("fs"), QStringLiteral("fsx"),
        QStringLiteral("vb"), QStringLiteral("dart"), QStringLiteral("csproj"), QStringLiteral("sln"),
        QStringLiteral("props"), QStringLiteral("targets"), QStringLiteral("plist"),
        QStringLiteral("xcconfig"), QStringLiteral("pbxproj"), QStringLiteral("entitlements"),
        QStringLiteral("storyboard"), QStringLiteral("xib"),
        // Functional / Scientific
        QStringLiteral("hs"), QStringLiteral("lhs"), QStringLiteral("ml"), QStringLiteral("mli"),
        QStringLiteral("ex"), QStringLiteral("exs"), QStringLiteral("erl"), QStringLiteral("hrl"),
        QStringLiteral("elm"), QStringLiteral("rkt"), QStringLiteral("scm"), QStringLiteral("lisp"),
        QStringLiteral("el"), QStringLiteral("jl"), QStringLiteral("r"), QStringLiteral("rmd"),
        QStringLiteral("f"), QStringLiteral("f90"), QStringLiteral("f95"), QStringLiteral("for"),
        QStringLiteral("tex"), QStringLiteral("bib"), QStringLiteral("sty"), QStringLiteral("cls"),
        // Shell
        QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("fish"),
        QStringLiteral("ksh"), QStringLiteral("csh"), QStringLiteral("ps1"), QStringLiteral("psm1"),
        QStringLiteral("psd1"), QStringLiteral("bat"), QStringLiteral("cmd"), QStringLiteral("awk"),
        QStringLiteral("sed"),
        // Build / Config / Data / Schema
        QStringLiteral("cmake"), QStringLiteral("mk"), QStringLiteral("mak"), QStringLiteral("make"),
        QStringLiteral("ninja"), QStringLiteral("meson"), QStringLiteral("bazel"), QStringLiteral("bzl"),
        QStringLiteral("star"), QStringLiteral("nix"), QStringLiteral("tf"), QStringLiteral("tfvars"),
        QStringLiteral("hcl"), QStringLiteral("dockerfile"),
        QStringLiteral("json"), QStringLiteral("jsonc"), QStringLiteral("jsonl"), QStringLiteral("ndjson"),
        QStringLiteral("yaml"), QStringLiteral("yml"), QStringLiteral("toml"),
        QStringLiteral("xml"), QStringLiteral("xsd"), QStringLiteral("xsl"), QStringLiteral("xslt"),
        QStringLiteral("ini"), QStringLiteral("cfg"), QStringLiteral("conf"), QStringLiteral("config"),
        QStringLiteral("env"), QStringLiteral("properties"), QStringLiteral("lock"),
        QStringLiteral("proto"), QStringLiteral("thrift"), QStringLiteral("avsc"),
        QStringLiteral("graphql"), QStringLiteral("gql"), QStringLiteral("sql"), QStringLiteral("ddl"),
        QStringLiteral("csv"), QStringLiteral("tsv"), QStringLiteral("parquet"),
        // Qt
        QStringLiteral("qss"), QStringLiteral("ui"), QStringLiteral("qrc"), QStringLiteral("pro"),
        QStringLiteral("pri"), QStringLiteral("qml"),
        // Docs / Text
        QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("mdx"), QStringLiteral("rst"),
        QStringLiteral("adoc"), QStringLiteral("asciidoc"), QStringLiteral("txt"), QStringLiteral("text"),
        QStringLiteral("log"), QStringLiteral("diff"), QStringLiteral("patch"), QStringLiteral("org"),
        // Shaders / GPU / Hardware / Assembly
        QStringLiteral("glsl"), QStringLiteral("vert"), QStringLiteral("frag"), QStringLiteral("geom"),
        QStringLiteral("comp"), QStringLiteral("tesc"), QStringLiteral("tese"), QStringLiteral("hlsl"),
        QStringLiteral("wgsl"), QStringLiteral("metal"), QStringLiteral("cu"), QStringLiteral("cuh"),
        QStringLiteral("cl"), QStringLiteral("s"), QStringLiteral("asm"), QStringLiteral("nasm"),
        QStringLiteral("v"), QStringLiteral("sv"), QStringLiteral("vh"), QStringLiteral("svh"),
        QStringLiteral("vhd"), QStringLiteral("vhdl"),
        // Assets
        QStringLiteral("svg"), QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("bmp"), QStringLiteral("ico"),
        QStringLiteral("icns"), QStringLiteral("pdf"), QStringLiteral("ttf"), QStringLiteral("otf"),
        QStringLiteral("woff"), QStringLiteral("woff2")
    };
    return kExtensions.contains(extLower);
}

bool isKnownExtensionlessFile(const QString& baseName) {
    static const QSet<QString> kFiles = {
        QStringLiteral("GNUmakefile"),
        QStringLiteral("Makefile"),
        QStringLiteral("Dockerfile"),
        QStringLiteral("Containerfile"),
        QStringLiteral("Justfile"),
        QStringLiteral("Procfile"),
        QStringLiteral("Gemfile"),
        QStringLiteral("Rakefile"),
        QStringLiteral("Brewfile"),
        QStringLiteral("Podfile"),
        QStringLiteral("Vagrantfile"),
        QStringLiteral("Tiltfile")
    };
    return kFiles.contains(baseName);
}

QString resolveCitationPath(const QString& rawPath,
                            const QStringList& projectRoots,
                            bool* outResolvedOnDisk) {
    if (outResolvedOnDisk != nullptr) {
        *outResolvedOnDisk = false;
    }

    const QString normalized = normalizeSeparators(rawPath);

    // Home-relative path (`~/...`).
    if (normalized.startsWith(QLatin1String("~/"))) {
        const QString expanded = QDir::cleanPath(QDir::homePath() + normalized.mid(1));
        const QFileInfo info(expanded);
        if (outResolvedOnDisk != nullptr) {
            *outResolvedOnDisk = info.exists() && !info.isDir();
        }
        return expanded;
    }

    // Already absolute (POSIX `/...` or Windows `C:/...`).
    if (normalized.startsWith(QLatin1Char('/')) || isWindowsDrivePath(normalized)) {
        const QString cleaned = QDir::cleanPath(normalized);
        const QFileInfo info(cleaned);
        if (outResolvedOnDisk != nullptr) {
            *outResolvedOnDisk = info.exists() && !info.isDir();
        }
        return cleaned;
    }

    // Relative path: normalize and try each workspace root if the file exists on disk.
    const QString cleanedRel = QDir::cleanPath(normalized);
    for (const QString& rawRoot : projectRoots) {
        const QString root = rawRoot.trimmed();
        if (root.isEmpty()) {
            continue;
        }
        const QString rootNorm = QDir::cleanPath(normalizeSeparators(root));
        const QString candidate = QDir::cleanPath(QDir(rootNorm).filePath(cleanedRel));
        const QFileInfo info(candidate);
        if (info.exists() && !info.isDir()) {
            if (outResolvedOnDisk != nullptr) {
                *outResolvedOnDisk = true;
            }
            return QDir::cleanPath(info.absoluteFilePath());
        }
    }

    // Research cache paths may also be staged under the user's home directory.
    if (isResearchCachePath(cleanedRel)) {
        const QString homeCandidate = QDir::cleanPath(QDir::home().filePath(cleanedRel));
        const QFileInfo homeInfo(homeCandidate);
        if (homeInfo.exists() && !homeInfo.isDir()) {
            if (outResolvedOnDisk != nullptr) {
                *outResolvedOnDisk = true;
            }
            return QDir::cleanPath(homeInfo.absoluteFilePath());
        }
    }

    return cleanedRel;
}

bool isInsideUrl(const QString& text, qsizetype matchStart) {
    qsizetype tokenStart = matchStart;
    while (tokenStart > 0) {
        const QChar ch = text.at(tokenStart - 1);
        if (ch.isSpace() || ch == QLatin1Char('<') || ch == QLatin1Char('>')
            || ch == QLatin1Char('"') || ch == QLatin1Char('\'')
            || ch == QLatin1Char('`') || ch == QLatin1Char('(')
            || ch == QLatin1Char(')') || ch == QLatin1Char('[')
            || ch == QLatin1Char(']') || ch == QLatin1Char('{')
            || ch == QLatin1Char('}') || ch == QLatin1Char(',')) {
            break;
        }
        --tokenStart;
    }

    const QStringView prefix = QStringView(text).mid(tokenStart, matchStart - tokenStart);
    if (prefix.isEmpty()) {
        return false;
    }

    // Advance past HTML entity quote/angle delimiters so `&quot;src/foo.cpp&quot;` is not
    // misclassified while `&quot;https://example.com/a.html&quot;` still is.
    qsizetype effectiveStart = 0;
    static constexpr QStringView kBoundaryEntities[] = {
        u"&quot;", u"&apos;", u"&lt;", u"&gt;", u"&#39;", u"&#x27;", u"&#34;"
    };
    for (const QStringView entity : kBoundaryEntities) {
        const qsizetype idx = prefix.lastIndexOf(entity);
        if (idx >= 0) {
            effectiveStart = std::max(effectiveStart, idx + entity.size());
        }
    }

    const QStringView activePrefix = prefix.mid(effectiveStart);
    if (activePrefix.isEmpty()) {
        return false;
    }

    if (activePrefix.contains(u"://")) {
        return true;
    }

    static const QRegularExpression kSchemeColonSlash(
        QStringLiteral(R"((?:^|[^A-Za-z0-9+.\-])[A-Za-z][A-Za-z0-9+.\-]{1,23}:/)"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (kSchemeColonSlash.matchView(activePrefix).hasMatch()) {
#else
    if (kSchemeColonSlash.match(activePrefix).hasMatch()) {
#endif
        return true;
    }

    if (activePrefix.endsWith(QLatin1Char(':'))) {
        static const QRegularExpression kOpaqueScheme(
            QStringLiteral(R"((?:^|[^A-Za-z0-9+.\-])(?:mailto|data|javascript|vbscript|about|tel|urn|blob|file|spellfile):$)"),
            QRegularExpression::CaseInsensitiveOption);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (kOpaqueScheme.matchView(activePrefix).hasMatch()) {
#else
        if (kOpaqueScheme.match(activePrefix).hasMatch()) {
#endif
            return true;
        }
    }

    return false;
}

const QRegularExpression& citationRegex() {
    // Capture groups:
    //   1: optional opening backtick
    //   2: file path (research_cache path, path with extension, or well-known extensionless file)
    //   3: optional closing backtick immediately after path
    //   4: start line from `:line` / `:Lline` / `#Lline` / `#line`
    //   5: optional end line from `:start-end` / `#Lstart-Lend`
    //   6: start line from `(L12)` / `(L12-L30)` / `(line 12)` / `(lines 12-30)`
    //   7: optional end line from parenthesized range
    //   8: optional closing backtick after line suffix
    static const QRegularExpression kRegex(QStringLiteral(
        "(?<![\\w/\\\\@.-])"
        "(`?)"
        "("
            "(?:(?:~|[A-Za-z]:)?[\\\\/](?:[A-Za-z0-9_.+\\-]+[\\\\/])*|(?:[A-Za-z0-9_.+\\-]+[\\\\/])+)?"
            "\\.(?:spell|beatrix)[\\\\/]research_cache[\\\\/]"
            "(?:[A-Za-z0-9_.+\\-%~]+[\\\\/])*[A-Za-z0-9_.+\\-%~]*[A-Za-z0-9_+\\-%~]"
            "(?![A-Za-z0-9_/\\\\]|\\.[A-Za-z0-9_/\\\\])"
        "|"
            "(?:(?:~|[A-Za-z]:)?[\\\\/](?:[A-Za-z0-9_.+\\-]+[\\\\/])*|(?:[A-Za-z0-9_.+\\-]+[\\\\/])+)?"
            "[A-Za-z0-9_][A-Za-z0-9_.+\\-]*\\.[A-Za-z][A-Za-z0-9]{0,11}"
            "(?![A-Za-z0-9_/\\\\]|\\.[A-Za-z0-9_/\\\\])"
        "|"
            "(?:(?:~|[A-Za-z]:)?[\\\\/](?:[A-Za-z0-9_.+\\-]+[\\\\/])*|(?:[A-Za-z0-9_.+\\-]+[\\\\/])+)?"
            "(?:GNUmakefile|Makefile|Dockerfile|Containerfile|Justfile|Procfile|"
            "Gemfile|Rakefile|Brewfile|Podfile|Vagrantfile|Tiltfile)"
            "(?![A-Za-z0-9_/\\\\]|\\.[A-Za-z0-9_/\\\\])"
        ")"
        "(`?)"
        "(?:"
            "(?::|#)[Ll]?(\\d{1,6})(?!\\d)(?:\\s*[-\\x{2013}]\\s*[Ll]?(\\d{1,6})(?!\\d)|:\\d{1,5}(?!\\d))?"
        "|"
            "\\s*\\(\\s*(?:[Ll]|[Ll][Ii][Nn][Ee][Ss]?\\s+)(\\d{1,6})(?!\\d)"
            "(?:\\s*[-\\x{2013}]\\s*[Ll]?(\\d{1,6})(?!\\d))?\\s*\\)"
        ")?"
        "(`?)"));
    return kRegex;
}

int parsePositiveLine(const QString& digits) {
    if (digits.isEmpty()) {
        return 0;
    }
    bool ok = false;
    const int value = digits.toInt(&ok);
    return (ok && value > 0) ? value : 0;
}

QVector<FileCitation> findCitationsImpl(const QString& text,
                                        const QStringList& projectRoots,
                                        bool allowBareFilenames) {
    QVector<FileCitation> results;
    if (text.isEmpty()) {
        return results;
    }

    const QRegularExpression& re = citationRegex();
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();

        const QString openTick = match.captured(1);
        const QString rawPath = match.captured(2);
        const QString midTick = match.captured(3);
        const QString colonHashStart = match.captured(4);
        const QString colonHashEnd = match.captured(5);
        const QString parenStart = match.captured(6);
        const QString parenEnd = match.captured(7);
        const QString endTick = match.captured(8);

        if (rawPath.isEmpty()) {
            continue;
        }

        bool hasSuffix = !colonHashStart.isEmpty() || !parenStart.isEmpty();
        int startLine = 0;
        int endLine = 0;
        if (!colonHashStart.isEmpty()) {
            startLine = parsePositiveLine(colonHashStart);
            endLine = (startLine > 0) ? parsePositiveLine(colonHashEnd) : 0;
        } else if (!parenStart.isEmpty()) {
            startLine = parsePositiveLine(parenStart);
            endLine = (startLine > 0) ? parsePositiveLine(parenEnd) : 0;
        }

        // Reconcile surrounding backticks so emitted spans are always cleanly balanced.
        const bool hasOpen = !openTick.isEmpty();
        const bool hasMid = !midTick.isEmpty();
        const bool hasEnd = !endTick.isEmpty();

        bool wrappedInBackticks = false;
        qsizetype spanStart = match.capturedStart(2);
        qsizetype spanEnd = match.capturedEnd(0);

        if (hasOpen && hasMid && !hasSuffix) {
            // Strictly wrapped `` `path` ``: keep inner span without literal backticks.
            wrappedInBackticks = true;
            spanStart = match.capturedStart(2);
            spanEnd = match.capturedEnd(2);
        } else if (hasOpen && !hasMid && hasEnd && hasSuffix) {
            // Strictly wrapped `` `path:42` ``: keep inner span without literal backticks.
            wrappedInBackticks = true;
            spanStart = match.capturedStart(2);
            spanEnd = match.capturedEnd(0) - 1;
        } else if (hasOpen && hasMid && hasSuffix) {
            // Wrapped path with trailing suffix (e.g. `` `src/foo.cpp` (L12-L30) ``):
            // preserve both balanced backticks inside the citation span.
            wrappedInBackticks = true;
            spanStart = match.capturedStart(0);
            spanEnd = hasEnd ? (match.capturedEnd(0) - 1) : match.capturedEnd(0);
        } else if (hasOpen && !hasMid && !hasEnd) {
            // Unclosed leading backtick: exclude it and do not grant bare-filename privilege.
            spanStart = match.capturedStart(2);
            spanEnd = match.capturedEnd(0);
        } else {
            // No opening backtick: never consume stray trailing backticks.
            if (hasMid) {
                spanStart = match.capturedStart(2);
                spanEnd = match.capturedEnd(2);
                if (hasSuffix) {
                    hasSuffix = false;
                    startLine = 0;
                    endLine = 0;
                }
            } else if (hasEnd) {
                spanStart = match.capturedStart(2);
                spanEnd = match.capturedEnd(0) - 1;
            } else {
                spanStart = match.capturedStart(2);
                spanEnd = match.capturedEnd(0);
            }
        }

        if (spanStart < 0 || spanEnd <= spanStart || spanEnd > text.size()) {
            continue;
        }

        // Skip matches embedded inside URLs (`https://...`, `file://...`, `spellfile://...`).
        if (isInsideUrl(text, spanStart)) {
            continue;
        }

        // Skip matches already inside markdown links (`[label](path)` or `[path](url)`).
        const QStringView before = QStringView(text).left(spanStart);
        const QStringView after = QStringView(text).mid(spanEnd);
        const QStringView rawBefore = QStringView(text).left(match.capturedStart(0));
        const QStringView rawAfter = QStringView(text).mid(match.capturedEnd(0));
        if (before.endsWith(u"](") || rawBefore.endsWith(u"](")) {
            continue;
        }
        if ((before.endsWith(u"[") && after.startsWith(u"]("))
            || (rawBefore.endsWith(u"[") && rawAfter.startsWith(u"]("))) {
            continue;
        }

        const QString normRaw = normalizeSeparators(rawPath);

        // Reject ellipsis artifacts (`...`), malformed leading double-dots, and UNC/comment `//`.
        if (normRaw.contains(QLatin1String("..."))
            || (normRaw.startsWith(QLatin1String("..")) && !normRaw.startsWith(QLatin1String("../")))
            || normRaw.startsWith(QLatin1String("//"))) {
            continue;
        }

        const bool researchCache = isResearchCachePath(normRaw);
        const bool hasSlash = normRaw.contains(QLatin1Char('/'));
        const bool isCodeContext = wrappedInBackticks || allowBareFilenames;

        // Avoid turning function calls like `obj.method()` into bare-filename citations.
        if (!hasSlash && !researchCache && !hasSuffix && after.startsWith(u"(")) {
            continue;
        }

        const qsizetype lastSlash = normRaw.lastIndexOf(QLatin1Char('/'));
        const QString baseName = (lastSlash >= 0) ? normRaw.mid(lastSlash + 1) : normRaw;
        const qsizetype lastDot = baseName.lastIndexOf(QLatin1Char('.'));
        const QString extLower = (lastDot > 0 && lastDot + 1 < baseName.size())
                                     ? baseName.mid(lastDot + 1).toLower()
                                     : QString();
        const bool knownExt = !extLower.isEmpty() && isKnownFileExtension(extLower);
        const bool knownExtensionless = extLower.isEmpty() && isKnownExtensionlessFile(baseName);

        bool resolvedOnDisk = false;
        const QString resolvedPath = resolveCitationPath(rawPath, projectRoots, &resolvedOnDisk);

        bool accepted = false;
        if (researchCache) {
            accepted = true;
        } else if (hasSlash) {
            accepted = true;
        } else {
            // Bare filename without directory separator: accept when verified on disk,
            // or when it has a recognized file extension/name in code context or with a line suffix.
            if (resolvedOnDisk) {
                accepted = true;
            } else if ((isCodeContext || hasSuffix) && (knownExt || knownExtensionless)) {
                accepted = true;
            }
        }

        if (!accepted) {
            continue;
        }

        FileCitation citation;
        citation.start = static_cast<int>(spanStart);
        citation.length = static_cast<int>(spanEnd - spanStart);
        citation.rawText = text.mid(spanStart, spanEnd - spanStart);
        citation.filePath = resolvedPath;
        citation.startLine = startLine;
        citation.endLine = endLine;
        results.push_back(std::move(citation));
    }

    return results;
}

QString buildSpellFileUrl(const QString& filePath, int startLine, int endLine) {
    QUrl url;
    url.setScheme(QString::fromLatin1(kSpellFileUrlScheme));
    // Explicit empty authority ensures `spellfile://` serialization.
    url.setAuthority(QStringLiteral(""));

    QString urlPath = normalizeSeparators(filePath);
    if (urlPath.startsWith(QLatin1Char('/'))) {
        // Absolute POSIX path -> `spellfile:///abs/path`.
    } else if (isWindowsDrivePath(urlPath)) {
        // Windows drive path (RFC 8089) -> `spellfile:///C:/abs/path`.
        urlPath.prepend(QLatin1Char('/'));
    } else {
        // Relative path -> encode with `/./` marker so the URI stays a valid
        // `spellfile:///...` URL and `parseSpellFileUrl` recovers the exact relative path.
        while (urlPath.startsWith(QLatin1String("./"))) {
            urlPath.remove(0, 2);
        }
        urlPath.prepend(QLatin1String("/./"));
    }

    url.setPath(urlPath, QUrl::DecodedMode);

    if (startLine > 0) {
        if (endLine > 0 && endLine != startLine) {
            url.setFragment(QStringLiteral("L%1-L%2").arg(startLine).arg(endLine));
        } else {
            url.setFragment(QStringLiteral("L%1").arg(startLine));
        }
    }

    return QString::fromLatin1(url.toEncoded());
}

QString linkifyTextSegment(const QString& segment,
                           const QStringList& projectRoots,
                           bool allowBareFilenames) {
    const QVector<FileCitation> citations =
        findCitationsImpl(segment, projectRoots, allowBareFilenames);
    if (citations.isEmpty()) {
        return segment;
    }

    QString out;
    out.reserve(segment.size() + citations.size() * 48);

    qsizetype cursor = 0;
    for (const FileCitation& citation : citations) {
        const qsizetype start = citation.start;
        const qsizetype end = start + citation.length;
        if (start < cursor || end > segment.size()) {
            continue;
        }
        out.append(QStringView(segment).mid(cursor, start - cursor));

        const QString href =
            buildSpellFileUrl(citation.filePath, citation.startLine, citation.endLine).toHtmlEscaped();
        out.append(QLatin1String("<a href=\""));
        out.append(href);
        out.append(QLatin1String("\">"));
        out.append(citation.rawText);
        out.append(QLatin1String("</a>"));

        cursor = end;
    }
    out.append(QStringView(segment).mid(cursor));
    return out;
}

struct ParsedHtmlTag {
    QString nameLower;
    bool isClosing = false;
    bool isSelfClosing = false;
};

ParsedHtmlTag parseHtmlTagHeader(QStringView tagView) {
    ParsedHtmlTag info;
    if (tagView.size() < 2 || tagView.first() != QLatin1Char('<')
        || tagView.last() != QLatin1Char('>')) {
        return info;
    }

    QStringView inner = tagView.mid(1, tagView.size() - 2).trimmed();
    if (inner.isEmpty()) {
        return info;
    }

    if (inner.startsWith(QLatin1Char('/'))) {
        info.isClosing = true;
        inner = inner.mid(1).trimmed();
    } else if (inner.endsWith(QLatin1Char('/'))) {
        info.isSelfClosing = true;
    }

    qsizetype nameLen = 0;
    while (nameLen < inner.size()) {
        const QChar ch = inner.at(nameLen);
        if ((ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
            || (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (nameLen > 0 && ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))) {
            ++nameLen;
        } else {
            break;
        }
    }

    if (nameLen > 0) {
        info.nameLower = inner.left(nameLen).toString().toLower();
    }
    return info;
}

} // namespace

QVector<FileCitation> findCitations(const QString& text, const QStringList& projectRoots) {
    return findCitationsImpl(text, projectRoots, /*allowBareFilenames=*/false);
}

QString linkifyHtml(const QString& escapedHtml, const QStringList& projectRoots) {
    if (escapedHtml.isEmpty()) {
        return escapedHtml;
    }

    QString out;
    out.reserve(escapedHtml.size() + 64);

    int anchorDepth = 0;
    int preDepth = 0;
    int codeDepth = 0;
    int scriptDepth = 0;
    int styleDepth = 0;

    qsizetype i = 0;
    const qsizetype n = escapedHtml.size();
    while (i < n) {
        if (escapedHtml.at(i) == QLatin1Char('<')) {
            // Preserve HTML comments `<!-- ... -->` verbatim.
            if (QStringView(escapedHtml).mid(i).startsWith(u"<!--")) {
                const qsizetype commentEnd = escapedHtml.indexOf(QLatin1String("-->"), i + 4);
                if (commentEnd < 0) {
                    out.append(QStringView(escapedHtml).mid(i));
                    break;
                }
                out.append(QStringView(escapedHtml).mid(i, (commentEnd + 3) - i));
                i = commentEnd + 3;
                continue;
            }

            // Preserve CDATA `<![CDATA[ ... ]]>` verbatim.
            if (QStringView(escapedHtml).mid(i).startsWith(u"<![CDATA[")) {
                const qsizetype cdataEnd = escapedHtml.indexOf(QLatin1String("]]>"), i + 9);
                if (cdataEnd < 0) {
                    out.append(QStringView(escapedHtml).mid(i));
                    break;
                }
                out.append(QStringView(escapedHtml).mid(i, (cdataEnd + 3) - i));
                i = cdataEnd + 3;
                continue;
            }

            // Scan tag respecting quoted attribute values so `>` inside attributes is preserved.
            qsizetype j = i + 1;
            QChar quote;
            while (j < n) {
                const QChar ch = escapedHtml.at(j);
                if (!quote.isNull()) {
                    if (ch == quote) {
                        quote = QChar();
                    }
                } else if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
                    quote = ch;
                } else if (ch == QLatin1Char('>')) {
                    break;
                }
                ++j;
            }

            if (j >= n) {
                // Unterminated `<`: copy remainder verbatim.
                out.append(QStringView(escapedHtml).mid(i));
                break;
            }

            const QStringView tagView = QStringView(escapedHtml).mid(i, (j - i) + 1);
            out.append(tagView);
            i = j + 1;

            const ParsedHtmlTag tag = parseHtmlTagHeader(tagView);
            if (!tag.nameLower.isEmpty() && !tag.isSelfClosing) {
                auto updateDepth = [&](int& depth) {
                    if (tag.isClosing) {
                        depth = std::max(0, depth - 1);
                    } else {
                        ++depth;
                    }
                };
                if (tag.nameLower == QLatin1String("a")) {
                    updateDepth(anchorDepth);
                } else if (tag.nameLower == QLatin1String("pre")) {
                    updateDepth(preDepth);
                } else if (tag.nameLower == QLatin1String("code")) {
                    updateDepth(codeDepth);
                } else if (tag.nameLower == QLatin1String("script")) {
                    updateDepth(scriptDepth);
                } else if (tag.nameLower == QLatin1String("style")) {
                    updateDepth(styleDepth);
                }
            }
            continue;
        }

        // Plain/escaped text segment up to the next HTML tag.
        qsizetype nextTag = escapedHtml.indexOf(QLatin1Char('<'), i);
        if (nextTag < 0) {
            nextTag = n;
        }
        const QString segment = escapedHtml.mid(i, nextTag - i);
        i = nextTag;

        if (anchorDepth > 0 || preDepth > 0 || scriptDepth > 0 || styleDepth > 0) {
            out.append(segment);
        } else {
            out.append(linkifyTextSegment(segment, projectRoots, /*allowBareFilenames=*/codeDepth > 0));
        }
    }

    return out;
}

bool parseSpellFileUrl(const QUrl& url, QString* outPath, int* outLine) {
    if (!url.isValid()) {
        return false;
    }
    if (url.scheme().compare(QLatin1String(kSpellFileUrlScheme), Qt::CaseInsensitive) != 0) {
        return false;
    }

    const QUrlQuery query(url);
    const QString host = url.host(QUrl::FullyDecoded);
    const QString rawPath = url.path(QUrl::FullyDecoded);

    QString path;
    if ((rawPath.isEmpty() || rawPath == QLatin1String("/"))
        && query.hasQueryItem(QStringLiteral("path"))) {
        path = query.queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded);
    } else if (!host.isEmpty()
               && host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) != 0) {
        path = host + rawPath;
    } else {
        path = rawPath;
    }

    // Restore relative paths encoded with our `/./` prefix.
    if (path.startsWith(QLatin1String("/./"))) {
        path.remove(0, 3);
    } else if (path.size() >= 3 && path.at(0) == QLatin1Char('/') && path.at(1).isLetter()
               && path.at(2) == QLatin1Char(':')
               && (path.size() == 3 || path.at(3) == QLatin1Char('/'))) {
        // Restore Windows drive paths (`/C:/foo/bar.cpp` -> `C:/foo/bar.cpp`).
        path.remove(0, 1);
    }

    if (path.isEmpty() || path == QLatin1String("/")) {
        return false;
    }

    int line = 0;
    const QString fragment = url.fragment(QUrl::FullyDecoded).trimmed();
    if (!fragment.isEmpty()) {
        static const QRegularExpression kLineFragmentRe(
            QStringLiteral("^[Ll]?(\\d{1,6})(?:\\s*[-\\x{2013}]\\s*[Ll]?\\d{1,6}|:\\d{1,5}|[Cc]\\d{1,5})?$"));
        const QRegularExpressionMatch m = kLineFragmentRe.match(fragment);
        if (m.hasMatch()) {
            line = parsePositiveLine(m.captured(1));
        }
    }

    if (line == 0 && query.hasQueryItem(QStringLiteral("line"))) {
        line = parsePositiveLine(query.queryItemValue(QStringLiteral("line")).trimmed());
    }

    if (outPath != nullptr) {
        *outPath = path;
    }
    if (outLine != nullptr) {
        *outLine = line;
    }
    return true;
}

} // namespace spell
