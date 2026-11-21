#pragma once

#include "core/SpellModels.h"

#include <QWidget>

#include <memory>

namespace spell {

class SpellCoreClient;

/**
 * Settings -> Agent pane for BeatrixLeslie IDE.
 *
 * Configures autonomous loop behavior and step limits, staged vs direct file
 * write mode, subagent delegation protocol, online research posture shortcut
 * (with immediate Air-Gap kill-switch), and per-tool permission policies
 * (`allow` / `ask` / `deny`).
 */
class AgentSettingsPane : public QWidget {
    Q_OBJECT

public:
    explicit AgentSettingsPane(SpellCoreClient* client, QWidget* parent = nullptr);
    ~AgentSettingsPane() override;

    AgentSettingsPane(const AgentSettingsPane&) = delete;
    AgentSettingsPane& operator=(const AgentSettingsPane&) = delete;
    AgentSettingsPane(AgentSettingsPane&&) = delete;
    AgentSettingsPane& operator=(AgentSettingsPane&&) = delete;

    /** Refreshes loop flags, write mode, online posture badge, and tool permission rows. */
    void updateSnapshot(const WorkspaceSnapshot& snap);

    /** Re-applies current `ThemeManager` styling to this pane. */
    void applyTheme();

signals:
    /** Emitted when the user clicks to jump to the Online Research settings pane. */
    void openOnlineResearchRequested();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
