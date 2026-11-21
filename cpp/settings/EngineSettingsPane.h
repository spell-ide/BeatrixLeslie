#pragma once

#include "core/SpellModels.h"

#include <QString>
#include <QVector>
#include <QWidget>

#include <memory>

namespace spell {

class SpellCoreClient;
class SpellSettingsStore;

/**
 * Settings -> Engine pane for BeatrixLeslie IDE.
 *
 * Endpoint-agnostic configuration for any OpenAI-compatible inference provider
 * (local server or hosted API): base URL, OS keyring API key, primary model
 * selection, optional subagent model override, Gatekeeper Critic toggle, model
 * discovery via `SpellCoreClient::fetchModels`, and live connection status.
 */
class EngineSettingsPane : public QWidget {
    Q_OBJECT

public:
    explicit EngineSettingsPane(SpellCoreClient* client,
                                SpellSettingsStore* store,
                                QWidget* parent = nullptr);
    ~EngineSettingsPane() override;

    EngineSettingsPane(const EngineSettingsPane&) = delete;
    EngineSettingsPane& operator=(const EngineSettingsPane&) = delete;
    EngineSettingsPane(EngineSettingsPane&&) = delete;
    EngineSettingsPane& operator=(EngineSettingsPane&&) = delete;

    /** Synchronizes endpoint status, latency, and active model fields from a snapshot. */
    void updateSnapshot(const WorkspaceSnapshot& snap);

    /** Populates model selectors or reports a fetch error after `fetchModels()`. */
    void onModelsFetched(const QVector<ModelDescriptor>& models, const QString& error);

    /** Re-applies current `ThemeManager` styling to this pane. */
    void applyTheme();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
