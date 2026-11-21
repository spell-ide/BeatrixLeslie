#pragma once

#include "core/SpellModels.h"

#include <QWidget>

#include <memory>

namespace spell {

class SpellCoreClient;

/**
 * Settings -> Statistics pane for BeatrixLeslie IDE.
 *
 * Displays live endpoint telemetry (decode tok/s, EWMA tok/s, endpoint latency,
 * compaction epoch, active and cumulative token counts), local context epoch
 * reset controls, and portable host hardware/OS summary from `SystemInfo`.
 */
class StatisticsSettingsPane : public QWidget {
    Q_OBJECT

public:
    explicit StatisticsSettingsPane(SpellCoreClient* client, QWidget* parent = nullptr);
    ~StatisticsSettingsPane() override;

    StatisticsSettingsPane(const StatisticsSettingsPane&) = delete;
    StatisticsSettingsPane& operator=(const StatisticsSettingsPane&) = delete;
    StatisticsSettingsPane(StatisticsSettingsPane&&) = delete;
    StatisticsSettingsPane& operator=(StatisticsSettingsPane&&) = delete;

    /** Updates telemetry tiles, token counters, and active thread context stats. */
    void updateSnapshot(const WorkspaceSnapshot& snap);

    /** Updates portable host OS, CPU, architecture, and memory summary rows. */
    void updateSystemInfo(const SystemInfo& info);

    /** Re-applies current `ThemeManager` styling to this pane. */
    void applyTheme();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
