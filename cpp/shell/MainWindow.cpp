#include "shell/MainWindow.h"

#include "shell/WindowChrome.h"
#include "core/SpellCoreClient.h"
#include "core/SpellModels.h"
#include "core/SpellSettingsStore.h"
#include "panes/AgentChatPane.h"
#include "panes/CodeEditorPane.h"
#include "panes/ComposerWidget.h"
#include "panes/EditorDocument.h"
#include "panes/ProjectExplorerPane.h"
#include "settings/SettingsDialog.h"
#include "terminal/SpellTerminalPanel.h"
#include "terminal/SpellTerminalSurface.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellSplitHandle.h"

#include <QAction>
#include <QActionGroup>
#include <QByteArray>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QMetaType>
#include <QPair>
#include <QPalette>
#include <QPointer>
#include <QSet>
#include <QSplitter>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

namespace spell {

namespace {

constexpr int kDefaultWindowWidth = 1400;
constexpr int kDefaultWindowHeight = 880;
constexpr int kMinWindowWidth = 960;
constexpr int kMinWindowHeight = 640;

constexpr int kMinLeftPaneWidth = 180;
constexpr int kMinCenterPaneWidth = 260;
constexpr int kMinRightPaneWidth = 300;
constexpr int kDefaultExpandedLeftWidth = 260;
constexpr int kDefaultExpandedRightWidth = 420;
constexpr int kMinTerminalHeight = 120;
constexpr int kMinComposerHeight = 96;
constexpr int kMinTimelineHeight = 140;

void registerSpellMetaTypesOnce() {
    static const bool registered = []() {
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
        return true;
    }();
    (void)registered;
}

bool isPathUnderRoot(const QString& candidatePath, const QString& rootPath) {
    if (candidatePath.isEmpty() || rootPath.isEmpty()) {
        return false;
    }
    const QString cleanCandidate = QDir::cleanPath(candidatePath);
    const QString cleanRoot = QDir::cleanPath(rootPath);
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    if (QString::compare(cleanCandidate, cleanRoot, cs) == 0) {
        return true;
    }
    const QString prefix = cleanRoot.endsWith(QLatin1Char('/'))
                               ? cleanRoot
                               : (cleanRoot + QLatin1Char('/'));
    return cleanCandidate.startsWith(prefix, cs);
}

QString normalizeTerminalDirectory(const QString& rawDir, const QString& fallbackDir) {
    QString candidate = rawDir.trimmed();
    if (candidate.isEmpty()) {
        candidate = fallbackDir.trimmed();
    }
    if (candidate.isEmpty()) {
        candidate = QDir::homePath();
    }
    if (candidate == QLatin1StringView("~")) {
        candidate = QDir::homePath();
    } else if (candidate.startsWith(QLatin1StringView("~/"))) {
        candidate = QDir::homePath() + candidate.mid(1);
    }

    QString cleaned = QDir::cleanPath(candidate);
    const QFileInfo fi(cleaned);
    if (fi.exists()) {
        if (fi.isFile()) {
            const QFileInfo parentInfo(fi.absolutePath());
            const QString canonParent = parentInfo.canonicalFilePath();
            cleaned = QDir::cleanPath(!canonParent.isEmpty() ? canonParent : fi.absolutePath());
        } else {
            const QString canon = fi.canonicalFilePath();
            cleaned = QDir::cleanPath(!canon.isEmpty() ? canon : fi.absoluteFilePath());
        }
    }
    if (cleaned.isEmpty()) {
        cleaned = QDir::cleanPath(QDir::homePath());
    }
    return cleaned;
}

QString folderTerminalHashKey(const QString& normalizedDir) {
#ifdef Q_OS_WIN
    return normalizedDir.toLower();
#else
    return normalizedDir;
#endif
}

bool sameDirectoryPath(const QString& a, const QString& b) {
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
    return folderTerminalHashKey(normalizeTerminalDirectory(a, QString())) ==
           folderTerminalHashKey(normalizeTerminalDirectory(b, QString()));
}

void applyFolderTerminalWindowTheme(QMainWindow* win, SpellTerminalPanel* panel) {
    if (!win) {
        return;
    }
    const UiPalette u = ThemeManager::instance().ui();
    QPalette pal = win->palette();
    pal.setColor(QPalette::Window, u.background);
    pal.setColor(QPalette::Base, u.surface);
    pal.setColor(QPalette::WindowText, u.textPrimary);
    pal.setColor(QPalette::Text, u.textPrimary);
    win->setPalette(pal);
    win->setStyleSheet(
        QStringLiteral("QMainWindow { background-color: %1; color: %2; }")
            .arg(qssColor(u.background), qssColor(u.textPrimary)));
    if (panel) {
        panel->applyTheme();
    }
}

/**
 * Lightweight `QMainWindow` host for per-folder pop-out terminals that notifies
 * `MainWindow` synchronously in `closeEvent` so `poppedOutFolderTerminalCount()`
 * and PTY shutdown stay immediately consistent even before `WA_DeleteOnClose`
 * runs deferred deletion.
 */
class FolderTerminalWindow final : public QMainWindow {
public:
    explicit FolderTerminalWindow(QWidget* parent = nullptr)
        : QMainWindow(parent) {}

    std::function<void()> onClosing;

protected:
    void closeEvent(QCloseEvent* event) override {
        if (onClosing) {
            auto callback = std::move(onClosing);
            onClosing = nullptr;
            callback();
        }
        QMainWindow::closeEvent(event);
    }
};

} // namespace

struct MainWindow::Impl {
    MainWindow* owner = nullptr;
    bool isInMemoryDb = false;
    bool shuttingDown = false;
    bool resourcesStopped = false;
    bool restoringLayout = false;
    bool userDraggedHorizontal = false;
    bool closingTerminal = false;
    bool sessionModifiedOpenFiles = false;
    int lastExpandedLeftWidth = kDefaultExpandedLeftWidth;
    int lastExpandedRightWidth = kDefaultExpandedRightWidth;
    bool leftPaneCollapsed = false;
    bool rightPaneCollapsed = false;

    WorkspaceSnapshot lastSnapshot;
    SystemInfo lastSystemInfo;
    bool hasSystemInfo = false;
    QSet<QString> seenApprovalIds;

    // Core services
    QPointer<SpellCoreClient> coreClient;
    QPointer<SpellSettingsStore> settingsStore;

    // Workspace panes & dialogs
    QPointer<SpellSplitter> mainSplitter;
    QPointer<ProjectExplorerPane> explorerPane;
    QPointer<CodeEditorPane> editorPane;
    QPointer<AgentChatPane> chatPane;
    QPointer<SpellTerminalPanel> terminalPanel;
    QPointer<SettingsDialog> settingsDialog;
    QHash<QString, QPointer<QMainWindow>> folderTerminalWindows;

    // File actions
    QAction* openProjectAction = nullptr;
    QAction* openFileAction = nullptr;
    QAction* newConversationAction = nullptr;
    QAction* saveAction = nullptr;
    QAction* saveAllAction = nullptr;
    QAction* closeTabAction = nullptr;
    QAction* quitAction = nullptr;

    // Edit actions
    QAction* findInFileAction = nullptr;
    QAction* reloadFilesAction = nullptr;

    // View actions
    QAction* toggleProjectsAction = nullptr;
    QAction* toggleChatAction = nullptr;
    QAction* toggleTerminalAction = nullptr;
    QAction* popOutTerminalAction = nullptr;
    QAction* focusProjectsAction = nullptr;
    QAction* focusEditorAction = nullptr;
    QAction* focusChatAction = nullptr;
    QActionGroup* appearanceGroup = nullptr;
    QAction* darkAppearanceAction = nullptr;
    QAction* lightAppearanceAction = nullptr;
    QActionGroup* codeThemeGroup = nullptr;
    QVector<QPair<CodeThemeId, QAction*>> codeThemeActions;

    // Agent actions
    QAction* configureEndpointAction = nullptr;
    QAction* fetchModelsAction = nullptr;
    QAction* askModeAction = nullptr;
    QAction* stageWriteAction = nullptr;
    QAction* resetContextCacheAction = nullptr;
    QAction* checkTimersAction = nullptr;
    QAction* stopGenerationAction = nullptr;

    // Terminal menu actions
    QAction* terminalToggleMenuAction = nullptr;
    QAction* terminalPopOutMenuAction = nullptr;
    QAction* terminalClearAction = nullptr;
    QAction* terminalRestartAction = nullptr;
    QAction* terminalStopAction = nullptr;
    QAction* terminalSettingsAction = nullptr;

    // Settings actions
    QAction* preferencesAction = nullptr;

    // Help actions
    QAction* aboutAction = nullptr;
    QAction* diagnosticsAction = nullptr;
    QAction* exportDiagnosticsAction = nullptr;

    void buildWorkspace();
    void buildMenus();
    void wireSignals();
    void restoreLayoutAndSession();
    void applyHorizontalSplitterWidths(int totalWidth);
    void restoreVerticalSplitterHeights();
    void persistSplitterSizes();

    QString activeProjectRoot() const;
    QPair<QString, QString> activeConversationTerminalTarget() const;
    bool isPathInReadOnlyProject(const QString& path) const;
    QString resolveCitationPath(const QString& rawPath) const;

    void ensureTerminalRunning();
    void restartTerminalInActiveProject();
    void dockAndFocusMainTerminalInDirectory(const QString& dir);
    void openProjectFolderDialog();
    void openFileDialog(const QString& rootDir = QString());
    void handleSnapshotUpdated(const WorkspaceSnapshot& snap);
    void handleTerminalSpellHandOff(const QString& instruction,
                                    const QString& recentOutput,
                                    const QString& cwd);
    void exportDiagnosticsBundle();

    void syncMenuStates();
    void syncPaneMenuStates();
    void syncTerminalUiState();
    void syncThemeMenuStates();
    void updateWindowTitle();
    void pushStatusNotice(const QString& message, bool isError = false);
    void shutdownResources();
};

void MainWindow::Impl::buildWorkspace() {
    mainSplitter = new SpellSplitter(Qt::Horizontal, owner);
    mainSplitter->setObjectName(QStringLiteral("MainWorkspaceSplitter"));
    mainSplitter->setOpaqueResize(true);
    mainSplitter->setChildrenCollapsible(false);
    mainSplitter->setAutoHideOnIdle(true);
    mainSplitter->setPaneToggleEnabled(true);

    explorerPane = new ProjectExplorerPane(mainSplitter);
    explorerPane->setObjectName(QStringLiteral("ProjectExplorerPane"));
    explorerPane->setMinimumWidth(kMinLeftPaneWidth);

    editorPane = new CodeEditorPane(mainSplitter);
    editorPane->setObjectName(QStringLiteral("CodeEditorPane"));
    editorPane->setMinimumWidth(kMinCenterPaneWidth);

    chatPane = new AgentChatPane(mainSplitter);
    chatPane->setObjectName(QStringLiteral("AgentChatPane"));
    chatPane->setMinimumWidth(kMinRightPaneWidth);
    chatPane->bindClient(coreClient);

    mainSplitter->addWidget(explorerPane);
    mainSplitter->addWidget(editorPane);
    mainSplitter->addWidget(chatPane);

    // Allow left and right panes to collapse to 0px while keeping their
    // SpellSplitterHandle live and hoverable at the window edges.
    mainSplitter->setCollapsible(0, true);
    mainSplitter->setCollapsible(1, false);
    mainSplitter->setCollapsible(2, true);

    // Editor absorbs extra space when the window grows while preserving user pane widths.
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setStretchFactor(2, 0);

    terminalPanel = new SpellTerminalPanel(coreClient, owner);
    terminalPanel->setObjectName(QStringLiteral("SpellTerminalPanel"));
    chatPane->setDockedTerminal(terminalPanel, false);

    owner->setCentralWidget(mainSplitter);
}

void MainWindow::Impl::buildMenus() {
    QMenuBar* bar = owner->menuBar();

    // -------------------------------------------------------------------------
    // File
    // -------------------------------------------------------------------------
    QMenu* fileMenu = bar->addMenu(MainWindow::tr("&File"));

    openProjectAction = fileMenu->addAction(MainWindow::tr("Open Project Folder…"));
    openProjectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    QObject::connect(openProjectAction, &QAction::triggered, owner, [this]() {
        openProjectFolderDialog();
    });

    openFileAction = fileMenu->addAction(MainWindow::tr("Open File…"));
    openFileAction->setShortcut(QKeySequence::Open);
    QObject::connect(openFileAction, &QAction::triggered, owner, [this]() {
        openFileDialog();
    });

    newConversationAction = fileMenu->addAction(MainWindow::tr("New Conversation"));
    newConversationAction->setShortcut(QKeySequence::New);
    QObject::connect(newConversationAction, &QAction::triggered, owner, [this]() {
        if (coreClient) {
            coreClient->createThread(lastSnapshot.selectedProjectId);
        }
        if (chatPane) {
            if (owner->isRightPaneCollapsed()) {
                owner->toggleRightPane();
            }
            chatPane->focusComposer();
        }
    });

    fileMenu->addSeparator();

    saveAction = fileMenu->addAction(MainWindow::tr("Save"));
    saveAction->setShortcut(QKeySequence::Save);
    saveAction->setEnabled(false);
    QObject::connect(saveAction, &QAction::triggered, owner, [this]() {
        if (editorPane) {
            editorPane->saveCurrentFile();
        }
    });

    saveAllAction = fileMenu->addAction(MainWindow::tr("Save All"));
    saveAllAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    saveAllAction->setEnabled(false);
    QObject::connect(saveAllAction, &QAction::triggered, owner, [this]() {
        if (editorPane) {
            editorPane->saveAllFiles();
        }
    });

    closeTabAction = fileMenu->addAction(MainWindow::tr("Close Tab"));
    closeTabAction->setShortcut(QKeySequence::Close);
    closeTabAction->setEnabled(false);
    QObject::connect(closeTabAction, &QAction::triggered, owner, [this]() {
        if (editorPane) {
            const QString current = editorPane->currentFilePath();
            if (!current.isEmpty()) {
                editorPane->closeFile(current);
            }
        }
    });

    fileMenu->addSeparator();

    quitAction = fileMenu->addAction(MainWindow::tr("Quit BeatrixLeslie IDE"));
    quitAction->setShortcut(QKeySequence::Quit);
    quitAction->setMenuRole(QAction::QuitRole);
    QObject::connect(quitAction, &QAction::triggered, owner, &QWidget::close);

    // -------------------------------------------------------------------------
    // Edit
    // -------------------------------------------------------------------------
    QMenu* editMenu = bar->addMenu(MainWindow::tr("&Edit"));

    findInFileAction = editMenu->addAction(MainWindow::tr("Find in File…"));
    findInFileAction->setShortcut(QKeySequence::Find);
    QObject::connect(findInFileAction, &QAction::triggered, owner, [this]() {
        if (editorPane) {
            editorPane->showFindBar();
        }
    });

    editMenu->addSeparator();

    reloadFilesAction = editMenu->addAction(MainWindow::tr("Reload Open Files from Disk"));
    QObject::connect(reloadFilesAction, &QAction::triggered, owner, [this]() {
        if (editorPane) {
            editorPane->reloadOpenDocumentsIfChanged();
            pushStatusNotice(MainWindow::tr("Checked open files for disk updates."), false);
        }
    });

    // -------------------------------------------------------------------------
    // View
    // -------------------------------------------------------------------------
    QMenu* viewMenu = bar->addMenu(MainWindow::tr("&View"));

    toggleProjectsAction = viewMenu->addAction(MainWindow::tr("Toggle Projects Pane"));
    toggleProjectsAction->setCheckable(true);
    toggleProjectsAction->setChecked(true);
    toggleProjectsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
    QObject::connect(toggleProjectsAction, &QAction::triggered, owner, [this]() {
        owner->toggleLeftPane();
    });

    toggleChatAction = viewMenu->addAction(MainWindow::tr("Toggle Agent Chat Pane"));
    toggleChatAction->setCheckable(true);
    toggleChatAction->setChecked(true);
    toggleChatAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
    QObject::connect(toggleChatAction, &QAction::triggered, owner, [this]() {
        owner->toggleRightPane();
    });

    toggleTerminalAction = viewMenu->addAction(MainWindow::tr("Toggle Terminal"));
    toggleTerminalAction->setCheckable(true);
    toggleTerminalAction->setChecked(false);
    toggleTerminalAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft));
    QObject::connect(toggleTerminalAction, &QAction::triggered, owner, [this]() {
        owner->toggleDockedTerminal();
    });

    popOutTerminalAction = viewMenu->addAction(MainWindow::tr("Pop Out Terminal"));
    popOutTerminalAction->setCheckable(true);
    popOutTerminalAction->setChecked(false);
    popOutTerminalAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_QuoteLeft));
    QObject::connect(popOutTerminalAction, &QAction::triggered, owner, [this]() {
        owner->togglePopOutTerminal();
    });

    viewMenu->addSeparator();

    focusProjectsAction = viewMenu->addAction(MainWindow::tr("Focus Projects Pane"));
    focusProjectsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
    QObject::connect(focusProjectsAction, &QAction::triggered, owner, [this]() {
        if (explorerPane) {
            if (owner->isLeftPaneCollapsed()) {
                owner->toggleLeftPane();
            }
            explorerPane->setFocus();
        }
    });

    focusEditorAction = viewMenu->addAction(MainWindow::tr("Focus Code Editor"));
    focusEditorAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_2));
    QObject::connect(focusEditorAction, &QAction::triggered, owner, [this]() {
        if (editorPane) {
            if (auto* sci = editorPane->scintilla()) {
                sci->setFocus();
            } else {
                editorPane->setFocus();
            }
        }
    });

    focusChatAction = viewMenu->addAction(MainWindow::tr("Focus Agent Chat"));
    focusChatAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_3));
    QObject::connect(focusChatAction, &QAction::triggered, owner, [this]() {
        if (chatPane) {
            if (owner->isRightPaneCollapsed()) {
                owner->toggleRightPane();
            }
            chatPane->focusComposer();
        }
    });

    viewMenu->addSeparator();

    QMenu* appearanceMenu = viewMenu->addMenu(MainWindow::tr("Appearance"));
    appearanceGroup = new QActionGroup(owner);
    appearanceGroup->setExclusive(true);

    darkAppearanceAction = appearanceMenu->addAction(MainWindow::tr("Dark"));
    darkAppearanceAction->setCheckable(true);
    appearanceGroup->addAction(darkAppearanceAction);
    QObject::connect(darkAppearanceAction, &QAction::triggered, owner, []() {
        ThemeManager::instance().setAppearance(Appearance::Dark);
    });

    lightAppearanceAction = appearanceMenu->addAction(MainWindow::tr("White"));
    lightAppearanceAction->setCheckable(true);
    appearanceGroup->addAction(lightAppearanceAction);
    QObject::connect(lightAppearanceAction, &QAction::triggered, owner, []() {
        ThemeManager::instance().setAppearance(Appearance::Light);
    });

    QMenu* codeThemeMenu = viewMenu->addMenu(MainWindow::tr("Code Theme"));
    codeThemeGroup = new QActionGroup(owner);
    codeThemeGroup->setExclusive(true);
    for (const CodeThemeId themeId : ThemeManager::allCodeThemes()) {
        QAction* act = codeThemeMenu->addAction(ThemeManager::codeThemeName(themeId));
        act->setCheckable(true);
        codeThemeGroup->addAction(act);
        codeThemeActions.append({themeId, act});
        QObject::connect(act, &QAction::triggered, owner, [themeId]() {
            ThemeManager::instance().setCodeTheme(themeId);
        });
    }

    // -------------------------------------------------------------------------
    // Agent
    // -------------------------------------------------------------------------
    QMenu* agentMenu = bar->addMenu(MainWindow::tr("&Agent"));

    configureEndpointAction = agentMenu->addAction(MainWindow::tr("Configure Endpoint…"));
    QObject::connect(configureEndpointAction, &QAction::triggered, owner, [this]() {
        owner->openSettings(SettingsPaneId::Engine);
    });

    fetchModelsAction = agentMenu->addAction(MainWindow::tr("Fetch Models"));
    QObject::connect(fetchModelsAction, &QAction::triggered, owner, [this]() {
        if (!coreClient) {
            return;
        }
        const QString configuredUrl = !lastSnapshot.endpointBaseUrl.trimmed().isEmpty()
                                          ? lastSnapshot.endpointBaseUrl.trimmed()
                                          : (settingsStore ? settingsStore->endpointBaseUrl().trimmed() : QString());
        if (configuredUrl.isEmpty()) {
            pushStatusNotice(MainWindow::tr("Configure an endpoint URL first in Settings → Engine."), false);
            owner->openSettings(SettingsPaneId::Engine);
            return;
        }
        pushStatusNotice(MainWindow::tr("Fetching models from %1…").arg(configuredUrl), false);
        coreClient->fetchModels();
    });

    agentMenu->addSeparator();

    askModeAction = agentMenu->addAction(MainWindow::tr("Ask Mode (Read-Only)"));
    askModeAction->setCheckable(true);
    askModeAction->setChecked(false);
    askModeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    QObject::connect(askModeAction, &QAction::toggled, owner, [this](bool askEnabled) {
        const QString tid = lastSnapshot.selectedThreadId;
        const QString profile = askEnabled ? QStringLiteral("ask") : QStringLiteral("write");
        if (coreClient && !tid.isEmpty()) {
            coreClient->setThreadProfile(tid, profile);
        }
        if (settingsStore) {
            settingsStore->appendLog(QStringLiteral("INFO"),
                                     QStringLiteral("[PROFILE] thread=%1 mode=%2").arg(tid, profile));
        }
        pushStatusNotice(askEnabled ? MainWindow::tr("Conversation mode: Ask (read-only)")
                                    : MainWindow::tr("Conversation mode: Write"),
                         false);
    });

    stageWriteAction = agentMenu->addAction(MainWindow::tr("Stage-Write Mode"));
    stageWriteAction->setCheckable(true);
    stageWriteAction->setChecked(false);
    QObject::connect(stageWriteAction, &QAction::toggled, owner, [this](bool enabled) {
        if (coreClient) {
            coreClient->setStageWriteMode(enabled);
        }
    });

    resetContextCacheAction = agentMenu->addAction(MainWindow::tr("Reset Context Cache"));
    resetContextCacheAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K));
    QObject::connect(resetContextCacheAction, &QAction::triggered, owner, [this]() {
        if (coreClient) {
            coreClient->requestLocalContextReset(lastSnapshot.selectedThreadId);
            pushStatusNotice(MainWindow::tr("Requested local context cache reset."), false);
        }
    });

    checkTimersAction = agentMenu->addAction(MainWindow::tr("Check Due Timers Now"));
    checkTimersAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    QObject::connect(checkTimersAction, &QAction::triggered, owner, [this]() {
        if (coreClient) {
            coreClient->fireDueTimersNow(lastSnapshot.selectedThreadId);
            pushStatusNotice(MainWindow::tr("Checked due local timers."), false);
        }
    });

    agentMenu->addSeparator();

    stopGenerationAction = agentMenu->addAction(MainWindow::tr("Stop Generation"));
    stopGenerationAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Period));
    stopGenerationAction->setEnabled(false);
    QObject::connect(stopGenerationAction, &QAction::triggered, owner, [this]() {
        if (coreClient) {
            coreClient->cancelActiveTurn();
            pushStatusNotice(MainWindow::tr("Cancelling active generation…"), false);
        }
    });

    // -------------------------------------------------------------------------
    // Terminal
    // -------------------------------------------------------------------------
    QMenu* terminalMenu = bar->addMenu(MainWindow::tr("&Terminal"));

    terminalToggleMenuAction = terminalMenu->addAction(MainWindow::tr("Show Docked Terminal"));
    QObject::connect(terminalToggleMenuAction, &QAction::triggered, owner, [this]() {
        owner->toggleDockedTerminal();
    });

    terminalPopOutMenuAction = terminalMenu->addAction(MainWindow::tr("Pop Out Terminal"));
    QObject::connect(terminalPopOutMenuAction, &QAction::triggered, owner, [this]() {
        owner->togglePopOutTerminal();
    });

    terminalMenu->addSeparator();

    terminalClearAction = terminalMenu->addAction(MainWindow::tr("Clear Terminal"));
    QObject::connect(terminalClearAction, &QAction::triggered, owner, [this]() {
        if (terminalPanel) {
            terminalPanel->clearTerminal();
        }
    });

    terminalRestartAction = terminalMenu->addAction(MainWindow::tr("Restart Shell"));
    QObject::connect(terminalRestartAction, &QAction::triggered, owner, [this]() {
        restartTerminalInActiveProject();
    });

    terminalStopAction = terminalMenu->addAction(MainWindow::tr("End Shell Session"));
    terminalStopAction->setEnabled(false);
    QObject::connect(terminalStopAction, &QAction::triggered, owner, [this]() {
        if (terminalPanel) {
            terminalPanel->stopShell();
        }
    });

    terminalMenu->addSeparator();

    terminalSettingsAction = terminalMenu->addAction(MainWindow::tr("Terminal Settings…"));
    QObject::connect(terminalSettingsAction, &QAction::triggered, owner, [this]() {
        owner->openSettings(SettingsPaneId::Terminal);
    });

    // -------------------------------------------------------------------------
    // Settings
    // -------------------------------------------------------------------------
    QMenu* settingsMenu = bar->addMenu(MainWindow::tr("&Settings"));

    preferencesAction = settingsMenu->addAction(MainWindow::tr("Preferences…"));
    preferencesAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    preferencesAction->setMenuRole(QAction::PreferencesRole);
    QObject::connect(preferencesAction, &QAction::triggered, owner, [this]() {
        owner->openSettings(SettingsPaneId::General);
    });

    settingsMenu->addSeparator();

    const QVector<QPair<QString, SettingsPaneId>> quickPanes = {
        {MainWindow::tr("General && Appearance…"), SettingsPaneId::General},
        {MainWindow::tr("Engine && Endpoint…"), SettingsPaneId::Engine},
        {MainWindow::tr("Statistics…"), SettingsPaneId::Statistics},
        {MainWindow::tr("Subagents…"), SettingsPaneId::Subagents},
        {MainWindow::tr("Conversation && Tags…"), SettingsPaneId::Conversation},
        {MainWindow::tr("Agent && Permissions…"), SettingsPaneId::Agent},
        {MainWindow::tr("Online Research…"), SettingsPaneId::OnlineResearch},
        {MainWindow::tr("Timers…"), SettingsPaneId::Timers},
        {MainWindow::tr("Terminal…"), SettingsPaneId::Terminal},
        {MainWindow::tr("Diagnostics…"), SettingsPaneId::Diagnostics},
    };
    for (const auto& entry : quickPanes) {
        QAction* act = settingsMenu->addAction(entry.first);
        const SettingsPaneId paneId = entry.second;
        QObject::connect(act, &QAction::triggered, owner, [this, paneId]() {
            owner->openSettings(paneId);
        });
    }

    // -------------------------------------------------------------------------
    // Help
    // -------------------------------------------------------------------------
    QMenu* helpMenu = bar->addMenu(MainWindow::tr("&Help"));

    aboutAction = helpMenu->addAction(MainWindow::tr("About BeatrixLeslie IDE"));
    aboutAction->setMenuRole(QAction::AboutRole);
    QObject::connect(aboutAction, &QAction::triggered, owner, [this]() {
        owner->openSettings(SettingsPaneId::About);
    });

    diagnosticsAction = helpMenu->addAction(MainWindow::tr("Diagnostics…"));
    QObject::connect(diagnosticsAction, &QAction::triggered, owner, [this]() {
        owner->openSettings(SettingsPaneId::Diagnostics);
    });

    exportDiagnosticsAction = helpMenu->addAction(MainWindow::tr("Export Diagnostics Bundle…"));
    QObject::connect(exportDiagnosticsAction, &QAction::triggered, owner, [this]() {
        exportDiagnosticsBundle();
    });

    syncThemeMenuStates();
}

void MainWindow::Impl::wireSignals() {
    // -------------------------------------------------------------------------
    // ProjectExplorerPane -> coreClient / editorPane / MainWindow
    // -------------------------------------------------------------------------
    QObject::connect(explorerPane, &ProjectExplorerPane::fileOpenRequested, owner,
                     [this](const QString& filePath, bool readOnly) {
                         if (editorPane) {
                             sessionModifiedOpenFiles = true;
                             editorPane->openFile(filePath, -1, readOnly);
                         }
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::addProjectRequested, owner, [this]() {
        openProjectFolderDialog();
    });

    QObject::connect(explorerPane, &ProjectExplorerPane::removeProjectRequested, owner,
                     [this](const QString& projectId) {
                         if (coreClient) {
                             coreClient->removeProject(projectId);
                         }
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::selectProjectRequested, owner,
                     [this](const QString& projectId) {
                         if (coreClient) {
                             coreClient->selectProject(projectId);
                         }
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::renameProjectRequested, owner,
                     [this](const QString& projectId, const QString& newName) {
                         if (coreClient) {
                             coreClient->renameProject(projectId, newName);
                         }
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::toggleProjectReadOnlyRequested, owner,
                     [this](const QString& projectId, bool readOnly) {
                         if (coreClient) {
                             coreClient->setProjectReadOnly(projectId, readOnly);
                         }
                         if (editorPane) {
                             if (auto* doc = editorPane->currentDocument()) {
                                 for (const auto& proj : lastSnapshot.projects) {
                                     if (proj.id == projectId && isPathUnderRoot(doc->filePath(), proj.rootPath)) {
                                         doc->setReadOnly(readOnly);
                                         break;
                                     }
                                 }
                             }
                         }
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::createThreadRequested, owner,
                     [this](const QString& projectId) {
                         if (coreClient) {
                             const QString targetProj =
                                 projectId.isEmpty() ? lastSnapshot.selectedProjectId : projectId;
                             coreClient->createThread(targetProj);
                         }
                         if (chatPane) {
                             if (owner->isRightPaneCollapsed()) {
                                 owner->toggleRightPane();
                             }
                             chatPane->focusComposer();
                         }
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::selectThreadRequested, owner,
                     [this](const QString& threadId) {
                         if (coreClient) {
                             coreClient->selectThread(threadId);
                         }
                         if (chatPane) {
                             chatPane->openConversationTab(threadId);
                         }
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::renameThreadRequested, owner,
                     [this](const QString& threadId, const QString& newTitle) {
                         if (coreClient) {
                             coreClient->renameThread(threadId, newTitle);
                         }
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::deleteThreadRequested, owner,
                     [this](const QString& threadId) {
                         if (chatPane) {
                             chatPane->closeConversationTab(threadId);
                         }
                         if (coreClient) {
                             coreClient->deleteThread(threadId);
                         }
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::setThreadProfileRequested, owner,
                     [this](const QString& threadId, const QString& profile) {
                         const bool isAsk = (profile.compare(QLatin1StringView("ask"), Qt::CaseInsensitive) == 0);
                         if (coreClient && !threadId.isEmpty()) {
                             coreClient->setThreadProfile(threadId, isAsk ? QStringLiteral("ask") : QStringLiteral("write"));
                         }
                         const QString notice = isAsk
                             ? MainWindow::tr("Conversation mode: Ask (read-only)")
                             : MainWindow::tr("Conversation mode: Write");
                         if (settingsStore) {
                             settingsStore->appendLog(QStringLiteral("INFO"),
                                                      QStringLiteral("[PROFILE] thread=%1 mode=%2")
                                                          .arg(threadId, isAsk ? QStringLiteral("ask") : QStringLiteral("write")));
                         }
                         pushStatusNotice(notice, false);
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::settingsRequested, owner,
                     [this](int initialPaneIndex) {
                         const int clamped = std::clamp(initialPaneIndex, 0, kSettingsPaneCount - 1);
                         owner->openSettings(static_cast<SettingsPaneId>(clamped));
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::terminalPopOutRequested, owner,
                     [this](const QString& workingDir, const QString& label) {
                         owner->openPopOutTerminalForDirectory(workingDir, label);
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::conversationLinkedFolderChanged, owner,
                     [this](const QString& threadId, const QString& folderPath) {
                         if (settingsStore) {
                             settingsStore->setConversationLinkedFolder(threadId, folderPath);
                             if (explorerPane) {
                                 explorerPane->setConversationLinkedFolders(
                                     settingsStore->allConversationLinkedFolders());
                             }
                         }
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::openOnlineResearchRequested, owner, [this]() {
        owner->openSettings(SettingsPaneId::OnlineResearch);
    });

    QObject::connect(explorerPane, &ProjectExplorerPane::airGapNowRequested, owner, [this]() {
        if (coreClient) {
            coreClient->airGapNow();
        }
        pushStatusNotice(MainWindow::tr("Online Research air-gapped"), false);
    });

    QObject::connect(explorerPane, &ProjectExplorerPane::toggleStageWriteRequested, owner,
                     [this](bool enabled) {
                         if (coreClient) {
                             coreClient->setStageWriteMode(enabled);
                         }
                     });

    QObject::connect(explorerPane, &ProjectExplorerPane::openProjectFileRequested, owner,
                     [this](const QString& rootPath) {
                         openFileDialog(rootPath);
                     });

    // -------------------------------------------------------------------------
    // CodeEditorPane -> MainWindow / settingsStore
    // -------------------------------------------------------------------------
    QObject::connect(editorPane, &CodeEditorPane::currentFileChanged, owner,
                     [this](const QString&) {
                         updateWindowTitle();
                         syncMenuStates();
                     });

    QObject::connect(editorPane, &CodeEditorPane::statusMessage, owner,
                     [this](const QString& msg) {
                         const bool isErr =
                             msg.startsWith(QLatin1StringView("Failed"), Qt::CaseInsensitive) ||
                             msg.startsWith(QLatin1StringView("Cannot"), Qt::CaseInsensitive) ||
                             msg.startsWith(QLatin1StringView("Could not"), Qt::CaseInsensitive) ||
                             msg.startsWith(QLatin1StringView("Error"), Qt::CaseInsensitive) ||
                             msg.endsWith(QLatin1StringView("failed)"), Qt::CaseInsensitive);
                         pushStatusNotice(msg, isErr);
                     });

    QObject::connect(editorPane, &CodeEditorPane::openDocumentsChanged, owner, [this]() {
        sessionModifiedOpenFiles = true;
        if (!isInMemoryDb && settingsStore && editorPane) {
            settingsStore->setOpenFiles(editorPane->openFilePaths());
        }
        updateWindowTitle();
        syncMenuStates();
    });

    QObject::connect(editorPane, &CodeEditorPane::openFileDialogRequested, owner, [this]() {
        openFileDialog();
    });

    // -------------------------------------------------------------------------
    // AgentChatPane -> editorPane / terminal / settings
    // -------------------------------------------------------------------------
    QObject::connect(chatPane, &AgentChatPane::fileCitationClicked, owner,
                     [this](const QString& filePath, int line) {
                         if (!editorPane) {
                             return;
                         }
                         const QString resolved = resolveCitationPath(filePath);
                         const bool ro = isPathInReadOnlyProject(resolved);
                         sessionModifiedOpenFiles = true;
                         editorPane->openFile(resolved, line, ro);
                         if (auto* sci = editorPane->scintilla()) {
                             sci->setFocus();
                         }
                     });

    QObject::connect(chatPane, &AgentChatPane::openSettingsRequested, owner,
                     [this](int paneIndex) {
                         const int clamped = std::clamp(paneIndex, 0, kSettingsPaneCount - 1);
                         owner->openSettings(static_cast<SettingsPaneId>(clamped));
                     });

    QObject::connect(chatPane, &AgentChatPane::terminalToggleRequested, owner, [this]() {
        owner->toggleDockedTerminal();
    });

    QObject::connect(chatPane, &AgentChatPane::popOutTerminalRequested, owner, [this]() {
        const auto [effectiveFolder, label] = activeConversationTerminalTarget();
        owner->openPopOutTerminalForDirectory(effectiveFolder, label);
    });

    QObject::connect(chatPane, &AgentChatPane::threadProfileChanged, owner,
                     [this](const QString& threadId, const QString& profile) {
                         const bool isAsk = (profile.compare(QLatin1StringView("ask"), Qt::CaseInsensitive) == 0);
                         const QString notice = isAsk
                             ? MainWindow::tr("Conversation mode: Ask (read-only)")
                             : MainWindow::tr("Conversation mode: Write");
                         if (settingsStore) {
                             settingsStore->appendLog(QStringLiteral("INFO"),
                                                      QStringLiteral("[PROFILE] thread=%1 mode=%2")
                                                          .arg(threadId, isAsk ? QStringLiteral("ask") : QStringLiteral("write")));
                         }
                         pushStatusNotice(notice, false);
                     });

    // Persist vertical splitter drags inside AgentChatPane (docked terminal & composer heights).
    if (auto* vSplit = chatPane->verticalSplitter()) {
        QObject::connect(vSplit, &QSplitter::splitterMoved, owner, [this](int, int) {
            if (restoringLayout || shuttingDown || !settingsStore || !chatPane) {
                return;
            }
            auto* split = chatPane->verticalSplitter();
            if (!split) {
                return;
            }
            const QList<int> sizes = split->sizes();
            if (auto* comp = chatPane->composerWidget()) {
                const int compIdx = split->indexOf(comp);
                if (compIdx >= 0 && compIdx < sizes.size() && sizes[compIdx] >= 60) {
                    settingsStore->setComposerHeight(sizes[compIdx]);
                }
            }
            if (terminalPanel && chatPane->isDockedTerminalVisible()) {
                const int termIdx = split->indexOf(terminalPanel);
                if (termIdx >= 0 && termIdx < sizes.size() && sizes[termIdx] >= 60) {
                    settingsStore->setDockedTerminalHeight(sizes[termIdx]);
                }
            }
        });
    }

    // Wire auto-hiding chalk splitter handle pane-toggle buttons.
    QObject::connect(mainSplitter, &SpellSplitter::paneToggleRequested, owner,
                     [this](int handleIndex) {
                         if (shuttingDown) {
                             return;
                         }
                         if (handleIndex == 1) {
                             owner->toggleLeftPane();
                         } else if (handleIndex == 2) {
                             owner->toggleRightPane();
                         }
                     });

    // Persist horizontal workspace splitter drags and sync collapsed/expanded handle states.
    QObject::connect(mainSplitter, &QSplitter::splitterMoved, owner, [this](int, int) {
        if (restoringLayout || shuttingDown || !mainSplitter) {
            return;
        }
        userDraggedHorizontal = true;
        const QList<int> sizes = mainSplitter->sizes();
        if (sizes.size() != 3) {
            return;
        }

        const bool leftNowCollapsed = (sizes[0] == 0);
        const bool rightNowCollapsed = (sizes[2] == 0);

        if (leftNowCollapsed) {
            if (explorerPane && explorerPane->minimumWidth() != 0) {
                explorerPane->setMinimumWidth(0);
            }
            mainSplitter->setHandleCollapsedState(1, true);
            if (!leftPaneCollapsed) {
                leftPaneCollapsed = true;
                if (settingsStore) {
                    settingsStore->setLeftPaneCollapsed(true);
                }
            }
        } else {
            if (explorerPane && explorerPane->minimumWidth() < kMinLeftPaneWidth) {
                explorerPane->setMinimumWidth(kMinLeftPaneWidth);
            }
            mainSplitter->setHandleCollapsedState(1, false);
            if (leftPaneCollapsed) {
                leftPaneCollapsed = false;
                if (settingsStore) {
                    settingsStore->setLeftPaneCollapsed(false);
                }
            }
            if (sizes[0] >= kMinLeftPaneWidth) {
                lastExpandedLeftWidth = sizes[0];
                if (settingsStore) {
                    settingsStore->setLeftPaneWidth(sizes[0]);
                }
            }
        }

        if (rightNowCollapsed) {
            if (chatPane && chatPane->minimumWidth() != 0) {
                chatPane->setMinimumWidth(0);
            }
            mainSplitter->setHandleCollapsedState(2, true);
            if (!rightPaneCollapsed) {
                rightPaneCollapsed = true;
                if (settingsStore) {
                    settingsStore->setRightPaneCollapsed(true);
                }
            }
        } else {
            if (chatPane && chatPane->minimumWidth() < kMinRightPaneWidth) {
                chatPane->setMinimumWidth(kMinRightPaneWidth);
            }
            mainSplitter->setHandleCollapsedState(2, false);
            if (rightPaneCollapsed) {
                rightPaneCollapsed = false;
                if (settingsStore) {
                    settingsStore->setRightPaneCollapsed(false);
                }
            }
            if (sizes[2] >= kMinRightPaneWidth) {
                lastExpandedRightWidth = sizes[2];
                if (settingsStore) {
                    settingsStore->setRightPaneWidth(sizes[2]);
                }
            }
        }

        syncPaneMenuStates();
    });

    // -------------------------------------------------------------------------
    // SpellTerminalPanel -> MainWindow / AgentChatPane / SettingsDialog
    // -------------------------------------------------------------------------
    QObject::connect(terminalPanel, &SpellTerminalPanel::popOutToggled, owner,
                     [this](bool poppedOut) {
                         if (shuttingDown) {
                             return;
                         }
                         if (poppedOut) {
                             persistSplitterSizes();
                             if (chatPane) {
                                 chatPane->setDockedTerminal(terminalPanel, false);
                             }
                             ensureTerminalRunning();
                             if (terminalPanel) {
                                 if (QWidget* popWin = terminalPanel->popOutWindow()) {
                                     popWin->setWindowIcon(icons::appWindowIcon());
                                     popWin->show();
                                     popWin->raise();
                                     popWin->activateWindow();
                                 }
                             }
                             if (settingsStore) {
                                 settingsStore->setTerminalDocked(false);
                             }
                         } else {
                             if (chatPane) {
                                 chatPane->setDockedTerminal(terminalPanel, !closingTerminal);
                                 if (!closingTerminal) {
                                     restoreVerticalSplitterHeights();
                                 }
                             }
                             if (settingsStore) {
                                 settingsStore->setTerminalDocked(true);
                             }
                         }
                         syncTerminalUiState();
                     });

    QObject::connect(terminalPanel, &SpellTerminalPanel::closeRequested, owner, [this]() {
        if (shuttingDown || !terminalPanel) {
            return;
        }
        closingTerminal = true;
        persistSplitterSizes();
        if (terminalPanel->isPoppedOut()) {
            terminalPanel->dockBack();
        }
        if (chatPane) {
            chatPane->setDockedTerminal(terminalPanel, false);
        }
        closingTerminal = false;
        syncTerminalUiState();
    });

    QObject::connect(terminalPanel, &SpellTerminalPanel::approvalResponded, owner,
                     [this](const QString& id, bool approved, const QString&) {
                         if (settingsStore) {
                             settingsStore->appendLog(
                                 QStringLiteral("INFO"),
                                 QStringLiteral("Terminal approval %1: %2")
                                     .arg(id, approved ? QStringLiteral("approved") : QStringLiteral("denied")));
                         }
                         pushStatusNotice(
                             approved ? MainWindow::tr("Approved terminal command.")
                                      : MainWindow::tr("Denied terminal command."),
                             false);
                     });

    QObject::connect(terminalPanel, &SpellTerminalPanel::spellHandOffRequested, owner,
                     [this](const QString& instruction, const QString& recentOutput, const QString& cwd) {
                         handleTerminalSpellHandOff(instruction, recentOutput, cwd);
                     });

    QObject::connect(terminalPanel, &SpellTerminalPanel::shellStateChanged, owner,
                     [this](bool) {
                         syncTerminalUiState();
                     });

    // -------------------------------------------------------------------------
    // SpellCoreClient -> all panes, terminal, menus
    // -------------------------------------------------------------------------
    QObject::connect(coreClient, &SpellCoreClient::snapshotUpdated, owner,
                     [this](const WorkspaceSnapshot& snap) {
                         handleSnapshotUpdated(snap);
                     });

    QObject::connect(coreClient, &SpellCoreClient::modelsFetched, owner,
                     [this](const QVector<ModelDescriptor>& models, const QString& error) {
                         if (!error.isEmpty()) {
                             if (settingsStore) {
                                 settingsStore->appendLog(
                                     QStringLiteral("WARN"),
                                     QStringLiteral("Model discovery failed: %1").arg(error));
                             }
                             pushStatusNotice(MainWindow::tr("Model fetch failed: %1").arg(error), true);
                         } else {
                             pushStatusNotice(
                                 MainWindow::tr("Discovered %1 model(s) from endpoint.").arg(models.size()),
                                 false);
                         }
                     });

    QObject::connect(coreClient, &SpellCoreClient::errorOccurred, owner,
                     [this](const QString& message) {
                         if (settingsStore) {
                             settingsStore->appendLog(QStringLiteral("ERROR"), message);
                         }
                         pushStatusNotice(message, true);
                     });

    QObject::connect(coreClient, &SpellCoreClient::busyChanged, owner, [this](bool busy) {
        if (stopGenerationAction) {
            stopGenerationAction->setEnabled(busy || lastSnapshot.isAgentBusy);
        }
        if (settingsStore) {
            if (busy) {
                settingsStore->appendLog(
                    QStringLiteral("INFO"),
                    QStringLiteral("[TURN START] thread=%1 model=%2")
                        .arg(lastSnapshot.selectedThreadId, lastSnapshot.modelId));
            } else {
                settingsStore->appendLog(
                    QStringLiteral("INFO"),
                    QStringLiteral("[TURN END] thread=%1 model=%2 tok_s=%3 prompt_tokens=%4 completion_tokens=%5 pending_diffs=%6")
                        .arg(lastSnapshot.selectedThreadId,
                             lastSnapshot.modelId,
                             QString::number(lastSnapshot.telemetry.decodeTokS, 'f', 1),
                             QString::number(lastSnapshot.telemetry.activePromptTokens),
                             QString::number(lastSnapshot.telemetry.totalCompletionTokens),
                             QString::number(lastSnapshot.pendingDiffs.size())));
            }
        }
        if (!busy && editorPane) {
            editorPane->reloadOpenDocumentsIfChanged();
        }
    });

    QObject::connect(coreClient, &SpellCoreClient::terminalCommandRequested, owner,
                     [this](const TerminalApprovalRequest& req) {
                         if (!terminalPanel) {
                             return;
                         }
                         if (!req.id.isEmpty()) {
                             seenApprovalIds.insert(req.id);
                         }
                         terminalPanel->showApprovalRequest(req);
                         if (terminalPanel->isPoppedOut()) {
                             if (QWidget* popWin = terminalPanel->popOutWindow()) {
                                 popWin->show();
                                 popWin->raise();
                                 popWin->activateWindow();
                             }
                         } else if (chatPane && !chatPane->isDockedTerminalVisible()) {
                             if (owner->isRightPaneCollapsed()) {
                                 owner->toggleRightPane();
                             }
                             ensureTerminalRunning();
                             chatPane->setDockedTerminal(terminalPanel, true);
                             restoreVerticalSplitterHeights();
                         }
                         syncTerminalUiState();
                         pushStatusNotice(
                             MainWindow::tr("Agent requested terminal command approval."), false);
                     });

    // -------------------------------------------------------------------------
    // ThemeManager -> MainWindow::applyTheme()
    // -------------------------------------------------------------------------
    QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged, owner, [this]() {
        if (!shuttingDown) {
            owner->applyTheme();
        }
    });

    QObject::connect(&ThemeManager::instance(), &ThemeManager::terminalThemeChanged, owner, [this]() {
        if (shuttingDown) {
            return;
        }
        if (terminalPanel) {
            terminalPanel->applyTheme();
        }
        for (const QPointer<QMainWindow>& winPtr : std::as_const(folderTerminalWindows)) {
            if (QMainWindow* win = winPtr.data()) {
                if (auto* panel = qobject_cast<SpellTerminalPanel*>(win->centralWidget())) {
                    panel->applyTheme();
                }
            }
        }
    });
}

void MainWindow::Impl::restoreLayoutAndSession() {
    if (explorerPane && settingsStore) {
        explorerPane->setConversationLinkedFolders(settingsStore->allConversationLinkedFolders());
    }

    if (!settingsStore) {
        owner->resize(kDefaultWindowWidth, kDefaultWindowHeight);
        applyHorizontalSplitterWidths(kDefaultWindowWidth);
        return;
    }

    leftPaneCollapsed = settingsStore->leftPaneCollapsed();
    rightPaneCollapsed = settingsStore->rightPaneCollapsed();
    if (const int savedLeft = settingsStore->leftPaneWidth(); savedLeft >= kMinLeftPaneWidth) {
        lastExpandedLeftWidth = savedLeft;
    }
    if (const int savedRight = settingsStore->rightPaneWidth(); savedRight >= kMinRightPaneWidth) {
        lastExpandedRightWidth = savedRight;
    }

    const QByteArray geom = settingsStore->windowGeometry();
    if (!geom.isEmpty()) {
        if (!owner->restoreGeometry(geom)) {
            owner->resize(kDefaultWindowWidth, kDefaultWindowHeight);
        }
    } else {
        owner->resize(kDefaultWindowWidth, kDefaultWindowHeight);
    }

    applyHorizontalSplitterWidths(std::max(owner->width(), kDefaultWindowWidth));
    restoreVerticalSplitterHeights();

    // Restore previously open files only for persistent sessions (never pollute ':memory:' runs).
    if (!isInMemoryDb && editorPane) {
        const QStringList files = settingsStore->openFiles();
        for (const QString& path : files) {
            const QFileInfo fi(path);
            if (fi.exists() && fi.isFile()) {
                editorPane->openFile(fi.absoluteFilePath(), -1, false);
            }
        }
    }

    // Re-apply horizontal splitter proportions once initial layout geometry settles.
    QTimer::singleShot(0, owner, [this]() {
        if (shuttingDown || userDraggedHorizontal || !mainSplitter) {
            return;
        }
        const int liveWidth = mainSplitter->width();
        if (liveWidth >= kMinWindowWidth) {
            applyHorizontalSplitterWidths(liveWidth);
        }
        restoreVerticalSplitterHeights();
    });
}

void MainWindow::Impl::applyHorizontalSplitterWidths(int totalWidth) {
    if (!mainSplitter || mainSplitter->count() < 3) {
        return;
    }
    const int minRequired = (leftPaneCollapsed ? 0 : kMinLeftPaneWidth)
                            + kMinCenterPaneWidth
                            + (rightPaneCollapsed ? 0 : kMinRightPaneWidth);
    const int usable = std::max(totalWidth, minRequired);
    const int savedLeft = settingsStore ? settingsStore->leftPaneWidth() : 0;
    const int savedRight = settingsStore ? settingsStore->rightPaneWidth() : 0;

    const int defaultLeft = std::clamp(static_cast<int>(usable * 0.22), kMinLeftPaneWidth, 340);
    const int defaultRight = std::clamp(static_cast<int>(usable * 0.35), kMinRightPaneWidth, 540);

    const int maxExpandedLeft = std::max(kMinLeftPaneWidth, usable - kMinCenterPaneWidth - kMinRightPaneWidth);
    const int expandedLeft = std::clamp(savedLeft > 0 ? savedLeft : defaultLeft,
                                        kMinLeftPaneWidth,
                                        maxExpandedLeft);

    const int maxExpandedRight = std::max(kMinRightPaneWidth, usable - expandedLeft - kMinCenterPaneWidth);
    const int expandedRight = std::clamp(savedRight > 0 ? savedRight : defaultRight,
                                         kMinRightPaneWidth,
                                         maxExpandedRight);

    lastExpandedLeftWidth = expandedLeft;
    lastExpandedRightWidth = expandedRight;

    const int left = leftPaneCollapsed ? 0 : expandedLeft;
    const int right = rightPaneCollapsed ? 0 : expandedRight;
    const int center = std::max(kMinCenterPaneWidth, usable - left - right);

    restoringLayout = true;
    if (explorerPane) {
        explorerPane->setMinimumWidth(leftPaneCollapsed ? 0 : kMinLeftPaneWidth);
    }
    if (chatPane) {
        chatPane->setMinimumWidth(rightPaneCollapsed ? 0 : kMinRightPaneWidth);
    }
    mainSplitter->setSizes({left, center, right});
    mainSplitter->setHandleCollapsedState(1, leftPaneCollapsed);
    mainSplitter->setHandleCollapsedState(2, rightPaneCollapsed);
    restoringLayout = false;
    syncPaneMenuStates();
}

void MainWindow::Impl::restoreVerticalSplitterHeights() {
    if (!chatPane || !settingsStore) {
        return;
    }
    auto* vSplit = chatPane->verticalSplitter();
    if (!vSplit || vSplit->count() < 2) {
        return;
    }
    const int totalH = std::max(vSplit->height(), owner->height() - 120);
    if (totalH < 240) {
        return;
    }

    QList<int> sizes = vSplit->sizes();
    if (sizes.size() < 2) {
        return;
    }

    const int compIdx = chatPane->composerWidget() ? vSplit->indexOf(chatPane->composerWidget()) : -1;
    const int termIdx = (terminalPanel && chatPane->isDockedTerminalVisible())
                            ? vSplit->indexOf(terminalPanel)
                            : -1;

    const int savedComposer = settingsStore->composerHeight();
    const int savedTerminal = settingsStore->dockedTerminalHeight();

    int desiredComposer = (savedComposer >= kMinComposerHeight) ? savedComposer : 148;
    int desiredTerminal = (termIdx >= 0)
                              ? ((savedTerminal >= kMinTerminalHeight) ? savedTerminal : 220)
                              : 0;

    const int maxNonTimeline = std::max(kMinComposerHeight, totalH - kMinTimelineHeight);
    if (desiredComposer + desiredTerminal > maxNonTimeline) {
        if (termIdx >= 0) {
            const int half = maxNonTimeline / 2;
            desiredComposer = std::clamp(desiredComposer, kMinComposerHeight, std::max(kMinComposerHeight, half));
            desiredTerminal = std::max(kMinTerminalHeight, maxNonTimeline - desiredComposer);
        } else {
            desiredComposer = maxNonTimeline;
        }
    }

    int assigned = 0;
    for (int i = 0; i < sizes.size(); ++i) {
        if (i == compIdx) {
            sizes[i] = desiredComposer;
            assigned += desiredComposer;
        } else if (i == termIdx) {
            sizes[i] = desiredTerminal;
            assigned += desiredTerminal;
        } else if (terminalPanel && i == vSplit->indexOf(terminalPanel) && !chatPane->isDockedTerminalVisible()) {
            sizes[i] = 0;
        } else {
            sizes[i] = -1;
        }
    }

    const int timelineH = std::max(kMinTimelineHeight, totalH - assigned);
    for (int i = 0; i < sizes.size(); ++i) {
        if (sizes[i] < 0) {
            sizes[i] = timelineH;
        }
    }

    restoringLayout = true;
    vSplit->setSizes(sizes);
    restoringLayout = false;
}

void MainWindow::Impl::persistSplitterSizes() {
    if (!settingsStore) {
        return;
    }
    if (mainSplitter) {
        const QList<int> sizes = mainSplitter->sizes();
        if (sizes.size() == 3) {
            const bool leftCollapsed = owner->isLeftPaneCollapsed();
            const bool rightCollapsed = owner->isRightPaneCollapsed();
            if (!leftCollapsed && sizes[0] >= kMinLeftPaneWidth) {
                lastExpandedLeftWidth = sizes[0];
                settingsStore->setLeftPaneWidth(sizes[0]);
            } else if (lastExpandedLeftWidth >= kMinLeftPaneWidth) {
                settingsStore->setLeftPaneWidth(lastExpandedLeftWidth);
            }
            if (!rightCollapsed && sizes[2] >= kMinRightPaneWidth) {
                lastExpandedRightWidth = sizes[2];
                settingsStore->setRightPaneWidth(sizes[2]);
            } else if (lastExpandedRightWidth >= kMinRightPaneWidth) {
                settingsStore->setRightPaneWidth(lastExpandedRightWidth);
            }
            settingsStore->setLeftPaneCollapsed(leftCollapsed);
            settingsStore->setRightPaneCollapsed(rightCollapsed);
        }
    }
    if (chatPane) {
        if (auto* vSplit = chatPane->verticalSplitter()) {
            const QList<int> vSizes = vSplit->sizes();
            if (auto* comp = chatPane->composerWidget()) {
                const int compIdx = vSplit->indexOf(comp);
                if (compIdx >= 0 && compIdx < vSizes.size() && vSizes[compIdx] >= 60) {
                    settingsStore->setComposerHeight(vSizes[compIdx]);
                }
            }
            if (terminalPanel && chatPane->isDockedTerminalVisible()) {
                const int termIdx = vSplit->indexOf(terminalPanel);
                if (termIdx >= 0 && termIdx < vSizes.size() && vSizes[termIdx] >= 60) {
                    settingsStore->setDockedTerminalHeight(vSizes[termIdx]);
                }
            }
        }
    }
}

QString MainWindow::Impl::activeProjectRoot() const {
    for (const auto& proj : lastSnapshot.projects) {
        if (proj.id == lastSnapshot.selectedProjectId && !proj.rootPath.isEmpty()) {
            return proj.rootPath;
        }
    }
    if (!lastSnapshot.projects.isEmpty() && !lastSnapshot.projects.first().rootPath.isEmpty()) {
        return lastSnapshot.projects.first().rootPath;
    }
    return QDir::homePath();
}

QPair<QString, QString> MainWindow::Impl::activeConversationTerminalTarget() const {
    QString threadId = chatPane ? chatPane->activeConversationTabId().trimmed() : QString();
    if (threadId.isEmpty()) {
        threadId = lastSnapshot.selectedThreadId.trimmed();
    }

    const ThreadSummary* activeThread = nullptr;
    if (!threadId.isEmpty()) {
        for (const auto& t : lastSnapshot.threads) {
            if (t.id == threadId) {
                activeThread = &t;
                break;
            }
        }
    }

    QString label;
    if (activeThread && !activeThread->title.trimmed().isEmpty()) {
        label = activeThread->title.trimmed();
    }

    // 1. Persisted per-conversation linked folder override.
    if (!threadId.isEmpty() && settingsStore) {
        const QString overrideDir = settingsStore->conversationLinkedFolder(threadId).trimmed();
        if (!overrideDir.isEmpty()) {
            const QFileInfo fi(overrideDir);
            if (fi.exists() && fi.isDir()) {
                return {QDir::cleanPath(overrideDir), label};
            }
        }
    }

    // 2. ThreadSummary::linkedFolder from spell_core snapshot.
    if (activeThread) {
        const QString summaryDir = activeThread->linkedFolder.trimmed();
        if (!summaryDir.isEmpty()) {
            return {QDir::cleanPath(summaryDir), label};
        }

        // 3. Owning project rootPath.
        if (!activeThread->projectId.isEmpty()) {
            for (const auto& proj : lastSnapshot.projects) {
                if (proj.id == activeThread->projectId && !proj.rootPath.trimmed().isEmpty()) {
                    if (label.isEmpty() && !proj.name.trimmed().isEmpty()) {
                        label = proj.name.trimmed();
                    }
                    return {QDir::cleanPath(proj.rootPath.trimmed()), label};
                }
            }
        }
    }

    // 4. Selected project rootPath, then first project, then home directory.
    for (const auto& proj : lastSnapshot.projects) {
        if (proj.id == lastSnapshot.selectedProjectId && !proj.rootPath.trimmed().isEmpty()) {
            if (label.isEmpty() && !proj.name.trimmed().isEmpty()) {
                label = proj.name.trimmed();
            }
            return {QDir::cleanPath(proj.rootPath.trimmed()), label};
        }
    }
    if (!lastSnapshot.projects.isEmpty() && !lastSnapshot.projects.first().rootPath.trimmed().isEmpty()) {
        const auto& firstProj = lastSnapshot.projects.first();
        if (label.isEmpty() && !firstProj.name.trimmed().isEmpty()) {
            label = firstProj.name.trimmed();
        }
        return {QDir::cleanPath(firstProj.rootPath.trimmed()), label};
    }

    return {QDir::homePath(), label};
}

bool MainWindow::Impl::isPathInReadOnlyProject(const QString& path) const {
    for (const auto& proj : lastSnapshot.projects) {
        if (isPathUnderRoot(path, proj.rootPath)) {
            return proj.isReadOnly;
        }
    }
    return false;
}

QString MainWindow::Impl::resolveCitationPath(const QString& rawPath) const {
    const QString trimmed = rawPath.trimmed();
    if (trimmed.isEmpty()) {
        return trimmed;
    }
    const QFileInfo direct(trimmed);
    if (direct.isAbsolute()) {
        return QDir::cleanPath(trimmed);
    }

    // Check active project first, then remaining workspace projects.
    const QString activeRoot = activeProjectRoot();
    if (!activeRoot.isEmpty()) {
        const QString candidate = QDir(activeRoot).filePath(trimmed);
        if (QFileInfo::exists(candidate)) {
            return QDir::cleanPath(candidate);
        }
    }
    for (const auto& proj : lastSnapshot.projects) {
        if (proj.rootPath.isEmpty()) {
            continue;
        }
        const QString candidate = QDir(proj.rootPath).filePath(trimmed);
        if (QFileInfo::exists(candidate)) {
            return QDir::cleanPath(candidate);
        }
    }
    return !activeRoot.isEmpty() ? QDir::cleanPath(QDir(activeRoot).filePath(trimmed)) : trimmed;
}

void MainWindow::Impl::ensureTerminalRunning() {
    if (terminalPanel) {
        terminalPanel->ensureShellRunning(activeProjectRoot());
    }
}

void MainWindow::Impl::restartTerminalInActiveProject() {
    if (!terminalPanel) {
        return;
    }
    terminalPanel->restartShell(activeProjectRoot());
    syncTerminalUiState();
    pushStatusNotice(MainWindow::tr("Restarted integrated terminal shell."), false);
}

void MainWindow::Impl::dockAndFocusMainTerminalInDirectory(const QString& dir) {
    if (shuttingDown || !terminalPanel || !chatPane) {
        return;
    }
    const QString targetDir = normalizeTerminalDirectory(dir, activeProjectRoot());

    if (owner->isRightPaneCollapsed()) {
        owner->toggleRightPane();
    }

    if (terminalPanel->isPoppedOut()) {
        closingTerminal = false;
        terminalPanel->dockBack();
    }

    if (!chatPane->isDockedTerminalVisible()) {
        chatPane->setDockedTerminal(terminalPanel, true);
        restoreVerticalSplitterHeights();
    }

    if (!terminalPanel->isShellRunning()) {
        terminalPanel->startShell(targetDir);
    } else if (!sameDirectoryPath(terminalPanel->workingDirectory(), targetDir)) {
        terminalPanel->restartShell(targetDir);
    }

    if (settingsStore) {
        settingsStore->setTerminalDocked(true);
    }
    syncTerminalUiState();

    owner->raise();
    owner->activateWindow();
    if (auto* surf = terminalPanel->surface()) {
        surf->setFocus(Qt::OtherFocusReason);
    }
}

void MainWindow::Impl::openProjectFolderDialog() {
    const QString startDir = !activeProjectRoot().isEmpty() ? activeProjectRoot() : QDir::homePath();
    const QString selected = QFileDialog::getExistingDirectory(
        owner,
        MainWindow::tr("Open Project Folder"),
        startDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty() || !coreClient) {
        return;
    }

    const QString cleanPath = QDir::cleanPath(selected);
    for (const auto& proj : lastSnapshot.projects) {
        if (QDir::cleanPath(proj.rootPath) == cleanPath) {
            coreClient->selectProject(proj.id);
            pushStatusNotice(MainWindow::tr("Selected project: %1").arg(proj.name), false);
            return;
        }
    }

    QString name = QFileInfo(cleanPath).fileName();
    if (name.isEmpty()) {
        name = cleanPath;
    }
    coreClient->createProject(name, cleanPath, false);
    pushStatusNotice(MainWindow::tr("Opened project: %1").arg(name), false);
}

void MainWindow::Impl::openFileDialog(const QString& rootDir) {
    QString startDir = rootDir.trimmed();
    if (!startDir.isEmpty()) {
        startDir = QDir::cleanPath(startDir);
        if (!QFileInfo::exists(startDir)) {
            startDir.clear();
        }
    }
    if (startDir.isEmpty()) {
        startDir = activeProjectRoot();
    }
    if (startDir.isEmpty() || !QFileInfo::exists(startDir)) {
        startDir = QDir::homePath();
    }
    const QString selected = QFileDialog::getOpenFileName(
        owner,
        MainWindow::tr("Open File"),
        startDir);
    if (selected.isEmpty() || !editorPane) {
        return;
    }
    const QString cleanPath = QDir::cleanPath(selected);
    sessionModifiedOpenFiles = true;
    editorPane->openFile(cleanPath, -1, isPathInReadOnlyProject(cleanPath));
}

void MainWindow::Impl::handleSnapshotUpdated(const WorkspaceSnapshot& snap) {
    if (shuttingDown) {
        return;
    }
    const bool wasBusy = lastSnapshot.isAgentBusy;
    const qsizetype prevDiffCount = lastSnapshot.pendingDiffs.size();

    bool hasNewApproval = false;
    QSet<QString> currentApprovalIds;
    for (const auto& req : snap.pendingTerminalApprovals) {
        if (!req.id.isEmpty()) {
            currentApprovalIds.insert(req.id);
            if (!seenApprovalIds.contains(req.id)) {
                hasNewApproval = true;
            }
        }
    }
    seenApprovalIds = currentApprovalIds;

    lastSnapshot = snap;

    if (explorerPane) {
        explorerPane->updateSnapshot(snap);
    }
    if (chatPane) {
        chatPane->updateSnapshot(snap);
    }
    if (terminalPanel) {
        terminalPanel->setPendingApprovals(snap.pendingTerminalApprovals);
        if (hasNewApproval && !terminalPanel->isPoppedOut() &&
            chatPane && !chatPane->isDockedTerminalVisible()) {
            if (owner->isRightPaneCollapsed()) {
                owner->toggleRightPane();
            }
            ensureTerminalRunning();
            chatPane->setDockedTerminal(terminalPanel, true);
            restoreVerticalSplitterHeights();
            syncTerminalUiState();
        }
    }
    if (settingsDialog) {
        settingsDialog->updateSnapshot(snap);
    }

    // Refresh clean open editor documents when the agent finishes a turn or modifies staged diffs.
    if (editorPane && ((wasBusy && !snap.isAgentBusy) || snap.pendingDiffs.size() != prevDiffCount)) {
        editorPane->reloadOpenDocumentsIfChanged();
    }

    syncMenuStates();
}

void MainWindow::Impl::handleTerminalSpellHandOff(const QString& instruction,
                                                  const QString& recentOutput,
                                                  const QString& cwd) {
    const QString userInstruction =
        instruction.trimmed().isEmpty()
            ? QStringLiteral("Analyze the recent terminal session and assist.")
            : instruction.trimmed();
    const QString safeCwd = cwd.trimmed().isEmpty() ? QStringLiteral("?") : cwd.trimmed();

    QString prompt = QStringLiteral("#spell %1\n\n[Terminal cwd: %2]").arg(userInstruction, safeCwd);
    const QString tail = recentOutput.trimmed();
    if (!tail.isEmpty()) {
        prompt += QStringLiteral("\n```text\n%1\n```").arg(tail);
    }

    const bool readyToSend = coreClient &&
                             lastSnapshot.endpointConfigured &&
                             !lastSnapshot.modelId.trimmed().isEmpty() &&
                             !lastSnapshot.selectedThreadId.isEmpty();
    if (readyToSend) {
        coreClient->sendPrompt(lastSnapshot.selectedThreadId, prompt);
        owner->raise();
        owner->activateWindow();
        pushStatusNotice(MainWindow::tr("Sent #spell handoff from terminal to active chat."), false);
        return;
    }

    if (chatPane) {
        if (owner->isRightPaneCollapsed()) {
            owner->toggleRightPane();
        }
        if (auto* composer = chatPane->composerWidget()) {
            composer->setInputText(prompt);
        }
        chatPane->focusComposer();
    }
    owner->raise();
    owner->activateWindow();
    if (!lastSnapshot.endpointConfigured || lastSnapshot.modelId.trimmed().isEmpty()) {
        pushStatusNotice(
            MainWindow::tr("Configure an endpoint in Settings → Engine to send #spell handoffs."), false);
    } else {
        pushStatusNotice(MainWindow::tr("Loaded #spell handoff into the chat composer."), false);
    }
}

void MainWindow::Impl::exportDiagnosticsBundle() {
    if (!settingsStore) {
        return;
    }
    const QString defaultDir =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString targetDir = QFileDialog::getExistingDirectory(
        owner,
        MainWindow::tr("Select Folder for Redacted Diagnostics Bundle"),
        defaultDir);
    if (targetDir.isEmpty()) {
        return;
    }
    const QString bundleDir =
        settingsStore->exportDiagnosticsBundle(lastSnapshot, lastSystemInfo, targetDir);
    if (bundleDir.isEmpty()) {
        pushStatusNotice(MainWindow::tr("Diagnostics export failed."), true);
        QMessageBox::warning(
            owner,
            MainWindow::tr("Diagnostics Export Failed"),
            MainWindow::tr("Could not write the redacted diagnostics bundle to the selected folder."));
        return;
    }
    pushStatusNotice(MainWindow::tr("Exported diagnostics bundle to %1").arg(bundleDir), false);
    QMessageBox::information(
        owner,
        MainWindow::tr("Diagnostics Exported"),
        MainWindow::tr("Redacted diagnostics bundle created at:\n%1\n\nNo API keys or secret tokens were included.")
            .arg(bundleDir));
}

void MainWindow::Impl::syncMenuStates() {
    const bool hasOpenDoc = editorPane && editorPane->openDocumentCount() > 0;
    const EditorDocument* currentDoc = editorPane ? editorPane->currentDocument() : nullptr;

    if (saveAction) {
        saveAction->setEnabled(currentDoc != nullptr && !currentDoc->isReadOnly() && currentDoc->isAvailable());
    }
    if (saveAllAction) {
        saveAllAction->setEnabled(hasOpenDoc);
    }
    if (closeTabAction) {
        closeTabAction->setEnabled(currentDoc != nullptr);
    }
    if (askModeAction) {
        bool activeIsAsk = false;
        for (const auto& t : lastSnapshot.threads) {
            if (t.id == lastSnapshot.selectedThreadId) {
                activeIsAsk = (t.profile.compare(QLatin1StringView("ask"), Qt::CaseInsensitive) == 0);
                break;
            }
        }
        const bool prevBlock = askModeAction->blockSignals(true);
        askModeAction->setEnabled(!lastSnapshot.selectedThreadId.isEmpty());
        askModeAction->setChecked(activeIsAsk);
        askModeAction->blockSignals(prevBlock);
    }
    if (stageWriteAction) {
        const bool prevBlock = stageWriteAction->blockSignals(true);
        stageWriteAction->setChecked(lastSnapshot.stageWriteMode);
        stageWriteAction->blockSignals(prevBlock);
    }
    if (stopGenerationAction) {
        const bool busy = lastSnapshot.isAgentBusy || (coreClient && coreClient->isBusy());
        stopGenerationAction->setEnabled(busy);
    }
    syncPaneMenuStates();
}

void MainWindow::Impl::syncPaneMenuStates() {
    if (toggleProjectsAction) {
        const bool prevBlock = toggleProjectsAction->blockSignals(true);
        toggleProjectsAction->setChecked(!owner->isLeftPaneCollapsed());
        toggleProjectsAction->blockSignals(prevBlock);
    }
    if (toggleChatAction) {
        const bool prevBlock = toggleChatAction->blockSignals(true);
        toggleChatAction->setChecked(!owner->isRightPaneCollapsed());
        toggleChatAction->blockSignals(prevBlock);
    }
}

void MainWindow::Impl::syncTerminalUiState() {
    const bool dockedVisible = chatPane && chatPane->isDockedTerminalVisible();
    const bool poppedOut = terminalPanel && terminalPanel->isPoppedOut();
    const bool running = terminalPanel && terminalPanel->isShellRunning();

    if (toggleTerminalAction) {
        const bool prevBlock = toggleTerminalAction->blockSignals(true);
        toggleTerminalAction->setChecked(dockedVisible);
        toggleTerminalAction->blockSignals(prevBlock);
    }
    if (popOutTerminalAction) {
        const bool prevBlock = popOutTerminalAction->blockSignals(true);
        popOutTerminalAction->setChecked(poppedOut);
        popOutTerminalAction->setText(poppedOut ? MainWindow::tr("Dock Terminal Back")
                                                : MainWindow::tr("Pop Out Terminal"));
        popOutTerminalAction->blockSignals(prevBlock);
    }
    if (terminalToggleMenuAction) {
        terminalToggleMenuAction->setText(dockedVisible ? MainWindow::tr("Hide Docked Terminal")
                                                        : MainWindow::tr("Show Docked Terminal"));
    }
    if (terminalPopOutMenuAction) {
        terminalPopOutMenuAction->setText(poppedOut ? MainWindow::tr("Dock Terminal in Chat")
                                                    : MainWindow::tr("Pop Out Terminal Window"));
    }
    if (terminalStopAction) {
        terminalStopAction->setEnabled(running);
    }
    if (chatPane) {
        if (auto* composer = chatPane->composerWidget()) {
            composer->setTerminalVisible(dockedVisible || poppedOut);
        }
    }
    if (settingsDialog) {
        settingsDialog->setTerminalState(dockedVisible, poppedOut);
    }
}

void MainWindow::Impl::syncThemeMenuStates() {
    const auto& tm = ThemeManager::instance();
    if (darkAppearanceAction && lightAppearanceAction) {
        const bool darkBlock = darkAppearanceAction->blockSignals(true);
        const bool lightBlock = lightAppearanceAction->blockSignals(true);
        darkAppearanceAction->setChecked(tm.appearance() == Appearance::Dark);
        lightAppearanceAction->setChecked(tm.appearance() == Appearance::Light);
        darkAppearanceAction->blockSignals(darkBlock);
        lightAppearanceAction->blockSignals(lightBlock);
    }
    for (const auto& pair : codeThemeActions) {
        if (pair.second) {
            const bool prev = pair.second->blockSignals(true);
            pair.second->setChecked(tm.codeTheme() == pair.first);
            pair.second->blockSignals(prev);
        }
    }
}

void MainWindow::Impl::updateWindowTitle() {
    const QString filePath = editorPane ? editorPane->currentFilePath() : QString();
    // The native title text is hidden: content runs flush to the top edge.
    Q_UNUSED(filePath);
    owner->setWindowTitle(QStringLiteral(" "));
}

void MainWindow::Impl::pushStatusNotice(const QString& message, bool isError) {
    if (explorerPane && !message.isEmpty()) {
        explorerPane->pushStatusNotice(message, isError);
    }
}

void MainWindow::Impl::shutdownResources() {
    if (resourcesStopped) {
        return;
    }
    resourcesStopped = true;

    if (coreClient) {
        coreClient->stopBackgroundPollers();
    }
    if (settingsDialog) {
        settingsDialog->close();
    }
    if (terminalPanel) {
        if (QWidget* popWin = terminalPanel->popOutWindow()) {
            popWin->close();
        }
        terminalPanel->stopShell();
    }

    const QList<QPointer<QMainWindow>> popOutWindows = folderTerminalWindows.values();
    folderTerminalWindows.clear();
    for (const QPointer<QMainWindow>& winPtr : popOutWindows) {
        if (QMainWindow* win = winPtr.data()) {
            if (auto* panel = qobject_cast<SpellTerminalPanel*>(win->centralWidget())) {
                panel->disconnect(owner);
                panel->stopShell();
            }
            win->close();
        }
    }
}

// =============================================================================
// MainWindow public & protected API
// =============================================================================

MainWindow::MainWindow(const QString& dbPath, QWidget* parent)
    : QMainWindow(parent)
    , m_impl(std::make_unique<Impl>()) {
    registerSpellMetaTypesOnce();

    m_impl->owner = this;
    m_impl->isInMemoryDb = (dbPath.trimmed() == QLatin1StringView(":memory:"));

    setObjectName(QStringLiteral("SpellMainWindow"));
    setWindowTitle(QStringLiteral(" "));
    setWindowIcon(icons::appWindowIcon());
    setMinimumSize(kMinWindowWidth, kMinWindowHeight);
    setStatusBar(nullptr);
    setUnifiedTitleAndToolBarOnMac(false);
    installFramelessTitlebarChrome(this);

    // Core async client & persistent preferences store
    m_impl->coreClient = new SpellCoreClient(dbPath, this);
    m_impl->settingsStore = new SpellSettingsStore(m_impl->coreClient, this);

    // Build layout, native menus, and connect all signals
    m_impl->buildWorkspace();
    m_impl->buildMenus();
    m_impl->wireSignals();
    installFramelessTitlebarChrome(this); // re-apply now that the central widget exists
    m_impl->restoreLayoutAndSession();

    applyTheme();
    m_impl->syncMenuStates();
    m_impl->syncTerminalUiState();

    // Start zero-network local pollers, load initial snapshot & host system info
    m_impl->coreClient->startBackgroundPollers();
    m_impl->coreClient->refreshSnapshot();

    QPointer<MainWindow> safeSelf(this);
    m_impl->coreClient->fetchSystemInfo([safeSelf](SystemInfo info) {
        if (!safeSelf || !safeSelf->m_impl || safeSelf->m_impl->shuttingDown) {
            return;
        }
        safeSelf->m_impl->lastSystemInfo = info;
        safeSelf->m_impl->hasSystemInfo = true;
        if (safeSelf->m_impl->settingsDialog) {
            safeSelf->m_impl->settingsDialog->updateSystemInfo(info);
        }
    });

    // Load OS keyring credentials asynchronously; push saved endpoint configuration
    // into spell_core for persistent sessions (performs zero network I/O).
    m_impl->settingsStore->loadAllSecrets([safeSelf]() {
        if (!safeSelf || !safeSelf->m_impl || safeSelf->m_impl->shuttingDown) {
            return;
        }
        auto& impl = *safeSelf->m_impl;
        if (impl.isInMemoryDb || !impl.coreClient || !impl.settingsStore) {
            return;
        }
        const QString savedUrl = impl.settingsStore->endpointBaseUrl().trimmed();
        const QString savedModel = impl.settingsStore->modelId().trimmed();
        const QString savedSubModel = impl.settingsStore->subagentModelId().trimmed();
        const QString apiKey = impl.settingsStore->cachedEndpointApiKey();
        if (!savedUrl.isEmpty()) {
            const std::optional<QString> keyOpt =
                apiKey.isEmpty() ? std::nullopt : std::optional<QString>(apiKey);
            impl.coreClient->configureEndpoint(savedUrl, keyOpt, savedModel, savedSubModel);
            impl.coreClient->refreshEndpointStatus();
        }
        impl.coreClient->setSlot2CriticEnabled(impl.settingsStore->slot2CriticEnabled());
    });
}

MainWindow::~MainWindow() {
    if (m_impl) {
        m_impl->shuttingDown = true;
        // Disconnect child emitters before QObject teardown so no signal reaches destroyed m_impl.
        if (m_impl->coreClient) {
            m_impl->coreClient->disconnect(this);
        }
        if (m_impl->explorerPane) {
            m_impl->explorerPane->disconnect(this);
        }
        if (m_impl->editorPane) {
            m_impl->editorPane->disconnect(this);
        }
        if (m_impl->chatPane) {
            m_impl->chatPane->disconnect(this);
            if (auto* vSplit = m_impl->chatPane->verticalSplitter()) {
                vSplit->disconnect(this);
            }
        }
        if (m_impl->terminalPanel) {
            m_impl->terminalPanel->disconnect(this);
        }
        if (m_impl->settingsDialog) {
            m_impl->settingsDialog->disconnect(this);
        }
        if (m_impl->mainSplitter) {
            m_impl->mainSplitter->disconnect(this);
        }
        ThemeManager::instance().disconnect(this);
        m_impl->shutdownResources();
    }
}

void MainWindow::openSettings(SettingsPaneId pane) {
    if (!m_impl || m_impl->shuttingDown) {
        return;
    }

    if (!m_impl->settingsDialog) {
        m_impl->settingsDialog = new SettingsDialog(m_impl->coreClient, m_impl->settingsStore, this);
        m_impl->settingsDialog->setWindowIcon(icons::appWindowIcon());

        connect(m_impl->settingsDialog, &SettingsDialog::toggleDockedTerminalRequested, this, [this]() {
            toggleDockedTerminal();
        });
        connect(m_impl->settingsDialog, &SettingsDialog::togglePopOutTerminalRequested, this, [this]() {
            togglePopOutTerminal();
        });
        connect(m_impl->settingsDialog, &SettingsDialog::clearTerminalRequested, this, [this]() {
            if (m_impl && m_impl->terminalPanel) {
                m_impl->terminalPanel->clearTerminal();
            }
        });
        connect(m_impl->settingsDialog, &SettingsDialog::restartTerminalRequested, this, [this]() {
            if (m_impl) {
                m_impl->restartTerminalInActiveProject();
            }
        });

        m_impl->settingsDialog->updateSnapshot(m_impl->lastSnapshot);
        if (m_impl->hasSystemInfo) {
            m_impl->settingsDialog->updateSystemInfo(m_impl->lastSystemInfo);
        }
        m_impl->settingsDialog->setTerminalState(
            m_impl->chatPane && m_impl->chatPane->isDockedTerminalVisible(),
            m_impl->terminalPanel && m_impl->terminalPanel->isPoppedOut());
        m_impl->settingsDialog->applyTheme();
    }

    if (!m_impl->hasSystemInfo && m_impl->coreClient) {
        QPointer<MainWindow> safeSelf(this);
        m_impl->coreClient->fetchSystemInfo([safeSelf](SystemInfo info) {
            if (!safeSelf || !safeSelf->m_impl || safeSelf->m_impl->shuttingDown) {
                return;
            }
            safeSelf->m_impl->lastSystemInfo = info;
            safeSelf->m_impl->hasSystemInfo = true;
            if (safeSelf->m_impl->settingsDialog) {
                safeSelf->m_impl->settingsDialog->updateSystemInfo(info);
            }
        });
    }

    m_impl->settingsDialog->selectPane(pane);
    m_impl->settingsDialog->show();
    m_impl->settingsDialog->raise();
    m_impl->settingsDialog->activateWindow();
}

void MainWindow::applyTheme() {
    if (!m_impl || m_impl->shuttingDown) {
        return;
    }

    if (m_impl->mainSplitter) {
        m_impl->mainSplitter->applyTheme();
    }
    if (m_impl->explorerPane) {
        m_impl->explorerPane->applyTheme();
    }
    if (m_impl->editorPane) {
        m_impl->editorPane->applyTheme();
    }
    if (m_impl->chatPane) {
        m_impl->chatPane->applyTheme();
    }
    if (m_impl->terminalPanel) {
        m_impl->terminalPanel->applyTheme();
    }
    if (m_impl->settingsDialog) {
        m_impl->settingsDialog->applyTheme();
    }
    for (const QPointer<QMainWindow>& winPtr : std::as_const(m_impl->folderTerminalWindows)) {
        if (QMainWindow* win = winPtr.data()) {
            auto* panel = qobject_cast<SpellTerminalPanel*>(win->centralWidget());
            applyFolderTerminalWindowTheme(win, panel);
        }
    }

    m_impl->syncThemeMenuStates();
    m_impl->syncTerminalUiState();
}

void MainWindow::toggleDockedTerminal() {
    if (!m_impl || m_impl->shuttingDown || !m_impl->terminalPanel || !m_impl->chatPane) {
        return;
    }

    if (m_impl->terminalPanel->isPoppedOut()) {
        // Dock back into AgentChatPane and show it.
        if (isRightPaneCollapsed()) {
            toggleRightPane();
        }
        m_impl->closingTerminal = false;
        m_impl->terminalPanel->dockBack();
        if (!m_impl->chatPane->isDockedTerminalVisible()) {
            m_impl->chatPane->setDockedTerminal(m_impl->terminalPanel, true);
            m_impl->restoreVerticalSplitterHeights();
        }
        m_impl->ensureTerminalRunning();
        if (auto* surface = m_impl->terminalPanel->surface()) {
            surface->setFocus();
        }
        if (m_impl->settingsStore) {
            m_impl->settingsStore->setTerminalDocked(true);
        }
        m_impl->syncTerminalUiState();
        return;
    }

    const bool currentlyVisible = m_impl->chatPane->isDockedTerminalVisible();
    if (currentlyVisible) {
        m_impl->persistSplitterSizes();
        m_impl->chatPane->setDockedTerminal(m_impl->terminalPanel, false);
    } else {
        if (isRightPaneCollapsed()) {
            toggleRightPane();
        }
        m_impl->ensureTerminalRunning();
        m_impl->chatPane->setDockedTerminal(m_impl->terminalPanel, true);
        m_impl->restoreVerticalSplitterHeights();
        if (auto* surface = m_impl->terminalPanel->surface()) {
            surface->setFocus();
        }
        if (m_impl->settingsStore) {
            m_impl->settingsStore->setTerminalDocked(true);
        }
    }
    m_impl->syncTerminalUiState();
}

void MainWindow::togglePopOutTerminal() {
    if (!m_impl || m_impl->shuttingDown || !m_impl->terminalPanel) {
        return;
    }

    if (m_impl->terminalPanel->isPoppedOut()) {
        m_impl->closingTerminal = false;
        m_impl->terminalPanel->dockBack();
    } else {
        m_impl->persistSplitterSizes();
        m_impl->ensureTerminalRunning();
        m_impl->terminalPanel->popOut();
    }
    m_impl->syncTerminalUiState();
}

void MainWindow::toggleLeftPane() {
    if (!m_impl || m_impl->shuttingDown || !m_impl->mainSplitter || !m_impl->explorerPane) {
        return;
    }
    if (m_impl->mainSplitter->count() < 3) {
        return;
    }

    const QList<int> sizes = m_impl->mainSplitter->sizes();
    const bool hasLiveSizes = (sizes.size() == 3 && (sizes[0] + sizes[1] + sizes[2]) > 0);
    const bool currentlyCollapsed = isLeftPaneCollapsed();

    m_impl->userDraggedHorizontal = true;
    m_impl->restoringLayout = true;

    if (!currentlyCollapsed) {
        if (hasLiveSizes && sizes[0] > 0) {
            m_impl->lastExpandedLeftWidth = sizes[0];
            if (m_impl->settingsStore && sizes[0] >= kMinLeftPaneWidth) {
                m_impl->settingsStore->setLeftPaneWidth(sizes[0]);
            }
        }
        m_impl->explorerPane->setMinimumWidth(0);
        if (hasLiveSizes) {
            m_impl->mainSplitter->setSizes({0, sizes[1] + sizes[0], sizes[2]});
        } else {
            const int total = std::max(m_impl->mainSplitter->width(), width());
            const int right = m_impl->rightPaneCollapsed
                                  ? 0
                                  : std::max(kMinRightPaneWidth, m_impl->lastExpandedRightWidth);
            const int center = std::max(kMinCenterPaneWidth, total - right);
            m_impl->mainSplitter->setSizes({0, center, right});
        }
        m_impl->mainSplitter->setHandleCollapsedState(1, true);
        m_impl->leftPaneCollapsed = true;
        if (m_impl->settingsStore) {
            m_impl->settingsStore->setLeftPaneCollapsed(true);
        }
    } else {
        m_impl->explorerPane->setMinimumWidth(kMinLeftPaneWidth);
        int w = std::max(200, m_impl->lastExpandedLeftWidth);
        if (hasLiveSizes) {
            if (sizes[1] - w < kMinCenterPaneWidth) {
                w = std::clamp(w,
                               kMinLeftPaneWidth,
                               std::max(kMinLeftPaneWidth, sizes[1] - kMinCenterPaneWidth));
            }
            const int center = std::max(kMinCenterPaneWidth, sizes[1] - w);
            m_impl->mainSplitter->setSizes({w, center, sizes[2]});
        } else {
            const int total = std::max(m_impl->mainSplitter->width(), width());
            const int right = m_impl->rightPaneCollapsed
                                  ? 0
                                  : std::max(kMinRightPaneWidth, m_impl->lastExpandedRightWidth);
            const int center = std::max(kMinCenterPaneWidth, total - w - right);
            m_impl->mainSplitter->setSizes({w, center, right});
        }
        m_impl->mainSplitter->setHandleCollapsedState(1, false);
        m_impl->leftPaneCollapsed = false;
        if (m_impl->settingsStore) {
            m_impl->settingsStore->setLeftPaneCollapsed(false);
        }
    }

    m_impl->restoringLayout = false;
    m_impl->syncPaneMenuStates();
}

void MainWindow::toggleRightPane() {
    if (!m_impl || m_impl->shuttingDown || !m_impl->mainSplitter || !m_impl->chatPane) {
        return;
    }
    if (m_impl->mainSplitter->count() < 3) {
        return;
    }

    const QList<int> sizes = m_impl->mainSplitter->sizes();
    const bool hasLiveSizes = (sizes.size() == 3 && (sizes[0] + sizes[1] + sizes[2]) > 0);
    const bool currentlyCollapsed = isRightPaneCollapsed();

    m_impl->userDraggedHorizontal = true;
    m_impl->restoringLayout = true;

    if (!currentlyCollapsed) {
        if (hasLiveSizes && sizes[2] > 0) {
            m_impl->lastExpandedRightWidth = sizes[2];
            if (m_impl->settingsStore && sizes[2] >= kMinRightPaneWidth) {
                m_impl->settingsStore->setRightPaneWidth(sizes[2]);
            }
        }
        m_impl->chatPane->setMinimumWidth(0);
        if (hasLiveSizes) {
            m_impl->mainSplitter->setSizes({sizes[0], sizes[1] + sizes[2], 0});
        } else {
            const int total = std::max(m_impl->mainSplitter->width(), width());
            const int left = m_impl->leftPaneCollapsed
                                 ? 0
                                 : std::max(200, m_impl->lastExpandedLeftWidth);
            const int center = std::max(kMinCenterPaneWidth, total - left);
            m_impl->mainSplitter->setSizes({left, center, 0});
        }
        m_impl->mainSplitter->setHandleCollapsedState(2, true);
        m_impl->rightPaneCollapsed = true;
        if (m_impl->settingsStore) {
            m_impl->settingsStore->setRightPaneCollapsed(true);
        }
    } else {
        m_impl->chatPane->setMinimumWidth(kMinRightPaneWidth);
        int w = std::max(kMinRightPaneWidth, m_impl->lastExpandedRightWidth);
        if (hasLiveSizes) {
            if (sizes[1] - w < kMinCenterPaneWidth) {
                w = std::clamp(w,
                               kMinRightPaneWidth,
                               std::max(kMinRightPaneWidth, sizes[1] - kMinCenterPaneWidth));
            }
            const int center = std::max(kMinCenterPaneWidth, sizes[1] - w);
            m_impl->mainSplitter->setSizes({sizes[0], center, w});
        } else {
            const int total = std::max(m_impl->mainSplitter->width(), width());
            const int left = m_impl->leftPaneCollapsed
                                 ? 0
                                 : std::max(200, m_impl->lastExpandedLeftWidth);
            const int center = std::max(kMinCenterPaneWidth, total - left - w);
            m_impl->mainSplitter->setSizes({left, center, w});
        }
        m_impl->mainSplitter->setHandleCollapsedState(2, false);
        m_impl->rightPaneCollapsed = false;
        if (m_impl->settingsStore) {
            m_impl->settingsStore->setRightPaneCollapsed(false);
        }
    }

    m_impl->restoringLayout = false;
    m_impl->syncPaneMenuStates();
}

bool MainWindow::isLeftPaneCollapsed() const {
    if (!m_impl) {
        return false;
    }
    if (m_impl->mainSplitter && m_impl->mainSplitter->count() >= 3) {
        const QList<int> sizes = m_impl->mainSplitter->sizes();
        if (sizes.size() == 3 && (sizes[0] + sizes[1] + sizes[2]) > 0) {
            return sizes[0] == 0;
        }
    }
    return m_impl->leftPaneCollapsed;
}

bool MainWindow::isRightPaneCollapsed() const {
    if (!m_impl) {
        return false;
    }
    if (m_impl->mainSplitter && m_impl->mainSplitter->count() >= 3) {
        const QList<int> sizes = m_impl->mainSplitter->sizes();
        if (sizes.size() == 3 && (sizes[0] + sizes[1] + sizes[2]) > 0) {
            return sizes[2] == 0;
        }
    }
    return m_impl->rightPaneCollapsed;
}

void MainWindow::openPopOutTerminalForDirectory(const QString& workingDir, const QString& label) {
    if (!m_impl || m_impl->shuttingDown) {
        return;
    }

    const QString normalizedDir = normalizeTerminalDirectory(workingDir, m_impl->activeProjectRoot());
    const QString key = folderTerminalHashKey(normalizedDir);

    if (auto it = m_impl->folderTerminalWindows.find(key); it != m_impl->folderTerminalWindows.end()) {
        if (QMainWindow* existing = it.value().data()) {
            existing->show();
            existing->raise();
            existing->activateWindow();
            if (auto* existingPanel = qobject_cast<SpellTerminalPanel*>(existing->centralWidget())) {
                if (auto* surf = existingPanel->surface()) {
                    surf->setFocus(Qt::OtherFocusReason);
                }
            }
            return;
        }
        m_impl->folderTerminalWindows.erase(it);
    }

    const QString cleanLabel = label.trimmed();
    QString dirName = QFileInfo(normalizedDir).fileName();
    if (dirName.isEmpty()) {
        dirName = normalizedDir;
    }
    const QString windowTitle =
        (!cleanLabel.isEmpty() && cleanLabel != dirName)
            ? QStringLiteral("Message Terminal \u2014 %1 (%2)").arg(cleanLabel, dirName)
            : QStringLiteral("Message Terminal \u2014 %1").arg(cleanLabel.isEmpty() ? dirName : cleanLabel);

    auto* popWin = new FolderTerminalWindow(nullptr);
    popWin->setObjectName(QStringLiteral("SpellFolderTerminalWindow"));
    popWin->setStatusBar(nullptr);
    popWin->setAttribute(Qt::WA_DeleteOnClose, true);
    popWin->setAttribute(Qt::WA_StyledBackground, true);
    popWin->setAutoFillBackground(true);
    popWin->setWindowTitle(windowTitle);
    popWin->setWindowIcon(icons::appWindowIcon());
    popWin->setMinimumSize(480, 260);
    popWin->resize(780, 500);
    installTerminalWindowControls(popWin);

    auto* panel = new SpellTerminalPanel(nullptr, popWin);
    panel->setObjectName(QStringLiteral("SpellFolderTerminalPanel"));
    popWin->setCentralWidget(panel);

    applyFolderTerminalWindowTheme(popWin, panel);

    QPointer<MainWindow> safeOwner(this);
    QPointer<QMainWindow> safePopWin(popWin);
    QPointer<SpellTerminalPanel> safePanel(panel);

    popWin->onClosing = [safeOwner, safePopWin, safePanel, key]() {
        if (safePanel) {
            safePanel->stopShell();
        }
        if (safeOwner && safeOwner->m_impl) {
            auto it = safeOwner->m_impl->folderTerminalWindows.find(key);
            if (it != safeOwner->m_impl->folderTerminalWindows.end() &&
                (it.value().isNull() || it.value().data() == safePopWin.data())) {
                safeOwner->m_impl->folderTerminalWindows.erase(it);
            }
        }
    };

    connect(popWin, &QObject::destroyed, this, [safeOwner, key](QObject* destroyedObj) {
        if (!safeOwner || !safeOwner->m_impl) {
            return;
        }
        auto it = safeOwner->m_impl->folderTerminalWindows.find(key);
        if (it != safeOwner->m_impl->folderTerminalWindows.end() &&
            (it.value().isNull() || static_cast<QObject*>(it.value().data()) == destroyedObj)) {
            safeOwner->m_impl->folderTerminalWindows.erase(it);
        }
    });

    connect(panel, &SpellTerminalPanel::closeRequested, popWin, [safePopWin]() {
        if (safePopWin) {
            safePopWin->close();
        }
    });

    connect(panel, &SpellTerminalPanel::popOutToggled, popWin,
            [safeOwner, safePopWin, safePanel, normalizedDir](bool poppedOut) {
                if (!safeOwner || !safeOwner->m_impl || safeOwner->m_impl->shuttingDown) {
                    return;
                }
                if (poppedOut) {
                    if (safePanel) {
                        safePanel->dockBack();
                    }
                    return;
                }
                safeOwner->m_impl->dockAndFocusMainTerminalInDirectory(normalizedDir);
                if (safePopWin) {
                    QMetaObject::invokeMethod(safePopWin.data(), &QWidget::close, Qt::QueuedConnection);
                }
            });

    connect(panel, &SpellTerminalPanel::spellHandOffRequested, this,
            [this](const QString& instruction, const QString& recentOutput, const QString& cwd) {
                if (m_impl && !m_impl->shuttingDown) {
                    m_impl->handleTerminalSpellHandOff(instruction, recentOutput, cwd);
                }
            });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, popWin,
            [safePopWin, safePanel]() {
                applyFolderTerminalWindowTheme(safePopWin.data(), safePanel.data());
            });

    m_impl->folderTerminalWindows.insert(key, popWin);

    if (auto* surf = panel->surface()) {
        surf->startShell(normalizedDir);
    }

    popWin->show();
    popWin->raise();
    popWin->activateWindow();
    if (auto* surf = panel->surface()) {
        surf->setFocus(Qt::OtherFocusReason);
    }
}

int MainWindow::poppedOutFolderTerminalCount() const {
    if (!m_impl) {
        return 0;
    }
    int count = 0;
    for (const QPointer<QMainWindow>& winPtr : std::as_const(m_impl->folderTerminalWindows)) {
        if (!winPtr.isNull()) {
            ++count;
        }
    }
    return count;
}

SpellSplitter* MainWindow::mainSplitter() const {
    return m_impl ? m_impl->mainSplitter.data() : nullptr;
}

SpellCoreClient* MainWindow::coreClient() const {
    return m_impl ? m_impl->coreClient.data() : nullptr;
}

SpellSettingsStore* MainWindow::settingsStore() const {
    return m_impl ? m_impl->settingsStore.data() : nullptr;
}

CodeEditorPane* MainWindow::editorPane() const {
    return m_impl ? m_impl->editorPane.data() : nullptr;
}

AgentChatPane* MainWindow::chatPane() const {
    return m_impl ? m_impl->chatPane.data() : nullptr;
}

ProjectExplorerPane* MainWindow::explorerPane() const {
    return m_impl ? m_impl->explorerPane.data() : nullptr;
}

SpellTerminalPanel* MainWindow::terminalPanel() const {
    return m_impl ? m_impl->terminalPanel.data() : nullptr;
}

SettingsDialog* MainWindow::settingsDialog() const {
    return m_impl ? m_impl->settingsDialog.data() : nullptr;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (m_impl) {
        m_impl->shuttingDown = true;

        if (m_impl->settingsStore) {
            m_impl->settingsStore->setWindowGeometry(saveGeometry());
            m_impl->persistSplitterSizes();
            if ((!m_impl->isInMemoryDb || m_impl->sessionModifiedOpenFiles) && m_impl->editorPane) {
                m_impl->settingsStore->setOpenFiles(m_impl->editorPane->openFilePaths());
            }
            m_impl->settingsStore->sync();
        }

        m_impl->shutdownResources();
    }
    QMainWindow::closeEvent(event);
}

} // namespace spell
