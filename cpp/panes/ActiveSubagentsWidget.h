#pragma once

#include "core/SpellModels.h"

#include <QSize>
#include <QVector>
#include <QWidget>

#include <memory>

class QEnterEvent;
class QEvent;
class QHideEvent;
class QMouseEvent;
class QPaintEvent;

namespace spell {

/**
 * Compact monochrome pill displayed in the Spell IDE chat header while one or more
 * subagents (e.g. coding, critic, prefill, timer triage) are actively running.
 *
 * Displays the Spell witch hat mark (`spell::icons::hatMarkPixmap`) with a subtle
 * pulse animation and an active count/summary badge. Automatically hides itself when
 * no subagents are active. Clicking the pill opens a clean popup listing every active
 * subagent (`role`, `modelId`, `status`, and elapsed runtime).
 */
class ActiveSubagentsWidget : public QWidget {
    Q_OBJECT

public:
    explicit ActiveSubagentsWidget(QWidget* parent = nullptr);
    ~ActiveSubagentsWidget() override;

    ActiveSubagentsWidget(const ActiveSubagentsWidget&) = delete;
    ActiveSubagentsWidget& operator=(const ActiveSubagentsWidget&) = delete;
    ActiveSubagentsWidget(ActiveSubagentsWidget&&) = delete;
    ActiveSubagentsWidget& operator=(ActiveSubagentsWidget&&) = delete;

    /**
     * Replaces the displayed list of running subagents.
     * Shows the pill and starts the subtle pulse animation when `list` is non-empty;
     * hides the pill, stops pulsing, and closes any open popup when `list` is empty.
     */
    void setActiveSubagents(const QVector<ActiveSubagentInfo>& list);

    /** Returns the currently tracked active subagents. */
    [[nodiscard]] QVector<ActiveSubagentInfo> activeSubagents() const;

    /** Returns the number of currently active subagents. */
    [[nodiscard]] int activeCount() const;

    /** Returns true while the details popup is open. */
    [[nodiscard]] bool isPopupVisible() const;

    /** Opens, closes, or toggles the active-subagents inspection popup. */
    void showPopup();
    void hidePopup();
    void togglePopup();

    /** Re-applies the active Spell IDE monochrome palette and typography. */
    void applyTheme();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void popupVisibilityChanged(bool visible);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
