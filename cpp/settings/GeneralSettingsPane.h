#pragma once

#include <QWidget>

#include <memory>

namespace spell {

/**
 * Settings -> General pane for BeatrixLeslie IDE.
 *
 * Manages cross-platform appearance (Dark / White), syntax-highlighting code
 * theme with a live code preview, editor monospace font family and size, and
 * integrated terminal palette and font size with a live ANSI preview.
 * All preferences persist through `ThemeManager::instance()`.
 */
class GeneralSettingsPane : public QWidget {
    Q_OBJECT

public:
    explicit GeneralSettingsPane(QWidget* parent = nullptr);
    ~GeneralSettingsPane() override;

    GeneralSettingsPane(const GeneralSettingsPane&) = delete;
    GeneralSettingsPane& operator=(const GeneralSettingsPane&) = delete;
    GeneralSettingsPane(GeneralSettingsPane&&) = delete;
    GeneralSettingsPane& operator=(GeneralSettingsPane&&) = delete;

    /** Re-applies current `ThemeManager` palette, fonts, and live previews. */
    void applyTheme();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
