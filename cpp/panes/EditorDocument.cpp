#include "EditorDocument.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QSaveFile>
#include <QtGlobal>

#include <algorithm>

namespace spell {

namespace {

/// Files larger than 4 MiB are not loaded into the text editor to keep editing responsive.
constexpr qint64 kMaxEditableBytes = 4LL * 1024LL * 1024LL;

/// Maximum file size for same-timestamp content probing in `reloadIfChangedOnDisk()`.
constexpr qint64 kContentProbeThresholdBytes = 512LL * 1024LL;

/// Window (in ms) around current time where a rapid edit could share the same mtime tick.
constexpr qint64 kRecentMtimeWindowMs = 2000;

enum class Utf8CheckResult {
    Valid,
    ContainsNul,
    InvalidUtf8
};

/**
 * Validates that `data` contains no NUL bytes and is strictly valid RFC 3629 UTF-8.
 * Rejects binary data, overlong encodings, UTF-16 surrogate code points (U+D800..U+DFFF),
 * code points above U+10FFFF, and truncated or stray continuation bytes.
 */
Utf8CheckResult validateUtf8Text(const QByteArray& data) {
    if (data.contains('\0')) {
        return Utf8CheckResult::ContainsNul;
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(data.constData());
    const qsizetype len = data.size();
    qsizetype i = 0;

    while (i < len) {
        const unsigned char b0 = bytes[i];
        if (b0 <= 0x7F) {
            ++i;
            continue;
        }
        if (b0 >= 0xC2 && b0 <= 0xDF) {
            if (i + 1 >= len) {
                return Utf8CheckResult::InvalidUtf8;
            }
            const unsigned char b1 = bytes[i + 1];
            if ((b1 & 0xC0) != 0x80) {
                return Utf8CheckResult::InvalidUtf8;
            }
            i += 2;
            continue;
        }
        if (b0 >= 0xE0 && b0 <= 0xEF) {
            if (i + 2 >= len) {
                return Utf8CheckResult::InvalidUtf8;
            }
            const unsigned char b1 = bytes[i + 1];
            const unsigned char b2 = bytes[i + 2];
            if ((b2 & 0xC0) != 0x80) {
                return Utf8CheckResult::InvalidUtf8;
            }
            if (b0 == 0xE0) {
                if (b1 < 0xA0 || b1 > 0xBF) {
                    return Utf8CheckResult::InvalidUtf8;
                }
            } else if (b0 == 0xED) {
                if (b1 < 0x80 || b1 > 0x9F) {
                    return Utf8CheckResult::InvalidUtf8;
                }
            } else {
                if ((b1 & 0xC0) != 0x80) {
                    return Utf8CheckResult::InvalidUtf8;
                }
            }
            i += 3;
            continue;
        }
        if (b0 >= 0xF0 && b0 <= 0xF4) {
            if (i + 3 >= len) {
                return Utf8CheckResult::InvalidUtf8;
            }
            const unsigned char b1 = bytes[i + 1];
            const unsigned char b2 = bytes[i + 2];
            const unsigned char b3 = bytes[i + 3];
            if ((b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
                return Utf8CheckResult::InvalidUtf8;
            }
            if (b0 == 0xF0) {
                if (b1 < 0x90 || b1 > 0xBF) {
                    return Utf8CheckResult::InvalidUtf8;
                }
            } else if (b0 == 0xF4) {
                if (b1 < 0x80 || b1 > 0x8F) {
                    return Utf8CheckResult::InvalidUtf8;
                }
            } else {
                if ((b1 & 0xC0) != 0x80) {
                    return Utf8CheckResult::InvalidUtf8;
                }
            }
            i += 4;
            continue;
        }
        return Utf8CheckResult::InvalidUtf8;
    }

    return Utf8CheckResult::Valid;
}

QString formatByteSize(qint64 bytes) {
    return QLocale::c().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString resolveSaveTargetPath(const QString& filePath) {
    QFileInfo info(filePath);
    if (info.isSymLink()) {
        const QString canonical = info.canonicalFilePath();
        if (!canonical.isEmpty()) {
            return canonical;
        }
    }
    return filePath;
}

} // namespace

struct EditorDocument::Impl {
    QString id;
    QString filePath;
    QString fileName;
    QString language;
    QString text;
    QString savedText;
    QString unavailableReason;
    QString lastError;
    QDateTime savedMtime;
    qint64 savedFileSize = -1;
    int cursorLine = 0;
    int cursorCol = 0;
    bool dirty = false;
    bool explicitReadOnly = false;
    bool osReadOnly = false;
    bool hasLoadedOnce = false;

    bool effectiveReadOnly() const {
        return explicitReadOnly || osReadOnly || !unavailableReason.isEmpty();
    }
};

EditorDocument::EditorDocument(const QString& filePath,
                               bool readOnly,
                               QObject* parent)
    : QObject(parent),
      m_impl(std::make_unique<Impl>()) {
    m_impl->explicitReadOnly = readOnly;

    if (!filePath.isEmpty()) {
        QFileInfo info(filePath);
        const QString cleanAbs = QDir::cleanPath(info.absoluteFilePath());
        const QString canonical = info.canonicalFilePath();
        m_impl->filePath = cleanAbs;
        m_impl->id = canonical.isEmpty() ? cleanAbs : canonical;
        m_impl->fileName = QFileInfo(cleanAbs).fileName();
        m_impl->language = detectLanguage(cleanAbs);
    } else {
        m_impl->language = QStringLiteral("plain");
    }

    loadFromDisk();
}

EditorDocument::~EditorDocument() = default;

QString EditorDocument::id() const {
    return m_impl->id;
}

QString EditorDocument::filePath() const {
    return m_impl->filePath;
}

QString EditorDocument::fileName() const {
    return m_impl->fileName;
}

QString EditorDocument::language() const {
    return m_impl->language;
}

QString EditorDocument::text() const {
    return m_impl->text;
}

bool EditorDocument::isDirty() const {
    return m_impl->dirty;
}

bool EditorDocument::isReadOnly() const {
    return m_impl->effectiveReadOnly();
}

QString EditorDocument::unavailableReason() const {
    return m_impl->unavailableReason;
}

QString EditorDocument::lastError() const {
    return m_impl->lastError;
}

int EditorDocument::cursorLine() const {
    return m_impl->cursorLine;
}

int EditorDocument::cursorCol() const {
    return m_impl->cursorCol;
}

bool EditorDocument::loadFromDisk() {
    const bool wasDirty = m_impl->dirty;
    const bool wasReadOnly = m_impl->effectiveReadOnly();
    const QString prevError = m_impl->lastError;

    auto emitStateChanges = [this, wasDirty, wasReadOnly, &prevError]() {
        if (m_impl->dirty != wasDirty) {
            emit dirtyChanged(m_impl->dirty);
        }
        if (m_impl->lastError != prevError) {
            emit errorChanged(m_impl->lastError);
        }
        const bool nowReadOnly = m_impl->effectiveReadOnly();
        if (nowReadOnly != wasReadOnly) {
            emit readOnlyChanged(nowReadOnly);
        }
    };

    if (m_impl->filePath.isEmpty()) {
        const QString err = QStringLiteral("No file path specified.");
        m_impl->unavailableReason = err;
        m_impl->lastError = err;
        m_impl->text.clear();
        m_impl->savedText.clear();
        m_impl->savedMtime = QDateTime();
        m_impl->savedFileSize = -1;
        m_impl->osReadOnly = false;
        m_impl->dirty = false;
        emitStateChanges();
        return false;
    }

    QFileInfo info(m_impl->filePath);
    info.refresh();

    if (!info.exists()) {
        const QString err = QStringLiteral("File not found: %1").arg(m_impl->filePath);
        m_impl->unavailableReason = err;
        m_impl->lastError = err;
        m_impl->text.clear();
        m_impl->savedText.clear();
        m_impl->savedMtime = QDateTime();
        m_impl->savedFileSize = -1;
        m_impl->osReadOnly = false;
        m_impl->dirty = false;
        emitStateChanges();
        return false;
    }

    if (info.isDir()) {
        const QString err = QStringLiteral("Cannot open a directory in the text editor: %1")
                                .arg(m_impl->filePath);
        m_impl->unavailableReason = err;
        m_impl->lastError = err;
        m_impl->text.clear();
        m_impl->savedText.clear();
        m_impl->savedMtime = QDateTime();
        m_impl->savedFileSize = -1;
        m_impl->osReadOnly = true;
        m_impl->dirty = false;
        emitStateChanges();
        return false;
    }

    const QDateTime observedMtime = info.lastModified();
    const qint64 observedSize = info.size();
    const bool observedOsReadOnly = !info.isWritable();

    if (observedSize > kMaxEditableBytes) {
        m_impl->unavailableReason = QStringLiteral(
            "This file is %1, too large to open in the editor (limit: 4 MB).")
            .arg(formatByteSize(observedSize));
        m_impl->lastError.clear();
        m_impl->text.clear();
        m_impl->savedText.clear();
        m_impl->savedMtime = observedMtime;
        m_impl->savedFileSize = observedSize;
        m_impl->osReadOnly = observedOsReadOnly;
        m_impl->hasLoadedOnce = true;
        m_impl->dirty = false;
        emitStateChanges();
        return false;
    }

    QFile file(m_impl->filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        const QString err = QStringLiteral("Failed to open file: %1").arg(file.errorString());
        m_impl->unavailableReason = err;
        m_impl->lastError = err;
        m_impl->text.clear();
        m_impl->savedText.clear();
        m_impl->savedMtime = QDateTime();
        m_impl->savedFileSize = -1;
        m_impl->osReadOnly = true;
        m_impl->dirty = false;
        emitStateChanges();
        return false;
    }

    const QByteArray rawBytes = file.read(kMaxEditableBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        const QString err = QStringLiteral("Failed to read file: %1").arg(file.errorString());
        file.close();
        m_impl->unavailableReason = err;
        m_impl->lastError = err;
        m_impl->text.clear();
        m_impl->savedText.clear();
        m_impl->savedMtime = QDateTime();
        m_impl->savedFileSize = -1;
        m_impl->osReadOnly = observedOsReadOnly;
        m_impl->dirty = false;
        emitStateChanges();
        return false;
    }
    file.close();

    if (rawBytes.size() > kMaxEditableBytes) {
        const qint64 reportedSize = std::max<qint64>(observedSize, rawBytes.size());
        m_impl->unavailableReason = QStringLiteral(
            "This file is %1, too large to open in the editor (limit: 4 MB).")
            .arg(formatByteSize(reportedSize));
        m_impl->lastError.clear();
        m_impl->text.clear();
        m_impl->savedText.clear();
        m_impl->savedMtime = observedMtime;
        m_impl->savedFileSize = reportedSize;
        m_impl->osReadOnly = observedOsReadOnly;
        m_impl->hasLoadedOnce = true;
        m_impl->dirty = false;
        emitStateChanges();
        return false;
    }

    const Utf8CheckResult utf8Check = validateUtf8Text(rawBytes);
    if (utf8Check != Utf8CheckResult::Valid) {
        m_impl->unavailableReason = (utf8Check == Utf8CheckResult::ContainsNul)
            ? QStringLiteral("This file appears to be binary and cannot be opened in the text editor.")
            : QStringLiteral("This file is not valid UTF-8 text and cannot be opened in the text editor.");
        m_impl->lastError.clear();
        m_impl->text.clear();
        m_impl->savedText.clear();
        m_impl->savedMtime = observedMtime;
        m_impl->savedFileSize = rawBytes.size();
        m_impl->osReadOnly = observedOsReadOnly;
        m_impl->hasLoadedOnce = true;
        m_impl->dirty = false;
        emitStateChanges();
        return false;
    }

    const QString decoded = QString::fromUtf8(rawBytes);
    m_impl->unavailableReason.clear();
    m_impl->lastError.clear();
    m_impl->text = decoded;
    m_impl->savedText = decoded;
    m_impl->savedMtime = observedMtime;
    m_impl->savedFileSize = rawBytes.size();
    m_impl->osReadOnly = observedOsReadOnly;
    m_impl->hasLoadedOnce = true;
    m_impl->dirty = false;
    emitStateChanges();
    return true;
}

bool EditorDocument::reloadIfChangedOnDisk() {
    // Never clobber unsaved local edits.
    if (m_impl->dirty || m_impl->filePath.isEmpty()) {
        return false;
    }

    QFileInfo info(m_impl->filePath);
    info.refresh();
    if (!info.exists() || info.isDir()) {
        return false;
    }

    const QDateTime currentMtime = info.lastModified();
    const qint64 currentSize = info.size();
    const bool metadataChanged = !m_impl->hasLoadedOnce
        || (currentMtime != m_impl->savedMtime)
        || (currentSize != m_impl->savedFileSize);

    // Sync OS write-permission changes even when contents are unchanged.
    const bool wasReadOnly = m_impl->effectiveReadOnly();
    m_impl->osReadOnly = !info.isWritable();
    if (m_impl->effectiveReadOnly() != wasReadOnly) {
        emit readOnlyChanged(m_impl->effectiveReadOnly());
    }

    const bool wasAvailable = isAvailable();
    const QString previousText = m_impl->text;

    if (!metadataChanged) {
        // Also detect rapid same-millisecond, same-length overwrites on disk.
        const bool isRecent = currentMtime.isValid()
            && qAbs(currentMtime.msecsTo(QDateTime::currentDateTimeUtc())) <= kRecentMtimeWindowMs;
        if (currentSize > kMaxEditableBytes
            || (currentSize > kContentProbeThresholdBytes && !isRecent)) {
            return false;
        }

        QFile probe(m_impl->filePath);
        if (!probe.open(QIODevice::ReadOnly)) {
            return false;
        }
        const QByteArray raw = probe.read(kMaxEditableBytes + 1);
        probe.close();

        if (raw.size() <= kMaxEditableBytes && validateUtf8Text(raw) == Utf8CheckResult::Valid) {
            if (wasAvailable && QString::fromUtf8(raw) == previousText) {
                return false;
            }
        } else if (!wasAvailable) {
            return false;
        }
    }

    loadFromDisk();

    const bool contentOrAvailabilityChanged =
        (isAvailable() != wasAvailable) || (m_impl->text != previousText);

    if (contentOrAvailabilityChanged) {
        emit reloadedFromDisk(m_impl->text);
        return true;
    }
    return false;
}

bool EditorDocument::saveToDisk() {
    auto setError = [this](const QString& err) {
        if (m_impl->lastError != err) {
            m_impl->lastError = err;
            emit errorChanged(err);
        }
    };

    if (m_impl->filePath.isEmpty()) {
        setError(QStringLiteral("Cannot save document: no file path specified."));
        return false;
    }

    if (!isAvailable()) {
        setError(QStringLiteral("Cannot save unavailable document: %1")
                     .arg(m_impl->unavailableReason));
        return false;
    }

    // Re-check OS permissions before saving so QSaveFile's atomic rename never
    // bypasses read-only file permissions on POSIX filesystems.
    {
        QFileInfo permInfo(m_impl->filePath);
        permInfo.refresh();
        if (permInfo.exists()) {
            const bool wasReadOnly = m_impl->effectiveReadOnly();
            m_impl->osReadOnly = !permInfo.isWritable();
            if (m_impl->effectiveReadOnly() != wasReadOnly) {
                emit readOnlyChanged(m_impl->effectiveReadOnly());
            }
        }
    }

    if (isReadOnly()) {
        setError(QStringLiteral("Cannot save read-only document."));
        return false;
    }

    if (!m_impl->dirty && QFileInfo::exists(m_impl->filePath)) {
        setError(QString());
        return true;
    }

    const QString targetPath = resolveSaveTargetPath(m_impl->filePath);
    QSaveFile saveFile(targetPath);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        setError(QStringLiteral("Failed to open file for saving: %1")
                     .arg(saveFile.errorString()));
        return false;
    }

    const QByteArray utf8 = m_impl->text.toUtf8();
    const qint64 bytesWritten = saveFile.write(utf8);
    if (bytesWritten != utf8.size()) {
        const QString err = QStringLiteral("Failed to write file: %1")
                                .arg(saveFile.errorString());
        saveFile.cancelWriting();
        setError(err);
        return false;
    }

    if (!saveFile.commit()) {
        setError(QStringLiteral("Failed to commit saved file: %1")
                     .arg(saveFile.errorString()));
        return false;
    }

    QFileInfo savedInfo(m_impl->filePath);
    savedInfo.refresh();

    m_impl->savedText = m_impl->text;
    m_impl->savedMtime = savedInfo.lastModified();
    m_impl->savedFileSize = savedInfo.exists() ? savedInfo.size() : utf8.size();
    m_impl->hasLoadedOnce = true;

    if (m_impl->dirty) {
        m_impl->dirty = false;
        emit dirtyChanged(false);
    }
    setError(QString());
    return true;
}

void EditorDocument::setTextFromEditor(const QString& text) {
    if (m_impl->text == text) {
        return;
    }
    m_impl->text = text;
    const bool nowDirty = (m_impl->text != m_impl->savedText);
    if (m_impl->dirty != nowDirty) {
        m_impl->dirty = nowDirty;
        emit dirtyChanged(nowDirty);
    }
}

void EditorDocument::setReadOnly(bool ro) {
    const bool wasReadOnly = m_impl->effectiveReadOnly();
    m_impl->explicitReadOnly = ro;
    if (!ro && !m_impl->filePath.isEmpty()) {
        QFileInfo info(m_impl->filePath);
        info.refresh();
        if (info.exists()) {
            m_impl->osReadOnly = !info.isWritable();
        }
    }
    const bool nowReadOnly = m_impl->effectiveReadOnly();
    if (nowReadOnly != wasReadOnly) {
        emit readOnlyChanged(nowReadOnly);
    }
}

void EditorDocument::setCursorPosition(int line, int col) {
    m_impl->cursorLine = std::max(0, line);
    m_impl->cursorCol = std::max(0, col);
}

QString EditorDocument::detectLanguage(const QString& filePath) {
    if (filePath.isEmpty()) {
        return QStringLiteral("plain");
    }

    QString normalized = filePath;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));

    const QFileInfo info(normalized);
    const QString fileName = info.fileName().toLower();
    if (fileName.isEmpty()) {
        return QStringLiteral("plain");
    }

    static const QHash<QString, QString> kFilenameLanguages = {
        {QStringLiteral("cmakelists.txt"), QStringLiteral("cmake")},
        {QStringLiteral("cargo.lock"),     QStringLiteral("toml")},
        {QStringLiteral("pipfile"),        QStringLiteral("toml")},
        {QStringLiteral("poetry.lock"),    QStringLiteral("toml")},
        {QStringLiteral("gemfile"),        QStringLiteral("ruby")},
        {QStringLiteral("rakefile"),       QStringLiteral("ruby")},
        {QStringLiteral("podfile"),        QStringLiteral("ruby")},
        {QStringLiteral("fastfile"),       QStringLiteral("ruby")},
        {QStringLiteral("brewfile"),       QStringLiteral("ruby")},
        {QStringLiteral("vagrantfile"),    QStringLiteral("ruby")},
        {QStringLiteral("makefile"),       QStringLiteral("bash")},
        {QStringLiteral("gnumakefile"),    QStringLiteral("bash")},
        {QStringLiteral("dockerfile"),     QStringLiteral("bash")},
        {QStringLiteral("containerfile"),  QStringLiteral("bash")},
        {QStringLiteral(".bashrc"),        QStringLiteral("bash")},
        {QStringLiteral(".bash_profile"),  QStringLiteral("bash")},
        {QStringLiteral(".bash_logout"),   QStringLiteral("bash")},
        {QStringLiteral(".zshrc"),         QStringLiteral("bash")},
        {QStringLiteral(".zshenv"),        QStringLiteral("bash")},
        {QStringLiteral(".zprofile"),      QStringLiteral("bash")},
        {QStringLiteral(".profile"),       QStringLiteral("bash")},
        {QStringLiteral(".clang-format"),  QStringLiteral("yaml")},
        {QStringLiteral(".clang-tidy"),    QStringLiteral("yaml")},
    };

    if (const auto it = kFilenameLanguages.constFind(fileName); it != kFilenameLanguages.constEnd()) {
        return *it;
    }

    // A dotfile with no additional dot (e.g. ".gitignore") has no file extension.
    if (fileName.startsWith(QLatin1Char('.')) && fileName.indexOf(QLatin1Char('.'), 1) < 0) {
        return QStringLiteral("plain");
    }

    const QString ext = info.suffix().toLower();
    if (ext.isEmpty()) {
        return QStringLiteral("plain");
    }

    static const QHash<QString, QString> kExtensionLanguages = {
        // Rust
        {QStringLiteral("rs"),           QStringLiteral("rust")},

        // C++
        {QStringLiteral("cpp"),          QStringLiteral("cpp")},
        {QStringLiteral("cc"),           QStringLiteral("cpp")},
        {QStringLiteral("cxx"),          QStringLiteral("cpp")},
        {QStringLiteral("c++"),          QStringLiteral("cpp")},
        {QStringLiteral("hpp"),          QStringLiteral("cpp")},
        {QStringLiteral("hh"),           QStringLiteral("cpp")},
        {QStringLiteral("hxx"),          QStringLiteral("cpp")},
        {QStringLiteral("h++"),          QStringLiteral("cpp")},
        {QStringLiteral("ipp"),          QStringLiteral("cpp")},
        {QStringLiteral("inl"),          QStringLiteral("cpp")},
        {QStringLiteral("tpp"),          QStringLiteral("cpp")},
        {QStringLiteral("mm"),           QStringLiteral("cpp")},
        {QStringLiteral("metal"),        QStringLiteral("cpp")},
        {QStringLiteral("cu"),           QStringLiteral("cpp")},
        {QStringLiteral("cuh"),          QStringLiteral("cpp")},
        {QStringLiteral("ixx"),          QStringLiteral("cpp")},
        {QStringLiteral("cppm"),         QStringLiteral("cpp")},

        // C & Headers
        {QStringLiteral("c"),            QStringLiteral("c")},
        {QStringLiteral("m"),            QStringLiteral("c")},
        {QStringLiteral("h"),            QStringLiteral("h")},

        // Swift
        {QStringLiteral("swift"),        QStringLiteral("swift")},

        // Python
        {QStringLiteral("py"),           QStringLiteral("python")},
        {QStringLiteral("pyi"),          QStringLiteral("python")},
        {QStringLiteral("pyw"),          QStringLiteral("python")},
        {QStringLiteral("pyx"),          QStringLiteral("python")},
        {QStringLiteral("pxd"),          QStringLiteral("python")},

        // JavaScript
        {QStringLiteral("js"),           QStringLiteral("js")},
        {QStringLiteral("mjs"),          QStringLiteral("js")},
        {QStringLiteral("cjs"),          QStringLiteral("js")},
        {QStringLiteral("jsx"),          QStringLiteral("js")},

        // TypeScript
        {QStringLiteral("ts"),           QStringLiteral("ts")},
        {QStringLiteral("mts"),          QStringLiteral("ts")},
        {QStringLiteral("cts"),          QStringLiteral("ts")},
        {QStringLiteral("tsx"),          QStringLiteral("ts")},

        // JSON
        {QStringLiteral("json"),         QStringLiteral("json")},
        {QStringLiteral("jsonc"),        QStringLiteral("json")},
        {QStringLiteral("json5"),        QStringLiteral("json")},
        {QStringLiteral("jsonl"),        QStringLiteral("json")},
        {QStringLiteral("ndjson"),       QStringLiteral("json")},
        {QStringLiteral("geojson"),      QStringLiteral("json")},

        // YAML
        {QStringLiteral("yaml"),         QStringLiteral("yaml")},
        {QStringLiteral("yml"),          QStringLiteral("yaml")},

        // TOML
        {QStringLiteral("toml"),         QStringLiteral("toml")},

        // Markdown
        {QStringLiteral("md"),           QStringLiteral("markdown")},
        {QStringLiteral("markdown"),     QStringLiteral("markdown")},
        {QStringLiteral("mdown"),        QStringLiteral("markdown")},
        {QStringLiteral("mkd"),          QStringLiteral("markdown")},
        {QStringLiteral("mkdn"),         QStringLiteral("markdown")},
        {QStringLiteral("mdx"),          QStringLiteral("markdown")},

        // Shell / Bash
        {QStringLiteral("sh"),           QStringLiteral("bash")},
        {QStringLiteral("bash"),         QStringLiteral("bash")},
        {QStringLiteral("zsh"),          QStringLiteral("bash")},
        {QStringLiteral("ksh"),          QStringLiteral("bash")},
        {QStringLiteral("fish"),         QStringLiteral("bash")},
        {QStringLiteral("csh"),          QStringLiteral("bash")},
        {QStringLiteral("tcsh"),         QStringLiteral("bash")},
        {QStringLiteral("command"),      QStringLiteral("bash")},

        // HTML
        {QStringLiteral("html"),         QStringLiteral("html")},
        {QStringLiteral("htm"),          QStringLiteral("html")},
        {QStringLiteral("xhtml"),        QStringLiteral("html")},
        {QStringLiteral("shtml"),        QStringLiteral("html")},
        {QStringLiteral("vue"),          QStringLiteral("html")},
        {QStringLiteral("svelte"),       QStringLiteral("html")},

        // CSS / Stylesheets
        {QStringLiteral("css"),          QStringLiteral("css")},
        {QStringLiteral("scss"),         QStringLiteral("css")},
        {QStringLiteral("sass"),         QStringLiteral("css")},
        {QStringLiteral("less"),         QStringLiteral("css")},
        {QStringLiteral("qss"),          QStringLiteral("css")},

        // SQL
        {QStringLiteral("sql"),          QStringLiteral("sql")},
        {QStringLiteral("ddl"),          QStringLiteral("sql")},
        {QStringLiteral("dml"),          QStringLiteral("sql")},
        {QStringLiteral("psql"),         QStringLiteral("sql")},

        // CMake
        {QStringLiteral("cmake"),        QStringLiteral("cmake")},

        // Diff / Patch
        {QStringLiteral("diff"),         QStringLiteral("diff")},
        {QStringLiteral("patch"),        QStringLiteral("diff")},

        // XML
        {QStringLiteral("xml"),          QStringLiteral("xml")},
        {QStringLiteral("xsd"),          QStringLiteral("xml")},
        {QStringLiteral("xsl"),          QStringLiteral("xml")},
        {QStringLiteral("xslt"),         QStringLiteral("xml")},
        {QStringLiteral("svg"),          QStringLiteral("xml")},
        {QStringLiteral("plist"),        QStringLiteral("xml")},
        {QStringLiteral("qrc"),          QStringLiteral("xml")},
        {QStringLiteral("ui"),           QStringLiteral("xml")},
        {QStringLiteral("xaml"),         QStringLiteral("xml")},
        {QStringLiteral("csproj"),       QStringLiteral("xml")},
        {QStringLiteral("vcxproj"),      QStringLiteral("xml")},
        {QStringLiteral("entitlements"), QStringLiteral("xml")},

        // Go
        {QStringLiteral("go"),           QStringLiteral("go")},

        // Java
        {QStringLiteral("java"),         QStringLiteral("java")},

        // Ruby
        {QStringLiteral("rb"),           QStringLiteral("ruby")},
        {QStringLiteral("rake"),         QStringLiteral("ruby")},
        {QStringLiteral("gemspec"),      QStringLiteral("ruby")},
        {QStringLiteral("podspec"),      QStringLiteral("ruby")},
        {QStringLiteral("ru"),           QStringLiteral("ruby")},
    };

    if (const auto it = kExtensionLanguages.constFind(ext); it != kExtensionLanguages.constEnd()) {
        return *it;
    }

    return QStringLiteral("plain");
}

} // namespace spell
