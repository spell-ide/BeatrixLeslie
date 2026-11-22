#pragma once

#include <QWidget>

#include <memory>

namespace spell {

/**
 * Settings -> About pane for BeatrixLeslie IDE.
 *
 * Displays BeatrixLeslie IDE branding (using the shipped logo/hat assets), version and
 * bundle identifier (`com.spell.BeatrixLeslieIDE`), portable runtime information, and
 * an embedded viewer for third-party open-source licenses and notices
 * (`THIRD_PARTY_LICENSES.txt` / `THIRD_PARTY_NOTICES.md`).
 */
class AboutSettingsPane : public QWidget {
    Q_OBJECT

public:
    explicit AboutSettingsPane(QWidget* parent = nullptr);
    ~AboutSettingsPane() override;

    AboutSettingsPane(const AboutSettingsPane&) = delete;
    AboutSettingsPane& operator=(const AboutSettingsPane&) = delete;
    AboutSettingsPane(AboutSettingsPane&&) = delete;
    AboutSettingsPane& operator=(AboutSettingsPane&&) = delete;

    /** Re-applies current `ThemeManager` styling and re-tints branding assets. */
    void applyTheme();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
