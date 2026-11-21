#pragma once

#include "core/SpellModels.h"

#include <QDialog>
#include <QMetaType>
#include <QString>

#include <memory>

namespace spell {

class SpellCoreClient;
class SpellSettingsStore;

/**
 * Sidebar panes available inside `SettingsDialog` (indices 0..10).
 */
enum class SettingsPaneId {
    General = 0,
    Engine,
    Statistics,
    Subagents,
    Conversation,
    Agent,
    OnlineResearch,
    Timers,
    Terminal,
    Diagnostics,
    About
};

inline constexpr int kSettingsPaneCount = 11;

/** Returns the human-readable sidebar/header title for `pane`. */
inline QString settingsPaneTitle(SettingsPaneId pane) {
    switch (pane) {
    case SettingsPaneId::General:        return QStringLiteral("General");
    case SettingsPaneId::Engine:         return QStringLiteral("Engine");
    case SettingsPaneId::Statistics:     return QStringLiteral("Statistics");
    case SettingsPaneId::Subagents:      return QStringLiteral("Subagents");
    case SettingsPaneId::Conversation:   return QStringLiteral("Conversation");
    case SettingsPaneId::Agent:          return QStringLiteral("Agent");
    case SettingsPaneId::OnlineResearch: return QStringLiteral("Online Research");
    case SettingsPaneId::Timers:         return QStringLiteral("Timers");
    case SettingsPaneId::Terminal:       return QStringLiteral("Terminal");
    case SettingsPaneId::Diagnostics:    return QStringLiteral("Diagnostics");
    case SettingsPaneId::About:          return QStringLiteral("About");
    }
    return QStringLiteral("General");
}

/** Returns the `spell::icons::vectorIcon` glyph name matching `pane`. */
inline QString settingsPaneIconName(SettingsPaneId pane) {
    switch (pane) {
    case SettingsPaneId::General:        return QStringLiteral("paintbrush");
    case SettingsPaneId::Engine:         return QStringLiteral("server");
    case SettingsPaneId::Statistics:     return QStringLiteral("speedometer");
    case SettingsPaneId::Subagents:      return QStringLiteral("cpu");
    case SettingsPaneId::Conversation:   return QStringLiteral("tag");
    case SettingsPaneId::Agent:          return QStringLiteral("repeat");
    case SettingsPaneId::OnlineResearch: return QStringLiteral("globe");
    case SettingsPaneId::Timers:         return QStringLiteral("bell");
    case SettingsPaneId::Terminal:       return QStringLiteral("terminal");
    case SettingsPaneId::Diagnostics:    return QStringLiteral("stethoscope");
    case SettingsPaneId::About:          return QStringLiteral("info");
    }
    return QStringLiteral("settings");
}

/** Parses a case-insensitive pane name (e.g. from `--settings-pane`) into `SettingsPaneId`. */
inline SettingsPaneId settingsPaneFromName(const QString& name) {
    const QString key = name.trimmed().toLower();
    if (key == QLatin1StringView("engine") || key == QLatin1StringView("endpoint")) {
        return SettingsPaneId::Engine;
    }
    if (key == QLatin1StringView("statistics") || key == QLatin1StringView("stats")) {
        return SettingsPaneId::Statistics;
    }
    if (key == QLatin1StringView("subagents") || key == QLatin1StringView("subagent")) {
        return SettingsPaneId::Subagents;
    }
    if (key == QLatin1StringView("conversation") || key == QLatin1StringView("tags")) {
        return SettingsPaneId::Conversation;
    }
    if (key == QLatin1StringView("agent")) {
        return SettingsPaneId::Agent;
    }
    if (key == QLatin1StringView("online research") ||
        key == QLatin1StringView("onlineresearch") ||
        key == QLatin1StringView("online_research") ||
        key == QLatin1StringView("online")) {
        return SettingsPaneId::OnlineResearch;
    }
    if (key == QLatin1StringView("timers") || key == QLatin1StringView("timer")) {
        return SettingsPaneId::Timers;
    }
    if (key == QLatin1StringView("terminal")) {
        return SettingsPaneId::Terminal;
    }
    if (key == QLatin1StringView("diagnostics")) {
        return SettingsPaneId::Diagnostics;
    }
    if (key == QLatin1StringView("about") || key == QLatin1StringView("licenses")) {
        return SettingsPaneId::About;
    }
    return SettingsPaneId::General;
}

/**
 * Unified frameless sidebar-driven Settings & Metrics sheet for BeatrixLeslie IDE.
 *
 * Hosts all 11 settings panes in a `QStackedWidget` of dedicated full-height
 * `QScrollArea` wrappers paired with a monochrome icon sidebar, distributes
 * workspace snapshots, system info, model discovery updates, and subagent slot
 * events to child panes, and forwards integrated terminal actions back to
 * `MainWindow`.
 */
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(SpellCoreClient* client,
                            SpellSettingsStore* store,
                            QWidget* parent = nullptr);
    ~SettingsDialog() override;

    SettingsDialog(const SettingsDialog&) = delete;
    SettingsDialog& operator=(const SettingsDialog&) = delete;
    SettingsDialog(SettingsDialog&&) = delete;
    SettingsDialog& operator=(SettingsDialog&&) = delete;

    /** Selects the active sidebar pane by enum identifier. */
    void selectPane(SettingsPaneId pane);

    /** Selects the active sidebar pane by zero-based index (`0 .. kSettingsPaneCount - 1`). */
    void selectPaneIndex(int idx);

    /** Returns the currently selected pane identifier. */
    SettingsPaneId currentPane() const;

    /** Returns the zero-based index of the currently selected pane. */
    int currentPaneIndex() const;

    /** Propagates a workspace snapshot update to all snapshot-aware child panes. */
    void updateSnapshot(const WorkspaceSnapshot& snap);

    /** Propagates host `SystemInfo` to Statistics and Diagnostics panes. */
    void updateSystemInfo(const SystemInfo& info);

    /** Synchronizes docked / popped-out state in `TerminalSettingsPane`. */
    void setTerminalState(bool docked, bool poppedOut);

    /** Re-applies current `ThemeManager` styling and icons across the dialog and all panes. */
    void applyTheme();

signals:
    void toggleDockedTerminalRequested();
    void togglePopOutTerminalRequested();
    void clearTerminalRequested();
    void restartTerminalRequested();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell

Q_DECLARE_METATYPE(spell::SettingsPaneId)
