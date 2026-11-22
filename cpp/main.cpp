#include "core/SpellCoreClient.h"
#include "core/SpellModels.h"
#include "panes/CodeEditorPane.h"
#include "settings/SettingsDialog.h"
#include "shell/MainWindow.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellSplitHandle.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QMetaType>
#include <QMutex>
#include <QMutexLocker>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>

#if defined(__APPLE__) || defined(__linux__)
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define SPELL_HAS_EXECINFO 1
#endif
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

QMutex gLogMutex;
QFile gDiagLogFile;
#if defined(__APPLE__) || defined(__linux__)
int gDiagLogFd = -1;

void writeStrFd(int fd, const char* s) {
    if (fd < 0 || !s) {
        return;
    }
    size_t len = 0;
    while (s[len] != '\0') {
        ++len;
    }
    if (len > 0) {
        const ssize_t ignored = ::write(fd, s, len);
        (void)ignored;
    }
}

void crashSignalHandler(int sig) {
    const char* name = "UNKNOWN";
    switch (sig) {
    case SIGSEGV: name = "SIGSEGV"; break;
    case SIGABRT: name = "SIGABRT"; break;
    case SIGBUS:  name = "SIGBUS"; break;
    case SIGILL:  name = "SIGILL"; break;
    case SIGFPE:  name = "SIGFPE"; break;
    default: break;
    }
    writeStrFd(STDERR_FILENO, "\n[SPELL CRASH] Fatal signal: ");
    writeStrFd(STDERR_FILENO, name);
    writeStrFd(STDERR_FILENO, "\n");
#if defined(SPELL_HAS_EXECINFO)
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
#endif
    if (gDiagLogFd >= 0) {
        writeStrFd(gDiagLogFd, "\n[SPELL CRASH] Fatal signal: ");
        writeStrFd(gDiagLogFd, name);
        writeStrFd(gDiagLogFd, "\n");
#if defined(SPELL_HAS_EXECINFO)
        ::backtrace_symbols_fd(frames, n, gDiagLogFd);
#endif
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

void initDiagnosticLogging() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    const QString diagPath = qEnvironmentVariable("SPELL_DIAG_LOG").trimmed();
    if (!diagPath.isEmpty()) {
        const QFileInfo fi(diagPath);
        QDir().mkpath(fi.absolutePath());
        QMutexLocker lock(&gLogMutex);
        gDiagLogFile.setFileName(diagPath);
        if (gDiagLogFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            gDiagLogFile.write(
                QStringLiteral("\n=== BeatrixLeslie IDE session started %1 (pid %2) ===\n")
                    .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
                    .arg(QCoreApplication::applicationPid())
                    .toUtf8());
            gDiagLogFile.flush();
        }
#if defined(__APPLE__) || defined(__linux__)
        gDiagLogFd = ::open(diagPath.toUtf8().constData(), O_WRONLY | O_CREAT | O_APPEND, 0644);
#endif
    }

    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
        const char* level = "INFO";
        switch (type) {
        case QtDebugMsg:    level = "DEBUG"; break;
        case QtInfoMsg:     level = "INFO"; break;
        case QtWarningMsg:  level = "WARN"; break;
        case QtCriticalMsg: level = "CRIT"; break;
        case QtFatalMsg:    level = "FATAL"; break;
        }
        const QString ts = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        const QString loc = (ctx.file && ctx.line > 0)
                                ? QStringLiteral(" (%1:%2)").arg(QString::fromUtf8(ctx.file)).arg(ctx.line)
                                : QString();
        const QString line = QStringLiteral("[%1] [QT %2]%3 %4\n")
                                 .arg(ts, QLatin1StringView(level), loc, msg);
        const QByteArray utf8 = line.toUtf8();
        QMutexLocker lock(&gLogMutex);
        const std::size_t written = std::fwrite(utf8.constData(), 1, static_cast<std::size_t>(utf8.size()), stderr);
        (void)written;
        std::fflush(stderr);
        if (gDiagLogFile.isOpen()) {
            gDiagLogFile.write(utf8);
            gDiagLogFile.flush();
        }
        if (type == QtFatalMsg) {
            std::abort();
        }
    });

    std::set_terminate([]() {
        const char* whatMsg = "unknown exception";
        if (const auto ep = std::current_exception()) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& ex) {
                whatMsg = ex.what();
            } catch (...) {
            }
        }
        std::fprintf(stderr, "\n[SPELL FATAL] Uncaught C++ exception: %s\n", whatMsg);
        std::fflush(stderr);
#if defined(__APPLE__) || defined(__linux__)
        if (gDiagLogFd >= 0) {
            writeStrFd(gDiagLogFd, "\n[SPELL FATAL] Uncaught C++ exception: ");
            writeStrFd(gDiagLogFd, whatMsg);
            writeStrFd(gDiagLogFd, "\n");
        }
#endif
        std::abort();
    });

#if defined(__APPLE__) || defined(__linux__)
    std::signal(SIGSEGV, crashSignalHandler);
    std::signal(SIGABRT, crashSignalHandler);
    std::signal(SIGBUS, crashSignalHandler);
    std::signal(SIGILL, crashSignalHandler);
    std::signal(SIGFPE, crashSignalHandler);
#endif
}

void registerSpellMetaTypes() {
    qRegisterMetaType<spell::ModelDescriptor>("spell::ModelDescriptor");
    qRegisterMetaType<spell::ProjectInfo>("spell::ProjectInfo");
    qRegisterMetaType<spell::ThreadSummary>("spell::ThreadSummary");
    qRegisterMetaType<spell::UiChatEvent>("spell::UiChatEvent");
    qRegisterMetaType<spell::StagedDiff>("spell::StagedDiff");
    qRegisterMetaType<spell::AgentTimer>("spell::AgentTimer");
    qRegisterMetaType<spell::RubricTag>("spell::RubricTag");
    qRegisterMetaType<spell::SubagentSlotStatus>("spell::SubagentSlotStatus");
    qRegisterMetaType<spell::ActiveSubagentInfo>("spell::ActiveSubagentInfo");
    qRegisterMetaType<spell::OnlineResearchSettings>("spell::OnlineResearchSettings");
    qRegisterMetaType<spell::ToolPermissionEntry>("spell::ToolPermissionEntry");
    qRegisterMetaType<spell::TerminalApprovalRequest>("spell::TerminalApprovalRequest");
    qRegisterMetaType<spell::SystemInfo>("spell::SystemInfo");
    qRegisterMetaType<spell::TelemetrySnapshot>("spell::TelemetrySnapshot");
    qRegisterMetaType<spell::WorkspaceSnapshot>("spell::WorkspaceSnapshot");
    qRegisterMetaType<spell::SettingsPaneId>("spell::SettingsPaneId");
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("Spell"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("com.spell.SpellIDE"));
    QCoreApplication::setApplicationName(QStringLiteral("SpellIDE"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("BeatrixLeslie IDE"));
    QGuiApplication::setDesktopFileName(QStringLiteral("com.spell.BeatrixLeslieIDE"));

    initDiagnosticLogging();
    registerSpellMetaTypes();

    // Parse cross-platform CLI options (filtering legacy macOS -psn_* LaunchServices tokens).
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("BeatrixLeslie IDE — Standalone Endpoint-Agnostic AI Coding IDE"));
    parser.addHelpOption();
    const QCommandLineOption versionOption(
        {QStringLiteral("v"), QStringLiteral("version")},
        QStringLiteral("Displays version information."));
    parser.addOption(versionOption);

    const QCommandLineOption dbOption(
        QStringLiteral("db"),
        QStringLiteral("Path to the SQLite workspace database (or ':memory:')."),
        QStringLiteral("path"));
    const QCommandLineOption settingsPaneOption(
        QStringLiteral("settings-pane"),
        QStringLiteral("Open Settings on launch focused on a specific pane (e.g. General, Engine, Terminal)."),
        QStringLiteral("name"));
    const QCommandLineOption openSettingsOption(
        QStringLiteral("open-settings"),
        QStringLiteral("Open the Settings dialog on launch."));
    const QCommandLineOption openTerminalWindowOption(
        QStringLiteral("open-terminal-window"),
        QStringLiteral("Open the integrated terminal popped out in a standalone window on launch."));
    const QCommandLineOption openFileOption(
        QStringLiteral("open-file"),
        QStringLiteral("Open a source file in the code editor on launch."),
        QStringLiteral("path"));
    const QCommandLineOption addProjectOption(
        QStringLiteral("add-project"),
        QStringLiteral("Add and select a project folder on launch."),
        QStringLiteral("dir"));
    const QCommandLineOption appearanceOption(
        QStringLiteral("appearance"),
        QStringLiteral("Select UI appearance on launch ('dark' or 'white')."),
        QStringLiteral("mode"));
    const QCommandLineOption exportScreenshotOption(
        QStringLiteral("export-screenshot"),
        QStringLiteral("Render a reproducible workspace screenshot to <path> and exit."),
        QStringLiteral("path"));

    parser.addOption(dbOption);
    parser.addOption(settingsPaneOption);
    parser.addOption(openSettingsOption);
    parser.addOption(openTerminalWindowOption);
    parser.addOption(openFileOption);
    parser.addOption(addProjectOption);
    parser.addOption(appearanceOption);
    parser.addOption(exportScreenshotOption);
    parser.addPositionalArgument(
        QStringLiteral("paths"),
        QStringLiteral("Optional files or project directories to open."),
        QStringLiteral("[paths...]"));

    QStringList filteredArgs = app.arguments();
    if (filteredArgs.size() > 1) {
        filteredArgs.erase(
            std::remove_if(filteredArgs.begin() + 1,
                           filteredArgs.end(),
                           [](const QString& arg) {
                               return arg.startsWith(QLatin1StringView("-psn_"));
                           }),
            filteredArgs.end());
    }
    parser.process(filteredArgs);

    if (parser.isSet(versionOption)) {
        std::fprintf(stdout, "BeatrixLeslie IDE %s\n",
                     QCoreApplication::applicationVersion().toUtf8().constData());
        return 0;
    }

    if (parser.isSet(appearanceOption)) {
        const QString mode = parser.value(appearanceOption).trimmed().toLower();
        if (mode == QLatin1StringView("white") || mode == QLatin1StringView("light")) {
            spell::ThemeManager::instance().setAppearance(spell::Appearance::Light);
        } else if (mode == QLatin1StringView("dark")) {
            spell::ThemeManager::instance().setAppearance(spell::Appearance::Dark);
        }
    }

    app.setWindowIcon(spell::icons::appWindowIcon());
    spell::ThemeManager::instance().applyToApplication(&app);

    const QString exportScreenshotPath = parser.isSet(exportScreenshotOption)
                                             ? parser.value(exportScreenshotOption).trimmed()
                                             : QString();
    const bool shouldExportScreenshot = !exportScreenshotPath.isEmpty();

    const QString dbPath = parser.isSet(dbOption)
                               ? parser.value(dbOption)
                               : (shouldExportScreenshot ? QStringLiteral(":memory:") : QString());
    QTemporaryDir screenshotSeedDir;

    spell::MainWindow window(dbPath);
    if (auto* client = window.coreClient()) {
        QObject::connect(client, &spell::SpellCoreClient::errorOccurred, &window,
                         [](const QString& message) {
                             qWarning().noquote() << "[CORE ERROR]" << message;
                         });
        QObject::connect(client, &spell::SpellCoreClient::modelsFetched, &window,
                         [](const QVector<spell::ModelDescriptor>& models, const QString& error) {
                             if (!error.isEmpty()) {
                                 qWarning().noquote() << "[MODELS FETCH ERROR]" << error;
                             } else {
                                 qInfo().noquote() << "[MODELS FETCHED]" << models.size() << "model(s)";
                             }
                         });
    }

    if (shouldExportScreenshot) {
        if (window.isLeftPaneCollapsed()) {
            window.toggleLeftPane();
        }
        if (window.isRightPaneCollapsed()) {
            window.toggleRightPane();
        }
        window.resize(1440, 900);
    }

    qInfo().noquote() << "[STARTUP] BeatrixLeslie IDE window ready (db:"
                      << (dbPath.isEmpty() ? QStringLiteral("default") : dbPath) << ")";
    window.show();

    const bool shouldOpenSettingsPane = parser.isSet(settingsPaneOption);
    const QString settingsPaneName = shouldOpenSettingsPane ? parser.value(settingsPaneOption) : QString();
    const bool shouldOpenSettings = parser.isSet(openSettingsOption);
    const bool shouldOpenTerminalWindow = parser.isSet(openTerminalWindowOption);
    const QStringList filesToOpen = parser.values(openFileOption);
    const QStringList projectsToAdd = parser.values(addProjectOption);
    const QStringList positionalPaths = parser.positionalArguments();

    if (shouldOpenSettingsPane || shouldOpenSettings || shouldOpenTerminalWindow ||
        shouldExportScreenshot || !filesToOpen.isEmpty() || !projectsToAdd.isEmpty() ||
        !positionalPaths.isEmpty()) {
        QTimer::singleShot(0, &window, [&app,
                                        &window,
                                        &screenshotSeedDir,
                                        shouldOpenSettingsPane,
                                        settingsPaneName,
                                        shouldOpenSettings,
                                        shouldOpenTerminalWindow,
                                        shouldExportScreenshot,
                                        exportScreenshotPath,
                                        filesToOpen,
                                        projectsToAdd,
                                        positionalPaths]() {
            bool addedAnyProject = false;
            bool hasSpellCoreProject = false;
            bool openedAnyFile = false;

            auto addProjectDir = [&window, &addedAnyProject, &hasSpellCoreProject](const QString& rawDir) {
                const QFileInfo fi(rawDir);
                if (!fi.exists() || !fi.isDir() || !window.coreClient()) {
                    return;
                }
                const QString cleanPath = QDir::cleanPath(fi.absoluteFilePath());
                QString name = QFileInfo(cleanPath).fileName();
                if (name.isEmpty()) {
                    name = cleanPath;
                }
                if (name == QLatin1StringView("spell_core")) {
                    hasSpellCoreProject = true;
                }
                addedAnyProject = true;
                window.coreClient()->createProject(name, cleanPath, false);
            };

            auto openSourceFile = [&window, &openedAnyFile](const QString& rawFile) {
                const QFileInfo fi(rawFile);
                if (!fi.exists() || !fi.isFile() || !window.editorPane()) {
                    return;
                }
                openedAnyFile = true;
                window.editorPane()->openFile(QDir::cleanPath(fi.absoluteFilePath()), -1, false);
            };

            for (const QString& dir : projectsToAdd) {
                addProjectDir(dir);
            }
            for (const QString& file : filesToOpen) {
                openSourceFile(file);
            }
            for (const QString& path : positionalPaths) {
                const QFileInfo fi(path);
                if (fi.isDir()) {
                    addProjectDir(path);
                } else if (fi.isFile()) {
                    openSourceFile(path);
                }
            }

            if (shouldOpenTerminalWindow) {
                window.togglePopOutTerminal();
            }

            if (shouldOpenSettingsPane) {
                window.openSettings(spell::settingsPaneFromName(settingsPaneName));
            } else if (shouldOpenSettings) {
                window.openSettings(spell::SettingsPaneId::General);
            }

            if (shouldExportScreenshot) {
                if (auto* sp = window.mainSplitter()) {
                    sp->setSizes({280, 720, 440});
                }

                // Seed a realistic Rust file & `spell_core` project when none were passed on CLI.
                QString seedProjectDir;
                const QFileInfo repoSpellCoreDir(QStringLiteral("rust/spell_core"));
                if (repoSpellCoreDir.exists() && repoSpellCoreDir.isDir()) {
                    seedProjectDir = QDir::cleanPath(repoSpellCoreDir.absoluteFilePath());
                } else if (screenshotSeedDir.isValid()) {
                    seedProjectDir = QDir::cleanPath(screenshotSeedDir.filePath(QStringLiteral("spell_core")));
                    QDir().mkpath(QDir(seedProjectDir).filePath(QStringLiteral("src")));
                }

                if (!openedAnyFile && window.editorPane() && screenshotSeedDir.isValid()) {
                    const QString sampleRsPath = QDir::cleanPath(
                        screenshotSeedDir.filePath(QStringLiteral("spell_core/src/lib.rs")));
                    QDir().mkpath(QFileInfo(sampleRsPath).absolutePath());
                    QFile sampleFile(sampleRsPath);
                    if (sampleFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                        sampleFile.write(
                            "// BeatrixLeslie IDE -- Standalone Endpoint-Agnostic Core Engine\n"
                            "use std::path::{Path, PathBuf};\n"
                            "use std::sync::Arc;\n\n"
                            "#[derive(Debug, Clone, PartialEq, Eq)]\n"
                            "pub struct EndpointConfig {\n"
                            "    pub base_url: String,\n"
                            "    pub model_id: String,\n"
                            "    pub subagent_model_id: Option<String>,\n"
                            "    pub critic_enabled: bool,\n"
                            "}\n\n"
                            "impl Default for EndpointConfig {\n"
                            "    fn default() -> Self {\n"
                            "        Self {\n"
                            "            base_url: String::new(),\n"
                            "            model_id: String::new(),\n"
                            "            subagent_model_id: None,\n"
                            "            critic_enabled: false,\n"
                            "        }\n"
                            "    }\n"
                            "}\n\n"
                            "pub struct WorkspaceSession {\n"
                            "    db_path: PathBuf,\n"
                            "    endpoint: EndpointConfig,\n"
                            "    max_agent_steps: usize,\n"
                            "}\n\n"
                            "impl WorkspaceSession {\n"
                            "    pub fn open(db_path: impl AsRef<Path>) -> Result<Arc<Self>, String> {\n"
                            "        let resolved = db_path.as_ref().to_path_buf();\n"
                            "        Ok(Arc::new(Self {\n"
                            "            db_path: resolved,\n"
                            "            endpoint: EndpointConfig::default(),\n"
                            "            max_agent_steps: 12,\n"
                            "        }))\n"
                            "    }\n"
                            "}\n");
                        sampleFile.close();
                    }
                    window.editorPane()->openFile(sampleRsPath, 1, false);
                }

                struct ExportCaptureState {
                    bool threadRequested = false;
                    bool captureScheduled = false;
                    QMetaObject::Connection snapConn;
                };
                auto state = std::make_shared<ExportCaptureState>();

                auto scheduleCaptureAfterSettle = [&app, &window, exportScreenshotPath, state]() {
                    if (state->captureScheduled) {
                        return;
                    }
                    state->captureScheduled = true;
                    if (state->snapConn) {
                        QObject::disconnect(state->snapConn);
                    }
                    QTimer::singleShot(250, &window, [&app, &window, exportScreenshotPath]() {
                        QWidget* target = &window;
                        if (window.settingsDialog() && window.settingsDialog()->isVisible()) {
                            target = window.settingsDialog();
                        }
                        const QPixmap pixmap = target->grab();
                        const QFileInfo outInfo(exportScreenshotPath);
                        QDir().mkpath(outInfo.absolutePath());
                        if (!pixmap.save(outInfo.absoluteFilePath())) {
                            qCritical().noquote() << "[SCREENSHOT ERROR] Failed to save screenshot to"
                                                  << outInfo.absoluteFilePath();
                            app.exit(1);
                            return;
                        }
                        qInfo().noquote() << "[SCREENSHOT] Saved screenshot to"
                                          << outInfo.absoluteFilePath();
                        app.quit();
                    });
                };

                if (auto* client = window.coreClient()) {
                    state->snapConn = QObject::connect(
                        client,
                        &spell::SpellCoreClient::snapshotUpdated,
                        &window,
                        [client, state, scheduleCaptureAfterSettle](const spell::WorkspaceSnapshot& snap) {
                            if (!state->threadRequested && !snap.projects.isEmpty()) {
                                QString targetProjectId = snap.selectedProjectId;
                                for (const auto& proj : snap.projects) {
                                    if (proj.name == QLatin1StringView("spell_core")) {
                                        targetProjectId = proj.id;
                                        break;
                                    }
                                }
                                if (targetProjectId.isEmpty()) {
                                    targetProjectId = snap.projects.first().id;
                                }
                                state->threadRequested = true;
                                client->createThread(
                                    targetProjectId,
                                    QStringLiteral("Architecture & Tooling"));
                                return;
                            }
                            if (state->threadRequested && !snap.threads.isEmpty()) {
                                scheduleCaptureAfterSettle();
                            }
                        });

                    if (!hasSpellCoreProject && !seedProjectDir.isEmpty()) {
                        client->createProject(QStringLiteral("spell_core"), seedProjectDir, false);
                    } else if (addedAnyProject) {
                        client->refreshSnapshot();
                    } else {
                        scheduleCaptureAfterSettle();
                    }

                    // Safety fallback so --export-screenshot never hangs if snapshot delivery is delayed.
                    QTimer::singleShot(1500, &window, scheduleCaptureAfterSettle);
                } else {
                    scheduleCaptureAfterSettle();
                }
            }
        });
    }

    return app.exec();
}
