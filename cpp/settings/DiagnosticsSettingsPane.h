#pragma once

#include "core/SpellModels.h"

#include <QWidget>

#include <memory>

namespace spell {

class SpellCoreClient;
class SpellSettingsStore;

/**
 * Settings -> Diagnostics pane for BeatrixLeslie IDE.
 *
 * Shows portable local data and log paths (`spell.log`), live log preview,
 * system and endpoint diagnostics summary, and one-click export of a redacted
 * diagnostics bundle via `SpellSettingsStore::exportDiagnosticsBundle` (never
 * including API keys or keyring secrets).
 */
class DiagnosticsSettingsPane : public QWidget {
    Q_OBJECT

public:
    explicit DiagnosticsSettingsPane(SpellCoreClient* client,
                                     SpellSettingsStore* store,
                                     QWidget* parent = nullptr);
    ~DiagnosticsSettingsPane() override;

    DiagnosticsSettingsPane(const DiagnosticsSettingsPane&) = delete;
    DiagnosticsSettingsPane& operator=(const DiagnosticsSettingsPane&) = delete;
    DiagnosticsSettingsPane(DiagnosticsSettingsPane&&) = delete;
    DiagnosticsSettingsPane& operator=(DiagnosticsSettingsPane&&) = delete;

    /** Caches the latest workspace snapshot for status rows and diagnostics export. */
    void updateSnapshot(const WorkspaceSnapshot& snap);

    /** Updates portable data/log directory rows and caches `SystemInfo` for export. */
    void updateSystemInfo(const SystemInfo& info);

    /** Re-applies current `ThemeManager` styling to this pane. */
    void applyTheme();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
