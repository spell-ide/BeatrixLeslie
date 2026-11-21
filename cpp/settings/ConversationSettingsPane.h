#pragma once

#include "core/SpellModels.h"

#include <QWidget>

#include <memory>

namespace spell {

class SpellCoreClient;

/**
 * Settings -> Conversation pane for BeatrixLeslie IDE.
 *
 * Manages rubric tags attached to the selected conversation thread as well as
 * creating, editing, enabling/disabling, and deleting custom rubric tags and
 * steering directives persisted in `spell_core`.
 */
class ConversationSettingsPane : public QWidget {
    Q_OBJECT

public:
    explicit ConversationSettingsPane(SpellCoreClient* client, QWidget* parent = nullptr);
    ~ConversationSettingsPane() override;

    ConversationSettingsPane(const ConversationSettingsPane&) = delete;
    ConversationSettingsPane& operator=(const ConversationSettingsPane&) = delete;
    ConversationSettingsPane(ConversationSettingsPane&&) = delete;
    ConversationSettingsPane& operator=(ConversationSettingsPane&&) = delete;

    /** Refreshes available rubric tags and active-thread tag selections. */
    void updateSnapshot(const WorkspaceSnapshot& snap);

    /** Re-applies current `ThemeManager` styling to this pane. */
    void applyTheme();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
