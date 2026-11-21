#pragma once

#include "settings/SettingsDialog.h"

#include <QMainWindow>
#include <QString>

#include <memory>

class QCloseEvent;

namespace spell {

class AgentChatPane;
class SpellCoreClient;
class CodeEditorPane;
class ProjectExplorerPane;
class SpellSettingsStore;
class SpellSplitter;
class SpellTerminalPanel;

/**
 * Top-level cross-platform application window for BeatrixLeslie IDE (`com.spell.SpellIDE`).
 *
 * Owns the asynchronous `SpellCoreClient` and `SpellSettingsStore`, lays out
 * the three-pane workspace inside a horizontal `SpellSplitter`
 * (`ProjectExplorerPane` | `CodeEditorPane` | `AgentChatPane`) running cleanly
 * to the bottom edge with no bottom `QStatusBar` (all status notices route to
 * `ProjectExplorerPane`'s top Info subpanel and status ticker), coordinates the
 * integrated `SpellTerminalPanel` (either docked in the workspace or popped out
 * in standalone per-folder `QMainWindow`s), manages a lazy/reusable
 * `SettingsDialog`, and exposes a native cross-platform menu bar (`File`,
 * `Edit`, `View`, `Agent`, `Terminal`, `Settings`, `Help`) with standard
 * keyboard shortcuts (`Cmd/Ctrl+S`, `Cmd/Ctrl+F`, `Cmd/Ctrl+N`, `Cmd/Ctrl+,`,
 * `Cmd/Ctrl+\``, `Cmd/Ctrl+1/2/3`, etc.).
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QString& dbPath = QString(), QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    /** Opens (lazily creating if needed) and focuses `SettingsDialog` on `pane`. */
    void openSettings(SettingsPaneId pane = SettingsPaneId::General);

    /** Re-applies current `ThemeManager` styling across the window, panes, terminal, and dialog. */
    void applyTheme();

    /** Toggles visibility of the docked integrated terminal panel. */
    void toggleDockedTerminal();

    /** Toggles the integrated terminal between docked and popped-out standalone window modes. */
    void togglePopOutTerminal();

    /** Toggles collapsing/expanding the left Project Explorer pane while keeping its splitter handle live. */
    void toggleLeftPane();

    /** Toggles collapsing/expanding the right Agent Chat pane while keeping its splitter handle live. */
    void toggleRightPane();

    /** Returns `true` when the left Project Explorer pane is collapsed to 0px. */
    bool isLeftPaneCollapsed() const;

    /** Returns `true` when the right Agent Chat pane is collapsed to 0px. */
    bool isRightPaneCollapsed() const;

    /** Opens (or focuses an existing) standalone pop-out terminal window rooted in `workingDir`. */
    void openPopOutTerminalForDirectory(const QString& workingDir, const QString& label = QString());

    /** Returns the number of currently open per-folder pop-out terminal windows. */
    int poppedOutFolderTerminalCount() const;

    /** Returns the main 3-pane horizontal workspace `SpellSplitter`. */
    SpellSplitter* mainSplitter() const;

    SpellCoreClient* coreClient() const;
    SpellSettingsStore* settingsStore() const;
    CodeEditorPane* editorPane() const;
    AgentChatPane* chatPane() const;
    ProjectExplorerPane* explorerPane() const;
    SpellTerminalPanel* terminalPanel() const;
    SettingsDialog* settingsDialog() const;

protected:
    /** Persists window geometry and splitter sizes to `SpellSettingsStore` and stops PTY/pollers cleanly. */
    void closeEvent(QCloseEvent* event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
