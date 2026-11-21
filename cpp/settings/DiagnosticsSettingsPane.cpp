#include "settings/DiagnosticsSettingsPane.h"

#include "core/SpellCoreClient.h"
#include "core/SpellSettingsStore.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStringList>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <functional>
#include <utility>

namespace spell {

namespace {

constexpr int kMaxLogTailLines = 200;
constexpr qint64 kMaxLogTailBytes = 64 * 1024;
constexpr int kLogPollIntervalMs = 2000;
constexpr int kWatcherDebounceMs = 150;

class VisibilityRefreshFilter final : public QObject {
public:
    explicit VisibilityRefreshFilter(std::function<void()> onShow, QObject* parent = nullptr)
        : QObject(parent)
        , m_onShow(std::move(onShow)) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event && event->type() == QEvent::Show && m_onShow) {
            m_onShow();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> m_onShow;
};

struct LogTailResult {
    bool exists = false;
    bool readable = false;
    bool truncated = false;
    qint64 fileSize = 0;
    qint64 lastModifiedMs = 0;
    int lineCount = 0;
    QString text;
    QString errorMessage;
};

QString formatByteSize(qint64 bytes) {
    if (bytes < 0) {
        return QStringLiteral("0 B");
    }
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const double kib = static_cast<double>(bytes) / 1024.0;
    if (kib < 1024.0) {
        return QStringLiteral("%1 KB").arg(kib, 0, 'f', 1);
    }
    const double mib = kib / 1024.0;
    if (mib < 1024.0) {
        return QStringLiteral("%1 MB").arg(mib, 0, 'f', 2);
    }
    const double gib = mib / 1024.0;
    return QStringLiteral("%1 GB").arg(gib, 0, 'f', 2);
}

QString defaultDataDirectory() {
    const QString envDir = qEnvironmentVariable("SPELL_DATA_DIR").trimmed();
    if (!envDir.isEmpty()) {
        return QDir::cleanPath(envDir);
    }
    const QString genericData =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation).trimmed();
    if (!genericData.isEmpty()) {
        return QDir::cleanPath(QDir(genericData).filePath(QStringLiteral("SpellIDE")));
    }
    const QString loc = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).trimmed();
    if (!loc.isEmpty()) {
        return QDir::cleanPath(loc);
    }
    return QDir::cleanPath(QDir::homePath() + QStringLiteral("/.spell-ide"));
}

QString formatOsArchString(const SystemInfo& info) {
    QString osPart = info.os.trimmed();
    QString verPart = info.osVersion.trimmed();
    QString archPart = info.arch.trimmed();

    if (osPart.isEmpty()) {
        osPart = QSysInfo::prettyProductName().trimmed();
        if (osPart.isEmpty()) {
            osPart = QSysInfo::productType().trimmed();
        }
        if (verPart.isEmpty()) {
            const QString prodVer = QSysInfo::productVersion().trimmed();
            if (!prodVer.isEmpty() && !osPart.contains(prodVer, Qt::CaseInsensitive)) {
                verPart = prodVer;
            }
        }
    }

    if (archPart.isEmpty()) {
        archPart = QSysInfo::currentCpuArchitecture().trimmed();
    }

    QString osDisplay = osPart;
    if (!verPart.isEmpty() && !osDisplay.contains(verPart, Qt::CaseInsensitive)) {
        osDisplay = osDisplay.isEmpty() ? verPart : QStringLiteral("%1 %2").arg(osDisplay, verPart);
    }

    if (!osDisplay.isEmpty() && !archPart.isEmpty()) {
        return QStringLiteral("%1 (%2)").arg(osDisplay, archPart);
    }
    if (!osDisplay.isEmpty()) {
        return osDisplay;
    }
    if (!archPart.isEmpty()) {
        return archPart;
    }
    return QStringLiteral("Unknown");
}

QString formatQtVersionString() {
    const QString runtimeVer = QString::fromLatin1(qVersion());
    const QString buildVer = QStringLiteral(QT_VERSION_STR);
    if (runtimeVer == buildVer || buildVer.isEmpty()) {
        return runtimeVer;
    }
    return QStringLiteral("%1 (built with %2)").arg(runtimeVer, buildVer);
}

LogTailResult readLogTail(const QString& filePath, int maxLines, qint64 maxBytes) {
    LogTailResult out;
    if (filePath.isEmpty()) {
        return out;
    }

    const QFileInfo fi(filePath);
    if (!fi.exists()) {
        return out;
    }

    out.exists = true;
    out.fileSize = fi.size();
    out.lastModifiedMs = fi.lastModified().toMSecsSinceEpoch();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        out.readable = false;
        out.errorMessage = file.errorString();
        return out;
    }
    out.readable = true;

    const qint64 size = file.size();
    bool slicedByBytes = false;
    if (size > maxBytes && maxBytes > 0) {
        if (file.seek(size - maxBytes)) {
            slicedByBytes = true;
        }
    }

    QByteArray raw = file.readAll();
    file.close();

    if (raw.isEmpty()) {
        return out;
    }

    if (slicedByBytes) {
        const qsizetype firstNewline = raw.indexOf('\n');
        if (firstNewline >= 0 && firstNewline + 1 < raw.size()) {
            raw = raw.mid(firstNewline + 1);
        }
        out.truncated = true;
    }

    QString content = QString::fromUtf8(raw);
    content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    content.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QStringList lines = content.split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }

    if (maxLines > 0 && lines.size() > maxLines) {
        lines = lines.mid(lines.size() - maxLines);
        out.truncated = true;
    }

    out.lineCount = static_cast<int>(lines.size());
    out.text = lines.join(QLatin1Char('\n'));
    return out;
}

void configurePathRow(SpellValueRow* row) {
    if (!row) {
        return;
    }
    if (QLabel* vw = row->valueWidget()) {
        vw->setWordWrap(true);
        vw->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    }
}

void setRowValueWithTooltip(SpellValueRow* row, const QString& displayValue, const QString& tooltip = QString()) {
    if (!row) {
        return;
    }
    row->setValue(displayValue);
    const QString tip = tooltip.isEmpty() ? displayValue : tooltip;
    row->setToolTip(tip);
    if (QLabel* vw = row->valueWidget()) {
        vw->setToolTip(tip);
    }
}

} // namespace

struct DiagnosticsSettingsPane::Impl {
    QPointer<SpellCoreClient> client;
    QPointer<SpellSettingsStore> store;

    WorkspaceSnapshot snapshot;
    SystemInfo systemInfo;

    QVector<QFrame*> cards;
    QVector<SpellSectionHeader*> sectionHeaders;
    QVector<SpellValueRow*> valueRows;
    QVector<SpellHairline*> hairlines;

    // Runtime & Endpoint Diagnostics
    QLabel* connectionCaption = nullptr;
    SpellBadge* connectionBadge = nullptr;
    SpellBadge* latencyBadge = nullptr;
    SpellValueRow* endpointUrlRow = nullptr;
    SpellValueRow* modelIdRow = nullptr;
    SpellValueRow* subagentModelIdRow = nullptr;
    SpellValueRow* dataDirRow = nullptr;
    SpellValueRow* logsDirRow = nullptr;
    SpellValueRow* osArchRow = nullptr;
    SpellValueRow* qtVersionRow = nullptr;
    QLabel* engineErrorLabel = nullptr;
    QPushButton* refreshDiagnosticsButton = nullptr;

    // Log File
    SpellValueRow* logFileRow = nullptr;
    SpellValueRow* logMetaRow = nullptr;
    QPushButton* revealLogButton = nullptr;
    QPushButton* copyLogPathButton = nullptr;
    QLabel* logActionStatusLabel = nullptr;
    bool logActionStatusIsError = false;

    // Diagnostics Bundle
    QLabel* bundleHintLabel = nullptr;
    QPushButton* exportBundleButton = nullptr;
    QPushButton* openBundleButton = nullptr;
    QLabel* exportedPathLabel = nullptr;
    QLabel* exportStatusLabel = nullptr;
    bool exportStatusIsError = false;
    QString lastExportedBundlePath;

    // Live Log Tail Preview
    QLabel* logTailMetaLabel = nullptr;
    SpellChipButton* autoRefreshChip = nullptr;
    QPushButton* reloadTailButton = nullptr;
    QPushButton* copyTailButton = nullptr;
    QPlainTextEdit* logPreview = nullptr;

    // Watchers & polling
    QFileSystemWatcher* logWatcher = nullptr;
    QTimer* pollTimer = nullptr;
    QTimer* debounceTimer = nullptr;
    bool autoRefreshEnabled = true;
    QString watchedLogFile;
    QString watchedLogsDir;
    QString lastLogPath;
    qint64 lastLogSize = -1;
    qint64 lastLogMtimeMs = -1;

    QString resolvedLogFilePath() const {
        if (store) {
            const QString p = store->logFilePath().trimmed();
            if (!p.isEmpty()) {
                return QDir::cleanPath(p);
            }
        }
        return QDir(defaultDataDirectory()).filePath(QStringLiteral("logs/spell.log"));
    }

    QString resolvedDataDir() const {
        if (!systemInfo.dataDir.trimmed().isEmpty()) {
            return QDir::cleanPath(systemInfo.dataDir.trimmed());
        }
        return defaultDataDirectory();
    }

    QString resolvedLogsDir() const {
        if (!systemInfo.logsDir.trimmed().isEmpty()) {
            return QDir::cleanPath(systemInfo.logsDir.trimmed());
        }
        const QString logPath = resolvedLogFilePath();
        if (!logPath.isEmpty()) {
            return QDir::cleanPath(QFileInfo(logPath).absolutePath());
        }
        return QDir(resolvedDataDir()).filePath(QStringLiteral("logs"));
    }

    SystemInfo effectiveSystemInfo() const {
        SystemInfo info = systemInfo;
        if (info.os.trimmed().isEmpty()) {
            info.os = QSysInfo::prettyProductName().trimmed();
            if (info.os.isEmpty()) {
                info.os = QSysInfo::productType().trimmed();
            }
        }
        if (info.osVersion.trimmed().isEmpty()) {
            info.osVersion = QSysInfo::productVersion().trimmed();
        }
        if (info.arch.trimmed().isEmpty()) {
            info.arch = QSysInfo::currentCpuArchitecture().trimmed();
        }
        if (info.hostname.trimmed().isEmpty()) {
            info.hostname = QSysInfo::machineHostName().trimmed();
        }
        if (info.dataDir.trimmed().isEmpty()) {
            info.dataDir = resolvedDataDir();
        }
        if (info.logsDir.trimmed().isEmpty()) {
            info.logsDir = resolvedLogsDir();
        }
        return info;
    }

    WorkspaceSnapshot effectiveSnapshotForExport() const {
        WorkspaceSnapshot snap = snapshot;
        if (store) {
            if (snap.endpointBaseUrl.trimmed().isEmpty()) {
                snap.endpointBaseUrl = store->endpointBaseUrl().trimmed();
            }
            if (snap.modelId.trimmed().isEmpty()) {
                snap.modelId = store->modelId().trimmed();
            }
            if (snap.subagentModelId.trimmed().isEmpty()) {
                snap.subagentModelId = store->subagentModelId().trimmed();
            }
            if (snap.effectiveSubagentModelId.trimmed().isEmpty()) {
                snap.effectiveSubagentModelId = !snap.subagentModelId.trimmed().isEmpty()
                    ? snap.subagentModelId.trimmed()
                    : snap.modelId.trimmed();
            }
            if (!snap.endpointConfigured && !snap.endpointBaseUrl.isEmpty()) {
                snap.endpointConfigured = true;
            }
        }
        return snap;
    }

    void refreshDiagnosticRows() {
        const QString endpointUrl = !snapshot.endpointBaseUrl.trimmed().isEmpty()
            ? snapshot.endpointBaseUrl.trimmed()
            : (store ? store->endpointBaseUrl().trimmed() : QString());

        QString primaryModel = snapshot.modelId.trimmed();
        if (primaryModel.isEmpty()) {
            primaryModel = snapshot.telemetry.modelId.trimmed();
        }
        if (primaryModel.isEmpty() && store) {
            primaryModel = store->modelId().trimmed();
        }

        QString explicitSubagentModel = snapshot.subagentModelId.trimmed();
        if (explicitSubagentModel.isEmpty() && store) {
            explicitSubagentModel = store->subagentModelId().trimmed();
        }

        QString effectiveSubagent = snapshot.effectiveSubagentModelId.trimmed();
        if (effectiveSubagent.isEmpty()) {
            effectiveSubagent = snapshot.telemetry.effectiveSubagentModelId.trimmed();
        }
        if (effectiveSubagent.isEmpty()) {
            effectiveSubagent = !explicitSubagentModel.isEmpty() ? explicitSubagentModel : primaryModel;
        }

        const bool configured = snapshot.endpointConfigured || snapshot.telemetry.endpointConfigured || !endpointUrl.isEmpty();
        const bool online = snapshot.telemetry.engineOnline;

        if (connectionBadge) {
            if (online) {
                connectionBadge->setText(QStringLiteral("ONLINE"));
                connectionBadge->setTone(BadgeTone::Success);
                connectionBadge->setShowDot(true);
            } else if (configured) {
                connectionBadge->setText(QStringLiteral("OFFLINE"));
                connectionBadge->setTone(BadgeTone::Danger);
                connectionBadge->setShowDot(true);
            } else {
                connectionBadge->setText(QStringLiteral("NOT CONFIGURED"));
                connectionBadge->setTone(BadgeTone::Neutral);
                connectionBadge->setShowDot(true);
            }
        }

        if (latencyBadge) {
            if (online && snapshot.telemetry.endpointLatencyMs >= 0) {
                latencyBadge->setText(QStringLiteral("%1 ms").arg(snapshot.telemetry.endpointLatencyMs));
                latencyBadge->setTone(BadgeTone::Info);
                latencyBadge->setVisible(true);
            } else {
                latencyBadge->setVisible(false);
            }
        }

        if (endpointUrlRow) {
            if (endpointUrl.isEmpty()) {
                setRowValueWithTooltip(endpointUrlRow, QStringLiteral("Not configured"));
                endpointUrlRow->setTone(BadgeTone::Neutral);
                endpointUrlRow->setValueIsPrimary(false);
            } else {
                setRowValueWithTooltip(endpointUrlRow, endpointUrl);
                endpointUrlRow->setTone(online ? BadgeTone::Success : BadgeTone::Accent);
                endpointUrlRow->setValueIsPrimary(true);
            }
        }

        if (modelIdRow) {
            if (primaryModel.isEmpty()) {
                setRowValueWithTooltip(modelIdRow, QStringLiteral("Not configured"));
                modelIdRow->setTone(BadgeTone::Neutral);
                modelIdRow->setValueIsPrimary(false);
            } else {
                setRowValueWithTooltip(modelIdRow, primaryModel);
                modelIdRow->setTone(BadgeTone::Accent);
                modelIdRow->setValueIsPrimary(true);
            }
        }

        if (subagentModelIdRow) {
            if (effectiveSubagent.isEmpty()) {
                setRowValueWithTooltip(subagentModelIdRow, QStringLiteral("Not configured"));
                subagentModelIdRow->setTone(BadgeTone::Neutral);
                subagentModelIdRow->setValueIsPrimary(false);
            } else if (explicitSubagentModel.isEmpty() && !primaryModel.isEmpty()) {
                setRowValueWithTooltip(
                    subagentModelIdRow,
                    QStringLiteral("%1 (inherits primary)").arg(effectiveSubagent),
                    effectiveSubagent);
                subagentModelIdRow->setTone(BadgeTone::Info);
                subagentModelIdRow->setValueIsPrimary(false);
            } else {
                setRowValueWithTooltip(subagentModelIdRow, effectiveSubagent);
                subagentModelIdRow->setTone(BadgeTone::Accent);
                subagentModelIdRow->setValueIsPrimary(true);
            }
        }

        const QString dataDir = resolvedDataDir();
        const QString logsDir = resolvedLogsDir();
        const QString logPath = resolvedLogFilePath();

        if (dataDirRow) {
            const QString nativeDataDir = QDir::toNativeSeparators(dataDir);
            setRowValueWithTooltip(dataDirRow, nativeDataDir);
            dataDirRow->setValueIsPrimary(false);
        }

        if (logsDirRow) {
            const QString nativeLogsDir = QDir::toNativeSeparators(logsDir);
            setRowValueWithTooltip(logsDirRow, nativeLogsDir);
            logsDirRow->setValueIsPrimary(false);
        }

        if (osArchRow) {
            const QString osArch = formatOsArchString(systemInfo);
            QStringList details;
            details.append(osArch);
            if (!systemInfo.hostname.trimmed().isEmpty()) {
                details.append(QStringLiteral("Host: %1").arg(systemInfo.hostname.trimmed()));
            }
            if (!systemInfo.cpuBrand.trimmed().isEmpty() || systemInfo.cpuCores > 0) {
                QString cpuLine = systemInfo.cpuBrand.trimmed();
                if (systemInfo.cpuCores > 0) {
                    const QString cores = QStringLiteral("%1 cores").arg(systemInfo.cpuCores);
                    cpuLine = cpuLine.isEmpty() ? cores : QStringLiteral("%1 (%2)").arg(cpuLine, cores);
                }
                details.append(QStringLiteral("CPU: %1").arg(cpuLine));
            }
            if (systemInfo.totalMemoryBytes > 0) {
                details.append(QStringLiteral("Memory: %1")
                                   .arg(formatByteSize(static_cast<qint64>(systemInfo.totalMemoryBytes))));
            }
            setRowValueWithTooltip(osArchRow, osArch, details.join(QLatin1Char('\n')));
            osArchRow->setValueIsPrimary(true);
        }

        if (qtVersionRow) {
            setRowValueWithTooltip(qtVersionRow, formatQtVersionString());
            qtVersionRow->setValueIsPrimary(true);
        }

        if (logFileRow) {
            const QString nativeLogPath = QDir::toNativeSeparators(logPath);
            setRowValueWithTooltip(logFileRow, nativeLogPath);
            logFileRow->setValueIsPrimary(true);
        }

        if (engineErrorLabel) {
            const QString err = snapshot.telemetry.engineError.trimmed();
            if (!err.isEmpty() && !online) {
                engineErrorLabel->setText(QStringLiteral("Endpoint error: %1").arg(err));
                engineErrorLabel->setVisible(true);
            } else {
                engineErrorLabel->clear();
                engineErrorLabel->setVisible(false);
            }
        }
    }

    void updateFileWatcher() {
        if (!logWatcher) {
            return;
        }
        const QString logPath = resolvedLogFilePath();
        const QString logsDir = QFileInfo(logPath).absolutePath();

        if (!watchedLogFile.isEmpty() && watchedLogFile != logPath) {
            if (logWatcher->files().contains(watchedLogFile)) {
                logWatcher->removePath(watchedLogFile);
            }
            watchedLogFile.clear();
        }

        if (!watchedLogsDir.isEmpty() && watchedLogsDir != logsDir) {
            if (logWatcher->directories().contains(watchedLogsDir)) {
                logWatcher->removePath(watchedLogsDir);
            }
            watchedLogsDir.clear();
        }

        if (!logsDir.isEmpty() && QFileInfo::exists(logsDir) && QFileInfo(logsDir).isDir()) {
            if (!logWatcher->directories().contains(logsDir)) {
                if (logWatcher->addPath(logsDir)) {
                    watchedLogsDir = logsDir;
                }
            } else {
                watchedLogsDir = logsDir;
            }
        }

        if (!logPath.isEmpty() && QFileInfo::exists(logPath) && QFileInfo(logPath).isFile()) {
            if (!logWatcher->files().contains(logPath)) {
                if (logWatcher->addPath(logPath)) {
                    watchedLogFile = logPath;
                }
            } else {
                watchedLogFile = logPath;
            }
        }
    }

    void refreshLogMetadataAndTail(bool force) {
        const QString logPath = resolvedLogFilePath();
        updateFileWatcher();

        if (logFileRow) {
            setRowValueWithTooltip(logFileRow, QDir::toNativeSeparators(logPath));
        }

        const QFileInfo fi(logPath);
        const bool exists = fi.exists() && fi.isFile();
        const qint64 currentSize = exists ? fi.size() : -1;
        const qint64 currentMtime = exists ? fi.lastModified().toMSecsSinceEpoch() : -1;

        if (!force && logPath == lastLogPath && currentSize == lastLogSize && currentMtime == lastLogMtimeMs) {
            return;
        }

        lastLogPath = logPath;
        lastLogSize = currentSize;
        lastLogMtimeMs = currentMtime;

        const LogTailResult tail = readLogTail(logPath, kMaxLogTailLines, kMaxLogTailBytes);

        if (logMetaRow) {
            if (!tail.exists) {
                setRowValueWithTooltip(logMetaRow, QStringLiteral("Not created yet"));
                logMetaRow->setTone(BadgeTone::Neutral);
            } else if (!tail.readable) {
                setRowValueWithTooltip(
                    logMetaRow,
                    QStringLiteral("Unreadable (%1)").arg(tail.errorMessage));
                logMetaRow->setTone(BadgeTone::Danger);
            } else {
                const QString updated = QDateTime::fromMSecsSinceEpoch(tail.lastModifiedMs)
                                            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                setRowValueWithTooltip(
                    logMetaRow,
                    QStringLiteral("%1 · updated %2").arg(formatByteSize(tail.fileSize), updated));
                logMetaRow->setTone(BadgeTone::Info);
            }
        }

        if (logTailMetaLabel) {
            const QString modeTag = autoRefreshEnabled ? QStringLiteral("Live") : QStringLiteral("Paused");
            if (!tail.exists) {
                logTailMetaLabel->setText(QStringLiteral("spell.log not created yet · %1").arg(modeTag));
            } else if (!tail.readable) {
                logTailMetaLabel->setText(QStringLiteral("Unable to read spell.log · %1").arg(modeTag));
            } else if (tail.lineCount == 0) {
                logTailMetaLabel->setText(QStringLiteral("0 lines · %1").arg(modeTag));
            } else if (tail.truncated) {
                logTailMetaLabel->setText(
                    QStringLiteral("Showing last %1 lines · %2").arg(tail.lineCount).arg(modeTag));
            } else {
                logTailMetaLabel->setText(
                    QStringLiteral("%1 %2 · %3")
                        .arg(tail.lineCount)
                        .arg(tail.lineCount == 1 ? QStringLiteral("line") : QStringLiteral("lines"), modeTag));
            }
        }

        if (logPreview) {
            QScrollBar* vbar = logPreview->verticalScrollBar();
            const bool wasAtBottom = !vbar || (vbar->maximum() - vbar->value() <= 2);
            const int prevScroll = vbar ? vbar->value() : 0;

            if (!tail.exists) {
                if (!logPreview->toPlainText().isEmpty()) {
                    logPreview->clear();
                }
            } else if (!tail.readable) {
                logPreview->setPlainText(
                    QStringLiteral("[Failed to read %1: %2]")
                        .arg(QDir::toNativeSeparators(logPath), tail.errorMessage));
            } else if (logPreview->toPlainText() != tail.text) {
                logPreview->setPlainText(tail.text);
                if (vbar) {
                    vbar->setValue(wasAtBottom ? vbar->maximum() : std::min(prevScroll, vbar->maximum()));
                }
            }
        }
    }

    void setLogActionStatus(const QString& message, bool isError) {
        logActionStatusIsError = isError;
        if (!logActionStatusLabel) {
            return;
        }
        logActionStatusLabel->setText(message);
        logActionStatusLabel->setVisible(!message.isEmpty());
        applyStatusLabelColors();
    }

    void setExportStatus(const QString& message, bool isError) {
        exportStatusIsError = isError;
        if (!exportStatusLabel) {
            return;
        }
        exportStatusLabel->setText(message);
        exportStatusLabel->setVisible(!message.isEmpty());
        applyStatusLabelColors();
    }

    void applyStatusLabelColors() {
        const UiPalette u = ThemeManager::instance().ui();
        const QColor secondary = compositeOver(u.textSecondary, u.background);
        const QColor muted = compositeOver(u.textMuted, u.background);

        if (connectionCaption) {
            connectionCaption->setStyleSheet(
                QStringLiteral("QLabel { background: transparent; color: %1; font-size: 12px; }")
                    .arg(qssColor(secondary)));
        }
        if (engineErrorLabel) {
            engineErrorLabel->setStyleSheet(
                QStringLiteral("QLabel { background: transparent; color: %1; font-size: 11px; }")
                    .arg(qssColor(u.danger)));
        }
        if (logActionStatusLabel) {
            const QColor col = logActionStatusIsError ? u.danger : secondary;
            logActionStatusLabel->setStyleSheet(
                QStringLiteral("QLabel { background: transparent; color: %1; font-size: 11px; }")
                    .arg(qssColor(col)));
        }
        if (bundleHintLabel) {
            bundleHintLabel->setStyleSheet(
                QStringLiteral("QLabel { background: transparent; color: %1; font-size: 11.5px; }")
                    .arg(qssColor(muted)));
        }
        if (exportedPathLabel) {
            exportedPathLabel->setStyleSheet(
                QStringLiteral("QLabel { background: transparent; color: %1; font-size: 11px; }")
                    .arg(qssColor(u.diffAdded)));
        }
        if (exportStatusLabel) {
            const QColor col = exportStatusIsError ? u.danger : secondary;
            exportStatusLabel->setStyleSheet(
                QStringLiteral("QLabel { background: transparent; color: %1; font-size: 11px; }")
                    .arg(qssColor(col)));
        }
        if (logTailMetaLabel) {
            logTailMetaLabel->setStyleSheet(
                QStringLiteral("QLabel { background: transparent; color: %1; font-size: 11px; }")
                    .arg(qssColor(muted)));
        }
    }
};

DiagnosticsSettingsPane::DiagnosticsSettingsPane(SpellCoreClient* client,
                                                 SpellSettingsStore* store,
                                                 QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->client = client;
    m_impl->store = store;

    setObjectName(QStringLiteral("diagnosticsSettingsPane"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(14);

    auto makeSection = [&](const QString& title, const QString& iconName) -> QVBoxLayout* {
        auto* header = new SpellSectionHeader(title, iconName, BadgeTone::Accent, this);
        m_impl->sectionHeaders.append(header);
        rootLayout->addWidget(header);

        auto* card = new QFrame(this);
        card->setObjectName(QStringLiteral("spellDiagnosticsCard"));
        card->setFrameShape(QFrame::NoFrame);
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_impl->cards.append(card);

        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 12, 14, 14);
        cardLayout->setSpacing(8);
        rootLayout->addWidget(card);
        return cardLayout;
    };

    auto addRow = [&](QVBoxLayout* layout, const QString& label, bool isPath) -> SpellValueRow* {
        auto* row = new SpellValueRow(label, QStringLiteral("—"), BadgeTone::Neutral, layout->parentWidget());
        if (isPath) {
            configurePathRow(row);
        }
        m_impl->valueRows.append(row);
        layout->addWidget(row);
        return row;
    };

    // -------------------------------------------------------------------------
    // Section 1: Runtime & Endpoint Diagnostics
    // -------------------------------------------------------------------------
    {
        QVBoxLayout* cardLayout = makeSection(
            QStringLiteral("Runtime & Endpoint Diagnostics"), QStringLiteral("stethoscope"));

        auto* statusRow = new QHBoxLayout();
        statusRow->setContentsMargins(0, 0, 0, 0);
        statusRow->setSpacing(8);

        m_impl->connectionCaption = new QLabel(QStringLiteral("Endpoint Status"), cardLayout->parentWidget());
        m_impl->connectionBadge = new SpellBadge(
            QStringLiteral("NOT CONFIGURED"), BadgeTone::Neutral, true, cardLayout->parentWidget());
        m_impl->latencyBadge = new SpellBadge(
            QString(), BadgeTone::Info, false, cardLayout->parentWidget());
        m_impl->latencyBadge->setVisible(false);

        statusRow->addWidget(m_impl->connectionCaption);
        statusRow->addStretch(1);
        statusRow->addWidget(m_impl->latencyBadge);
        statusRow->addWidget(m_impl->connectionBadge);
        cardLayout->addLayout(statusRow);

        auto* sep1 = new SpellHairline(Qt::Horizontal, cardLayout->parentWidget());
        m_impl->hairlines.append(sep1);
        cardLayout->addWidget(sep1);

        m_impl->endpointUrlRow = addRow(cardLayout, QStringLiteral("Endpoint URL"), true);
        m_impl->modelIdRow = addRow(cardLayout, QStringLiteral("Model ID"), false);
        m_impl->subagentModelIdRow = addRow(cardLayout, QStringLiteral("Subagent Model ID"), false);

        auto* sep2 = new SpellHairline(Qt::Horizontal, cardLayout->parentWidget());
        m_impl->hairlines.append(sep2);
        cardLayout->addWidget(sep2);

        m_impl->dataDirRow = addRow(cardLayout, QStringLiteral("Data Dir"), true);
        m_impl->logsDirRow = addRow(cardLayout, QStringLiteral("Logs Dir"), true);
        m_impl->osArchRow = addRow(cardLayout, QStringLiteral("OS / Arch"), false);
        m_impl->qtVersionRow = addRow(cardLayout, QStringLiteral("Qt Version"), false);

        m_impl->engineErrorLabel = new QLabel(cardLayout->parentWidget());
        m_impl->engineErrorLabel->setWordWrap(true);
        m_impl->engineErrorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_impl->engineErrorLabel->setVisible(false);
        cardLayout->addWidget(m_impl->engineErrorLabel);

        auto* actionsRow = new QHBoxLayout();
        actionsRow->setContentsMargins(0, 4, 0, 0);
        actionsRow->setSpacing(8);

        m_impl->refreshDiagnosticsButton = new QPushButton(
            QStringLiteral("Refresh Diagnostics"), cardLayout->parentWidget());
        m_impl->refreshDiagnosticsButton->setCursor(Qt::PointingHandCursor);
        m_impl->refreshDiagnosticsButton->setToolTip(
            QStringLiteral("Re-query host system info, endpoint status, and log preview"));
        actionsRow->addWidget(m_impl->refreshDiagnosticsButton);
        actionsRow->addStretch(1);
        cardLayout->addLayout(actionsRow);

        connect(m_impl->refreshDiagnosticsButton, &QPushButton::clicked, this, [this]() {
            if (m_impl->client) {
                m_impl->client->refreshEndpointStatus();
                QPointer<DiagnosticsSettingsPane> guard(this);
                m_impl->client->fetchSystemInfo([guard](SystemInfo info) {
                    if (guard) {
                        guard->updateSystemInfo(info);
                    }
                });
            }
            m_impl->refreshDiagnosticRows();
            m_impl->refreshLogMetadataAndTail(true);
        });
    }

    // -------------------------------------------------------------------------
    // Section 2: Log File
    // -------------------------------------------------------------------------
    {
        QVBoxLayout* cardLayout = makeSection(QStringLiteral("Log File"), QStringLiteral("file"));

        m_impl->logFileRow = addRow(cardLayout, QStringLiteral("Log File"), true);
        m_impl->logMetaRow = addRow(cardLayout, QStringLiteral("Status"), false);

        auto* btnRow = new QHBoxLayout();
        btnRow->setContentsMargins(0, 4, 0, 0);
        btnRow->setSpacing(8);

        m_impl->revealLogButton = new QPushButton(
            QStringLiteral("Reveal in File Manager"), cardLayout->parentWidget());
        m_impl->revealLogButton->setCursor(Qt::PointingHandCursor);
        m_impl->revealLogButton->setToolTip(
            QStringLiteral("Open the logs directory containing spell.log in your OS file manager"));

        m_impl->copyLogPathButton = new QPushButton(
            QStringLiteral("Copy Path"), cardLayout->parentWidget());
        m_impl->copyLogPathButton->setCursor(Qt::PointingHandCursor);
        m_impl->copyLogPathButton->setToolTip(
            QStringLiteral("Copy the full path of spell.log to the clipboard"));

        btnRow->addWidget(m_impl->revealLogButton);
        btnRow->addWidget(m_impl->copyLogPathButton);
        btnRow->addStretch(1);
        cardLayout->addLayout(btnRow);

        m_impl->logActionStatusLabel = new QLabel(cardLayout->parentWidget());
        m_impl->logActionStatusLabel->setWordWrap(true);
        m_impl->logActionStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_impl->logActionStatusLabel->setVisible(false);
        cardLayout->addWidget(m_impl->logActionStatusLabel);

        connect(m_impl->revealLogButton, &QPushButton::clicked, this, [this]() {
            const QString logPath = m_impl->resolvedLogFilePath();
            const QString logsDir = QFileInfo(logPath).absolutePath();
            if (logsDir.isEmpty()) {
                m_impl->setLogActionStatus(
                    QStringLiteral("Log directory path is unavailable."), true);
                return;
            }

            QDir dir(logsDir);
            if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
                m_impl->setLogActionStatus(
                    QStringLiteral("Failed to create logs directory: %1")
                        .arg(QDir::toNativeSeparators(logsDir)),
                    true);
                return;
            }

            const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(logsDir));
            if (opened) {
                m_impl->setLogActionStatus(
                    QStringLiteral("Opened %1 in file manager.")
                        .arg(QDir::toNativeSeparators(logsDir)),
                    false);
            } else {
                m_impl->setLogActionStatus(
                    QStringLiteral("Could not open file manager for %1.")
                        .arg(QDir::toNativeSeparators(logsDir)),
                    true);
            }
        });

        connect(m_impl->copyLogPathButton, &QPushButton::clicked, this, [this]() {
            const QString nativePath = QDir::toNativeSeparators(m_impl->resolvedLogFilePath());
            if (QClipboard* cb = QApplication::clipboard()) {
                cb->setText(nativePath);
                m_impl->setLogActionStatus(QStringLiteral("Copied log file path to clipboard."), false);
                QPointer<QPushButton> btn(m_impl->copyLogPathButton);
                if (btn) {
                    btn->setText(QStringLiteral("Copied"));
                    QTimer::singleShot(1500, btn, [btn]() {
                        if (btn) {
                            btn->setText(QStringLiteral("Copy Path"));
                        }
                    });
                }
            } else {
                m_impl->setLogActionStatus(QStringLiteral("Clipboard is unavailable."), true);
            }
        });
    }

    // -------------------------------------------------------------------------
    // Section 3: Diagnostics Bundle
    // -------------------------------------------------------------------------
    {
        QVBoxLayout* cardLayout = makeSection(
            QStringLiteral("Diagnostics Bundle"), QStringLiteral("shield"));

        m_impl->bundleHintLabel = new QLabel(
            QStringLiteral("Exports a redacted diagnostics summary (diagnostics_summary.json) and a copy of "
                           "spell.log into a timestamped folder. API keys and OS keyring secrets are never included."),
            cardLayout->parentWidget());
        m_impl->bundleHintLabel->setWordWrap(true);
        cardLayout->addWidget(m_impl->bundleHintLabel);

        auto* btnRow = new QHBoxLayout();
        btnRow->setContentsMargins(0, 4, 0, 0);
        btnRow->setSpacing(8);

        m_impl->exportBundleButton = new QPushButton(
            QStringLiteral("Export Diagnostics Bundle…"), cardLayout->parentWidget());
        m_impl->exportBundleButton->setProperty("primary", true);
        m_impl->exportBundleButton->setCursor(Qt::PointingHandCursor);
        m_impl->exportBundleButton->setToolTip(
            QStringLiteral("Choose a folder and write a redacted BeatrixLeslie IDE diagnostics bundle"));

        m_impl->openBundleButton = new QPushButton(
            QStringLiteral("Reveal Bundle"), cardLayout->parentWidget());
        m_impl->openBundleButton->setCursor(Qt::PointingHandCursor);
        m_impl->openBundleButton->setVisible(false);
        m_impl->openBundleButton->setToolTip(
            QStringLiteral("Open the exported diagnostics bundle folder in your OS file manager"));

        btnRow->addWidget(m_impl->exportBundleButton);
        btnRow->addWidget(m_impl->openBundleButton);
        btnRow->addStretch(1);
        cardLayout->addLayout(btnRow);

        m_impl->exportedPathLabel = new QLabel(cardLayout->parentWidget());
        m_impl->exportedPathLabel->setWordWrap(true);
        m_impl->exportedPathLabel->setTextInteractionFlags(
            Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        m_impl->exportedPathLabel->setVisible(false);
        cardLayout->addWidget(m_impl->exportedPathLabel);

        m_impl->exportStatusLabel = new QLabel(cardLayout->parentWidget());
        m_impl->exportStatusLabel->setWordWrap(true);
        m_impl->exportStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_impl->exportStatusLabel->setVisible(false);
        cardLayout->addWidget(m_impl->exportStatusLabel);

        connect(m_impl->exportBundleButton, &QPushButton::clicked, this, [this]() {
            if (!m_impl->store) {
                m_impl->setExportStatus(
                    QStringLiteral("Settings store is unavailable; cannot export diagnostics bundle."), true);
                return;
            }

            const QString defaultExportDir = QDir(m_impl->resolvedDataDir()).filePath(QStringLiteral("diagnostics"));
            QDir().mkpath(defaultExportDir);
            const QString initialDir = QFileInfo::exists(defaultExportDir)
                ? defaultExportDir
                : m_impl->resolvedDataDir();

            const QString targetDir = QFileDialog::getExistingDirectory(
                this,
                tr("Export BeatrixLeslie IDE Diagnostics Bundle"),
                initialDir,
                QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

            if (targetDir.isEmpty()) {
                return;
            }

            const WorkspaceSnapshot snap = m_impl->effectiveSnapshotForExport();
            const SystemInfo sysInfo = m_impl->effectiveSystemInfo();
            const QString bundlePath = m_impl->store->exportDiagnosticsBundle(snap, sysInfo, targetDir);

            if (!bundlePath.isEmpty()) {
                m_impl->lastExportedBundlePath = bundlePath;
                m_impl->exportedPathLabel->setText(QDir::toNativeSeparators(bundlePath));
                m_impl->exportedPathLabel->setVisible(true);
                m_impl->openBundleButton->setVisible(true);
                m_impl->setExportStatus(
                    QStringLiteral("Diagnostics bundle exported (redacted — no secrets included)."), false);
                m_impl->store->appendLog(
                    QStringLiteral("INFO"),
                    QStringLiteral("Exported diagnostics bundle to %1").arg(bundlePath));
                m_impl->refreshLogMetadataAndTail(true);
            } else {
                m_impl->lastExportedBundlePath.clear();
                m_impl->exportedPathLabel->clear();
                m_impl->exportedPathLabel->setVisible(false);
                m_impl->openBundleButton->setVisible(false);
                m_impl->setExportStatus(
                    QStringLiteral("Failed to export diagnostics bundle. Verify folder write permissions."), true);
            }
        });

        connect(m_impl->openBundleButton, &QPushButton::clicked, this, [this]() {
            if (m_impl->lastExportedBundlePath.isEmpty()) {
                return;
            }
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(m_impl->lastExportedBundlePath))) {
                m_impl->setExportStatus(
                    QStringLiteral("Could not open bundle folder: %1")
                        .arg(QDir::toNativeSeparators(m_impl->lastExportedBundlePath)),
                    true);
            }
        });
    }

    // -------------------------------------------------------------------------
    // Section 4: Live Log Tail Preview (spell.log)
    // -------------------------------------------------------------------------
    {
        QVBoxLayout* cardLayout = makeSection(
            QStringLiteral("Live Log Preview (spell.log)"), QStringLiteral("terminal"));

        auto* toolbarRow = new QHBoxLayout();
        toolbarRow->setContentsMargins(0, 0, 0, 0);
        toolbarRow->setSpacing(6);

        m_impl->logTailMetaLabel = new QLabel(
            QStringLiteral("0 lines · Live"), cardLayout->parentWidget());

        m_impl->autoRefreshChip = new SpellChipButton(
            QStringLiteral("Live Tail"), QStringLiteral("refresh"), BadgeTone::Accent, cardLayout->parentWidget());
        m_impl->autoRefreshChip->setCheckable(true);
        m_impl->autoRefreshChip->setChecked(true);
        m_impl->autoRefreshChip->setToolTip(
            QStringLiteral("Automatically refresh recent lines as spell.log updates"));

        m_impl->reloadTailButton = new QPushButton(
            QStringLiteral("Reload"), cardLayout->parentWidget());
        m_impl->reloadTailButton->setCursor(Qt::PointingHandCursor);
        m_impl->reloadTailButton->setToolTip(QStringLiteral("Reload recent lines from spell.log now"));

        m_impl->copyTailButton = new QPushButton(
            QStringLiteral("Copy Tail"), cardLayout->parentWidget());
        m_impl->copyTailButton->setCursor(Qt::PointingHandCursor);
        m_impl->copyTailButton->setToolTip(QStringLiteral("Copy visible log lines to the clipboard"));

        toolbarRow->addWidget(m_impl->logTailMetaLabel);
        toolbarRow->addStretch(1);
        toolbarRow->addWidget(m_impl->autoRefreshChip);
        toolbarRow->addWidget(m_impl->reloadTailButton);
        toolbarRow->addWidget(m_impl->copyTailButton);
        cardLayout->addLayout(toolbarRow);

        m_impl->logPreview = new QPlainTextEdit(cardLayout->parentWidget());
        m_impl->logPreview->setObjectName(QStringLiteral("spellDiagnosticsLogPreview"));
        m_impl->logPreview->setReadOnly(true);
        m_impl->logPreview->setUndoRedoEnabled(false);
        m_impl->logPreview->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        m_impl->logPreview->setMinimumHeight(180);
        m_impl->logPreview->setMaximumHeight(240);
        m_impl->logPreview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_impl->logPreview->setPlaceholderText(
            QStringLiteral("No entries in spell.log yet. Diagnostic log lines will appear here as BeatrixLeslie IDE runs."));
        cardLayout->addWidget(m_impl->logPreview);

        connect(m_impl->autoRefreshChip, &QPushButton::toggled, this, [this](bool checked) {
            m_impl->autoRefreshEnabled = checked;
            if (m_impl->pollTimer) {
                if (checked) {
                    m_impl->pollTimer->start(kLogPollIntervalMs);
                } else {
                    m_impl->pollTimer->stop();
                }
            }
            m_impl->refreshLogMetadataAndTail(true);
        });

        connect(m_impl->reloadTailButton, &QPushButton::clicked, this, [this]() {
            m_impl->refreshLogMetadataAndTail(true);
        });

        connect(m_impl->copyTailButton, &QPushButton::clicked, this, [this]() {
            if (!m_impl->logPreview) {
                return;
            }
            if (QClipboard* cb = QApplication::clipboard()) {
                cb->setText(m_impl->logPreview->toPlainText());
                QPointer<QPushButton> btn(m_impl->copyTailButton);
                if (btn) {
                    btn->setText(QStringLiteral("Copied"));
                    QTimer::singleShot(1500, btn, [btn]() {
                        if (btn) {
                            btn->setText(QStringLiteral("Copy Tail"));
                        }
                    });
                }
            }
        });
    }

    rootLayout->addStretch(1);

    // -------------------------------------------------------------------------
    // Live file watcher & periodic tail poller
    // -------------------------------------------------------------------------
    m_impl->logWatcher = new QFileSystemWatcher(this);
    m_impl->debounceTimer = new QTimer(this);
    m_impl->debounceTimer->setSingleShot(true);
    m_impl->debounceTimer->setInterval(kWatcherDebounceMs);

    connect(m_impl->debounceTimer, &QTimer::timeout, this, [this]() {
        if (!isVisible() || !m_impl->autoRefreshEnabled) {
            return;
        }
        m_impl->refreshLogMetadataAndTail(false);
    });

    auto scheduleTailRefresh = [this]() {
        if (!isVisible() || !m_impl->autoRefreshEnabled || !m_impl->debounceTimer) {
            return;
        }
        if (!m_impl->debounceTimer->isActive()) {
            m_impl->debounceTimer->start();
        }
    };

    connect(m_impl->logWatcher, &QFileSystemWatcher::fileChanged, this, [scheduleTailRefresh](const QString&) {
        scheduleTailRefresh();
    });
    connect(m_impl->logWatcher, &QFileSystemWatcher::directoryChanged, this, [scheduleTailRefresh](const QString&) {
        scheduleTailRefresh();
    });

    m_impl->pollTimer = new QTimer(this);
    m_impl->pollTimer->setInterval(kLogPollIntervalMs);
    connect(m_impl->pollTimer, &QTimer::timeout, this, [this]() {
        if (!isVisible() || !m_impl->autoRefreshEnabled) {
            return;
        }
        m_impl->refreshLogMetadataAndTail(false);
    });
    m_impl->pollTimer->start();

    installEventFilter(new VisibilityRefreshFilter([this]() {
        m_impl->refreshDiagnosticRows();
        m_impl->refreshLogMetadataAndTail(false);
    }, this));

    // -------------------------------------------------------------------------
    // Reactive connections to ThemeManager, SpellSettingsStore, SpellCoreClient
    // -------------------------------------------------------------------------
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &DiagnosticsSettingsPane::applyTheme);

    if (m_impl->store) {
        connect(m_impl->store, &SpellSettingsStore::endpointConfigChanged, this, [this]() {
            m_impl->refreshDiagnosticRows();
        });
        connect(m_impl->store, &SpellSettingsStore::apiKeyChanged, this, [this]() {
            m_impl->refreshDiagnosticRows();
        });
    }

    if (m_impl->client) {
        m_impl->snapshot = m_impl->client->currentSnapshot();
        connect(m_impl->client, &SpellCoreClient::snapshotUpdated,
                this, &DiagnosticsSettingsPane::updateSnapshot);

        QPointer<DiagnosticsSettingsPane> guard(this);
        m_impl->client->fetchSystemInfo([guard](SystemInfo info) {
            if (guard) {
                guard->updateSystemInfo(info);
            }
        });
    }

    m_impl->refreshDiagnosticRows();
    m_impl->refreshLogMetadataAndTail(true);
    applyTheme();
}

DiagnosticsSettingsPane::~DiagnosticsSettingsPane() {
    if (m_impl) {
        if (m_impl->pollTimer) {
            m_impl->pollTimer->stop();
        }
        if (m_impl->debounceTimer) {
            m_impl->debounceTimer->stop();
        }
    }
}

void DiagnosticsSettingsPane::updateSnapshot(const WorkspaceSnapshot& snap) {
    m_impl->snapshot = snap;
    m_impl->refreshDiagnosticRows();
}

void DiagnosticsSettingsPane::updateSystemInfo(const SystemInfo& info) {
    m_impl->systemInfo = info;
    m_impl->refreshDiagnosticRows();
    m_impl->refreshLogMetadataAndTail(false);
}

void DiagnosticsSettingsPane::applyTheme() {
    const ThemeManager& tm = ThemeManager::instance();
    const UiPalette u = tm.ui();
    const QColor solidBorder = compositeOver(u.subtleBorder, u.background);
    const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
    const QColor solidSelected = compositeOver(u.selectedFill, u.background);

    setStyleSheet(
        QStringLiteral("QWidget#diagnosticsSettingsPane { background-color: %1; }")
            .arg(qssColor(u.background)));

    const QString cardStyle = QStringLiteral(
        "QFrame#spellDiagnosticsCard {\n"
        "  background-color: %1;\n"
        "  border: 1px solid %2;\n"
        "  border-radius: 8px;\n"
        "}")
        .arg(qssColor(u.surface), qssColor(solidBorder));

    for (QFrame* card : m_impl->cards) {
        if (card) {
            card->setStyleSheet(cardStyle);
        }
    }

    for (SpellSectionHeader* header : m_impl->sectionHeaders) {
        if (header) {
            header->applyTheme();
        }
    }
    for (SpellValueRow* row : m_impl->valueRows) {
        if (row) {
            row->applyTheme();
        }
    }
    for (SpellHairline* line : m_impl->hairlines) {
        if (line) {
            line->applyTheme();
        }
    }

    if (m_impl->connectionBadge) {
        m_impl->connectionBadge->applyTheme();
    }
    if (m_impl->latencyBadge) {
        m_impl->latencyBadge->applyTheme();
    }
    if (m_impl->autoRefreshChip) {
        m_impl->autoRefreshChip->applyTheme();
    }

    auto setButtonIcon = [&u](QPushButton* btn, const QString& iconName) {
        if (btn) {
            btn->setIcon(icons::vectorIcon(iconName, u.textPrimary, 14));
            btn->setIconSize(QSize(14, 14));
        }
    };

    setButtonIcon(m_impl->refreshDiagnosticsButton, QStringLiteral("refresh"));
    setButtonIcon(m_impl->revealLogButton, QStringLiteral("folder-open"));
    setButtonIcon(m_impl->copyLogPathButton, QStringLiteral("copy"));
    setButtonIcon(m_impl->exportBundleButton, QStringLiteral("save"));
    setButtonIcon(m_impl->openBundleButton, QStringLiteral("popout"));
    setButtonIcon(m_impl->reloadTailButton, QStringLiteral("refresh"));
    setButtonIcon(m_impl->copyTailButton, QStringLiteral("copy"));

    if (m_impl->engineErrorLabel) {
        QFont mono = tm.editorFont();
        mono.setPointSizeF(std::max(10.0, tm.editorFontSize() - 2.0));
        m_impl->engineErrorLabel->setFont(mono);
    }

    if (m_impl->exportedPathLabel) {
        QFont mono = tm.editorFont();
        mono.setPointSizeF(std::max(10.0, tm.editorFontSize() - 2.0));
        m_impl->exportedPathLabel->setFont(mono);
    }

    if (m_impl->logPreview) {
        QFont logFont = tm.editorFont();
        logFont.setPointSizeF(std::max(10.0, tm.editorFontSize() - 1.5));
        m_impl->logPreview->setFont(logFont);
        m_impl->logPreview->setStyleSheet(
            QStringLiteral(
                "QPlainTextEdit#spellDiagnosticsLogPreview {\n"
                "  background-color: %1;\n"
                "  color: %2;\n"
                "  border: 1px solid %3;\n"
                "  border-radius: 6px;\n"
                "  padding: 8px;\n"
                "  selection-background-color: %4;\n"
                "  selection-color: %2;\n"
                "}")
                .arg(qssColor(solidElevated),
                     qssColor(u.textPrimary),
                     qssColor(solidBorder),
                     qssColor(solidSelected)));
    }

    m_impl->applyStatusLabelColors();
}

} // namespace spell
