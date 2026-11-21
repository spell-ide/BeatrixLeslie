#include "shell/WindowChrome.h"
#include <QShortcut>
#include "core/SpellCoreClient.h"
#include "core/SpellCitationLinkifier.h"
#include "core/SpellModels.h"
#include "core/SpellSettingsStore.h"
#include "panes/AgentChatPane.h"
#include "panes/ChatTimelineWidget.h"
#include "panes/CodeEditorPane.h"
#include "panes/ComposerWidget.h"
#include "panes/EditorDocument.h"
#include "panes/ProjectExplorerPane.h"
#include "panes/RustCustomLexer.h"
#include "settings/SettingsDialog.h"
#include "shell/MainWindow.h"
#include "terminal/SpellTerminalEmulator.h"
#include "terminal/SpellTerminalPanel.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellSplitHandle.h"

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QObject>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUrl>
#include <QVector>
#include <QWidget>
#include <QtTest>

#include <Qsci/qsciscintilla.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace {

bool writeUtf8File(const QString& filePath, const QString& contents) {
    const QFileInfo fi(filePath);
    if (!fi.dir().exists() && !QDir().mkpath(fi.absolutePath())) {
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    const QByteArray encoded = contents.toUtf8();
    const qint64 written = file.write(encoded);
    file.close();
    return written == encoded.size();
}

std::optional<QString> readUtf8File(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;
    }
    return QString::fromUtf8(file.readAll());
}

std::optional<spell::ProjectInfo> findProjectByName(const spell::WorkspaceSnapshot& snap,
                                                    const QString& name) {
    for (const auto& proj : snap.projects) {
        if (proj.name == name) {
            return proj;
        }
    }
    return std::nullopt;
}

std::optional<spell::ThreadSummary> findThreadByTitle(const spell::WorkspaceSnapshot& snap,
                                                      const QString& title) {
    for (const auto& thread : snap.threads) {
        if (thread.title == title) {
            return thread;
        }
    }
    return std::nullopt;
}

std::optional<spell::AgentTimer> findActiveTimerByLabel(const spell::WorkspaceSnapshot& snap,
                                                        const QString& label) {
    for (const auto& timer : snap.activeTimers) {
        if (timer.label == label &&
            timer.status != QLatin1StringView("cancelled") &&
            !timer.delivered) {
            return timer;
        }
    }
    return std::nullopt;
}

bool hasActiveTimerById(const spell::WorkspaceSnapshot& snap, const QString& timerId) {
    for (const auto& timer : snap.activeTimers) {
        if (timer.id == timerId &&
            timer.status != QLatin1StringView("cancelled") &&
            !timer.delivered) {
            return true;
        }
    }
    return false;
}

} // namespace

class SpellQtTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testThemePalettesAndAllCodeThemes();
    void testModelsSnapshotJsonDecoding();
    void testCitationLinkifier();
    void testTerminalEmulatorAnsiAnd24BitColor();
    void testScintillaEditorAndRustLexer();
    void testSettingsDialogAllElevenPanes();
    void testCoreClientInMemoryFfiRoundtrip();
    void testMainWindowInstantiationAndSplitters();
    void testChalkSplitterAutoHideAndPaneToggle();
    void testPerConversationLinkedFolderTerminalPopOut();
    void testStrictMinimalistUxRules();

private:
    QTemporaryDir m_isolatedSettingsDir;
};

void SpellQtTests::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("Spell"));
    QCoreApplication::setApplicationName(QStringLiteral("SpellIDE"));

    QVERIFY2(m_isolatedSettingsDir.isValid(), "Failed to create isolated QSettings temp dir");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_isolatedSettingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, m_isolatedSettingsDir.path());

    qRegisterMetaType<spell::WorkspaceSnapshot>();
    qRegisterMetaType<spell::ModelDescriptor>();
    qRegisterMetaType<spell::TelemetrySnapshot>();
    qRegisterMetaType<spell::AgentTimer>();
    qRegisterMetaType<spell::StagedDiff>();
    qRegisterMetaType<spell::TerminalApprovalRequest>();
    qRegisterMetaType<spell::SettingsPaneId>();
}

void SpellQtTests::cleanupTestCase() {
    auto& tm = spell::ThemeManager::instance();
    tm.setAppearance(spell::Appearance::Dark);
    tm.setCodeTheme(spell::CodeThemeId::OneDark);
    tm.setTerminalTheme(spell::TerminalThemeId::MessageTerminal);
    tm.setEditorFontSize(13.0);
    tm.setTerminalFontSize(12.5);
    QCoreApplication::processEvents();
}

void SpellQtTests::testThemePalettesAndAllCodeThemes() {
    using namespace spell;

    // 1. Dark monochrome UI palette (#000000 canvas, #FFFFFF foreground, semantic diff/error accents)
    const UiPalette dark = UiPalette::forAppearance(Appearance::Dark);
    QCOMPARE(dark.background, rgbHex(0x000000));
    QCOMPARE(dark.surface, rgbHex(0x000000));
    QCOMPARE(dark.textPrimary, rgbHex(0xFFFFFF));
    QCOMPARE(dark.accent, rgbHex(0xFFFFFF));
    QCOMPARE(dark.diffAdded, rgbHex(0x3FB950));
    QCOMPARE(dark.diffRemoved, rgbHex(0xF85149));
    QCOMPARE(dark.danger, rgbHex(0xF85149));
    QCOMPARE(ThemeManager::appearanceName(Appearance::Dark), QStringLiteral("Dark"));

    // 2. White monochrome UI palette (#FFFFFF canvas, #000000 foreground)
    const UiPalette light = UiPalette::forAppearance(Appearance::Light);
    QCOMPARE(light.background, rgbHex(0xFFFFFF));
    QCOMPARE(light.surface, rgbHex(0xFFFFFF));
    QCOMPARE(light.textPrimary, rgbHex(0x000000));
    QCOMPARE(light.accent, rgbHex(0x000000));
    QCOMPARE(light.diffAdded, rgbHex(0x1A7F37));
    QCOMPARE(light.diffRemoved, rgbHex(0xCF222E));
    QCOMPARE(light.danger, rgbHex(0xCF222E));
    QCOMPARE(ThemeManager::appearanceName(Appearance::Light), QStringLiteral("White"));

    // Color utility invariants
    QCOMPARE(qssColor(rgbHex(0x000000)), QStringLiteral("#000000"));
    QCOMPARE(qssColor(rgbHex(0xFFFFFF)), QStringLiteral("#ffffff"));
    QVERIFY(qssColor(withAlpha(rgbHex(0xFFFFFF), 0.5)).startsWith(QStringLiteral("rgba(")));
    QCOMPARE(compositeOver(withAlpha(rgbHex(0xFFFFFF), 0.5), rgbHex(0x000000)).alpha(), 255);

    // 3. All 8 syntax code themes
    const QVector<CodeThemeId> codeThemes = ThemeManager::allCodeThemes();
    QCOMPARE(codeThemes.size(), 8);

    QVERIFY(!isLightCodeTheme(CodeThemeId::OneDark));
    QVERIFY(!isLightCodeTheme(CodeThemeId::GitHubDark));
    QVERIFY(!isLightCodeTheme(CodeThemeId::Dracula));
    QVERIFY(!isLightCodeTheme(CodeThemeId::Monokai));
    QVERIFY(!isLightCodeTheme(CodeThemeId::SolarizedDark));
    QVERIFY(isLightCodeTheme(CodeThemeId::GitHubLight));
    QVERIFY(isLightCodeTheme(CodeThemeId::XcodeLight));
    QVERIFY(isLightCodeTheme(CodeThemeId::SolarizedLight));

    for (const CodeThemeId id : codeThemes) {
        QVERIFY(!ThemeManager::codeThemeName(id).isEmpty());
        for (const Appearance app : {Appearance::Dark, Appearance::Light}) {
            const CodePalette cp = codePaletteFor(id, app);
            QVERIFY(cp.background.isValid());
            QVERIFY(cp.plain.isValid());
            QVERIFY(cp.keyword.isValid());
            QVERIFY(cp.function.isValid());
            QVERIFY(cp.string.isValid());
            QVERIFY(cp.type.isValid());
            QVERIFY(cp.number.isValid());
            QVERIFY(cp.comment.isValid());
            QVERIFY(cp.added.isValid());
            QVERIFY(cp.removed.isValid());
            QVERIFY(cp.hunk.isValid());
            QVERIFY(cp.background != cp.plain);
            QVERIFY(cp.keyword != cp.comment);
        }
    }

    QCOMPARE(codePaletteFor(CodeThemeId::OneDark, Appearance::Dark).background, rgbHex(0x000000));
    QCOMPARE(codePaletteFor(CodeThemeId::OneDark, Appearance::Light).background, rgbHex(0x282C34));
    QCOMPARE(codePaletteFor(CodeThemeId::GitHubDark, Appearance::Dark).background, rgbHex(0x0D1117));
    QCOMPARE(codePaletteFor(CodeThemeId::Dracula, Appearance::Dark).background, rgbHex(0x282A36));
    QCOMPARE(codePaletteFor(CodeThemeId::Monokai, Appearance::Dark).background, rgbHex(0x272822));
    QCOMPARE(codePaletteFor(CodeThemeId::SolarizedDark, Appearance::Dark).background, rgbHex(0x002B36));
    QCOMPARE(codePaletteFor(CodeThemeId::GitHubLight, Appearance::Light).background, rgbHex(0xFFFFFF));
    QCOMPARE(codePaletteFor(CodeThemeId::XcodeLight, Appearance::Light).background, rgbHex(0xFFFFFF));
    QCOMPARE(codePaletteFor(CodeThemeId::SolarizedLight, Appearance::Light).background, rgbHex(0xFDF6E3));

    // 4. All 4 integrated terminal themes
    const QVector<TerminalThemeId> termThemes = ThemeManager::allTerminalThemes();
    QCOMPARE(termThemes.size(), 4);

    for (const TerminalThemeId tid : termThemes) {
        QVERIFY(!ThemeManager::terminalThemeName(tid).isEmpty());
        for (const Appearance app : {Appearance::Dark, Appearance::Light}) {
            const TerminalPalette tp = terminalPaletteFor(tid, CodeThemeId::OneDark, app);
            QVERIFY(tp.background.isValid());
            QVERIFY(tp.foreground.isValid());
            QVERIFY(tp.bold.isValid());
            QVERIFY(tp.cursor.isValid());
            QVERIFY(tp.cursorText.isValid());
            QVERIFY(tp.selection.isValid());
            QVERIFY(tp.background != tp.foreground);
            for (const QColor& ansiColor : tp.ansi) {
                QVERIFY(ansiColor.isValid());
            }
        }
    }

    QCOMPARE(terminalPaletteFor(TerminalThemeId::MessageTerminal, CodeThemeId::OneDark, Appearance::Dark).background,
             rgbHex(0x0A0F0A));
    QCOMPARE(terminalPaletteFor(TerminalThemeId::ClassicDark, CodeThemeId::OneDark, Appearance::Dark).background,
             rgbHex(0x000000));
    QCOMPARE(terminalPaletteFor(TerminalThemeId::ClassicLight, CodeThemeId::OneDark, Appearance::Light).background,
             rgbHex(0xFFFFFF));

    const CodePalette dracula = codePaletteFor(CodeThemeId::Dracula, Appearance::Dark);
    const TerminalPalette followDracula =
        terminalPaletteFor(TerminalThemeId::FollowCodeTheme, CodeThemeId::Dracula, Appearance::Dark);
    QCOMPARE(followDracula.background, dracula.background);
    QCOMPARE(followDracula.foreground, dracula.plain);

    const CodePalette solLight = codePaletteFor(CodeThemeId::SolarizedLight, Appearance::Light);
    const TerminalPalette followSolLight =
        terminalPaletteFor(TerminalThemeId::FollowCodeTheme, CodeThemeId::SolarizedLight, Appearance::Light);
    QCOMPARE(followSolLight.background, solLight.background);
    QCOMPARE(followSolLight.foreground, solLight.plain);

    // 5. ThemeManager stylesheet generation & switching
    auto& tm = ThemeManager::instance();
    tm.setAppearance(Appearance::Dark);
    tm.setCodeTheme(CodeThemeId::OneDark);
    tm.setTerminalTheme(TerminalThemeId::MessageTerminal);

    const QString darkQss = tm.buildApplicationStyleSheet();
    QVERIFY(!darkQss.isEmpty());
    QVERIFY(darkQss.contains(QStringLiteral("#000000")));
    QVERIFY(darkQss.contains(QStringLiteral("#ffffff")));
    QVERIFY(darkQss.contains(QStringLiteral("QSplitter")));
    QVERIFY(darkQss.contains(QStringLiteral("QPushButton")));
    QVERIFY(darkQss.contains(QStringLiteral("QTabBar")));
    QVERIFY(darkQss.contains(QStringLiteral("QCheckBox::indicator:checked")));
    QVERIFY(darkQss.contains(QStringLiteral("qradialgradient")));

    QSignalSpy themeSpy(&tm, &ThemeManager::themeChanged);
    tm.setAppearance(Appearance::Light);
    QCOMPARE(themeSpy.count(), 1);
    QCOMPARE(tm.appearance(), Appearance::Light);
    QCOMPARE(tm.codeTheme(), CodeThemeId::XcodeLight);

    const QString lightQss = tm.buildApplicationStyleSheet();
    QVERIFY(!lightQss.isEmpty());
    QVERIFY(lightQss != darkQss);
    QVERIFY(lightQss.contains(QStringLiteral("#ffffff")));
    QVERIFY(lightQss.contains(QStringLiteral("#000000")));
    QVERIFY(lightQss.contains(QStringLiteral("QCheckBox::indicator:checked")));
    QVERIFY(lightQss.contains(QStringLiteral("qradialgradient")));

    tm.setEditorFontSize(15.0);
    QCOMPARE(tm.editorFontSize(), 15.0);
    QCOMPARE(tm.editorFont().pointSizeF(), 15.0);

    tm.setTerminalFontSize(14.0);
    QCOMPARE(tm.terminalFontSize(), 14.0);
    QCOMPARE(tm.terminalFont().pointSizeF(), 14.0);

    // Restore baseline dark theme and apply to QApplication
    tm.setAppearance(Appearance::Dark);
    QCOMPARE(tm.codeTheme(), CodeThemeId::OneDark);
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        tm.applyToApplication(app);
    }
}

void SpellQtTests::testModelsSnapshotJsonDecoding() {
    using namespace spell;

    // 1. ModelDescriptor: snake_case, camelCase, provider/details/topProvider aliases
    const ModelDescriptor snakeModel = ModelDescriptor::fromJson(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("gpt-4o")},
        {QStringLiteral("owned_by"), QStringLiteral("openai")},
        {QStringLiteral("context_window"), 128000},
    });
    QCOMPARE(snakeModel.id, QStringLiteral("gpt-4o"));
    QCOMPARE(snakeModel.ownedBy, QStringLiteral("openai"));
    QCOMPARE(snakeModel.contextWindow, 128000);
    QCOMPARE(ModelDescriptor::fromJson(snakeModel.toJson()), snakeModel);

    const ModelDescriptor camelModel = ModelDescriptor::fromJson(QJsonObject{
        {QStringLiteral("name"), QStringLiteral("qwen2.5-coder:32b")},
        {QStringLiteral("ownedBy"), QStringLiteral("ollama")},
        {QStringLiteral("contextWindow"), 32768},
    });
    QCOMPARE(camelModel.id, QStringLiteral("qwen2.5-coder:32b"));
    QCOMPARE(camelModel.ownedBy, QStringLiteral("ollama"));
    QCOMPARE(camelModel.contextWindow, 32768);

    const ModelDescriptor nestedModel = ModelDescriptor::fromJson(QJsonObject{
        {QStringLiteral("model"), QStringLiteral("claude-sonnet-4")},
        {QStringLiteral("details"), QJsonObject{{QStringLiteral("family"), QStringLiteral("anthropic")}}},
        {QStringLiteral("topProvider"), QJsonObject{{QStringLiteral("contextLength"), 200000}}},
    });
    QCOMPARE(nestedModel.id, QStringLiteral("claude-sonnet-4"));
    QCOMPARE(nestedModel.ownedBy, QStringLiteral("anthropic"));
    QCOMPARE(nestedModel.contextWindow, 200000);

    // 2. TelemetrySnapshot: unconfigured default, snake_case, camelCase
    const TelemetrySnapshot emptyTel = TelemetrySnapshot::fromJson(QJsonObject{});
    QVERIFY(!emptyTel.endpointConfigured);
    QVERIFY(!emptyTel.engineOnline);
    QCOMPARE(emptyTel.endpointLatencyMs, -1);
    QVERIFY(emptyTel.modelId.isEmpty());

    const TelemetrySnapshot snakeTel = TelemetrySnapshot::fromJson(QJsonObject{
        {QStringLiteral("endpoint_configured"), true},
        {QStringLiteral("engine_online"), true},
        {QStringLiteral("engine_error"), QStringLiteral("")},
        {QStringLiteral("model_id"), QStringLiteral("gpt-4o")},
        {QStringLiteral("effective_subagent_model_id"), QStringLiteral("gpt-4o-mini")},
        {QStringLiteral("endpoint_latency_ms"), 42},
        {QStringLiteral("decode_tok_s"), 68.5},
        {QStringLiteral("decode_tok_s_ewma"), 64.25},
        {QStringLiteral("active_prompt_tokens"), 4096},
        {QStringLiteral("total_prompt_tokens"), 12000},
        {QStringLiteral("total_completion_tokens"), 3500},
        {QStringLiteral("total_cached_tokens"), 8000},
        {QStringLiteral("compaction_epoch"), 2},
        {QStringLiteral("critic_enabled"), true},
    });
    const TelemetrySnapshot camelTel = TelemetrySnapshot::fromJson(QJsonObject{
        {QStringLiteral("endpointConfigured"), true},
        {QStringLiteral("engineOnline"), true},
        {QStringLiteral("engineError"), QStringLiteral("")},
        {QStringLiteral("modelId"), QStringLiteral("gpt-4o")},
        {QStringLiteral("effectiveSubagentModelId"), QStringLiteral("gpt-4o-mini")},
        {QStringLiteral("endpointLatencyMs"), 42},
        {QStringLiteral("decodeTokS"), 68.5},
        {QStringLiteral("decodeTokSEwma"), 64.25},
        {QStringLiteral("activePromptTokens"), 4096},
        {QStringLiteral("totalPromptTokens"), 12000},
        {QStringLiteral("totalCompletionTokens"), 3500},
        {QStringLiteral("totalCachedTokens"), 8000},
        {QStringLiteral("compactionEpoch"), 2},
        {QStringLiteral("criticEnabled"), true},
    });
    QCOMPARE(snakeTel, camelTel);
    QCOMPARE(snakeTel.endpointLatencyMs, 42);
    QCOMPARE(snakeTel.decodeTokS, 68.5);
    QCOMPARE(snakeTel.totalCachedTokens, quint64{8000});
    QCOMPARE(TelemetrySnapshot::fromJson(snakeTel.toJson()), snakeTel);

    // 3. AgentTimer: snake_case & camelCase
    const AgentTimer snakeTimer = AgentTimer::fromJson(QJsonObject{
        {QStringLiteral("timer_id"), QStringLiteral("tmr-1")},
        {QStringLiteral("thread_id"), QStringLiteral("thr-1")},
        {QStringLiteral("label"), QStringLiteral("build-poll")},
        {QStringLiteral("alarm_note"), QStringLiteral("Check cargo build")},
        {QStringLiteral("duration_s"), 120.0},
        {QStringLiteral("remaining_s"), 85.5},
        {QStringLiteral("status"), QStringLiteral("armed")},
        {QStringLiteral("delivered"), false},
        {QStringLiteral("watch_path"), QStringLiteral("target/build.log")},
    });
    const AgentTimer camelTimer = AgentTimer::fromJson(QJsonObject{
        {QStringLiteral("timerId"), QStringLiteral("tmr-1")},
        {QStringLiteral("threadId"), QStringLiteral("thr-1")},
        {QStringLiteral("name"), QStringLiteral("build-poll")},
        {QStringLiteral("alarmNote"), QStringLiteral("Check cargo build")},
        {QStringLiteral("durationSecs"), 120.0},
        {QStringLiteral("remainingSecs"), 85.5},
        {QStringLiteral("status"), QStringLiteral("armed")},
        {QStringLiteral("delivered"), false},
        {QStringLiteral("watchTarget"), QJsonObject{{QStringLiteral("path"), QStringLiteral("target/build.log")}}},
    });
    QCOMPARE(snakeTimer, camelTimer);
    QCOMPARE(AgentTimer::fromJson(snakeTimer.toJson()), snakeTimer);

    // 4. StagedDiff: snake_case & camelCase (with isVerified promotion)
    const StagedDiff snakeDiff = StagedDiff::fromJson(QJsonObject{
        {QStringLiteral("diff_id"), QStringLiteral("diff-1")},
        {QStringLiteral("thread_id"), QStringLiteral("thr-1")},
        {QStringLiteral("file_path"), QStringLiteral("src/main.rs")},
        {QStringLiteral("unified_diff"), QStringLiteral("@@ -1,2 +1,3 @@\n+fn added() {}\n")},
        {QStringLiteral("status"), QStringLiteral("verified")},
        {QStringLiteral("lines_added"), 3},
        {QStringLiteral("lines_removed"), 1},
        {QStringLiteral("summary"), QStringLiteral("Add helper function")},
    });
    const StagedDiff camelDiff = StagedDiff::fromJson(QJsonObject{
        {QStringLiteral("diffId"), QStringLiteral("diff-1")},
        {QStringLiteral("threadId"), QStringLiteral("thr-1")},
        {QStringLiteral("filePath"), QStringLiteral("src/main.rs")},
        {QStringLiteral("unifiedDiff"), QStringLiteral("@@ -1,2 +1,3 @@\n+fn added() {}\n")},
        {QStringLiteral("isVerified"), true},
        {QStringLiteral("linesAdded"), 3},
        {QStringLiteral("linesRemoved"), 1},
        {QStringLiteral("description"), QStringLiteral("Add helper function")},
    });
    QCOMPARE(snakeDiff, camelDiff);
    QCOMPARE(StagedDiff::fromJson(snakeDiff.toJson()), snakeDiff);

    // 5. Full WorkspaceSnapshot: snake_case wrapped in {"snapshot": ...} vs camelCase top-level
    const QJsonObject snakeWorkspace{
        {QStringLiteral("ok"), true},
        {QStringLiteral("snapshot"), QJsonObject{
            {QStringLiteral("projects"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("proj-1")},
                    {QStringLiteral("name"), QStringLiteral("SpellDemo")},
                    {QStringLiteral("root_path"), QStringLiteral("/tmp/spell_demo")},
                    {QStringLiteral("is_read_only"), false},
                    {QStringLiteral("created_at"), 1700000000},
                },
            }},
            {QStringLiteral("selected_project_id"), QStringLiteral("proj-1")},
            {QStringLiteral("threads"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("thr-1")},
                    {QStringLiteral("project_id"), QStringLiteral("proj-1")},
                    {QStringLiteral("title"), QStringLiteral("Main Session")},
                    {QStringLiteral("updated_at"), 1700000100},
                    {QStringLiteral("message_count"), 2},
                },
            }},
            {QStringLiteral("selected_thread_id"), QStringLiteral("thr-1")},
            {QStringLiteral("active_events"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("ev-1")},
                    {QStringLiteral("kind"), QStringLiteral("user_message")},
                    {QStringLiteral("text"), QStringLiteral("Inspect src/main.rs")},
                    {QStringLiteral("timestamp"), 1700000050},
                },
                QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("ev-2")},
                    {QStringLiteral("kind"), QStringLiteral("assistant_markdown")},
                    {QStringLiteral("text"), QStringLiteral("Done inspecting.")},
                    {QStringLiteral("model_id"), QStringLiteral("gpt-4o")},
                    {QStringLiteral("tok_per_sec"), 55.2},
                    {QStringLiteral("timestamp"), 1700000060},
                },
            }},
            {QStringLiteral("pending_diffs"), QJsonArray{snakeDiff.toJson()}},
            {QStringLiteral("active_timers"), QJsonArray{snakeTimer.toJson()}},
            {QStringLiteral("available_models"), QJsonArray{snakeModel.toJson()}},
            {QStringLiteral("subagent_slots"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("slot"), 1},
                    {QStringLiteral("state"), QStringLiteral("idle")},
                    {QStringLiteral("model_id"), QStringLiteral("gpt-4o-mini")},
                    {QStringLiteral("turn_count"), 3},
                },
            }},
            {QStringLiteral("active_subagents"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("sub-1")},
                    {QStringLiteral("role"), QStringLiteral("researcher")},
                    {QStringLiteral("label"), QStringLiteral("Slot 1")},
                    {QStringLiteral("model_id"), QStringLiteral("gpt-4o-mini")},
                    {QStringLiteral("status"), QStringLiteral("running")},
                    {QStringLiteral("started_at"), 1700000090},
                },
            }},
            {QStringLiteral("tags"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("tag-concise")},
                    {QStringLiteral("name"), QStringLiteral("concise")},
                    {QStringLiteral("description"), QStringLiteral("Keep responses brief")},
                    {QStringLiteral("directive"), QStringLiteral("Be concise.")},
                    {QStringLiteral("enabled"), true},
                    {QStringLiteral("is_built_in"), true},
                },
            }},
            {QStringLiteral("active_thread_tag_ids"), QJsonArray{QStringLiteral("tag-concise")}},
            {QStringLiteral("tool_permissions"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("tool_name"), QStringLiteral("read_file")},
                    {QStringLiteral("mode"), QStringLiteral("allow")},
                },
                QJsonObject{
                    {QStringLiteral("tool_name"), QStringLiteral("run_terminal")},
                    {QStringLiteral("mode"), QStringLiteral("ask")},
                },
            }},
            {QStringLiteral("online_research"), QJsonObject{
                {QStringLiteral("mode"), QStringLiteral("curated")},
                {QStringLiteral("max_fetches_per_turn"), 8},
                {QStringLiteral("timeout_secs"), 25},
            }},
            {QStringLiteral("telemetry"), snakeTel.toJson()},
            {QStringLiteral("endpoint_base_url"), QStringLiteral("https://api.example.invalid/v1")},
            {QStringLiteral("model_id"), QStringLiteral("gpt-4o")},
            {QStringLiteral("subagent_model_id"), QStringLiteral("gpt-4o-mini")},
            {QStringLiteral("effective_subagent_model_id"), QStringLiteral("gpt-4o-mini")},
            {QStringLiteral("endpoint_configured"), true},
            {QStringLiteral("stage_write_mode"), true},
            {QStringLiteral("subagent_protocol"), QStringLiteral("native_tool_calls")},
            {QStringLiteral("autonomous_loop_enabled"), true},
            {QStringLiteral("max_agent_steps"), 16},
            {QStringLiteral("critic_enabled"), true},
            {QStringLiteral("is_agent_busy"), false},
        }},
    };

    const QJsonObject camelWorkspace{
        {QStringLiteral("projects"), QJsonArray{
            QJsonObject{
                {QStringLiteral("projectId"), QStringLiteral("proj-1")},
                {QStringLiteral("name"), QStringLiteral("SpellDemo")},
                {QStringLiteral("rootPath"), QStringLiteral("/tmp/spell_demo")},
                {QStringLiteral("isReadOnly"), false},
                {QStringLiteral("createdAt"), 1700000000},
            },
        }},
        {QStringLiteral("selectedProjectId"), QStringLiteral("proj-1")},
        {QStringLiteral("threads"), QJsonArray{
            QJsonObject{
                {QStringLiteral("threadId"), QStringLiteral("thr-1")},
                {QStringLiteral("projectId"), QStringLiteral("proj-1")},
                {QStringLiteral("title"), QStringLiteral("Main Session")},
                {QStringLiteral("updatedAt"), 1700000100},
                {QStringLiteral("messageCount"), 2},
            },
        }},
        {QStringLiteral("selectedThreadId"), QStringLiteral("thr-1")},
        {QStringLiteral("activeEvents"), QJsonArray{
            QJsonObject{
                {QStringLiteral("eventId"), QStringLiteral("ev-1")},
                {QStringLiteral("eventKind"), QStringLiteral("userMessage")},
                {QStringLiteral("content"), QStringLiteral("Inspect src/main.rs")},
                {QStringLiteral("createdAt"), 1700000050},
            },
            QJsonObject{
                {QStringLiteral("eventId"), QStringLiteral("ev-2")},
                {QStringLiteral("eventKind"), QStringLiteral("assistantMessage")},
                {QStringLiteral("content"), QStringLiteral("Done inspecting.")},
                {QStringLiteral("modelId"), QStringLiteral("gpt-4o")},
                {QStringLiteral("tokPerSec"), 55.2},
                {QStringLiteral("createdAt"), 1700000060},
            },
        }},
        {QStringLiteral("pendingDiffs"), QJsonArray{
            QJsonObject{
                {QStringLiteral("diffId"), QStringLiteral("diff-1")},
                {QStringLiteral("threadId"), QStringLiteral("thr-1")},
                {QStringLiteral("filePath"), QStringLiteral("src/main.rs")},
                {QStringLiteral("unifiedDiff"), QStringLiteral("@@ -1,2 +1,3 @@\n+fn added() {}\n")},
                {QStringLiteral("isVerified"), true},
                {QStringLiteral("linesAdded"), 3},
                {QStringLiteral("linesRemoved"), 1},
                {QStringLiteral("description"), QStringLiteral("Add helper function")},
            },
        }},
        {QStringLiteral("activeTimers"), QJsonArray{
            QJsonObject{
                {QStringLiteral("timerId"), QStringLiteral("tmr-1")},
                {QStringLiteral("threadId"), QStringLiteral("thr-1")},
                {QStringLiteral("label"), QStringLiteral("build-poll")},
                {QStringLiteral("alarmNote"), QStringLiteral("Check cargo build")},
                {QStringLiteral("durationS"), 120.0},
                {QStringLiteral("remainingS"), 85.5},
                {QStringLiteral("status"), QStringLiteral("armed")},
                {QStringLiteral("delivered"), false},
                {QStringLiteral("watchPath"), QStringLiteral("target/build.log")},
            },
        }},
        {QStringLiteral("availableModels"), QJsonArray{
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("gpt-4o")},
                {QStringLiteral("ownedBy"), QStringLiteral("openai")},
                {QStringLiteral("contextWindow"), 128000},
            },
        }},
        {QStringLiteral("subagentSlots"), QJsonArray{
            QJsonObject{
                {QStringLiteral("slotId"), 1},
                {QStringLiteral("status"), QStringLiteral("idle")},
                {QStringLiteral("modelId"), QStringLiteral("gpt-4o-mini")},
                {QStringLiteral("turnCount"), 3},
            },
        }},
        {QStringLiteral("activeSubagents"), QJsonArray{
            QJsonObject{
                {QStringLiteral("subagentId"), QStringLiteral("sub-1")},
                {QStringLiteral("role"), QStringLiteral("researcher")},
                {QStringLiteral("slotLabel"), QStringLiteral("Slot 1")},
                {QStringLiteral("modelId"), QStringLiteral("gpt-4o-mini")},
                {QStringLiteral("state"), QStringLiteral("running")},
                {QStringLiteral("startedAt"), 1700000090},
            },
        }},
        {QStringLiteral("availableTags"), QJsonArray{
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("tag-concise")},
                {QStringLiteral("name"), QStringLiteral("concise")},
                {QStringLiteral("summary"), QStringLiteral("Keep responses brief")},
                {QStringLiteral("rubricPrompt"), QStringLiteral("Be concise.")},
                {QStringLiteral("isEnabled"), true},
                {QStringLiteral("isBuiltIn"), true},
            },
        }},
        {QStringLiteral("activeThreadTagIds"), QJsonArray{QStringLiteral("tag-concise")}},
        {QStringLiteral("toolPermissions"), QJsonObject{
            {QStringLiteral("run_terminal"), QStringLiteral("ask")},
            {QStringLiteral("read_file"), QStringLiteral("allow")},
        }},
        {QStringLiteral("onlineResearch"), QJsonObject{
            {QStringLiteral("onlineResearchMode"), QStringLiteral("curatedAllowlist")},
            {QStringLiteral("maxFetchesPerTurn"), 8},
            {QStringLiteral("timeoutSecs"), 25},
        }},
        {QStringLiteral("telemetry"), camelTel.toJson()},
        {QStringLiteral("endpointBaseUrl"), QStringLiteral("https://api.example.invalid/v1")},
        {QStringLiteral("modelId"), QStringLiteral("gpt-4o")},
        {QStringLiteral("subagentModelId"), QStringLiteral("gpt-4o-mini")},
        {QStringLiteral("effectiveSubagentModelId"), QStringLiteral("gpt-4o-mini")},
        {QStringLiteral("endpointConfigured"), true},
        {QStringLiteral("stageWriteMode"), true},
        {QStringLiteral("subagentProtocol"), QStringLiteral("nativeToolCalls")},
        {QStringLiteral("autonomousLoopEnabled"), true},
        {QStringLiteral("maxAgentSteps"), 16},
        {QStringLiteral("criticEnabled"), true},
        {QStringLiteral("isAgentBusy"), false},
    };

    const WorkspaceSnapshot snapSnake = WorkspaceSnapshot::fromJson(snakeWorkspace);
    const WorkspaceSnapshot snapCamel = WorkspaceSnapshot::fromJson(camelWorkspace);

    QCOMPARE(snapSnake.projects, snapCamel.projects);
    QCOMPARE(snapSnake.projects.size(), 1);
    QCOMPARE(snapSnake.selectedProjectId, QStringLiteral("proj-1"));
    QCOMPARE(snapCamel.selectedProjectId, QStringLiteral("proj-1"));

    QCOMPARE(snapSnake.threads, snapCamel.threads);
    QCOMPARE(snapSnake.selectedThreadId, QStringLiteral("thr-1"));
    QCOMPARE(snapCamel.selectedThreadId, QStringLiteral("thr-1"));

    QCOMPARE(snapSnake.activeEvents.size(), 2);
    QCOMPARE(snapCamel.activeEvents.size(), 2);
    QCOMPARE(snapSnake.activeEvents[0].kind, QStringLiteral("user"));
    QCOMPARE(snapCamel.activeEvents[0].kind, QStringLiteral("user"));
    QCOMPARE(snapSnake.activeEvents[1].kind, QStringLiteral("assistant"));
    QCOMPARE(snapCamel.activeEvents[1].kind, QStringLiteral("assistant"));
    QCOMPARE(snapSnake.activeEvents[1].tokPerSec, snapCamel.activeEvents[1].tokPerSec);

    QCOMPARE(snapSnake.pendingDiffs, snapCamel.pendingDiffs);
    QCOMPARE(snapSnake.activeTimers, snapCamel.activeTimers);
    QCOMPARE(snapSnake.availableModels, snapCamel.availableModels);
    QCOMPARE(snapSnake.subagentSlots, snapCamel.subagentSlots);
    QCOMPARE(snapSnake.activeSubagents, snapCamel.activeSubagents);
    QCOMPARE(snapSnake.tags, snapCamel.tags);
    QCOMPARE(snapSnake.activeThreadTagIds, snapCamel.activeThreadTagIds);
    QCOMPARE(snapSnake.toolPermissions, snapCamel.toolPermissions);
    QCOMPARE(snapSnake.onlineResearch.mode, QStringLiteral("curated"));
    QCOMPARE(snapCamel.onlineResearch.mode, QStringLiteral("curated"));
    QCOMPARE(snapSnake.onlineResearch.maxFetchesPerTurn, 8);
    QCOMPARE(snapCamel.onlineResearch.maxFetchesPerTurn, 8);
    QCOMPARE(snapSnake.telemetry, snapCamel.telemetry);
    QCOMPARE(snapSnake.endpointBaseUrl, QStringLiteral("https://api.example.invalid/v1"));
    QCOMPARE(snapCamel.endpointBaseUrl, QStringLiteral("https://api.example.invalid/v1"));
    QCOMPARE(snapSnake.modelId, QStringLiteral("gpt-4o"));
    QCOMPARE(snapCamel.modelId, QStringLiteral("gpt-4o"));
    QCOMPARE(snapSnake.effectiveSubagentModelId, QStringLiteral("gpt-4o-mini"));
    QCOMPARE(snapCamel.effectiveSubagentModelId, QStringLiteral("gpt-4o-mini"));
    QVERIFY(snapSnake.endpointConfigured);
    QVERIFY(snapCamel.endpointConfigured);
    QVERIFY(snapSnake.stageWriteMode);
    QVERIFY(snapCamel.stageWriteMode);
    QCOMPARE(snapSnake.subagentProtocol, QStringLiteral("native"));
    QCOMPARE(snapCamel.subagentProtocol, QStringLiteral("native"));
    QCOMPARE(snapSnake.maxAgentSteps, 16);
    QCOMPARE(snapCamel.maxAgentSteps, 16);
    QVERIFY(snapSnake.criticEnabled);
    QVERIFY(snapCamel.criticEnabled);

    // Roundtrip toJson -> fromJson
    const WorkspaceSnapshot roundtripped = WorkspaceSnapshot::fromJson(snapSnake.toJson());
    QCOMPARE(roundtripped.projects, snapSnake.projects);
    QCOMPARE(roundtripped.threads, snapSnake.threads);
    QCOMPARE(roundtripped.pendingDiffs, snapSnake.pendingDiffs);
    QCOMPARE(roundtripped.activeTimers, snapSnake.activeTimers);
    QCOMPARE(roundtripped.availableModels, snapSnake.availableModels);
    QCOMPARE(roundtripped.telemetry, snapSnake.telemetry);
    QCOMPARE(roundtripped.endpointBaseUrl, snapSnake.endpointBaseUrl);
    QCOMPARE(roundtripped.endpointConfigured, snapSnake.endpointConfigured);
}

void SpellQtTests::testCitationLinkifier() {
    using namespace spell;

    QTemporaryDir workspaceDir;
    QVERIFY(workspaceDir.isValid());
    const QString canonicalRoot = QFileInfo(workspaceDir.path()).canonicalFilePath();
    QVERIFY(!canonicalRoot.isEmpty());

    const QString mainRsPath = QDir(canonicalRoot).filePath(QStringLiteral("src/main.rs"));
    const QString researchDocPath =
        QDir(canonicalRoot).filePath(QStringLiteral(".spell/research_cache/doc.md"));

    QVERIFY(writeUtf8File(mainRsPath, QStringLiteral("fn main() {\n    println!(\"hello\");\n}\n")));
    QVERIFY(writeUtf8File(researchDocPath, QStringLiteral("# Cached Research Note\n")));

    const QStringList roots{canonicalRoot};

    // 1. findCitations parses workspace file + line number, line range, and .spell/research_cache path
    const QString prose = QStringLiteral(
        "Check src/main.rs:42, inspect range src/main.rs:L12-L30, "
        "and review .spell/research_cache/doc.md for notes.");

    const QVector<FileCitation> citations = findCitations(prose, roots);
    QVERIFY2(citations.size() >= 3, "Expected at least 3 parsed citations");

    bool foundMainLine42 = false;
    bool foundMainRange = false;
    bool foundResearchDoc = false;

    for (const FileCitation& c : citations) {
        QVERIFY(c.start >= 0);
        QVERIFY(c.length > 0);
        QCOMPARE(prose.mid(c.start, c.length), c.rawText);

        const QString cleanedPath = QDir::cleanPath(c.filePath);
        if (cleanedPath.endsWith(QStringLiteral("src/main.rs")) && c.startLine == 42) {
            foundMainLine42 = true;
            if (QFileInfo(c.filePath).isAbsolute()) {
                QCOMPARE(QFileInfo(c.filePath).canonicalFilePath(),
                         QFileInfo(mainRsPath).canonicalFilePath());
            }
        } else if (cleanedPath.endsWith(QStringLiteral("src/main.rs")) && c.startLine == 12) {
            foundMainRange = true;
            QCOMPARE(c.endLine, 30);
        } else if (cleanedPath.endsWith(QStringLiteral(".spell/research_cache/doc.md"))) {
            foundResearchDoc = true;
            QCOMPARE(c.startLine, 0);
            if (QFileInfo(c.filePath).isAbsolute()) {
                QCOMPARE(QFileInfo(c.filePath).canonicalFilePath(),
                         QFileInfo(researchDocPath).canonicalFilePath());
            }
        }
    }

    QVERIFY2(foundMainLine42, "Missing citation for src/main.rs:42");
    QVERIFY2(foundMainRange, "Missing citation for src/main.rs:L12-L30");
    QVERIFY2(foundResearchDoc, "Missing citation for .spell/research_cache/doc.md");

    // 2. linkifyHtml turns prose citations into spellfile:// links while preserving <pre> and <a>
    const QString escapedHtml = QStringLiteral(
        "<p>Open src/main.rs:42 and .spell/research_cache/doc.md.</p>"
        "<pre>src/main.rs:99</pre>"
        "<a href=\"https://example.com\">src/main.rs:77</a>");

    const QString linked = linkifyHtml(escapedHtml, roots);
    QVERIFY(linked.contains(QStringLiteral("<pre>src/main.rs:99</pre>")));
    QVERIFY(linked.contains(QStringLiteral("<a href=\"https://example.com\">src/main.rs:77</a>")));

    // Extract emitted spellfile:// hrefs and roundtrip through parseSpellFileUrl
    const QRegularExpression hrefRe(QStringLiteral("href=\"(spellfile://[^\"]+)\""));
    auto it = hrefRe.globalMatch(linked);

    bool roundtrippedMain42 = false;
    bool roundtrippedResearchDoc = false;
    while (it.hasNext()) {
        const QString href = it.next().captured(1);
        QString parsedPath;
        int parsedLine = -1;
        QVERIFY2(parseSpellFileUrl(QUrl(href), &parsedPath, &parsedLine),
                 qPrintable(QStringLiteral("parseSpellFileUrl failed for %1").arg(href)));
        const QString cleaned = QDir::cleanPath(parsedPath);
        if (cleaned.endsWith(QStringLiteral("src/main.rs")) && parsedLine == 42) {
            roundtrippedMain42 = true;
        } else if (cleaned.endsWith(QStringLiteral(".spell/research_cache/doc.md")) && parsedLine == 0) {
            roundtrippedResearchDoc = true;
        }
    }
    QVERIFY2(roundtrippedMain42, "Expected spellfile:// roundtrip for src/main.rs:42");
    QVERIFY2(roundtrippedResearchDoc, "Expected spellfile:// roundtrip for .spell/research_cache/doc.md");

    // 3. Direct parseSpellFileUrl contract & negative cases
    QString outPath;
    int outLine = -1;

    QVERIFY(parseSpellFileUrl(QUrl(QStringLiteral("spellfile:///workspace/src/main.rs#L42")),
                              &outPath, &outLine));
    QCOMPARE(outPath, QStringLiteral("/workspace/src/main.rs"));
    QCOMPARE(outLine, 42);

    QVERIFY(parseSpellFileUrl(QUrl(QStringLiteral("spellfile:///workspace/src/main.rs#L42-L58")),
                              &outPath, &outLine));
    QCOMPARE(outPath, QStringLiteral("/workspace/src/main.rs"));
    QCOMPARE(outLine, 42);

    QVERIFY(parseSpellFileUrl(QUrl(QStringLiteral("spellfile:///workspace/.spell/research_cache/doc.md")),
                              &outPath, &outLine));
    QCOMPARE(outPath, QStringLiteral("/workspace/.spell/research_cache/doc.md"));
    QCOMPARE(outLine, 0);

    QVERIFY(!parseSpellFileUrl(QUrl(QStringLiteral("https://example.com/src/main.rs#L42")),
                               &outPath, &outLine));
    QVERIFY(!parseSpellFileUrl(QUrl(QStringLiteral("spellfile://")), &outPath, &outLine));
}

void SpellQtTests::testTerminalEmulatorAnsiAnd24BitColor() {
    using namespace spell;

    SpellTerminalEmulator term(40, 10, 50);
    QCOMPARE(term.cols(), 40);
    QCOMPARE(term.rows(), 10);
    QCOMPARE(term.scrollbackCount(), 0);
    QVERIFY(!term.isAlternateScreen());
    QVERIFY(term.cursorVisible());

    // 1. SGR 24-bit RGB foreground/background, bold/underline, reset, and 256-color indexed
    const uint64_t genBefore = term.generation();
    term.feed(QByteArray("\x1b[1;4;38;2;12;34;56;48;2;200;150;100mHi\x1b[0m!\x1b[38;5;196mR\x1b[0m"));
    QVERIFY(term.generation() > genBefore);

    const QVector<TermCell>& row0 = term.lineAt(term.scrollbackCount());
    QCOMPARE(row0.size(), 40);

    QCOMPARE(row0[0].codepoint, uint32_t{U'H'});
    QVERIFY(row0[0].bold);
    QVERIFY(row0[0].underline);
    QCOMPARE(row0[0].fg, TermColor::rgb(12, 34, 56));
    QCOMPARE(row0[0].bg, TermColor::rgb(200, 150, 100));

    QCOMPARE(row0[1].codepoint, uint32_t{U'i'});
    QCOMPARE(row0[1].fg, TermColor::rgb(12, 34, 56));
    QCOMPARE(row0[1].bg, TermColor::rgb(200, 150, 100));

    QCOMPARE(row0[2].codepoint, uint32_t{U'!'});
    QVERIFY(!row0[2].bold);
    QVERIFY(!row0[2].underline);
    QCOMPARE(row0[2].fg, TermColor::defaultFg());
    QCOMPARE(row0[2].bg, TermColor::defaultBg());

    QCOMPARE(row0[3].codepoint, uint32_t{U'R'});
    QCOMPARE(row0[3].fg, TermColor::indexed(196));

    // 2. Cursor movement: CUP (row 3, col 5 -> 0-based 2, 4), CUU/CUB, CUD/CUF
    term.feed(QByteArray("\x1b[3;5HX"));
    QCOMPARE(term.lineAt(term.scrollbackCount() + 2)[4].codepoint, uint32_t{U'X'});
    QCOMPARE(term.cursorRow(), 2);
    QCOMPARE(term.cursorCol(), 5);

    term.feed(QByteArray("\x1b[1A\x1b[2DY"));
    QCOMPARE(term.lineAt(term.scrollbackCount() + 1)[3].codepoint, uint32_t{U'Y'});
    QCOMPARE(term.cursorRow(), 1);
    QCOMPARE(term.cursorCol(), 4);

    term.feed(QByteArray("\x1b[2B\x1b[3CZ"));
    QCOMPARE(term.lineAt(term.scrollbackCount() + 3)[7].codepoint, uint32_t{U'Z'});
    QCOMPARE(term.cursorRow(), 3);
    QCOMPARE(term.cursorCol(), 8);

    // 3. OSC 0 (BEL terminated) and OSC 2 (ST terminated) window titles
    term.feed(QByteArray("\x1b]0;Spell Terminal Test\x07"));
    QCOMPARE(term.title(), QStringLiteral("Spell Terminal Test"));

    term.feed(QByteArray("\x1b]2;Spell IDE Shell\x1b\\"));
    QCOMPARE(term.title(), QStringLiteral("Spell IDE Shell"));

    // 4. Alternate screen buffer (DECSET 1049 / DECRST 1049)
    term.feed(QByteArray("\x1b[?1049h"));
    QVERIFY(term.isAlternateScreen());
    QCOMPARE(term.totalLineCount(), term.rows());
    term.feed(QByteArray("ALT-SCREEN-ACTIVE"));
    QVERIFY(term.screenText().contains(QStringLiteral("ALT-SCREEN-ACTIVE")));

    term.feed(QByteArray("\x1b[?1049l"));
    QVERIFY(!term.isAlternateScreen());
    QVERIFY(!term.screenText().contains(QStringLiteral("ALT-SCREEN-ACTIVE")));
    QVERIFY(term.screenText().contains(QStringLiteral("Hi!R")));
    QCOMPARE(term.lineAt(term.scrollbackCount() + 2)[4].codepoint, uint32_t{U'X'});

    // 5. Scrollback retention and bounded eviction
    SpellTerminalEmulator scrollTerm(30, 5, 20);
    for (int i = 0; i < 15; ++i) {
        scrollTerm.feed(QStringLiteral("Line %1\r\n").arg(i, 2, 10, QLatin1Char('0')).toUtf8());
    }
    QVERIFY(scrollTerm.scrollbackCount() > 0);
    QCOMPARE(scrollTerm.totalLineCount(), scrollTerm.scrollbackCount() + scrollTerm.rows());
    const QString initialRecent = scrollTerm.recentText(50);
    QVERIFY(initialRecent.contains(QStringLiteral("Line 00")));
    QVERIFY(initialRecent.contains(QStringLiteral("Line 14")));

    for (int i = 15; i < 50; ++i) {
        scrollTerm.feed(QStringLiteral("Line %1\r\n").arg(i, 2, 10, QLatin1Char('0')).toUtf8());
    }
    QVERIFY(scrollTerm.scrollbackCount() <= 20);
    QVERIFY(scrollTerm.droppedScrollbackLines() > 0);
    QVERIFY(scrollTerm.recentText(30).contains(QStringLiteral("Line 49")));

    // 6. Resize up, resize down, minimum clamping, and out-of-range safety
    scrollTerm.resize(60, 12);
    QCOMPARE(scrollTerm.cols(), 60);
    QCOMPARE(scrollTerm.rows(), 12);
    QVERIFY(scrollTerm.cursorRow() >= 0 && scrollTerm.cursorRow() < scrollTerm.rows());
    QVERIFY(scrollTerm.cursorCol() >= 0 && scrollTerm.cursorCol() < scrollTerm.cols());
    QCOMPARE(scrollTerm.lineAt(scrollTerm.scrollbackCount()).size(), 60);

    scrollTerm.resize(20, 4);
    QCOMPARE(scrollTerm.cols(), 20);
    QCOMPARE(scrollTerm.rows(), 4);
    QVERIFY(scrollTerm.cursorRow() >= 0 && scrollTerm.cursorRow() < scrollTerm.rows());
    QVERIFY(scrollTerm.cursorCol() >= 0 && scrollTerm.cursorCol() < scrollTerm.cols());

    scrollTerm.resize(1, 1);
    QVERIFY(scrollTerm.cols() >= 2);
    QVERIFY(scrollTerm.rows() >= 2);

    QVERIFY(scrollTerm.lineAt(-1).isEmpty());
    QVERIFY(scrollTerm.lineAt(scrollTerm.totalLineCount() + 100).isEmpty());

    scrollTerm.clearScrollback();
    QCOMPARE(scrollTerm.scrollbackCount(), 0);
}

void SpellQtTests::testScintillaEditorAndRustLexer() {
    using namespace spell;

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString rsPath = tempDir.filePath(QStringLiteral("sample.rs"));

    const QString initialSource = QStringLiteral(
        "// Sample Rust file for Spell IDE\n"
        "fn main() {\n"
        "    let answer: i32 = 42;\n"
        "    println!(\"answer = {}\", answer);\n"
        "}\n");
    QVERIFY(writeUtf8File(rsPath, initialSource));

    CodeEditorPane editor;
    editor.resize(800, 600);
    editor.openFile(rsPath);
    QCoreApplication::processEvents();

    QCOMPARE(editor.openDocumentCount(), 1);
    EditorDocument* doc = editor.currentDocument();
    QVERIFY(doc != nullptr);
    QCOMPARE(QFileInfo(editor.currentFilePath()).canonicalFilePath(),
             QFileInfo(rsPath).canonicalFilePath());
    QCOMPARE(doc->language(), QStringLiteral("rust"));
    QVERIFY(!doc->isDirty());
    QVERIFY(!editor.hasUnsavedChanges());

    QsciScintilla* sci = editor.scintilla();
    QVERIFY(sci != nullptr);
    QVERIFY(sci->text().contains(QStringLiteral("let answer: i32 = 42;")));

    // Verify SpellCustomLexer attaches for Rust
    auto* rustLexer = qobject_cast<SpellCustomLexer*>(sci->lexer());
    QVERIFY2(rustLexer != nullptr, "Expected SpellCustomLexer to attach for .rs file");
    QCOMPARE(rustLexer->customLanguage(), SpellCustomLexer::CustomLang::Rust);
    QCOMPARE(QString::fromLatin1(rustLexer->language()).toLower(), QStringLiteral("rust"));
    QVERIFY(!rustLexer->description(SpellCustomLexer::Keyword).isEmpty());
    QVERIFY(!rustLexer->description(SpellCustomLexer::Comment).isEmpty());

    // Edit text in QScintilla -> marks document dirty
    const QString appendedFn = QStringLiteral("\nfn spell_helper() -> i32 {\n    7\n}\n");
    sci->append(appendedFn);
    QCoreApplication::processEvents();

    QVERIFY(doc->isDirty());
    QVERIFY(editor.hasUnsavedChanges());
    QVERIFY(doc->text().contains(QStringLiteral("fn spell_helper() -> i32")));

    // Save atomically to disk -> clears dirty flag and writes updated contents
    editor.saveCurrentFile();
    QCoreApplication::processEvents();

    QVERIFY(!doc->isDirty());
    QVERIFY(!editor.hasUnsavedChanges());

    const auto diskAfterSave = readUtf8File(rsPath);
    QVERIFY(diskAfterSave.has_value());
    QVERIFY(diskAfterSave->contains(QStringLiteral("let answer: i32 = 42;")));
    QVERIFY(diskAfterSave->contains(QStringLiteral("fn spell_helper() -> i32")));

    // External disk update -> reload cleanly without leaving dirty state
    const QString externallyUpdated =
        *diskAfterSave + QStringLiteral("\n// external edit\nfn reloaded_fn() -> bool {\n    true\n}\n");
    QVERIFY(writeUtf8File(rsPath, externallyUpdated));

    editor.reloadOpenDocumentsIfChanged();
    QCoreApplication::processEvents();

    QVERIFY(!doc->isDirty());
    QVERIFY(doc->text().contains(QStringLiteral("fn reloaded_fn() -> bool")));
    QVERIFY(sci->text().contains(QStringLiteral("fn reloaded_fn() -> bool")));

    // Reopen cleanly after close and confirm lexer re-attaches
    editor.closeFile(editor.currentFilePath());
    QCOMPARE(editor.openDocumentCount(), 0);

    editor.openFile(rsPath, 2);
    QCoreApplication::processEvents();
    QCOMPARE(editor.openDocumentCount(), 1);
    QVERIFY(editor.scintilla()->text().contains(QStringLiteral("fn reloaded_fn() -> bool")));
    QVERIFY(qobject_cast<SpellCustomLexer*>(editor.scintilla()->lexer()) != nullptr);
    editor.applyTheme();
}

void SpellQtTests::testSettingsDialogAllElevenPanes() {
    using namespace spell;

    QCOMPARE(kSettingsPaneCount, 11);
    for (int i = 0; i < kSettingsPaneCount; ++i) {
        const auto paneId = static_cast<SettingsPaneId>(i);
        const QString title = settingsPaneTitle(paneId);
        const QString iconName = settingsPaneIconName(paneId);
        QVERIFY(!title.isEmpty());
        QVERIFY(!iconName.isEmpty());
        QCOMPARE(settingsPaneFromName(title), paneId);
    }

    SpellCoreClient client(QStringLiteral(":memory:"));
    SpellSettingsStore store(&client);
    SettingsDialog dialog(&client, &store);

    dialog.resize(980, 680);
    dialog.show();
    QCoreApplication::processEvents();

    WorkspaceSnapshot snap;
    snap.endpointConfigured = false;
    snap.onlineResearch.mode = QStringLiteral("offline");
    dialog.updateSnapshot(snap);

    SystemInfo sysInfo;
    sysInfo.os = QStringLiteral("SpellTestOS");
    sysInfo.arch = QStringLiteral("arm64");
    sysInfo.cpuCores = 8;
    sysInfo.totalMemoryBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
    dialog.updateSystemInfo(sysInfo);
    dialog.setTerminalState(true, false);
    QCoreApplication::processEvents();

    // Switch forward across all 11 panes (0..10)
    for (int idx = 0; idx < kSettingsPaneCount; ++idx) {
        dialog.selectPaneIndex(idx);
        QCoreApplication::processEvents();
        QCOMPARE(dialog.currentPaneIndex(), idx);
        QCOMPARE(dialog.currentPane(), static_cast<SettingsPaneId>(idx));
    }

    // Switch by enum and re-apply theme
    dialog.selectPane(SettingsPaneId::Engine);
    QCoreApplication::processEvents();
    QCOMPARE(dialog.currentPane(), SettingsPaneId::Engine);

    dialog.selectPane(SettingsPaneId::About);
    QCoreApplication::processEvents();
    QCOMPARE(dialog.currentPane(), SettingsPaneId::About);

    dialog.selectPane(SettingsPaneId::General);
    dialog.applyTheme();
    QCoreApplication::processEvents();
    QCOMPARE(dialog.currentPaneIndex(), 0);

    // Footer close button must be arrow-icon-only (empty text(), no "Save & Close" text)
    QAbstractButton* saveCloseBtn =
        dialog.findChild<QAbstractButton*>(QStringLiteral("spellSettingsSaveCloseButton"));
    QVERIFY2(saveCloseBtn != nullptr && saveCloseBtn->isVisible(),
             "Expected visible spellSettingsSaveCloseButton in SettingsDialog footer");
    QVERIFY2(saveCloseBtn->text().isEmpty(),
             qPrintable(QStringLiteral("spellSettingsSaveCloseButton must have empty text() (found: %1)")
                            .arg(saveCloseBtn->text())));
    QVERIFY2(!saveCloseBtn->icon().isNull(),
             "spellSettingsSaveCloseButton must display an arrow icon");

    // SettingsDialog must have NO "SETTINGS" caption label
    QLabel* sidebarCaption = dialog.findChild<QLabel*>(QStringLiteral("spellSettingsSidebarCaption"));
    QVERIFY2(sidebarCaption == nullptr || !sidebarCaption->isVisible(),
             "SettingsDialog must not display a 'SETTINGS' sidebar caption label");
    for (const QLabel* lbl : dialog.findChildren<QLabel*>()) {
        QVERIFY2(lbl->text().trimmed() != QStringLiteral("SETTINGS"),
                 "SettingsDialog must not contain a 'SETTINGS' caption label");
    }
    if (QWidget* sidebar = dialog.findChild<QWidget*>(QStringLiteral("spellSettingsSidebar"))) {
        for (const QLabel* lbl : sidebar->findChildren<QLabel*>()) {
            QVERIFY2(lbl->text().trimmed().compare(QStringLiteral("SETTINGS"), Qt::CaseInsensitive) != 0,
                     "SettingsDialog sidebar must not contain a 'SETTINGS' caption label");
        }
    }

    // None of the 11 settings panes may contain a nested QScrollArea inside another QScrollArea
    const QList<QScrollArea*> dialogScrollAreas = dialog.findChildren<QScrollArea*>();
    for (const QScrollArea* outerScroll : dialogScrollAreas) {
        const QList<QScrollArea*> nestedScrolls = outerScroll->findChildren<QScrollArea*>();
        QVERIFY2(nestedScrolls.isEmpty(),
                 qPrintable(QStringLiteral("Nested QScrollArea detected inside %1 (nested: %2)")
                                .arg(outerScroll->objectName(),
                                     nestedScrolls.isEmpty() ? QString() : nestedScrolls.first()->objectName())));
    }
    if (auto* paneStack = dialog.findChild<QStackedWidget*>(QStringLiteral("spellSettingsPaneStack"))) {
        QCOMPARE(paneStack->count(), kSettingsPaneCount);
        bool stackInsideScrollArea = false;
        for (QWidget* ancestor = paneStack->parentWidget();
             ancestor != nullptr;
             ancestor = ancestor->parentWidget()) {
            if (qobject_cast<QScrollArea*>(ancestor) != nullptr) {
                stackInsideScrollArea = true;
                break;
            }
        }
        if (stackInsideScrollArea) {
            for (int idx = 0; idx < paneStack->count(); ++idx) {
                QWidget* pane = paneStack->widget(idx);
                QVERIFY(pane != nullptr);
                const QList<QScrollArea*> paneScrolls = pane->findChildren<QScrollArea*>();
                QVERIFY2(paneScrolls.isEmpty(),
                         qPrintable(QStringLiteral("Settings pane %1 (%2) must not contain a nested QScrollArea")
                                        .arg(idx)
                                        .arg(settingsPaneTitle(static_cast<SettingsPaneId>(idx)))));
            }
        }
    }

    dialog.close();
    QCoreApplication::processEvents();
}

void SpellQtTests::testCoreClientInMemoryFfiRoundtrip() {
    using namespace spell;

    SpellCoreClient client(QStringLiteral(":memory:"));
    QSignalSpy snapSpy(&client, &SpellCoreClient::snapshotUpdated);

    client.refreshSnapshot();
    QTRY_VERIFY_WITH_TIMEOUT(snapSpy.count() >= 1, 5000);

    // 1. Verify unconfigured endpoint defaults (zero network calls)
    const WorkspaceSnapshot initial = client.currentSnapshot();
    QVERIFY(!initial.endpointConfigured);
    QVERIFY(initial.endpointBaseUrl.isEmpty());
    QVERIFY(!initial.telemetry.endpointConfigured);
    QVERIFY(!initial.telemetry.engineOnline);

    // 2. Create project in a temporary directory
    QTemporaryDir projDir;
    QVERIFY(projDir.isValid());
    const QString projName = QStringLiteral("SpellFfiProject");
    client.createProject(projName, projDir.path(), false);

    QTRY_VERIFY_WITH_TIMEOUT(findProjectByName(client.currentSnapshot(), projName).has_value(), 5000);
    const auto createdProj = findProjectByName(client.currentSnapshot(), projName);
    QVERIFY(createdProj.has_value());
    QVERIFY(!createdProj->id.isEmpty());
    QVERIFY(!createdProj->isReadOnly);

    // 3. Create thread under the new project
    const QString threadTitle = QStringLiteral("Spell Offline Thread");
    client.createThread(createdProj->id, threadTitle);

    QTRY_VERIFY_WITH_TIMEOUT(findThreadByTitle(client.currentSnapshot(), threadTitle).has_value(), 5000);
    const auto createdThread = findThreadByTitle(client.currentSnapshot(), threadTitle);
    QVERIFY(createdThread.has_value());
    QVERIFY(!createdThread->id.isEmpty());
    QCOMPARE(createdThread->projectId, createdProj->id);

    // 4. Schedule & cancel a local SQLite timer with zero endpoint configured
    const QString timerLabel = QStringLiteral("local-sqlite-timer");
    const QString timerNote = QStringLiteral("Verify local SQLite timer operates offline");
    client.scheduleTimer(createdThread->id, 300.0, timerLabel, timerNote);

    QTRY_VERIFY_WITH_TIMEOUT(findActiveTimerByLabel(client.currentSnapshot(), timerLabel).has_value(), 5000);
    const auto armedTimer = findActiveTimerByLabel(client.currentSnapshot(), timerLabel);
    QVERIFY(armedTimer.has_value());
    QVERIFY(!armedTimer->id.isEmpty());
    QCOMPARE(armedTimer->label, timerLabel);
    QVERIFY(armedTimer->durationS >= 299.0);
    QVERIFY(!armedTimer->delivered);

    // Confirm endpoint remains completely unconfigured
    QVERIFY(!client.currentSnapshot().endpointConfigured);
    QVERIFY(client.currentSnapshot().endpointBaseUrl.isEmpty());

    const QString timerId = armedTimer->id;
    client.cancelTimer(timerId);
    QTRY_VERIFY_WITH_TIMEOUT(!hasActiveTimerById(client.currentSnapshot(), timerId), 5000);

    // 5. Verify unrestricted FFI file read helper
    const QString samplePath = projDir.filePath(QStringLiteral("ffi_read_test.txt"));
    QVERIFY(writeUtf8File(samplePath, QStringLiteral("alpha line\nbeta line\n")));
    const QString readBack = client.readFileUnrestricted(samplePath, 0, 0);
    QVERIFY(readBack.contains(QStringLiteral("alpha line")));
    QVERIFY(readBack.contains(QStringLiteral("beta line")));

    client.stopBackgroundPollers();
    QCoreApplication::processEvents();
}

void SpellQtTests::testMainWindowInstantiationAndSplitters() {
    using namespace spell;

    MainWindow window(QStringLiteral(":memory:"));
    QVERIFY(window.coreClient() != nullptr);
    QVERIFY(window.settingsStore() != nullptr);
    QVERIFY(window.explorerPane() != nullptr);
    QVERIFY(window.editorPane() != nullptr);
    QVERIFY(window.chatPane() != nullptr);
    QVERIFY(window.terminalPanel() != nullptr);

    window.resize(1440, 900);
    window.show();
    QCoreApplication::processEvents();

    // Locate the 3-pane horizontal SpellSplitter (Explorer | Editor | Chat)
    const QList<SpellSplitter*> splitters = window.findChildren<SpellSplitter*>();
    SpellSplitter* workspaceSplitter = nullptr;
    for (SpellSplitter* sp : splitters) {
        if (sp != nullptr && sp->orientation() == Qt::Horizontal && sp->count() == 3) {
            workspaceSplitter = sp;
            break;
        }
    }

    QVERIFY2(workspaceSplitter != nullptr, "Expected 3-pane horizontal SpellSplitter inside MainWindow");
    QCOMPARE(workspaceSplitter->count(), 3);
    QVERIFY(!workspaceSplitter->childrenCollapsible());
    QVERIFY(workspaceSplitter->opaqueResize());
    QVERIFY(qobject_cast<SpellSplitterHandle*>(workspaceSplitter->handle(1)) != nullptr);
    QVERIFY(qobject_cast<SpellSplitterHandle*>(workspaceSplitter->handle(2)) != nullptr);

    auto hasNonZeroThreePaneWidths = [workspaceSplitter]() {
        const QList<int> sizes = workspaceSplitter->sizes();
        return sizes.size() == 3 && sizes[0] > 0 && sizes[1] > 0 && sizes[2] > 0;
    };

    QTRY_VERIFY_WITH_TIMEOUT(hasNonZeroThreePaneWidths(), 2000);
    QVERIFY(window.explorerPane()->width() > 0);
    QVERIFY(window.editorPane()->width() > 0);
    QVERIFY(window.chatPane()->width() > 0);

    // Resize window and confirm all 3 splitter panes remain non-zero without crashes
    window.resize(1120, 720);
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(hasNonZeroThreePaneWidths(), 2000);

    window.applyTheme();
    QCoreApplication::processEvents();

    window.close();
    QCoreApplication::processEvents();
}

void SpellQtTests::testChalkSplitterAutoHideAndPaneToggle() {
    using namespace spell;

    MainWindow mainWindow(QStringLiteral(":memory:"));
    mainWindow.resize(1280, 800);
    mainWindow.show();
    QTest::qWait(50);

    SpellSplitter* splitter = mainWindow.mainSplitter();
    QVERIFY2(splitter != nullptr, "Expected MainWindow::mainSplitter() to return the workspace splitter");
    QCOMPARE(splitter->orientation(), Qt::Horizontal);
    QCOMPARE(splitter->count(), 3);
    QVERIFY(splitter->autoHideOnIdle());
    QVERIFY(splitter->paneToggleEnabled());

    auto hasNonZeroThreePaneWidths = [splitter]() {
        const QList<int> sizes = splitter->sizes();
        return sizes.size() == 3 && sizes[0] > 0 && sizes[1] > 0 && sizes[2] > 0;
    };
    QTRY_VERIFY_WITH_TIMEOUT(hasNonZeroThreePaneWidths(), 2000);

    auto* handle1 = qobject_cast<SpellSplitterHandle*>(splitter->handle(1));
    auto* handle2 = qobject_cast<SpellSplitterHandle*>(splitter->handle(2));
    QVERIFY2(handle1 != nullptr, "Expected SpellSplitterHandle at index 1");
    QVERIFY2(handle2 != nullptr, "Expected SpellSplitterHandle at index 2");

    // Handle 1 (and handle 2) must start auto-hidden on idle with a narrow centered toggle button rect.
    QVERIFY(handle1->autoHideOnIdle());
    QVERIFY(handle1->paneToggleEnabled());
    QVERIFY(handle2->autoHideOnIdle());
    QVERIFY(handle2->paneToggleEnabled());
    QCOMPARE(handle1->revealOpacity(), 0.0);
    QCOMPARE(handle2->revealOpacity(), 0.0);

    const QRect handle1Rect = handle1->rect();
    const QRect toggle1Rect = handle1->toggleButtonRect();
    QVERIFY2(!handle1Rect.isEmpty(), "Handle 1 must have non-empty geometry");
    QVERIFY2(!toggle1Rect.isEmpty(), "Handle 1 toggleButtonRect() must be non-empty");
    QVERIFY2(handle1Rect.contains(toggle1Rect), "Handle 1 toggleButtonRect() must lie inside handle bounds");
    QVERIFY2(toggle1Rect.width() > 0 && toggle1Rect.width() <= handle1Rect.width() && toggle1Rect.width() <= 16,
             "Handle 1 toggleButtonRect() must be narrow");
    QVERIFY2(toggle1Rect.height() > 0 && toggle1Rect.height() < handle1Rect.height(),
             "Handle 1 toggleButtonRect() must span only a centered vertical segment");
    const int handle1CenterY = handle1Rect.top() + handle1Rect.height() / 2;
    const int toggle1CenterY = toggle1Rect.top() + toggle1Rect.height() / 2;
    QVERIFY2(std::abs(toggle1CenterY - handle1CenterY) <= 1,
             "Handle 1 toggleButtonRect() must be centered vertically inside the handle");
    QVERIFY(toggle1Rect.top() > handle1Rect.top());
    QVERIFY(toggle1Rect.bottom() < handle1Rect.bottom());

    const QRect handle2Rect = handle2->rect();
    const QRect toggle2Rect = handle2->toggleButtonRect();
    QVERIFY(!handle2Rect.isEmpty());
    QVERIFY(!toggle2Rect.isEmpty());
    QVERIFY(handle2Rect.contains(toggle2Rect));
    const int handle2CenterY = handle2Rect.top() + handle2Rect.height() / 2;
    const int toggle2CenterY = toggle2Rect.top() + toggle2Rect.height() / 2;
    QVERIFY(std::abs(toggle2CenterY - handle2CenterY) <= 1);

    // Initially neither side pane is collapsed.
    QVERIFY(!mainWindow.isLeftPaneCollapsed());
    QVERIFY(!mainWindow.isRightPaneCollapsed());

    // Collapse left pane -> sizes[0] == 0 while handle 1 remains visible.
    mainWindow.toggleLeftPane();
    QTest::qWait(50);
    QVERIFY(mainWindow.isLeftPaneCollapsed());
    QVERIFY(!mainWindow.isRightPaneCollapsed());
    {
        const QList<int> sizes = splitter->sizes();
        QCOMPARE(sizes.size(), 3);
        QCOMPARE(sizes[0], 0);
        QVERIFY(sizes[1] > 0);
        QVERIFY(sizes[2] > 0);
    }
    QVERIFY2(handle1->isVisible(), "Handle 1 must stay visible when left pane is collapsed");
    QVERIFY(handle1->width() > 0);
    QVERIFY(handle1->isSidePaneCollapsed());

    // Restore left pane -> left pane width restores to > 0.
    mainWindow.toggleLeftPane();
    QTest::qWait(50);
    QVERIFY(!mainWindow.isLeftPaneCollapsed());
    {
        const QList<int> sizes = splitter->sizes();
        QCOMPARE(sizes.size(), 3);
        QVERIFY(sizes[0] > 0);
        QVERIFY(sizes[1] > 0);
        QVERIFY(sizes[2] > 0);
    }
    QVERIFY(handle1->isVisible());
    QVERIFY(!handle1->isSidePaneCollapsed());

    // Collapse right pane -> sizes[2] == 0 while handle 2 remains visible.
    mainWindow.toggleRightPane();
    QTest::qWait(50);
    QVERIFY(mainWindow.isRightPaneCollapsed());
    QVERIFY(!mainWindow.isLeftPaneCollapsed());
    {
        const QList<int> sizes = splitter->sizes();
        QCOMPARE(sizes.size(), 3);
        QVERIFY(sizes[0] > 0);
        QVERIFY(sizes[1] > 0);
        QCOMPARE(sizes[2], 0);
    }
    QVERIFY2(handle2->isVisible(), "Handle 2 must stay visible when right pane is collapsed");
    QVERIFY(handle2->width() > 0);
    QVERIFY(handle2->isSidePaneCollapsed());

    // Restore right pane -> right pane width restores to > 0.
    mainWindow.toggleRightPane();
    QTest::qWait(50);
    QVERIFY(!mainWindow.isRightPaneCollapsed());
    {
        const QList<int> sizes = splitter->sizes();
        QCOMPARE(sizes.size(), 3);
        QVERIFY(sizes[0] > 0);
        QVERIFY(sizes[1] > 0);
        QVERIFY(sizes[2] > 0);
    }
    QVERIFY(handle2->isVisible());
    QVERIFY(!handle2->isSidePaneCollapsed());

    mainWindow.close();
    QCoreApplication::processEvents();
}

void SpellQtTests::testPerConversationLinkedFolderTerminalPopOut() {
    using namespace spell;

    // 1. Create a temporary project directory with two subfolders subA and subB.
    QTemporaryDir projectDir;
    QVERIFY2(projectDir.isValid(), "Failed to create temporary project directory");
    const QString projectRoot = QDir::cleanPath(projectDir.path());
    const QString subA = QDir::cleanPath(projectDir.filePath(QStringLiteral("subA")));
    const QString subB = QDir::cleanPath(projectDir.filePath(QStringLiteral("subB")));
    QVERIFY(QDir().mkpath(subA));
    QVERIFY(QDir().mkpath(subB));
    QVERIFY(QFileInfo(subA).isDir());
    QVERIFY(QFileInfo(subB).isDir());

    const QString projectId = QStringLiteral("proj-linked-1");
    const QString threadId = QStringLiteral("thread-1");
    const QString threadTitle = QStringLiteral("Conv 1");

    // 2. Verify SpellSettingsStore sets, retrieves, persists, and updates thread-1's linked folder
    //    from subA to subB cleanly while staying under the same project root.
    SpellCoreClient settingsClient(QStringLiteral(":memory:"));
    SpellSettingsStore store(&settingsClient);
    QSignalSpy folderChangedSpy(&store, &SpellSettingsStore::conversationLinkedFolderChanged);

    store.setConversationLinkedFolder(threadId, QString());
    folderChangedSpy.clear();
    QVERIFY(store.conversationLinkedFolder(threadId).isEmpty());

    store.setConversationLinkedFolder(threadId, subA);
    QCOMPARE(store.conversationLinkedFolder(threadId), subA);
    QCOMPARE(store.allConversationLinkedFolders().value(threadId), subA);
    QCOMPARE(folderChangedSpy.count(), 1);
    QCOMPARE(folderChangedSpy.at(0).at(0).toString(), threadId);
    QCOMPARE(folderChangedSpy.at(0).at(1).toString(), subA);

    store.setConversationLinkedFolder(threadId, subB);
    QCOMPARE(store.conversationLinkedFolder(threadId), subB);
    QCOMPARE(store.allConversationLinkedFolders().value(threadId), subB);
    QCOMPARE(folderChangedSpy.count(), 2);
    QCOMPARE(folderChangedSpy.at(1).at(0).toString(), threadId);
    QCOMPARE(folderChangedSpy.at(1).at(1).toString(), subB);
    QVERIFY(subA.startsWith(projectRoot + QLatin1Char('/')));
    QVERIFY(subB.startsWith(projectRoot + QLatin1Char('/')));

    // Idempotent repeat call must not emit a duplicate change signal.
    store.setConversationLinkedFolder(threadId, subB);
    QCOMPARE(folderChangedSpy.count(), 2);

    {
        SpellSettingsStore readerStore;
        QCOMPARE(readerStore.conversationLinkedFolder(threadId), subB);
        QCOMPARE(readerStore.allConversationLinkedFolders().value(threadId), subB);
    }

    // 3. Verify ProjectExplorerPane accepts setConversationLinkedFolders(...) and emits
    //    terminalPopOutRequested cleanly for the active linked folder under the same project.
    WorkspaceSnapshot snap;
    ProjectInfo proj;
    proj.id = projectId;
    proj.name = QStringLiteral("LinkedFolderProject");
    proj.rootPath = projectRoot;
    proj.isReadOnly = false;
    snap.projects.append(proj);
    snap.selectedProjectId = projectId;

    ThreadSummary thread;
    thread.id = threadId;
    thread.projectId = projectId;
    thread.title = threadTitle;
    thread.messageCount = 2;
    snap.threads.append(thread);
    snap.selectedThreadId = threadId;

    ProjectExplorerPane explorer;
    explorer.resize(300, 640);
    explorer.show();
    QSignalSpy popOutSpy(&explorer, &ProjectExplorerPane::terminalPopOutRequested);

    explorer.updateSnapshot(snap);
    explorer.setConversationLinkedFolders(QHash<QString, QString>{{threadId, subA}});
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTest::qWait(20);

    const QList<QToolButton*> popOutButtonsA =
        explorer.findChildren<QToolButton*>(QStringLiteral("spellThreadTerminalPopOutBtn"));
    QCOMPARE(popOutButtonsA.size(), 1);
    popOutButtonsA.first()->click();
    QCoreApplication::processEvents();

    QCOMPARE(popOutSpy.count(), 1);
    QCOMPARE(QDir::cleanPath(popOutSpy.at(0).at(0).toString()), subA);
    QCOMPARE(popOutSpy.at(0).at(1).toString(), threadTitle);

    // Switch linked folder to subB while keeping thread-1 in the exact same project.
    explorer.setConversationLinkedFolders(store.allConversationLinkedFolders());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTest::qWait(20);

    const QList<QToolButton*> popOutButtonsB =
        explorer.findChildren<QToolButton*>(QStringLiteral("spellThreadTerminalPopOutBtn"));
    QCOMPARE(popOutButtonsB.size(), 1);
    popOutButtonsB.first()->click();
    QCoreApplication::processEvents();

    QCOMPARE(popOutSpy.count(), 2);
    QCOMPARE(QDir::cleanPath(popOutSpy.at(1).at(0).toString()), subB);
    QCOMPARE(popOutSpy.at(1).at(1).toString(), threadTitle);

    explorer.close();
    QCoreApplication::processEvents();

    // 4. Verify MainWindow::openPopOutTerminalForDirectory opens a pop-out terminal for subB
    //    and reuses the existing window without duplicating on repeat calls for the same folder.
    MainWindow mainWindow(QStringLiteral(":memory:"));
    mainWindow.resize(1280, 800);
    mainWindow.show();
    QTest::qWait(50);

    QCOMPARE(mainWindow.poppedOutFolderTerminalCount(), 0);

    mainWindow.openPopOutTerminalForDirectory(subB, threadTitle);
    QTest::qWait(50);
    const int countAfterFirst = mainWindow.poppedOutFolderTerminalCount();
    QVERIFY2(countAfterFirst >= 1, "Expected poppedOutFolderTerminalCount() >= 1 after opening subB terminal");
    {
        // Pop-out terminal window controls: Close + Minimize only, with OS-portable shortcuts.
        QMainWindow* termWin = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->objectName() == QStringLiteral("SpellFolderTerminalWindow")) {
                termWin = qobject_cast<QMainWindow*>(w);
            }
        }
        QVERIFY2(termWin != nullptr, "Expected a SpellFolderTerminalWindow top-level window");
        const Qt::WindowFlags f = termWin->windowFlags();
        QVERIFY(f.testFlag(Qt::WindowCloseButtonHint));
        QVERIFY(f.testFlag(Qt::WindowMinimizeButtonHint));
        QVERIFY(!f.testFlag(Qt::WindowMaximizeButtonHint));
        auto* closeSc = termWin->findChild<QShortcut*>(QStringLiteral("spellTerminalCloseShortcut"));
        auto* minSc = termWin->findChild<QShortcut*>(QStringLiteral("spellTerminalMinimizeShortcut"));
        QVERIFY2(closeSc && minSc, "Expected close + minimize shortcuts on the pop-out terminal");
#ifdef Q_OS_MACOS
        QCOMPARE(closeSc->key(), QKeySequence(Qt::CTRL | Qt::Key_W));
        QCOMPARE(minSc->key(), QKeySequence(Qt::CTRL | Qt::Key_M));
#else
        QCOMPARE(closeSc->key(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
        QCOMPARE(minSc->key(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
#endif
        QVERIFY(spell::isTerminalWindowControlShortcut(Qt::Key_W, closeSc->key()[0].keyboardModifiers()));
    }

    mainWindow.openPopOutTerminalForDirectory(subB, threadTitle);
    QTest::qWait(50);
    QCOMPARE(mainWindow.poppedOutFolderTerminalCount(), countAfterFirst);

    store.setConversationLinkedFolder(threadId, QString());
    settingsClient.stopBackgroundPollers();
    mainWindow.close();
    QCoreApplication::processEvents();
}

void SpellQtTests::testStrictMinimalistUxRules() {
    using namespace spell;

    MainWindow window(QStringLiteral(":memory:"));
    window.resize(1360, 860);
    window.show();
    QTest::qWait(50);

    QVERIFY(!window.isLeftPaneCollapsed());
    QVERIFY(!window.isRightPaneCollapsed());

    // -------------------------------------------------------------------------
    // 1. MainWindow has NO visible bottom status bar (findChild<QStatusBar*>()
    //    is null or hidden). Avoid window.statusBar() which lazily creates one.
    // -------------------------------------------------------------------------
    const QStatusBar* statusBar = window.findChild<QStatusBar*>();
    QVERIFY2(statusBar == nullptr || statusBar->isHidden() || !statusBar->isVisible(),
             "MainWindow must have NO visible bottom QStatusBar");
    for (const QStatusBar* sb : window.findChildren<QStatusBar*>()) {
        QVERIFY2(sb->isHidden() || !sb->isVisible(),
                 "All QStatusBar instances inside MainWindow must be absent or hidden");
    }

    // -------------------------------------------------------------------------
    // 2. Right pane empty conversation canvas shows ONLY the hat logo
    //    (spellEmptyChatHatLogo visible, ZERO visible text QLabels inside the
    //    empty state, ZERO endpoint notification banner box in AgentChatPane).
    // -------------------------------------------------------------------------
    AgentChatPane* chatPane = window.chatPane();
    QVERIFY2(chatPane != nullptr, "Expected AgentChatPane inside MainWindow");
    QVERIFY2(!chatPane->isEndpointBannerVisible(),
             "AgentChatPane must not show an endpoint notification banner");

    for (const QWidget* child : chatPane->findChildren<QWidget*>()) {
        const QString objName = child->objectName();
        QVERIFY2(!objName.contains(QStringLiteral("endpointBanner"), Qt::CaseInsensitive) &&
                     !objName.contains(QStringLiteral("endpointNotification"), Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("Unexpected endpoint banner widget in AgentChatPane: %1")
                                .arg(objName)));
    }

    ChatTimelineWidget* timeline = chatPane->timelineWidget();
    QVERIFY2(timeline != nullptr, "Expected ChatTimelineWidget inside AgentChatPane");

    QWidget* emptyPage = timeline->findChild<QWidget*>(QStringLiteral("spellEmptyChatPage"));
    QVERIFY2(emptyPage != nullptr, "Expected spellEmptyChatPage inside ChatTimelineWidget");
    QVERIFY2(emptyPage->isVisible(),
             "Empty conversation canvas must be visible when there are no messages");

    QLabel* emptyHatLogo = emptyPage->findChild<QLabel*>(QStringLiteral("spellEmptyChatHatLogo"));
    QVERIFY2(emptyHatLogo != nullptr,
             "Expected spellEmptyChatHatLogo inside empty conversation state");
    QVERIFY2(emptyHatLogo->isVisible(),
             "spellEmptyChatHatLogo must be visible in empty conversation canvas");
    QVERIFY2(!emptyHatLogo->pixmap().isNull(),
             "spellEmptyChatHatLogo must render the monochrome witch hat logo");
    QVERIFY2(emptyHatLogo->text().trimmed().isEmpty(),
             "spellEmptyChatHatLogo must not contain text");

    // ZERO visible text QLabels and ZERO visible buttons inside the empty state
    int visibleEmptyLabels = 0;
    for (const QLabel* lbl : emptyPage->findChildren<QLabel*>()) {
        if (lbl->isVisible()) {
            ++visibleEmptyLabels;
            QVERIFY2(lbl == emptyHatLogo,
                     "Only spellEmptyChatHatLogo may be visible inside the empty conversation state");
            QVERIFY2(lbl->text().trimmed().isEmpty(),
                     qPrintable(QStringLiteral("Unexpected visible text QLabel in empty chat state: %1")
                                    .arg(lbl->text())));
        }
    }
    QCOMPARE(visibleEmptyLabels, 1);

    for (const QAbstractButton* btn : emptyPage->findChildren<QAbstractButton*>()) {
        QVERIFY2(!btn->isVisible(), "Empty conversation canvas must have ZERO visible buttons");
    }

    // Confirm ZERO visible endpoint notification labels/buttons anywhere in AgentChatPane
    for (const QLabel* lbl : chatPane->findChildren<QLabel*>()) {
        if (lbl->isVisible()) {
            const QString txt = lbl->text();
            QVERIFY2(!txt.contains(QStringLiteral("Endpoint not configured"), Qt::CaseInsensitive) &&
                         !txt.contains(QStringLiteral("No endpoint"), Qt::CaseInsensitive) &&
                         !txt.contains(QStringLiteral("Configure Endpoint"), Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("Unexpected endpoint notice label in AgentChatPane: %1")
                                    .arg(txt)));
        }
    }
    for (const QAbstractButton* btn : chatPane->findChildren<QAbstractButton*>()) {
        if (btn->isVisible()) {
            const QString txt = btn->text();
            const QString tip = btn->toolTip();
            QVERIFY2(!txt.contains(QStringLiteral("Configure Endpoint"), Qt::CaseInsensitive) &&
                         !tip.contains(QStringLiteral("Configure Endpoint"), Qt::CaseInsensitive),
                     "AgentChatPane must not contain any Configure Endpoint banner/button");
        }
    }

    // -------------------------------------------------------------------------
    // 3. Left pane has NO vertical QSplitter separating Projects from
    //    Conversations, bottom settings footer contains ONLY hat + gear
    //    (spellSettingsGearBtn) with zero "Endpoint not configured" label, and
    //    pushStatusNotice("Test notice", false) populates the top Info subpanel
    //    status ticker cleanly.
    // -------------------------------------------------------------------------
    ProjectExplorerPane* explorer = window.explorerPane();
    QVERIFY2(explorer != nullptr, "Expected ProjectExplorerPane inside MainWindow");

    const QList<QSplitter*> explorerSplitters = explorer->findChildren<QSplitter*>();
    for (const QSplitter* sp : explorerSplitters) {
        QVERIFY2(sp->orientation() != Qt::Vertical,
                 "ProjectExplorerPane must have NO vertical QSplitter separating Projects from Conversations");
    }
    QVERIFY2(explorerSplitters.isEmpty(),
             "ProjectExplorerPane must not contain internal QSplitters");

    QWidget* settingsFooter = explorer->findChild<QWidget*>(QStringLiteral("spellSettingsSubpanel"));
    QVERIFY2(settingsFooter != nullptr && settingsFooter->isVisible(),
             "Expected visible spellSettingsSubpanel footer in ProjectExplorerPane");

    QToolButton* gearBtn = settingsFooter->findChild<QToolButton*>(QStringLiteral("spellSettingsGearBtn"));
    QVERIFY2(gearBtn != nullptr && gearBtn->isVisible(),
             "Expected visible spellSettingsGearBtn inside bottom settings footer");

    const QList<QLabel*> footerLabels = settingsFooter->findChildren<QLabel*>();
    QCOMPARE(footerLabels.size(), 1);
    QLabel* footerHat = footerLabels.first();
    QVERIFY2(footerHat->isVisible(), "Expected visible witch hat QLabel in bottom settings footer");
    QVERIFY2(!footerHat->pixmap().isNull(), "Bottom settings footer hat QLabel must render a pixmap");
    QVERIFY2(footerHat->text().trimmed().isEmpty(),
             "Bottom settings footer must contain ZERO text labels");

    const QList<QAbstractButton*> footerButtons = settingsFooter->findChildren<QAbstractButton*>();
    QCOMPARE(footerButtons.size(), 1);
    QCOMPARE(footerButtons.first(), static_cast<QAbstractButton*>(gearBtn));

    const QList<QWidget*> footerDirectChildren =
        settingsFooter->findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
    QCOMPARE(footerDirectChildren.size(), 2);

    for (const QLabel* lbl : explorer->findChildren<QLabel*>()) {
        const QString txt = lbl->text();
        QVERIFY2(!txt.contains(QStringLiteral("Endpoint not configured"), Qt::CaseInsensitive),
                 "ProjectExplorerPane must not display an 'Endpoint not configured' label");
        QVERIFY2(!txt.contains(QStringLiteral("No projects yet"), Qt::CaseInsensitive),
                 "ProjectExplorerPane must not display a 'No projects yet' label");
    }

    for (const QAbstractButton* btn : explorer->findChildren<QAbstractButton*>()) {
        const QString txt = btn->text();
        QVERIFY2(!txt.contains(QStringLiteral("Add Project Folder"), Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("ProjectExplorerPane must not contain an 'Add Project Folder…' button (found: %1)")
                                .arg(txt)));
    }

    QWidget* projectsEmptyPage = explorer->findChild<QWidget*>(QStringLiteral("spellProjectsEmptyPage"));
    QVERIFY2(projectsEmptyPage != nullptr, "Expected spellProjectsEmptyPage inside ProjectExplorerPane");
    for (const QLabel* lbl : projectsEmptyPage->findChildren<QLabel*>()) {
        QVERIFY2(!lbl->isVisible(), "spellProjectsEmptyPage must contain ZERO visible labels");
    }
    for (const QAbstractButton* btn : projectsEmptyPage->findChildren<QAbstractButton*>()) {
        QVERIFY2(!btn->isVisible(), "spellProjectsEmptyPage must contain ZERO visible buttons");
    }

    QWidget* infoSubpanel = explorer->findChild<QWidget*>(QStringLiteral("spellInfoSubpanel"));
    QVERIFY2(infoSubpanel != nullptr && infoSubpanel->isVisible(),
             "Expected visible spellInfoSubpanel at top of ProjectExplorerPane");

    QWidget* infoTopBar = infoSubpanel->findChild<QWidget*>(QStringLiteral("spellInfoTopBar"));
    QVERIFY2(infoTopBar != nullptr && infoTopBar->isVisible(),
             "Expected visible spellInfoTopBar at top of ProjectExplorerPane");
    QCOMPARE(infoTopBar->height(), 34);
    QLabel* explorerTitle = infoTopBar->findChild<QLabel*>(QStringLiteral("spellExplorerTitleLabel"));
    QVERIFY2(explorerTitle != nullptr && explorerTitle->isVisible(),
             "Expected visible left-aligned 'BEATRIXLESLIE IDE' title label in spellInfoTopBar");
    QCOMPARE(explorerTitle->text(), QStringLiteral("BEATRIXLESLIE IDE"));
    QVERIFY2((explorerTitle->alignment() & Qt::AlignLeft) != 0 &&
                 (explorerTitle->alignment() & Qt::AlignVCenter) != 0,
             "Projects title label must be left-aligned and vertically centered");
    QVERIFY2(explorerTitle->testAttribute(Qt::WA_TransparentForMouseEvents),
             "Projects title label must be mouse-transparent so dragging across it moves the window");
    if (auto* modeBtn = explorer->findChild<QPushButton*>(QStringLiteral("spellModePillBtn"))) {
        QVERIFY2(!modeBtn->isVisibleTo(explorer), "Mode selector must not appear in the info panel");
    }
    if (auto* netBtn = explorer->findChild<QPushButton*>(QStringLiteral("spellNetworkPillBtn"))) {
        QVERIFY2(!netBtn->isVisibleTo(explorer), "Network row must not appear in the info panel");
    }
    if (auto* airGap = explorer->findChild<QWidget*>(QStringLiteral("spellAirGapNowBtn"))) {
        QVERIFY2(!airGap->isVisibleTo(explorer), "Air-Gap button lives only in Settings");
    }
    QVERIFY2(infoTopBar->property("_spellDragRegionInstalled").toBool(),
             "Projects top bar must be a window drag region");

    QWidget* statusTicker = infoSubpanel->findChild<QWidget*>(QStringLiteral("spellStatusTicker"));
    QVERIFY2(statusTicker != nullptr,
             "Expected spellStatusTicker inside top spellInfoSubpanel");

    explorer->pushStatusNotice(QStringLiteral("Test notice"), false);
    QCoreApplication::processEvents();

    QVERIFY2(statusTicker->isVisible(),
             "spellStatusTicker must become visible after pushStatusNotice()");
    bool foundTestNotice = false;
    for (const QLabel* lbl : statusTicker->findChildren<QLabel*>()) {
        if (lbl->isVisible() && lbl->text() == QStringLiteral("Test notice")) {
            foundTestNotice = true;
            break;
        }
    }
    QVERIFY2(foundTestNotice,
             "Expected pushStatusNotice(\"Test notice\", false) to populate spellStatusTicker cleanly");

    // -------------------------------------------------------------------------
    // 4. Both per-project (spellProjectTerminalPopOutBtn) and per-conversation
    //    (spellThreadTerminalPopOutBtn) terminal pop-out buttons exist inside
    //    the nested Projects list and emit terminalPopOutRequested with the
    //    exact project / conversation-linked folder.
    // -------------------------------------------------------------------------
    QTemporaryDir nestedDir;
    QVERIFY2(nestedDir.isValid(), "Failed to create temporary project dir for nested explorer check");
    const QString projectRoot = QDir::cleanPath(nestedDir.path());
    const QString convLinkedDir = QDir::cleanPath(nestedDir.filePath(QStringLiteral("conv_linked")));
    QVERIFY(QDir().mkpath(convLinkedDir));

    const QString projId = QStringLiteral("proj-ux-1");
    const QString projName = QStringLiteral("MinimalistProject");
    const QString threadId = QStringLiteral("thread-ux-1");
    const QString threadTitle = QStringLiteral("NestedConversation");

    WorkspaceSnapshot nestedSnap;
    ProjectInfo nestedProj;
    nestedProj.id = projId;
    nestedProj.name = projName;
    nestedProj.rootPath = projectRoot;
    nestedProj.isReadOnly = false;
    nestedSnap.projects.append(nestedProj);
    nestedSnap.selectedProjectId = projId;

    ThreadSummary nestedThread;
    nestedThread.id = threadId;
    nestedThread.projectId = projId;
    nestedThread.title = threadTitle;
    nestedThread.messageCount = 1;
    nestedSnap.threads.append(nestedThread);
    nestedSnap.selectedThreadId = threadId;

    ProjectExplorerPane nestedExplorer;
    nestedExplorer.resize(300, 640);
    nestedExplorer.show();
    QSignalSpy nestedPopOutSpy(&nestedExplorer, &ProjectExplorerPane::terminalPopOutRequested);

    nestedExplorer.updateSnapshot(nestedSnap);
    nestedExplorer.setConversationLinkedFolders(QHash<QString, QString>{{threadId, convLinkedDir}});
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTest::qWait(20);

    QVERIFY2(nestedExplorer.findChildren<QSplitter*>().isEmpty(),
             "Nested ProjectExplorerPane must contain zero internal QSplitters");

    QWidget* projectsSubpanel =
        nestedExplorer.findChild<QWidget*>(QStringLiteral("spellProjectsSubpanel"));
    QVERIFY2(projectsSubpanel != nullptr && projectsSubpanel->isVisible(),
             "Expected visible spellProjectsSubpanel");
    QWidget* projectsList =
        projectsSubpanel->findChild<QWidget*>(QStringLiteral("spellProjectsScrollContent"));
    QVERIFY2(projectsList != nullptr,
             "Expected spellProjectsScrollContent holding nested projects and conversations");

    const QList<QToolButton*> projectPopBtns =
        projectsList->findChildren<QToolButton*>(QStringLiteral("spellProjectTerminalPopOutBtn"));
    QCOMPARE(projectPopBtns.size(), 1);
    QVERIFY(projectPopBtns.first()->isVisible());
    QVERIFY(projectPopBtns.first()->isEnabled());

    const QList<QToolButton*> threadPopBtns =
        projectsList->findChildren<QToolButton*>(QStringLiteral("spellThreadTerminalPopOutBtn"));
    QCOMPARE(threadPopBtns.size(), 1);
    QVERIFY(threadPopBtns.first()->isVisible());
    QVERIFY(threadPopBtns.first()->isEnabled());

    // Confirm the nested conversation row is horizontally indented beneath its project header row
    QWidget* projectHeaderRow = projectPopBtns.first()->parentWidget();
    QWidget* conversationRow = threadPopBtns.first()->parentWidget();
    QVERIFY(projectHeaderRow != nullptr && conversationRow != nullptr);
    QVERIFY2(conversationRow->mapTo(projectsList, QPoint(0, 0)).x() >
                 projectHeaderRow->mapTo(projectsList, QPoint(0, 0)).x(),
             "Nested conversation row must be horizontally indented inside its project section");

    projectPopBtns.first()->click();
    QCoreApplication::processEvents();
    QCOMPARE(nestedPopOutSpy.count(), 1);
    QCOMPARE(QDir::cleanPath(nestedPopOutSpy.at(0).at(0).toString()), projectRoot);
    QCOMPARE(nestedPopOutSpy.at(0).at(1).toString(), projName);

    threadPopBtns.first()->click();
    QCoreApplication::processEvents();
    QCOMPARE(nestedPopOutSpy.count(), 2);
    QCOMPARE(QDir::cleanPath(nestedPopOutSpy.at(1).at(0).toString()), convLinkedDir);
    QCOMPARE(nestedPopOutSpy.at(1).at(1).toString(), threadTitle);

    nestedExplorer.close();
    QCoreApplication::processEvents();

    // -------------------------------------------------------------------------
    // 5. Composer has ZERO stray "Configure Endpoint", "Tags", or "Stage-Write"
    //    toolbar buttons.
    // -------------------------------------------------------------------------
    ComposerWidget* composer = chatPane->composerWidget();
    QVERIFY2(composer != nullptr, "Expected ComposerWidget inside AgentChatPane");

    auto* composerEditor = composer->findChild<QPlainTextEdit*>(QStringLiteral("composerInput"));
    QVERIFY2(composerEditor != nullptr, "Expected composerInput QPlainTextEdit inside ComposerWidget");
    QCOMPARE(composerEditor->placeholderText(), QStringLiteral("Message BeatrixLeslie\u2026"));

    // Exercise compatibility setters to confirm none re-introduce stray toolbar controls
    RubricTag sampleTag;
    sampleTag.id = QStringLiteral("tag-1");
    sampleTag.name = QStringLiteral("Concise");
    sampleTag.enabled = true;
    composer->setEndpointStatus(false, QString(), false);
    composer->setStageWriteMode(true);
    composer->setAvailableTags(QVector<RubricTag>{sampleTag}, QStringList{sampleTag.id});
    composer->setThreadProfile(QStringLiteral("ask"));
    QCoreApplication::processEvents();
    QCOMPARE(composerEditor->placeholderText(), QStringLiteral("Ask BeatrixLeslie (read-only)\u2026"));

    const QList<QAbstractButton*> composerButtons = composer->findChildren<QAbstractButton*>();
    QCOMPARE(composerButtons.size(), 3);
    QVERIFY(composer->findChild<QPushButton*>(QStringLiteral("composerExpandButton")) != nullptr);
    QVERIFY(composer->findChild<QPushButton*>(QStringLiteral("composerPopOutTerminalButton")) != nullptr);
    QVERIFY(composer->findChild<QPushButton*>(QStringLiteral("composerSendStopButton")) != nullptr);

    composer->setThreadProfile(QStringLiteral("write"));
    QCOMPARE(composerEditor->placeholderText(), QStringLiteral("Message BeatrixLeslie\u2026"));

    for (const QAbstractButton* btn : composerButtons) {
        const QString txt = btn->text();
        const QString tip = btn->toolTip();
        const QString acc = btn->accessibleName();
        const QString obj = btn->objectName();

        QVERIFY2(!txt.contains(QStringLiteral("Configure Endpoint"), Qt::CaseInsensitive) &&
                     !tip.contains(QStringLiteral("Configure Endpoint"), Qt::CaseInsensitive) &&
                     !acc.contains(QStringLiteral("Configure Endpoint"), Qt::CaseInsensitive) &&
                     !obj.contains(QStringLiteral("endpoint"), Qt::CaseInsensitive),
                 "Composer must have ZERO stray 'Configure Endpoint' buttons");
        QVERIFY2(!txt.contains(QStringLiteral("Tags"), Qt::CaseInsensitive) &&
                     !tip.contains(QStringLiteral("Tags"), Qt::CaseInsensitive) &&
                     !acc.contains(QStringLiteral("Tags"), Qt::CaseInsensitive) &&
                     !obj.contains(QStringLiteral("tag"), Qt::CaseInsensitive),
                 "Composer must have ZERO stray 'Tags' toolbar buttons");
        QVERIFY2(!txt.contains(QStringLiteral("Stage-Write"), Qt::CaseInsensitive) &&
                     !txt.contains(QStringLiteral("Stage Write"), Qt::CaseInsensitive) &&
                     !tip.contains(QStringLiteral("Stage-Write"), Qt::CaseInsensitive) &&
                     !tip.contains(QStringLiteral("Stage Write"), Qt::CaseInsensitive) &&
                     !acc.contains(QStringLiteral("Stage-Write"), Qt::CaseInsensitive) &&
                     !obj.contains(QStringLiteral("stage"), Qt::CaseInsensitive),
                 "Composer must have ZERO stray 'Stage-Write' toolbar buttons");
    }

    for (const QLabel* lbl : composer->findChildren<QLabel*>()) {
        if (lbl->isVisible()) {
            const QString txt = lbl->text();
            QVERIFY2(!txt.contains(QStringLiteral("Configure Endpoint"), Qt::CaseInsensitive) &&
                         !txt.contains(QStringLiteral("Tags"), Qt::CaseInsensitive) &&
                         !txt.contains(QStringLiteral("Stage-Write"), Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("Unexpected stray label in ComposerWidget: %1").arg(txt)));
        }
    }

    // -------------------------------------------------------------------------
    // 6. CodeEditorPane has NO "No file open" label in its empty state
    //    (minimalist Open File… chip button only).
    // -------------------------------------------------------------------------
    CodeEditorPane* editorPane = window.editorPane();
    QVERIFY2(editorPane != nullptr, "Expected CodeEditorPane inside MainWindow");
    QCOMPARE(editorPane->openDocumentCount(), 0);

    for (const QLabel* lbl : editorPane->findChildren<QLabel*>()) {
        QVERIFY2(!lbl->text().contains(QStringLiteral("No file open"), Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("CodeEditorPane must not contain a 'No file open' label (found: %1)")
                                .arg(lbl->text())));
    }

    QWidget* editorEmptyPage = editorPane->findChild<QWidget*>(QStringLiteral("spellEditorEmptyPage"));
    QVERIFY2(editorEmptyPage != nullptr && editorEmptyPage->isVisible(),
             "Expected visible spellEditorEmptyPage when no files are open");
    for (const QLabel* lbl : editorEmptyPage->findChildren<QLabel*>()) {
        QVERIFY2(!lbl->isVisible(),
                 qPrintable(QStringLiteral("CodeEditorPane empty state must have ZERO visible labels (found: %1)")
                                .arg(lbl->text())));
    }

    window.close();
    QCoreApplication::processEvents();
}

QTEST_MAIN(SpellQtTests)
#include "SpellQtTests.moc"
