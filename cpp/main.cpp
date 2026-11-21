#include "core/SpellCoreClient.h"
#include "core/SpellModels.h"
#include "panes/CodeEditorPane.h"
#include "settings/SettingsDialog.h"
#include "shell/MainWindow.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaType>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>

#if defined(__APPLE__) || defined(__linux__)
#include <execinfo.h>
#include <fcntl.h>
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
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
    if (gDiagLogFd >= 0) {
        writeStrFd(gDiagLogFd, "\n[SPELL CRASH] Fatal signal: ");
        writeStrFd(gDiagLogFd, name);
        writeStrFd(gDiagLogFd, "\n");
        ::backtrace_symbols_fd(frames, n, gDiagLogFd);
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
        std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stderr);
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
    parser.addVersionOption();

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

    parser.addOption(dbOption);
    parser.addOption(settingsPaneOption);
    parser.addOption(openSettingsOption);
    parser.addOption(openTerminalWindowOption);
    parser.addOption(openFileOption);
    parser.addOption(addProjectOption);
    parser.addOption(appearanceOption);
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

    const QString dbPath = parser.isSet(dbOption) ? parser.value(dbOption) : QString();
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
        !filesToOpen.isEmpty() || !projectsToAdd.isEmpty() || !positionalPaths.isEmpty()) {
        QTimer::singleShot(0, &window, [&window,
                                        shouldOpenSettingsPane,
                                        settingsPaneName,
                                        shouldOpenSettings,
                                        shouldOpenTerminalWindow,
                                        filesToOpen,
                                        projectsToAdd,
                                        positionalPaths]() {
            auto addProjectDir = [&window](const QString& rawDir) {
                const QFileInfo fi(rawDir);
                if (!fi.exists() || !fi.isDir() || !window.coreClient()) {
                    return;
                }
                const QString cleanPath = QDir::cleanPath(fi.absoluteFilePath());
                QString name = QFileInfo(cleanPath).fileName();
                if (name.isEmpty()) {
                    name = cleanPath;
                }
                window.coreClient()->createProject(name, cleanPath, false);
            };

            auto openSourceFile = [&window](const QString& rawFile) {
                const QFileInfo fi(rawFile);
                if (!fi.exists() || !fi.isFile() || !window.editorPane()) {
                    return;
                }
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
        });
    }

    return app.exec();
}
