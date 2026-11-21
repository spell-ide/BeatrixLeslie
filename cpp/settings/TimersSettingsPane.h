#pragma once

#include "core/SpellModels.h"

#include <QWidget>

#include <memory>

namespace spell {

class SpellCoreClient;

/**
 * Settings -> Timers pane for BeatrixLeslie IDE.
 *
 * Displays active local SQLite wakeup timers with live countdown badges,
 * cancel and fire-due-now actions, and a form to schedule a new timer
 * targeting the active conversation thread and optional watched file path.
 */
class TimersSettingsPane : public QWidget {
    Q_OBJECT

public:
    explicit TimersSettingsPane(SpellCoreClient* client, QWidget* parent = nullptr);
    ~TimersSettingsPane() override;

    TimersSettingsPane(const TimersSettingsPane&) = delete;
    TimersSettingsPane& operator=(const TimersSettingsPane&) = delete;
    TimersSettingsPane(TimersSettingsPane&&) = delete;
    TimersSettingsPane& operator=(TimersSettingsPane&&) = delete;

    /** Refreshes the active timer list, countdowns, and default watch path. */
    void updateSnapshot(const WorkspaceSnapshot& snap);

    /** Re-applies current `ThemeManager` styling to this pane. */
    void applyTheme();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
