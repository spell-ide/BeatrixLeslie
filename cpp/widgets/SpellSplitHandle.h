#pragma once

#include <QPointF>
#include <QRect>
#include <QSize>
#include <QSplitter>
#include <QSplitterHandle>

class QEnterEvent;
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QPropertyAnimation;

namespace spell {

/**
 * Custom splitter handle that always paints a crisp 1px theme-aware resting border
 * (`ui.subtleBorder` over `ui.background`, matching `SpellHairline` and composer borders)
 * plus top hairline joins for the 34px workspace tab bars, and either:
 * - Highlights subtly on hover/drag (default mode for internal sub-splitters), or
 * - Smoothly fades in a narrow hand-drawn white-chalk highlight and centered hide/show
 *   pane toggle segment on top of the resting border on hover/drag
 *   (`autoHideOnIdle` + `paneToggleEnabled`).
 *
 * Zero layout shift or cursor flicker on hover/drag.
 * Note: Overrides stylesheet handle rendering via custom `paintEvent` so the global
 * `QSplitter::handle` QSS rule never collapses the hit target to 1px.
 */
class SpellSplitterHandle : public QSplitterHandle {
    Q_OBJECT
    Q_PROPERTY(qreal revealOpacity READ revealOpacity WRITE setRevealOpacity)

public:
    explicit SpellSplitterHandle(Qt::Orientation orientation, QSplitter* parent);
    ~SpellSplitterHandle() override = default;

    bool isHovered() const { return m_hovered; }
    bool isDragging() const { return m_dragging; }

    /** When enabled on vertical splitters, draws a subtle centered grab pill over the hairline. */
    bool showGripPill() const { return m_showGripPill; }
    void setShowGripPill(bool show);

    /** Logical hit-region extent along the resize axis (default 9px for effortless grabbing). */
    int grabExtent() const { return m_grabExtent; }
    void setGrabExtent(int px);

    /**
     * When true, shows only the crisp 1px resting border on idle and smoothly fades in
     * the narrow hand-drawn chalk highlight + toggle segment on hover/drag.
     */
    bool autoHideOnIdle() const { return m_autoHideOnIdle; }
    void setAutoHideOnIdle(bool enabled);

    /** Enables the centered clickable hide/show pane toggle segment along the handle. */
    bool paneToggleEnabled() const { return m_paneToggleEnabled; }
    void setPaneToggleEnabled(bool enabled);

    /** Reflects whether the side pane controlled by this handle is currently collapsed. */
    bool isSidePaneCollapsed() const { return m_sidePaneCollapsed; }
    void setSidePaneCollapsed(bool collapsed);

    /** Current animated opacity in [0.0, 1.0] used when autoHideOnIdle is enabled. */
    qreal revealOpacity() const { return m_revealOpacity; }
    void setRevealOpacity(qreal opacity);

    /** Narrow centered clickable hit rectangle for the hide/show pane toggle segment. */
    QRect toggleButtonRect() const;

    QSize sizeHint() const override;

signals:
    void togglePaneClicked();

public slots:
    void applyTheme();

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void animateRevealTo(qreal targetOpacity, int durationMs);
    void updateCursor();
    void updateToolTip();
    void paintChalkSeparator(QPainter& painter, const QRect& r, const QRect& dirty, qreal globalOpacity);

    bool m_hovered = false;
    bool m_dragging = false;
    bool m_showGripPill = false;
    int m_grabExtent = 9;

    bool m_autoHideOnIdle = false;
    bool m_paneToggleEnabled = false;
    bool m_sidePaneCollapsed = false;
    bool m_toggleHovered = false;

    bool m_pressActive = false;
    bool m_pressStartedInToggle = false;
    bool m_dragForwardedToBase = false;
    QPointF m_pressPos;
    QPointF m_pressGlobalPos;

    qreal m_revealOpacity = 1.0;
    QPropertyAnimation* m_revealAnim = nullptr;
};

/**
 * Theme-aware QSplitter configured for flicker-free live resizing in Spell IDE.
 *
 * Defaults:
 * - `setOpaqueResize(true)`
 * - `setChildrenCollapsible(false)`
 * - `setHandleWidth(5)` (automatically expanded to 9px when `autoHideOnIdle` is enabled)
 * - `createHandle()` returns `new SpellSplitterHandle(orientation(), this)`
 */
class SpellSplitter : public QSplitter {
    Q_OBJECT

public:
    explicit SpellSplitter(QWidget* parent = nullptr);
    explicit SpellSplitter(Qt::Orientation orientation, QWidget* parent = nullptr);
    ~SpellSplitter() override = default;

    bool showGripPill() const { return m_showGripPill; }
    void setShowGripPill(bool show);

    bool autoHideOnIdle() const { return m_autoHideOnIdle; }
    void setAutoHideOnIdle(bool enabled);

    bool paneToggleEnabled() const { return m_paneToggleEnabled; }
    void setPaneToggleEnabled(bool enabled);

    void setHandleCollapsedState(int handleIndex, bool collapsed);

signals:
    void paneToggleRequested(int handleIndex);

public slots:
    void applyTheme();

protected:
    QSplitterHandle* createHandle() override;

private:
    void initDefaults();

    bool m_showGripPill = false;
    bool m_autoHideOnIdle = false;
    bool m_paneToggleEnabled = false;
    bool m_autoAdjustedHandleWidth = false;
};

} // namespace spell
