#include "widgets/SpellSplitHandle.h"

#include "theme/SpellTheme.h"

#include <QColor>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QEvent>
#include <QFrame>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPropertyAnimation>
#include <QRect>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace spell {

namespace {

constexpr int kFadeInDurationMs = 140;
constexpr int kFadeOutDurationMs = 180;
constexpr int kToggleButtonLengthPx = 44;
constexpr int kAutoHideHandleWidthPx = 9;
constexpr int kDefaultHandleWidthPx = 5;
constexpr int kTopBarHairlineY = 34;
constexpr qreal kDragThresholdPx = 4.0;
constexpr qreal kRevealEpsilon = 0.005;

inline uint32_t chalkHash(int coord) noexcept {
    return static_cast<uint32_t>(coord) * 2654435761u;
}

inline qreal hashUnit16(uint32_t h, int shift = 0) noexcept {
    return static_cast<qreal>((h >> shift) & 0xFFFFu) / 65535.0;
}

} // namespace

// ============================================================================
// SpellSplitterHandle
// ============================================================================

SpellSplitterHandle::SpellSplitterHandle(Qt::Orientation orientation, QSplitter* parent)
    : QSplitterHandle(orientation, parent) {
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setAutoFillBackground(false);
    setStyleSheet(QStringLiteral("background: transparent; border: none;"));

    m_revealAnim = new QPropertyAnimation(this, QByteArrayLiteral("revealOpacity"), this);

    updateCursor();
    updateToolTip();

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SpellSplitterHandle::applyTheme);
}

void SpellSplitterHandle::setShowGripPill(bool show) {
    if (m_showGripPill == show) {
        return;
    }
    m_showGripPill = show;
    update();
}

void SpellSplitterHandle::setGrabExtent(int px) {
    const int clamped = std::clamp(px, 1, 48);
    if (m_grabExtent == clamped) {
        return;
    }
    m_grabExtent = clamped;
    updateGeometry();
    if (QSplitter* s = splitter()) {
        s->refresh();
    }
    update();
}

void SpellSplitterHandle::setAutoHideOnIdle(bool enabled) {
    if (m_autoHideOnIdle == enabled) {
        return;
    }
    m_autoHideOnIdle = enabled;
    if (m_revealAnim) {
        m_revealAnim->stop();
    }
    m_revealOpacity = m_autoHideOnIdle ? ((m_hovered || m_dragging) ? 1.0 : 0.0) : 1.0;
    update();
}

void SpellSplitterHandle::setPaneToggleEnabled(bool enabled) {
    if (m_paneToggleEnabled == enabled) {
        return;
    }
    m_paneToggleEnabled = enabled;
    if (!m_paneToggleEnabled) {
        m_toggleHovered = false;
        m_pressStartedInToggle = false;
    }
    updateCursor();
    updateToolTip();
    update();
}

void SpellSplitterHandle::setSidePaneCollapsed(bool collapsed) {
    if (m_sidePaneCollapsed == collapsed) {
        return;
    }
    m_sidePaneCollapsed = collapsed;
    updateToolTip();
    update();
}

void SpellSplitterHandle::setRevealOpacity(qreal opacity) {
    const qreal clamped = std::clamp<qreal>(opacity, 0.0, 1.0);
    if (std::abs(m_revealOpacity - clamped) < 1e-4) {
        return;
    }
    m_revealOpacity = clamped;
    update();
}

QRect SpellSplitterHandle::toggleButtonRect() const {
    const QRect r = rect();
    if (r.isEmpty()) {
        return QRect();
    }

    if (orientation() == Qt::Horizontal) {
        const int maxH = (r.height() >= (kToggleButtonLengthPx + 8))
                             ? kToggleButtonLengthPx
                             : std::max(12, r.height() - 4);
        const int btnH = std::min(r.height(), maxH);
        const int top = r.top() + (r.height() - btnH) / 2;
        return QRect(r.left(), top, r.width(), btnH);
    }

    const int maxW = (r.width() >= (kToggleButtonLengthPx + 8))
                         ? kToggleButtonLengthPx
                         : std::max(12, r.width() - 4);
    const int btnW = std::min(r.width(), maxW);
    const int left = r.left() + (r.width() - btnW) / 2;
    return QRect(left, r.top(), btnW, r.height());
}

QSize SpellSplitterHandle::sizeHint() const {
    const int splitterWidth = splitter() ? splitter()->handleWidth() : 0;
    const int extent = std::max(1, std::max(m_grabExtent, splitterWidth));
    return QSize(extent, extent);
}

void SpellSplitterHandle::applyTheme() {
    update();
}

void SpellSplitterHandle::animateRevealTo(qreal targetOpacity, int durationMs) {
    if (!m_autoHideOnIdle) {
        if (m_revealAnim) {
            m_revealAnim->stop();
        }
        setRevealOpacity(1.0);
        return;
    }

    const qreal clampedTarget = std::clamp<qreal>(targetOpacity, 0.0, 1.0);
    if (!m_revealAnim) {
        setRevealOpacity(clampedTarget);
        return;
    }

    m_revealAnim->stop();
    const qreal delta = std::abs(clampedTarget - m_revealOpacity);
    if (delta < 1e-4) {
        setRevealOpacity(clampedTarget);
        return;
    }

    const int scaledDuration = std::max(
        16,
        static_cast<int>(std::lround(static_cast<qreal>(durationMs) * delta))
    );
    m_revealAnim->setDuration(scaledDuration);
    m_revealAnim->setStartValue(m_revealOpacity);
    m_revealAnim->setEndValue(clampedTarget);
    m_revealAnim->setEasingCurve(
        clampedTarget > m_revealOpacity ? QEasingCurve::OutCubic : QEasingCurve::InOutCubic
    );
    m_revealAnim->start();
}

void SpellSplitterHandle::updateCursor() {
    const Qt::CursorShape desired =
        (m_paneToggleEnabled && m_toggleHovered && !m_dragging)
            ? Qt::PointingHandCursor
            : (orientation() == Qt::Horizontal ? Qt::SplitHCursor : Qt::SplitVCursor);
    if (cursor().shape() != desired) {
        setCursor(desired);
    }
}

void SpellSplitterHandle::updateToolTip() {
    if (!m_paneToggleEnabled) {
        if (!toolTip().isEmpty()) {
            setToolTip(QString());
        }
        return;
    }

    if (m_hovered && !m_toggleHovered) {
        if (!toolTip().isEmpty()) {
            setToolTip(QString());
        }
        return;
    }

    const QString tip = m_sidePaneCollapsed ? tr("Show Pane") : tr("Hide/Show Pane");
    if (toolTip() != tip) {
        setToolTip(tip);
    }
}

void SpellSplitterHandle::paintChalkSeparator(QPainter& painter,
                                              const QRect& r,
                                              const QRect& dirty,
                                              qreal globalOpacity) {
    const bool isLight = (ThemeManager::instance().appearance() == Appearance::Light);
    const QColor chalkColor = isLight ? QColor(0x22, 0x22, 0x22) : QColor(0xFF, 0xFF, 0xFF);
    const qreal lineBaseAlpha = isLight
                                    ? (m_dragging ? 0.88 : 0.66)
                                    : (m_dragging ? 0.86 : 0.62);

    const QRect btnRect = m_paneToggleEnabled ? toggleButtonRect() : QRect();
    const bool hasToggle = m_paneToggleEnabled && !btnRect.isEmpty();
    const bool btnActive = m_toggleHovered || (m_pressActive && m_pressStartedInToggle);

    painter.save();
    painter.setOpacity(globalOpacity);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    if (orientation() == Qt::Horizontal) {
        const qreal cx = static_cast<qreal>(r.left()) + static_cast<qreal>(r.width()) * 0.5;
        const int clipTop = std::max(r.top(), dirty.top() - 1);
        const int clipBottom = std::min(r.bottom(), dirty.bottom() + 1);

        // 1. Single narrow hand-drawn white-chalk vertical line (~1.0-1.45px wide).
        for (int y = clipTop; y <= clipBottom; ++y) {
            if (hasToggle && y > btnRect.top() && y < btnRect.bottom()) {
                continue;
            }

            qreal edgeFade = 1.0;
            const int distFromEnd = std::min(y - r.top(), r.bottom() - y);
            if (distFromEnd < 14) {
                edgeFade = 0.25 + 0.75 * (static_cast<qreal>(distFromEnd) / 14.0);
            }

            qreal breakFade = 1.0;
            if (hasToggle) {
                const int distToBreak = std::min(std::abs(y - btnRect.top()),
                                                 std::abs(y - btnRect.bottom()));
                if (distToBreak <= 2) {
                    breakFade = 0.68 + 0.14 * static_cast<qreal>(distToBreak);
                }
            }

            const uint32_t h0 = chalkHash(y);
            const uint32_t h1 = (h0 ^ (h0 >> 16)) * 2246822519u;
            const uint32_t h2 = chalkHash(y / 3) ^ (h0 >> 13);

            const qreal n0 = hashUnit16(h0, 0);
            const qreal n1 = hashUnit16(h1, 0);
            const qreal n2 = hashUnit16(h2, 0);

            const qreal strokeW = 1.0 + n1 * 0.45;
            const qreal wobble = (hashUnit16(h1, 16) - 0.5) * 0.34;
            const qreal grain = 0.56 + 0.44 * (0.65 * n0 + 0.35 * n2);
            const qreal tooth = ((h0 >> 24) < 12u) ? 0.38 : 1.0;
            const qreal alpha = std::clamp(lineBaseAlpha * grain * tooth * edgeFade * breakFade,
                                           0.08, 1.0);

            painter.fillRect(
                QRectF(cx + wobble - strokeW * 0.5, static_cast<qreal>(y), strokeW, 1.05),
                withAlpha(chalkColor, alpha)
            );

            // Sparse ultra-fine chalk-dust micro-speck hugging the narrow line.
            if (((h1 >> 24) & 0xFFu) < 52u) {
                const qreal side = ((h0 >> 17) & 1u) ? 1.0 : -1.0;
                const qreal dustOffset = 0.65 + (static_cast<qreal>((h2 >> 16) & 0xFFu) / 255.0) * 0.35;
                const qreal dustAlpha = alpha * (0.16 + 0.16 * n0);
                painter.fillRect(
                    QRectF(cx + side * dustOffset - 0.35, static_cast<qreal>(y), 0.70, 0.90),
                    withAlpha(chalkColor, dustAlpha)
                );
            }
        }

        // 2. Centered hide/show toggle button seamlessly integrated into the narrow chalk line.
        if (hasToggle && btnRect.intersects(dirty)) {
            const int inset = (btnRect.height() >= 20) ? 4 : 1;
            const int segTop = btnRect.top() + inset;
            const int segBottom = btnRect.bottom() - inset;

            if (segBottom >= segTop) {
                if (btnActive) {
                    const qreal glowW = (m_pressActive && m_pressStartedInToggle) ? 4.5 : 4.0;
                    const qreal glowAlpha = (m_pressActive && m_pressStartedInToggle) ? 0.22 : 0.14;
                    const QRectF glowRect(
                        cx - glowW * 0.5,
                        static_cast<qreal>(segTop) - 1.5,
                        glowW,
                        static_cast<qreal>(segBottom - segTop + 1) + 3.0
                    );
                    painter.setBrush(withAlpha(chalkColor, glowAlpha));
                    painter.drawRoundedRect(glowRect, glowW * 0.5, glowW * 0.5);
                }

                const qreal baseSegW = btnActive ? 1.90 : (m_sidePaneCollapsed ? 1.75 : 1.55);
                const qreal segBaseAlpha = btnActive ? 0.98 : (m_sidePaneCollapsed ? 0.92 : 0.84);

                for (int y = segTop; y <= segBottom; ++y) {
                    const uint32_t h0 = chalkHash(y);
                    const uint32_t h1 = (h0 ^ (h0 >> 16)) * 2246822519u;
                    const qreal n0 = hashUnit16(h0, 0);
                    const qreal n1 = hashUnit16(h1, 0);

                    const qreal segW = std::min<qreal>(2.0, baseSegW + (n1 - 0.5) * 0.20);
                    const qreal wobble = (hashUnit16(h1, 16) - 0.5) * 0.14;
                    const qreal grain = 0.85 + 0.15 * n0;
                    const qreal alpha = std::clamp(segBaseAlpha * grain, 0.20, 1.0);

                    painter.fillRect(
                        QRectF(cx + wobble - segW * 0.5, static_cast<qreal>(y), segW, 1.05),
                        withAlpha(chalkColor, alpha)
                    );
                }

                // Delicate chalk-dust break end-ticks framing the toggle segment.
                const qreal tickW = btnActive ? 3.8 : 3.0;
                const qreal tickAlpha = btnActive ? 0.94 : 0.72;
                const QColor tickColor = withAlpha(chalkColor, tickAlpha);
                painter.fillRect(QRectF(cx - tickW * 0.5, static_cast<qreal>(segTop), tickW, 1.0),
                                 tickColor);
                painter.fillRect(QRectF(cx - tickW * 0.5, static_cast<qreal>(segBottom), tickW, 1.0),
                                 tickColor);

                if (m_sidePaneCollapsed && (segBottom - segTop) >= 12) {
                    const qreal midY = static_cast<qreal>(segTop + segBottom) * 0.5;
                    const qreal midW = btnActive ? 4.0 : 3.2;
                    painter.fillRect(QRectF(cx - midW * 0.5, midY - 0.5, midW, 1.0),
                                     withAlpha(chalkColor, btnActive ? 0.98 : 0.86));
                }
            }
        }
        painter.restore();
        return;
    }

    // Qt::Vertical splitter (horizontal chalk separator bar).
    const qreal cy = static_cast<qreal>(r.top()) + static_cast<qreal>(r.height()) * 0.5;
    const int clipLeft = std::max(r.left(), dirty.left() - 1);
    const int clipRight = std::min(r.right(), dirty.right() + 1);

    for (int x = clipLeft; x <= clipRight; ++x) {
        if (hasToggle && x > btnRect.left() && x < btnRect.right()) {
            continue;
        }

        qreal edgeFade = 1.0;
        const int distFromEnd = std::min(x - r.left(), r.right() - x);
        if (distFromEnd < 14) {
            edgeFade = 0.25 + 0.75 * (static_cast<qreal>(distFromEnd) / 14.0);
        }

        qreal breakFade = 1.0;
        if (hasToggle) {
            const int distToBreak = std::min(std::abs(x - btnRect.left()),
                                             std::abs(x - btnRect.right()));
            if (distToBreak <= 2) {
                breakFade = 0.68 + 0.14 * static_cast<qreal>(distToBreak);
            }
        }

        const uint32_t h0 = chalkHash(x);
        const uint32_t h1 = (h0 ^ (h0 >> 16)) * 2246822519u;
        const uint32_t h2 = chalkHash(x / 3) ^ (h0 >> 13);

        const qreal n0 = hashUnit16(h0, 0);
        const qreal n1 = hashUnit16(h1, 0);
        const qreal n2 = hashUnit16(h2, 0);

        const qreal strokeH = 1.0 + n1 * 0.45;
        const qreal wobble = (hashUnit16(h1, 16) - 0.5) * 0.34;
        const qreal grain = 0.56 + 0.44 * (0.65 * n0 + 0.35 * n2);
        const qreal tooth = ((h0 >> 24) < 12u) ? 0.38 : 1.0;
        const qreal alpha = std::clamp(lineBaseAlpha * grain * tooth * edgeFade * breakFade,
                                       0.08, 1.0);

        painter.fillRect(
            QRectF(static_cast<qreal>(x), cy + wobble - strokeH * 0.5, 1.05, strokeH),
            withAlpha(chalkColor, alpha)
        );

        if (((h1 >> 24) & 0xFFu) < 52u) {
            const qreal side = ((h0 >> 17) & 1u) ? 1.0 : -1.0;
            const qreal dustOffset = 0.65 + (static_cast<qreal>((h2 >> 16) & 0xFFu) / 255.0) * 0.35;
            const qreal dustAlpha = alpha * (0.16 + 0.16 * n0);
            painter.fillRect(
                QRectF(static_cast<qreal>(x), cy + side * dustOffset - 0.35, 0.90, 0.70),
                withAlpha(chalkColor, dustAlpha)
            );
        }
    }

    if (hasToggle && btnRect.intersects(dirty)) {
        const int inset = (btnRect.width() >= 20) ? 4 : 1;
        const int segLeft = btnRect.left() + inset;
        const int segRight = btnRect.right() - inset;

        if (segRight >= segLeft) {
            if (btnActive) {
                const qreal glowH = (m_pressActive && m_pressStartedInToggle) ? 4.5 : 4.0;
                const qreal glowAlpha = (m_pressActive && m_pressStartedInToggle) ? 0.22 : 0.14;
                const QRectF glowRect(
                    static_cast<qreal>(segLeft) - 1.5,
                    cy - glowH * 0.5,
                    static_cast<qreal>(segRight - segLeft + 1) + 3.0,
                    glowH
                );
                painter.setBrush(withAlpha(chalkColor, glowAlpha));
                painter.drawRoundedRect(glowRect, glowH * 0.5, glowH * 0.5);
            }

            const qreal baseSegH = btnActive ? 1.90 : (m_sidePaneCollapsed ? 1.75 : 1.55);
            const qreal segBaseAlpha = btnActive ? 0.98 : (m_sidePaneCollapsed ? 0.92 : 0.84);

            for (int x = segLeft; x <= segRight; ++x) {
                const uint32_t h0 = chalkHash(x);
                const uint32_t h1 = (h0 ^ (h0 >> 16)) * 2246822519u;
                const qreal n0 = hashUnit16(h0, 0);
                const qreal n1 = hashUnit16(h1, 0);

                const qreal segH = std::min<qreal>(2.0, baseSegH + (n1 - 0.5) * 0.20);
                const qreal wobble = (hashUnit16(h1, 16) - 0.5) * 0.14;
                const qreal grain = 0.85 + 0.15 * n0;
                const qreal alpha = std::clamp(segBaseAlpha * grain, 0.20, 1.0);

                painter.fillRect(
                    QRectF(static_cast<qreal>(x), cy + wobble - segH * 0.5, 1.05, segH),
                    withAlpha(chalkColor, alpha)
                );
            }

            const qreal tickH = btnActive ? 3.8 : 3.0;
            const qreal tickAlpha = btnActive ? 0.94 : 0.72;
            const QColor tickColor = withAlpha(chalkColor, tickAlpha);
            painter.fillRect(QRectF(static_cast<qreal>(segLeft), cy - tickH * 0.5, 1.0, tickH),
                             tickColor);
            painter.fillRect(QRectF(static_cast<qreal>(segRight), cy - tickH * 0.5, 1.0, tickH),
                             tickColor);

            if (m_sidePaneCollapsed && (segRight - segLeft) >= 12) {
                const qreal midX = static_cast<qreal>(segLeft + segRight) * 0.5;
                const qreal midH = btnActive ? 4.0 : 3.2;
                painter.fillRect(QRectF(midX - 0.5, cy - midH * 0.5, 1.0, midH),
                                 withAlpha(chalkColor, btnActive ? 0.98 : 0.86));
            }
        }
    }

    painter.restore();
}

void SpellSplitterHandle::paintEvent(QPaintEvent* event) {
    if (event && event->rect().isEmpty()) {
        return;
    }

    const QRect r = rect();
    if (r.isEmpty()) {
        return;
    }

    const QRect dirty = event ? event->rect().intersected(r) : r;
    if (dirty.isEmpty()) {
        return;
    }

    const UiPalette u = ThemeManager::instance().ui();
    const QColor restingBorder = compositeOver(u.subtleBorder, u.background);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    // 1. Always paint the crisp 1px resting pane border matching SpellHairline
    //    and the composer border. On horizontal workspace splitters, also join
    //    the y = 34 top tab-bar hairline cleanly with zero gap:
    //      - Handle 1 (Explorer | Editor): join right half from cx to r.right()
    //      - Handle 2 (Editor | Chat): join full width from r.left() to r.right()
    if (orientation() == Qt::Horizontal) {
        const int cx = r.left() + (r.width() - 1) / 2;
        painter.fillRect(QRect(cx, r.top(), 1, r.height()), restingBorder);

        const int joinY = r.top() + kTopBarHairlineY;
        if (joinY >= r.top() && joinY <= r.bottom()) {
            if (QSplitter* s = splitter()) {
                const int paneCount = s->count();
                const bool isHandle1 = (paneCount > 1 && s->handle(1) == this);
                const bool isHandle2 = (paneCount > 2 && s->handle(2) == this);
                if (isHandle2) {
                    painter.fillRect(QRect(r.left(), joinY, r.width(), 1), restingBorder);
                } else if (isHandle1) {
                    painter.fillRect(QRect(cx, joinY, r.right() - cx + 1, 1), restingBorder);
                }
            }
        }
    } else {
        const int cy = r.top() + (r.height() - 1) / 2;
        painter.fillRect(QRect(r.left(), cy, r.width(), 1), restingBorder);
    }

    // 2. Auto-hide-on-idle mode: smoothly fade the narrow hand-drawn white-chalk
    //    highlight + centered hide/show pane toggle button on top of the resting border.
    if (m_autoHideOnIdle) {
        const qreal opacity = m_dragging ? 1.0 : m_revealOpacity;
        if (opacity > kRevealEpsilon) {
            paintChalkSeparator(painter, r, dirty, opacity);
        }
        return;
    }

    // 3. Standard mode (internal sub-splitters): hover/drag highlight & optional grip pill.
    if (m_dragging || m_hovered) {
        const QColor activeColor = compositeOver(
            m_dragging ? u.selectedBorder : withAlpha(u.accent, 0.55),
            u.background
        );

        if (orientation() == Qt::Horizontal) {
            const int thickness = (m_dragging && r.width() >= 2) ? 2 : 1;
            const int x = r.left() + (r.width() - thickness) / 2;
            painter.fillRect(QRect(x, r.top(), thickness, r.height()), activeColor);
        } else {
            const int thickness = (m_dragging && !m_showGripPill && r.height() >= 2) ? 2 : 1;
            const int y = r.top() + (r.height() - thickness) / 2;
            painter.fillRect(QRect(r.left(), y, r.width(), thickness), activeColor);
        }
    }

    if (orientation() == Qt::Horizontal) {
        if (m_paneToggleEnabled && (m_hovered || m_toggleHovered || m_sidePaneCollapsed)) {
            const QRect btnRect = toggleButtonRect();
            if (!btnRect.isEmpty()) {
                const int segThickness = std::min(2, r.width());
                const int segX = r.left() + (r.width() - segThickness) / 2;
                const QColor segColor = m_toggleHovered
                                            ? u.accent
                                            : compositeOver(
                                                  withAlpha(u.accent, m_sidePaneCollapsed ? 0.85 : 0.72),
                                                  u.background);
                painter.fillRect(QRect(segX, btnRect.top() + 4, segThickness,
                                       std::max(1, btnRect.height() - 8)),
                                 segColor);
            }
        }
        return;
    }

    if (m_showGripPill && r.width() >= 24 && r.height() >= 3) {
        QColor pillColor = restingBorder;
        if (m_dragging) {
            pillColor = compositeOver(u.selectedBorder, u.background);
        } else if (m_hovered) {
            pillColor = compositeOver(withAlpha(u.accent, 0.55), u.background);
        }

        const qreal pillW = std::min<qreal>(40.0, static_cast<qreal>(r.width() - 16));
        const qreal pillH = (r.height() >= 8) ? 4.0 : 3.0;
        const QRectF pillRect(
            static_cast<qreal>(r.left()) + (static_cast<qreal>(r.width()) - pillW) * 0.5,
            static_cast<qreal>(r.top()) + (static_cast<qreal>(r.height()) - pillH) * 0.5,
            pillW,
            pillH
        );
        const qreal radius = pillH * 0.5;

        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(pillColor);
        painter.drawRoundedRect(pillRect, radius, radius);
    }
}

void SpellSplitterHandle::enterEvent(QEnterEvent* event) {
    m_hovered = true;
    if (event && m_paneToggleEnabled) {
        m_toggleHovered = toggleButtonRect().contains(event->position().toPoint());
    } else {
        m_toggleHovered = false;
    }
    updateCursor();
    updateToolTip();
    if (m_autoHideOnIdle) {
        animateRevealTo(1.0, kFadeInDurationMs);
    }
    update();
    QSplitterHandle::enterEvent(event);
}

void SpellSplitterHandle::leaveEvent(QEvent* event) {
    m_hovered = false;
    m_toggleHovered = false;
    updateCursor();
    updateToolTip();
    if (m_autoHideOnIdle && !m_dragging && !m_pressActive) {
        animateRevealTo(0.0, kFadeOutDurationMs);
    }
    update();
    QSplitterHandle::leaveEvent(event);
}

void SpellSplitterHandle::mousePressEvent(QMouseEvent* event) {
    if (!event || event->button() != Qt::LeftButton) {
        QSplitterHandle::mousePressEvent(event);
        return;
    }

    m_pressActive = true;
    m_pressPos = event->position();
    m_pressGlobalPos = event->globalPosition();
    m_pressStartedInToggle = m_paneToggleEnabled && toggleButtonRect().contains(event->pos());

    if (m_autoHideOnIdle) {
        animateRevealTo(1.0, kFadeInDurationMs);
    }

    if (m_pressStartedInToggle) {
        // Defer forwarding to QSplitterHandle until movement >= 4px so a pure click
        // on the centered toggle button never twitches the splitter by even 1px.
        m_dragging = false;
        m_dragForwardedToBase = false;
        m_toggleHovered = true;
        updateCursor();
        updateToolTip();
        event->accept();
        update();
        return;
    }

    m_dragging = true;
    m_dragForwardedToBase = true;
    m_toggleHovered = false;
    updateCursor();
    updateToolTip();
    update();
    QSplitterHandle::mousePressEvent(event);
}

void SpellSplitterHandle::mouseMoveEvent(QMouseEvent* event) {
    if (!event) {
        QSplitterHandle::mouseMoveEvent(event);
        return;
    }

    if (!m_hovered && rect().contains(event->pos())) {
        m_hovered = true;
        if (m_autoHideOnIdle && !m_dragging) {
            animateRevealTo(1.0, kFadeInDurationMs);
        }
    }

    if (m_pressActive && (event->buttons() & Qt::LeftButton)) {
        if (!m_dragForwardedToBase) {
            const QPointF delta = event->position() - m_pressPos;
            const qreal dist = std::hypot(delta.x(), delta.y());
            if (dist >= kDragThresholdPx) {
                m_dragging = true;
                m_dragForwardedToBase = true;
                m_toggleHovered = false;
                updateCursor();
                updateToolTip();

                QMouseEvent syntheticPress(
                    QEvent::MouseButtonPress,
                    m_pressPos,
                    m_pressGlobalPos,
                    Qt::LeftButton,
                    Qt::LeftButton,
                    event->modifiers()
                );
                QSplitterHandle::mousePressEvent(&syntheticPress);
                QSplitterHandle::mouseMoveEvent(event);
                update();
                return;
            }

            const bool nowInToggle = m_paneToggleEnabled && toggleButtonRect().contains(event->pos());
            if (m_toggleHovered != nowInToggle) {
                m_toggleHovered = nowInToggle;
                updateCursor();
                updateToolTip();
                update();
            }
            event->accept();
            return;
        }

        QSplitterHandle::mouseMoveEvent(event);
        return;
    }

    const bool nowInToggle = m_paneToggleEnabled && toggleButtonRect().contains(event->pos());
    if (m_toggleHovered != nowInToggle) {
        m_toggleHovered = nowInToggle;
        updateCursor();
        updateToolTip();
        update();
    } else {
        updateCursor();
    }

    QSplitterHandle::mouseMoveEvent(event);
}

void SpellSplitterHandle::mouseReleaseEvent(QMouseEvent* event) {
    if (!event || event->button() != Qt::LeftButton) {
        QSplitterHandle::mouseReleaseEvent(event);
        return;
    }

    const bool wasPressActive = m_pressActive;
    const bool wasInToggle = m_pressStartedInToggle;
    const bool wasForwarded = m_dragForwardedToBase;
    const bool wasDragging = m_dragging;
    const QPointF delta = event->position() - m_pressPos;
    const qreal movedDist = std::hypot(delta.x(), delta.y());

    m_pressActive = false;
    m_pressStartedInToggle = false;
    m_dragForwardedToBase = false;
    m_dragging = false;

    if (wasForwarded) {
        QSplitterHandle::mouseReleaseEvent(event);
    } else {
        event->accept();
    }

    const bool stillInside = rect().contains(event->pos());
    m_hovered = stillInside;
    m_toggleHovered = stillInside && m_paneToggleEnabled && toggleButtonRect().contains(event->pos());
    updateCursor();
    updateToolTip();

    if (m_autoHideOnIdle && !m_hovered) {
        animateRevealTo(0.0, kFadeOutDurationMs);
    }
    update();

    if (wasPressActive && wasInToggle && !wasForwarded && !wasDragging
        && m_paneToggleEnabled && movedDist < kDragThresholdPx) {
        emit togglePaneClicked();
    }
}

// ============================================================================
// SpellSplitter
// ============================================================================

SpellSplitter::SpellSplitter(QWidget* parent)
    : QSplitter(parent) {
    initDefaults();
}

SpellSplitter::SpellSplitter(Qt::Orientation orientation, QWidget* parent)
    : QSplitter(orientation, parent) {
    initDefaults();
}

void SpellSplitter::initDefaults() {
    setOpaqueResize(true);
    setChildrenCollapsible(false);
    setHandleWidth(kDefaultHandleWidthPx);
    setFrameShape(QFrame::NoFrame);
    setAutoFillBackground(true);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SpellSplitter::applyTheme);

    applyTheme();
}

void SpellSplitter::setShowGripPill(bool show) {
    if (m_showGripPill == show) {
        return;
    }
    m_showGripPill = show;

    const int n = count();
    for (int i = 0; i < n; ++i) {
        if (auto* h = qobject_cast<SpellSplitterHandle*>(handle(i))) {
            h->setShowGripPill(m_showGripPill);
        }
    }
}

void SpellSplitter::setAutoHideOnIdle(bool enabled) {
    if (m_autoHideOnIdle == enabled) {
        return;
    }
    m_autoHideOnIdle = enabled;

    if (m_autoHideOnIdle) {
        if (handleWidth() < kAutoHideHandleWidthPx) {
            setHandleWidth(kAutoHideHandleWidthPx);
            m_autoAdjustedHandleWidth = true;
        }
    } else if (m_autoAdjustedHandleWidth && handleWidth() == kAutoHideHandleWidthPx) {
        setHandleWidth(kDefaultHandleWidthPx);
        m_autoAdjustedHandleWidth = false;
    }

    const int n = count();
    for (int i = 0; i < n; ++i) {
        if (auto* h = qobject_cast<SpellSplitterHandle*>(handle(i))) {
            h->setAutoHideOnIdle(m_autoHideOnIdle);
        }
    }
}

void SpellSplitter::setPaneToggleEnabled(bool enabled) {
    if (m_paneToggleEnabled == enabled) {
        return;
    }
    m_paneToggleEnabled = enabled;

    const int n = count();
    for (int i = 0; i < n; ++i) {
        if (auto* h = qobject_cast<SpellSplitterHandle*>(handle(i))) {
            h->setPaneToggleEnabled(m_paneToggleEnabled);
        }
    }
}

void SpellSplitter::setHandleCollapsedState(int handleIndex, bool collapsed) {
    if (handleIndex < 0 || handleIndex >= count()) {
        return;
    }
    if (auto* h = qobject_cast<SpellSplitterHandle*>(handle(handleIndex))) {
        h->setSidePaneCollapsed(collapsed);
    }
}

void SpellSplitter::applyTheme() {
    const UiPalette u = ThemeManager::instance().ui();
    QPalette pal = palette();
    pal.setColor(QPalette::Window, u.background);
    pal.setColor(QPalette::Base, u.surface);
    setPalette(pal);
    update();

    const int n = count();
    for (int i = 0; i < n; ++i) {
        if (auto* h = qobject_cast<SpellSplitterHandle*>(handle(i))) {
            h->applyTheme();
        }
    }
}

QSplitterHandle* SpellSplitter::createHandle() {
    auto* h = new SpellSplitterHandle(orientation(), this);
    h->setShowGripPill(m_showGripPill);
    h->setAutoHideOnIdle(m_autoHideOnIdle);
    h->setPaneToggleEnabled(m_paneToggleEnabled);
    connect(h, &SpellSplitterHandle::togglePaneClicked, this, [this, h]() {
        const int n = count();
        for (int i = 0; i < n; ++i) {
            if (handle(i) == h) {
                emit paneToggleRequested(i);
                return;
            }
        }
    });
    return h;
}

} // namespace spell
