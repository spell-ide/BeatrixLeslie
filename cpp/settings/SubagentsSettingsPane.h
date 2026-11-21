#pragma once

#include "core/SpellModels.h"

#include <QString>
#include <QStringList>
#include <QWidget>

#include <memory>

namespace spell {

class SpellCoreClient;
class SpellSettingsStore;

/**
 * Settings -> Subagents pane for BeatrixLeslie IDE.
 *
 * Shows active subagent pool slots and running delegations, lets the user
 * choose the delegation protocol (`auto` / `native` / `json`) and Gatekeeper
 * Critic review toggle, and provides interactive Spawn, Steer, Terminate,
 * Duplicate, and output-inspection controls for individual subagent slots.
 */
class SubagentsSettingsPane : public QWidget {
    Q_OBJECT

public:
    explicit SubagentsSettingsPane(SpellCoreClient* client,
                                   SpellSettingsStore* store,
                                   QWidget* parent = nullptr);
    ~SubagentsSettingsPane() override;

    SubagentsSettingsPane(const SubagentsSettingsPane&) = delete;
    SubagentsSettingsPane& operator=(const SubagentsSettingsPane&) = delete;
    SubagentsSettingsPane(SubagentsSettingsPane&&) = delete;
    SubagentsSettingsPane& operator=(SubagentsSettingsPane&&) = delete;

    /** Synchronizes slot states, active subagents, model, and protocol from a snapshot. */
    void updateSnapshot(const WorkspaceSnapshot& snap);

    /** Displays the latest reply or error returned by `SpellCoreClient::runSubagentSlot`. */
    void onSlotReply(int slot, const QString& reply, const QString& err);

    /** Updates the slot transcript view from `SpellCoreClient::readSubagentSlotLines`. */
    void onSlotLines(int slot, const QStringList& lines);

    /** Re-applies current `ThemeManager` styling to this pane. */
    void applyTheme();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
