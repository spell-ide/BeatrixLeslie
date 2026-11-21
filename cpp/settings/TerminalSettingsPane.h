#pragma once

#include "core/SpellModels.h"

#include <QWidget>

#include <memory>

namespace spell {

class SpellCoreClient;

/**
 * Settings -> Terminal pane for BeatrixLeslie IDE.
 *
 * Manages integrated message terminal placement (docked in the workspace or
 * popped out into a standalone window), clear/restart shell actions, terminal
 * theme and font preferences, and agent terminal access/approval status.
 */
class TerminalSettingsPane : public QWidget {
    Q_OBJECT

public:
    explicit TerminalSettingsPane(SpellCoreClient* client, QWidget* parent = nullptr);
    ~TerminalSettingsPane() override;

    TerminalSettingsPane(const TerminalSettingsPane&) = delete;
    TerminalSettingsPane& operator=(const TerminalSettingsPane&) = delete;
    TerminalSettingsPane(TerminalSettingsPane&&) = delete;
    TerminalSettingsPane& operator=(TerminalSettingsPane&&) = delete;

    /** Updates pending terminal approval counters and workspace metadata. */
    void updateSnapshot(const WorkspaceSnapshot& snap);

    /** Reflects whether the integrated terminal is currently docked and/or popped out. */
    void setTerminalState(bool docked, bool poppedOut);

    /** Re-applies current `ThemeManager` styling to this pane. */
    void applyTheme();

signals:
    void toggleDockedRequested();
    void togglePopOutRequested();
    void clearTerminalRequested();
    void restartTerminalRequested();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
