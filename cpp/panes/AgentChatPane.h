#pragma once

#include "ActiveSubagentsWidget.h"
#include "ChatTimelineWidget.h"
#include "ComposerWidget.h"
#include "core/SpellModels.h"

#include <QString>
#include <QStringList>
#include <QWidget>

#include <memory>

class QEvent;

namespace spell {

class SpellCoreClient;
class SpellSplitter;
class SpellTerminalPanel;

/**
 * Right-hand Agent Chat pane for Spell IDE.
 *
 * Combines:
 * 1. Unified flat 34px Conversation Tab Bar (matching FileTabBar pixel-for-pixel)
 *    followed by a 1px SpellHairline.
 * 2. Vertical `SpellSplitter` holding:
 *    - `ChatTimelineWidget*` (with `ActiveSubagentsWidget*` cleanly overlaid in its
 *      top-right corner when one or more subagents are active)
 *    - Optional docked `SpellTerminalPanel*` (hidden by default, shown/hidden smoothly)
 *    - `ComposerWidget*` (resizes smoothly inside the splitter with zero flicker)
 */
class AgentChatPane : public QWidget {
    Q_OBJECT

public:
    /** Settings dialog pane index corresponding to Settings → Engine. */
    static constexpr int kEngineSettingsPaneIndex = 1;

    explicit AgentChatPane(QWidget* parent = nullptr);
    ~AgentChatPane() override;

    AgentChatPane(const AgentChatPane&) = delete;
    AgentChatPane& operator=(const AgentChatPane&) = delete;
    AgentChatPane(AgentChatPane&&) = delete;
    AgentChatPane& operator=(AgentChatPane&&) = delete;

    /**
     * Binds the asynchronous `SpellCoreClient` used to dispatch chat turns, thread
     * switches, new threads, stage-write toggles, tag updates, and diff approvals.
     */
    void bindClient(SpellCoreClient* client);

    /** Synchronizes conversation tabs, active subagents overlay, timeline, and composer from `snap`. */
    void updateSnapshot(const WorkspaceSnapshot& snap);

    /**
     * Mounts or updates the optional docked `SpellTerminalPanel` inside the vertical
     * `SpellSplitter` between `ChatTimelineWidget` and `ComposerWidget`.
     */
    void setDockedTerminal(SpellTerminalPanel* panel, bool visible);

    /** Opens (or activates if already open) a conversation tab for `threadId`. */
    void openConversationTab(const QString& threadId);

    /** Closes the conversation tab for `threadId` if currently open. */
    void closeConversationTab(const QString& threadId);

    /** Focuses the prompt editor inside `ComposerWidget`. */
    void focusComposer();

    /** Re-applies the active Spell IDE monochrome theme to this pane and all child widgets. */
    void applyTheme();

    /** Read-only state and child widget accessors for shell coordination and tests. */
    [[nodiscard]] QStringList openConversationTabIds() const;
    [[nodiscard]] QString activeConversationTabId() const;
    [[nodiscard]] bool isDockedTerminalVisible() const;
    [[nodiscard]] bool isEndpointBannerVisible() const;

    [[nodiscard]] ChatTimelineWidget* timelineWidget() const;
    [[nodiscard]] ComposerWidget* composerWidget() const;
    [[nodiscard]] ActiveSubagentsWidget* activeSubagentsWidget() const;
    [[nodiscard]] SpellSplitter* verticalSplitter() const;

signals:
    void fileCitationClicked(const QString& filePath, int line);
    void openSettingsRequested(int paneIndex);
    void terminalToggleRequested();
    void popOutTerminalRequested();
    void threadProfileChanged(const QString& threadId, const QString& profile);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
