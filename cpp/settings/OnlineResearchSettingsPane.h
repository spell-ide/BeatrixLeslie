#pragma once

#include "core/SpellModels.h"

#include <QWidget>

#include <memory>

namespace spell {

class SpellCoreClient;
class SpellSettingsStore;

/**
 * Settings -> Online Research pane for BeatrixLeslie IDE.
 *
 * Controls outbound research posture (`offline` / `curated` / `open_web`) with
 * an immediate Air-Gap Now kill-switch, domain allowlist/denylist rules, fetch
 * limits and timeout, web search provider & endpoint settings, and OS keyring
 * API tokens (`com.spell.SpellIDE.credentials`) managed through `SpellSettingsStore`.
 */
class OnlineResearchSettingsPane : public QWidget {
    Q_OBJECT

public:
    explicit OnlineResearchSettingsPane(SpellCoreClient* client,
                                        SpellSettingsStore* store,
                                        QWidget* parent = nullptr);
    ~OnlineResearchSettingsPane() override;

    OnlineResearchSettingsPane(const OnlineResearchSettingsPane&) = delete;
    OnlineResearchSettingsPane& operator=(const OnlineResearchSettingsPane&) = delete;
    OnlineResearchSettingsPane(OnlineResearchSettingsPane&&) = delete;
    OnlineResearchSettingsPane& operator=(OnlineResearchSettingsPane&&) = delete;

    /** Synchronizes network mode, domain lists, search provider, and token status badges. */
    void updateSnapshot(const WorkspaceSnapshot& snap);

    /** Re-applies current `ThemeManager` styling to this pane. */
    void applyTheme();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
