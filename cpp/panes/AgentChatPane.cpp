#include "panes/AgentChatPane.h"

#include "core/SpellCoreClient.h"
#include "shell/WindowChrome.h"
#include "terminal/SpellTerminalPanel.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"
#include "widgets/SpellSplitHandle.h"

#include <QEnterEvent>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPen>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSize>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace spell {

namespace {

constexpr int kTopBarHeight = 34;
constexpr int kTabPadH = 12;
constexpr int kTabSpacing = 6;
constexpr int kTabSlotSize = 12;
constexpr int kTabDividerWidth = 1;
constexpr int kTabMaxWidth = 220;

constexpr int kBusySpinnerIntervalMs = 50;
constexpr int kBusySpinnerStepDeg = 18;

constexpr int kMinTimelineHeight = 120;
constexpr int kMinTerminalHeight = 120;
constexpr int kMinComposerHeightFloor = 72;

constexpr int kSubagentOverlayTopMargin = 12;
constexpr int kSubagentOverlayRightMargin = 16;

struct ScopedBoolGuard {
    bool& target;
    bool previous;

    explicit ScopedBoolGuard(bool& flag, bool value = true)
        : target(flag)
        , previous(flag) {
        target = value;
    }

    ~ScopedBoolGuard() {
        target = previous;
    }

    ScopedBoolGuard(const ScopedBoolGuard&) = delete;
    ScopedBoolGuard& operator=(const ScopedBoolGuard&) = delete;
};

/// Minimalist flat conversation tab matching FileTabBar pixel-for-pixel
/// (intentionally no Q_OBJECT macro for clean automoc).
class ConversationTabWidget final : public QWidget {
public:
    using Callback = std::function<void()>;

    ConversationTabWidget(const QString& threadId,
                          const QString& title,
                          const QString& projectName,
                          Callback onSelect,
                          Callback onClose,
                          QWidget* parent = nullptr)
        : QWidget(parent)
        , m_threadId(threadId)
        , m_title(title)
        , m_projectName(projectName)
        , m_onSelect(std::move(onSelect))
        , m_onClose(std::move(onClose)) {
        setObjectName(QStringLiteral("conversationTab"));
        setFixedHeight(kTopBarHeight);
        setMaximumWidth(kTabMaxWidth);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover, true);
        setCursor(Qt::PointingHandCursor);
        refreshTooltip();
    }

    [[nodiscard]] const QString& threadId() const {
        return m_threadId;
    }

    void setTitleAndProject(const QString& title, const QString& projectName) {
        const bool titleChanged = (m_title != title);
        const bool projectChanged = (m_projectName != projectName);
        if (!titleChanged && !projectChanged) {
            return;
        }
        m_title = title;
        m_projectName = projectName;
        refreshTooltip();
        if (titleChanged) {
            updateGeometry();
        }
        update();
    }

    void setActive(bool active) {
        if (m_active == active) {
            return;
        }
        m_active = active;
        update();
    }

    void setBusy(bool busy) {
        if (m_busy == busy) {
            return;
        }
        m_busy = busy;
        update();
    }

    [[nodiscard]] bool isBusy() const {
        return m_busy;
    }

    void advanceSpinner(int stepDeg) {
        if (!m_busy) {
            return;
        }
        m_spinnerPhaseDeg = (m_spinnerPhaseDeg + stepDeg) % 360;
        if (!m_hovered) {
            update(slotRect().adjusted(-2, -2, 2, 2));
        }
    }

    void applyTheme() {
        updateGeometry();
        update();
    }

    QSize sizeHint() const override {
        QFont measureFont = QGuiApplication::font();
        measureFont.setPointSizeF(11.5);
        measureFont.setWeight(QFont::DemiBold);
        const QFontMetrics fm(measureFont);
        const int textWidth = fm.horizontalAdvance(effectiveTitle());
        const int chromeWidth = (2 * kTabPadH) + kTabSpacing + kTabSlotSize + kTabDividerWidth;
        const int preferredWidth = chromeWidth + std::max(8, textWidth);
        return QSize(std::min(kTabMaxWidth, preferredWidth), kTopBarHeight);
    }

    QSize minimumSizeHint() const override {
        return sizeHint();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);

        const UiPalette u = ThemeManager::instance().ui();
        const QColor dividerCol = compositeOver(u.divider, u.background);

        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        const int contentW = std::max(0, width() - kTabDividerWidth);
        QColor tabBg = u.background;
        if (m_active) {
            tabBg = compositeOver(u.elevatedSurface, u.background);
            p.fillRect(QRect(0, 0, contentW, height()), tabBg);
        } else if (m_hovered) {
            const QColor hoverTint = withAlpha(u.elevatedSurface, u.elevatedSurface.alphaF() * 0.55);
            tabBg = compositeOver(hoverTint, u.background);
            p.fillRect(QRect(0, 0, contentW, height()), tabBg);
        }

        // Crisp 1px solid ui.textPrimary bottom underline bar on selected tab
        if (m_active) {
            p.fillRect(QRect(0, height() - 1, contentW, 1), u.textPrimary);
        }

        // Crisp 1px right vertical hairline divider
        p.fillRect(QRect(width() - kTabDividerWidth, 0, kTabDividerWidth, height()), dividerCol);

        // Conversation title label (11.5pt semibold active / regular inactive)
        QFont font = QGuiApplication::font();
        font.setPointSizeF(11.5);
        font.setWeight(m_active ? QFont::DemiBold : QFont::Normal);
        p.setFont(font);
        const QColor textCol = m_active ? u.textPrimary : compositeOver(u.textSecondary, tabBg);
        p.setPen(textCol);

        const QRect slot = slotRect();
        const int textW = std::max(0, slot.left() - kTabSpacing - kTabPadH);
        const QRect textRect(kTabPadH, 0, textW, height());
        const QFontMetrics fm(font);
        const QString elided = fm.elidedText(effectiveTitle(), Qt::ElideRight, textW);
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, elided);

        // Right 12x12 busy indicator / close button slot
        const QPointF center(slot.x() + (kTabSlotSize * 0.5),
                             slot.y() + (kTabSlotSize * 0.5));
        if (showsBusyIndicator()) {
            p.setRenderHint(QPainter::Antialiasing, true);
            const QRectF arcRect(center.x() - 4.2, center.y() - 4.2, 8.4, 8.4);
            const QColor trackCol = compositeOver(withAlpha(u.textMuted, 0.28), tabBg);
            const QColor arcCol = compositeOver(u.textSecondary, tabBg);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(trackCol, 1.35, Qt::SolidLine, Qt::RoundCap));
            p.drawEllipse(arcRect);
            p.setPen(QPen(arcCol, 1.45, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(arcRect, (90 - m_spinnerPhaseDeg) * 16, -240 * 16);
        } else if (showsCloseButton()) {
            p.setRenderHint(QPainter::Antialiasing, true);
            if (m_closeHovered) {
                p.setPen(Qt::NoPen);
                p.setBrush(compositeOver(u.selectedFill, tabBg));
                p.drawRoundedRect(QRectF(center.x() - 7.5, center.y() - 7.5, 15.0, 15.0), 3.0, 3.0);
            }
            const QColor xColor = m_closeHovered ? u.textPrimary : compositeOver(u.textMuted, tabBg);
            QPen xPen(xColor, 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(xPen);
            p.setBrush(Qt::NoBrush);
            constexpr qreal arm = 3.0;
            p.drawLine(QPointF(center.x() - arm, center.y() - arm),
                       QPointF(center.x() + arm, center.y() + arm));
            p.drawLine(QPointF(center.x() + arm, center.y() - arm),
                       QPointF(center.x() - arm, center.y() + arm));
        }
    }

    void enterEvent(QEnterEvent* event) override {
        m_hovered = true;
        m_closeHovered = event && closeHitRect().contains(event->position().toPoint());
        update();
        QWidget::enterEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (event) {
            const bool nowCloseHovered = closeHitRect().contains(event->position().toPoint());
            if (nowCloseHovered != m_closeHovered) {
                m_closeHovered = nowCloseHovered;
                update();
            }
        }
        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        m_hovered = false;
        m_closeHovered = false;
        m_closePressed = false;
        update();
        QWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (!event) {
            QWidget::mousePressEvent(event);
            return;
        }
        if (event->button() == Qt::LeftButton) {
            const QPoint pos = event->position().toPoint();
            if (showsCloseButton() && closeHitRect().contains(pos)) {
                m_closePressed = true;
                event->accept();
                return;
            }
            m_closePressed = false;
            if (m_onSelect) {
                m_onSelect();
            }
            event->accept();
            return;
        }
        if (event->button() == Qt::MiddleButton) {
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!event) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        if (event->button() == Qt::LeftButton && m_closePressed) {
            const bool inside = closeHitRect().contains(event->position().toPoint());
            m_closePressed = false;
            if (inside && m_onClose) {
                m_onClose();
            }
            event->accept();
            return;
        }
        if (event->button() == Qt::MiddleButton) {
            if (rect().contains(event->position().toPoint()) && m_onClose) {
                m_onClose();
            }
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    [[nodiscard]] QString effectiveTitle() const {
        const QString trimmed = m_title.trimmed();
        return trimmed.isEmpty() ? QStringLiteral("Conversation") : trimmed;
    }

    void refreshTooltip() {
        const QString title = effectiveTitle();
        const QString project = m_projectName.trimmed();
        const QString tip = project.isEmpty()
            ? title
            : QStringLiteral("%1 · %2").arg(project, title);
        if (toolTip() != tip) {
            setToolTip(tip);
        }
    }

    [[nodiscard]] QRect slotRect() const {
        const int x = width() - kTabDividerWidth - kTabPadH - kTabSlotSize;
        const int y = (height() - kTabSlotSize) / 2;
        return QRect(x, y, kTabSlotSize, kTabSlotSize);
    }

    [[nodiscard]] QRect closeHitRect() const {
        return slotRect().adjusted(-3, -3, 3, 3);
    }

    [[nodiscard]] bool showsBusyIndicator() const {
        return m_busy && !m_hovered;
    }

    [[nodiscard]] bool showsCloseButton() const {
        return !showsBusyIndicator() && (m_hovered || m_active);
    }

    QString m_threadId;
    QString m_title;
    QString m_projectName;
    Callback m_onSelect;
    Callback m_onClose;
    bool m_active = false;
    bool m_busy = false;
    bool m_hovered = false;
    bool m_closeHovered = false;
    bool m_closePressed = false;
    int m_spinnerPhaseDeg = 0;
};

/// Frameless horizontal scroll strip for flat conversation tabs (matching FileTabScrollArea).
class ConversationTabScrollArea final : public QScrollArea {
public:
    explicit ConversationTabScrollArea(QWidget* parent = nullptr)
        : QScrollArea(parent) {
        setObjectName(QStringLiteral("conversationTabBar"));
        setFrameShape(QFrame::NoFrame);
        setFixedHeight(kTopBarHeight);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setWidgetResizable(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void wheelEvent(QWheelEvent* event) override {
        if (!event) {
            return;
        }
        QScrollBar* hBar = horizontalScrollBar();
        if (!hBar || hBar->maximum() <= hBar->minimum()) {
            event->ignore();
            return;
        }
        const QPoint pixelDelta = event->pixelDelta();
        const QPoint angleDelta = event->angleDelta();
        int delta = 0;
        if (!pixelDelta.isNull()) {
            delta = (std::abs(pixelDelta.x()) >= std::abs(pixelDelta.y()))
                        ? pixelDelta.x()
                        : pixelDelta.y();
        } else if (!angleDelta.isNull()) {
            delta = (std::abs(angleDelta.x()) >= std::abs(angleDelta.y()))
                        ? angleDelta.x()
                        : angleDelta.y();
            delta /= 2;
        }
        if (delta != 0) {
            hBar->setValue(hBar->value() - delta);
            event->accept();
            return;
        }
        QScrollArea::wheelEvent(event);
    }
};

} // namespace

struct AgentChatPane::Impl {
    AgentChatPane* owner = nullptr;
    QPointer<SpellCoreClient> client;
    QVector<QMetaObject::Connection> clientConnections;
    QMetaObject::Connection terminalDestroyedConn;

    WorkspaceSnapshot lastSnapshot;
    bool hasSeenThreadsInSnapshot = false;

    QStringList openTabIds;
    QString activeTabId;
    QString lastClosedThreadId;
    QString lastRequestedThreadId;
    bool syncingTabs = false;

    // Unified flat 34px ConversationTabBar
    ConversationTabScrollArea* tabScrollArea = nullptr;
    QWidget* tabRowWidget = nullptr;
    QHBoxLayout* tabRowLayout = nullptr;
    QVector<ConversationTabWidget*> tabWidgets;
    QTimer* busySpinnerTimer = nullptr;
    SpellHairline* headerHairline = nullptr;

    // Vertical splitter & child panes
    SpellSplitter* splitter = nullptr;
    ChatTimelineWidget* timeline = nullptr;
    ActiveSubagentsWidget* activeSubagents = nullptr;
    QPointer<SpellTerminalPanel> dockedTerminal;
    ComposerWidget* composer = nullptr;

    int savedComposerHeight = 110;
    int savedTerminalHeight = 220;
    bool adjustingSplitter = false;

    [[nodiscard]] QString effectiveThreadId() const {
        if (!activeTabId.isEmpty()) {
            return activeTabId;
        }
        return lastSnapshot.selectedThreadId;
    }

    [[nodiscard]] const ThreadSummary* findThread(const QString& threadId) const {
        for (const auto& t : lastSnapshot.threads) {
            if (t.id == threadId) {
                return &t;
            }
        }
        return nullptr;
    }

    [[nodiscard]] QString projectNameForId(const QString& projectId) const {
        if (projectId.isEmpty()) {
            return {};
        }
        for (const auto& p : lastSnapshot.projects) {
            if (p.id == projectId) {
                return p.name;
            }
        }
        return {};
    }

    [[nodiscard]] QString tabRawTitle(const QString& threadId) const {
        if (const ThreadSummary* t = findThread(threadId)) {
            const QString trimmed = t->title.trimmed();
            if (!trimmed.isEmpty()) {
                return trimmed;
            }
        }
        return QStringLiteral("Conversation");
    }

    [[nodiscard]] QString tabProjectName(const QString& threadId) const {
        if (const ThreadSummary* t = findThread(threadId)) {
            return projectNameForId(t->projectId);
        }
        return {};
    }

    [[nodiscard]] bool isTabBusy(const QString& threadId) const {
        return lastSnapshot.isAgentBusy
            && !threadId.isEmpty()
            && threadId == lastSnapshot.selectedThreadId;
    }

    [[nodiscard]] QStringList orderedProjectRoots() const {
        QStringList roots;
        roots.reserve(lastSnapshot.projects.size());
        for (const auto& p : lastSnapshot.projects) {
            if (p.id == lastSnapshot.selectedProjectId && !p.rootPath.trimmed().isEmpty()) {
                roots.append(p.rootPath);
            }
        }
        for (const auto& p : lastSnapshot.projects) {
            if (!p.rootPath.trimmed().isEmpty() && !roots.contains(p.rootPath)) {
                roots.append(p.rootPath);
            }
        }
        return roots;
    }

    [[nodiscard]] int minComposerHeight() const {
        if (!composer) {
            return kMinComposerHeightFloor;
        }
        return std::max(kMinComposerHeightFloor,
                        std::min(composer->minimumSizeHint().height(), 148));
    }

    void clearTabWidgets() {
        for (ConversationTabWidget* tab : tabWidgets) {
            if (!tab) {
                continue;
            }
            tab->hide();
            if (tabRowLayout) {
                tabRowLayout->removeWidget(tab);
            }
            tab->setParent(nullptr);
            tab->deleteLater();
        }
        tabWidgets.clear();
    }

    void syncBusySpinnerTimer() {
        if (!busySpinnerTimer) {
            return;
        }
        const bool anyBusy = std::any_of(
            tabWidgets.cbegin(), tabWidgets.cend(),
            [](const ConversationTabWidget* tab) { return tab && tab->isBusy(); });
        if (anyBusy) {
            if (!busySpinnerTimer->isActive()) {
                busySpinnerTimer->start();
            }
        } else if (busySpinnerTimer->isActive()) {
            busySpinnerTimer->stop();
        }
    }

    void syncTabBar() {
        if (!tabScrollArea || !tabRowWidget || !tabRowLayout) {
            return;
        }
        ScopedBoolGuard guard(syncingTabs, true);

        bool sameSequence = (tabWidgets.size() == openTabIds.size());
        if (sameSequence) {
            for (qsizetype i = 0; i < openTabIds.size(); ++i) {
                if (!tabWidgets[i] || tabWidgets[i]->threadId() != openTabIds[i]) {
                    sameSequence = false;
                    break;
                }
            }
        }

        if (!sameSequence) {
            clearTabWidgets();
            tabWidgets.reserve(openTabIds.size());
            for (qsizetype i = 0; i < openTabIds.size(); ++i) {
                const QString tid = openTabIds.at(i);
                auto* tab = new ConversationTabWidget(
                    tid,
                    tabRawTitle(tid),
                    tabProjectName(tid),
                    [this, tid]() {
                        if (owner && !syncingTabs) {
                            owner->openConversationTab(tid);
                        }
                    },
                    [this, tid]() {
                        if (owner && !syncingTabs) {
                            owner->closeConversationTab(tid);
                        }
                    },
                    tabRowWidget);
                tabRowLayout->insertWidget(static_cast<int>(i), tab);
                tabWidgets.append(tab);
            }
        }

        ConversationTabWidget* activeWidget = nullptr;
        for (qsizetype i = 0; i < tabWidgets.size(); ++i) {
            ConversationTabWidget* tab = tabWidgets[i];
            if (!tab) {
                continue;
            }
            const QString& tid = openTabIds.at(i);
            const bool isSelected = !activeTabId.isEmpty() && (tid == activeTabId);
            tab->setTitleAndProject(tabRawTitle(tid), tabProjectName(tid));
            tab->setActive(isSelected);
            tab->setBusy(isTabBusy(tid));
            if (isSelected) {
                activeWidget = tab;
            }
        }

        syncBusySpinnerTimer();

        if (activeWidget) {
            tabScrollArea->ensureWidgetVisible(activeWidget, 16, 0);
        }
    }

    void positionActiveSubagentsOverlay() {
        if (!activeSubagents || !timeline) {
            return;
        }
        if (activeSubagents->activeCount() <= 0 || activeSubagents->isHidden()) {
            return;
        }
        const QSize hint = activeSubagents->sizeHint();
        const int maxOverlayW = std::max(48, timeline->width() - (kSubagentOverlayRightMargin + 12));
        const int w = std::max(1, std::min(hint.width(), maxOverlayW));
        const int h = std::max(1, hint.height());
        const int x = std::max(12, timeline->width() - w - kSubagentOverlayRightMargin);
        const int y = kSubagentOverlayTopMargin;
        activeSubagents->setGeometry(x, y, w, h);
        activeSubagents->raise();
    }

    void rebalanceSplitterForTerminal(bool terminalVisible) {
        if (!splitter) {
            return;
        }
        ScopedBoolGuard guard(adjustingSplitter, true);

        const int minCompH = minComposerHeight();
        const int totalH = splitter->height();
        const int count = splitter->count();
        if (count == 3) {
            if (terminalVisible) {
                if (totalH < 360) {
                    splitter->setSizes({360, savedTerminalHeight, std::max(minCompH, savedComposerHeight)});
                } else {
                    const int maxCompH = std::max(minCompH, totalH / 2);
                    const int compH = std::clamp(savedComposerHeight, minCompH, maxCompH);
                    const int maxTermH = std::max(kMinTerminalHeight, (totalH - compH) / 2);
                    const int termH = std::clamp(savedTerminalHeight, kMinTerminalHeight, maxTermH);
                    const int timeH = std::max(kMinTimelineHeight, totalH - termH - compH);
                    splitter->setSizes({timeH, termH, compH});
                }
            } else {
                if (totalH < 240) {
                    splitter->setSizes({520, 0, std::max(minCompH, savedComposerHeight)});
                } else {
                    const int maxCompH = std::max(minCompH, (totalH * 3) / 5);
                    const int compH = std::clamp(savedComposerHeight, minCompH, maxCompH);
                    const int timeH = std::max(kMinTimelineHeight, totalH - compH);
                    splitter->setSizes({timeH, 0, compH});
                }
            }
        } else if (count == 2) {
            if (totalH >= 240) {
                const int maxCompH = std::max(minCompH, (totalH * 3) / 5);
                const int compH = std::clamp(savedComposerHeight, minCompH, maxCompH);
                const int timeH = std::max(kMinTimelineHeight, totalH - compH);
                splitter->setSizes({timeH, compH});
            }
        }
    }
};

AgentChatPane::AgentChatPane(QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;
    setObjectName(QStringLiteral("agentChatPane"));
    setAutoFillBackground(true);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // -------------------------------------------------------------------------
    // 1. Unified flat 34px ConversationTabBar + 1px SpellHairline
    // -------------------------------------------------------------------------
    m_impl->tabScrollArea = new ConversationTabScrollArea(this);
    m_impl->tabRowWidget = new QWidget(m_impl->tabScrollArea);
    m_impl->tabRowWidget->setObjectName(QStringLiteral("conversationTabRow"));
    m_impl->tabRowWidget->setAttribute(Qt::WA_StyledBackground, true);
    m_impl->tabRowWidget->setFixedHeight(kTopBarHeight);

    m_impl->tabRowLayout = new QHBoxLayout(m_impl->tabRowWidget);
    m_impl->tabRowLayout->setContentsMargins(0, 0, 0, 0);
    m_impl->tabRowLayout->setSpacing(0);
    m_impl->tabRowLayout->addStretch(1);

    m_impl->tabScrollArea->setWidget(m_impl->tabRowWidget);
    installWindowDragRegion(m_impl->tabRowWidget);
    if (QWidget* vp = m_impl->tabScrollArea->viewport()) {
        installWindowDragRegion(vp);
    }

    m_impl->busySpinnerTimer = new QTimer(this);
    m_impl->busySpinnerTimer->setInterval(kBusySpinnerIntervalMs);
    m_impl->busySpinnerTimer->setTimerType(Qt::CoarseTimer);
    connect(m_impl->busySpinnerTimer, &QTimer::timeout, this, [this]() {
        bool anyBusy = false;
        for (ConversationTabWidget* tab : m_impl->tabWidgets) {
            if (tab && tab->isBusy()) {
                anyBusy = true;
                tab->advanceSpinner(kBusySpinnerStepDeg);
            }
        }
        if (!anyBusy && m_impl->busySpinnerTimer) {
            m_impl->busySpinnerTimer->stop();
        }
    });

    m_impl->headerHairline = new SpellHairline(Qt::Horizontal, this);

    rootLayout->addWidget(m_impl->tabScrollArea);
    rootLayout->addWidget(m_impl->headerHairline);

    // -------------------------------------------------------------------------
    // 2. Vertical SpellSplitter: ChatTimelineWidget | [SpellTerminalPanel] | ComposerWidget
    // -------------------------------------------------------------------------
    m_impl->splitter = new SpellSplitter(Qt::Vertical, this);
    m_impl->splitter->setObjectName(QStringLiteral("agentChatSplitter"));
    m_impl->splitter->setShowGripPill(false);
    m_impl->splitter->setOpaqueResize(true);
    m_impl->splitter->setChildrenCollapsible(false);

    m_impl->timeline = new ChatTimelineWidget(m_impl->splitter);
    m_impl->timeline->setMinimumHeight(kMinTimelineHeight);
    m_impl->timeline->installEventFilter(this);

    // ActiveSubagentsWidget cleanly overlaid in the top-right corner of the message area
    // (completely hidden when 0 subagents are active).
    m_impl->activeSubagents = new ActiveSubagentsWidget(m_impl->timeline);
    m_impl->activeSubagents->setObjectName(QStringLiteral("activeSubagentsPill"));
    m_impl->activeSubagents->setAutoFillBackground(false);
    m_impl->activeSubagents->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    m_impl->activeSubagents->installEventFilter(this);

    m_impl->composer = new ComposerWidget(m_impl->splitter);
    const int initialComposerH = std::clamp(
        m_impl->composer->sizeHint().height(),
        m_impl->minComposerHeight(),
        220);
    m_impl->savedComposerHeight = initialComposerH;

    m_impl->splitter->addWidget(m_impl->timeline);
    m_impl->splitter->addWidget(m_impl->composer);
    m_impl->splitter->setStretchFactor(0, 1);
    m_impl->splitter->setStretchFactor(1, 0);
    m_impl->splitter->setSizes({520, m_impl->savedComposerHeight});

    rootLayout->addWidget(m_impl->splitter, 1);

    // -------------------------------------------------------------------------
    // Internal signal wiring
    // -------------------------------------------------------------------------
    connect(m_impl->splitter, &QSplitter::splitterMoved, this, [this]() {
        if (!m_impl->splitter || m_impl->adjustingSplitter) {
            return;
        }
        const QList<int> sizes = m_impl->splitter->sizes();
        const int compIdx = m_impl->splitter->indexOf(m_impl->composer);
        if (compIdx >= 0 && compIdx < sizes.size() && sizes.at(compIdx) >= kMinComposerHeightFloor) {
            m_impl->savedComposerHeight = sizes.at(compIdx);
        }
        if (m_impl->dockedTerminal && isDockedTerminalVisible()) {
            const int termIdx = m_impl->splitter->indexOf(m_impl->dockedTerminal);
            if (termIdx >= 0 && termIdx < sizes.size() && sizes.at(termIdx) >= 90) {
                m_impl->savedTerminalHeight = sizes.at(termIdx);
            }
        }
    });

    // ChatTimelineWidget signals
    connect(m_impl->timeline, &ChatTimelineWidget::fileCitationClicked,
            this, &AgentChatPane::fileCitationClicked);

    connect(m_impl->timeline, &ChatTimelineWidget::approveDiffRequested,
            this, [this](const QString& diffId) {
                if (m_impl->client && !diffId.isEmpty()) {
                    m_impl->client->approveStagedDiff(diffId);
                }
            });

    connect(m_impl->timeline, &ChatTimelineWidget::rejectDiffRequested,
            this, [this](const QString& diffId) {
                if (m_impl->client && !diffId.isEmpty()) {
                    m_impl->client->rejectStagedDiff(diffId);
                }
            });

    connect(m_impl->timeline, &ChatTimelineWidget::editAndResendRequested,
            this, [this](int turnIndex, const QString& newPrompt) {
                const QString threadId = m_impl->effectiveThreadId();
                if (m_impl->client && !threadId.isEmpty()) {
                    m_impl->client->editAndResendTurn(threadId, turnIndex, newPrompt);
                }
            });

    connect(m_impl->timeline, &ChatTimelineWidget::configureEndpointRequested,
            this, [this]() {
                emit openSettingsRequested(kEngineSettingsPaneIndex);
            });

    // ComposerWidget signals
    connect(m_impl->composer, &ComposerWidget::sendRequested,
            this, [this](const QString& text, const QStringList& attachedFiles) {
                const bool ready = m_impl->lastSnapshot.endpointConfigured
                    && !m_impl->lastSnapshot.modelId.trimmed().isEmpty();
                if (!ready) {
                    emit openSettingsRequested(kEngineSettingsPaneIndex);
                    return;
                }
                if (!m_impl->client) {
                    return;
                }
                m_impl->lastClosedThreadId.clear();
                m_impl->client->sendPrompt(m_impl->effectiveThreadId(), text, attachedFiles);
                if (m_impl->timeline) {
                    m_impl->timeline->scrollToBottom(true);
                }
            });

    connect(m_impl->composer, &ComposerWidget::stopRequested,
            this, [this]() {
                if (m_impl->client) {
                    m_impl->client->cancelActiveTurn();
                }
            });

    connect(m_impl->composer, &ComposerWidget::threadProfileToggled,
            this, [this](const QString& nextProfile) {
                const QString tid = m_impl->effectiveThreadId();
                if (m_impl->client && !tid.isEmpty()) {
                    m_impl->client->setThreadProfile(tid, nextProfile);
                }
                emit threadProfileChanged(tid, nextProfile);
            });

    connect(m_impl->composer, &ComposerWidget::stageWriteToggled,
            this, [this](bool enabled) {
                if (m_impl->client) {
                    m_impl->client->setStageWriteMode(enabled);
                }
            });

    connect(m_impl->composer, &ComposerWidget::terminalToggled,
            this, &AgentChatPane::terminalToggleRequested);

    connect(m_impl->composer, &ComposerWidget::activeTagsChanged,
            this, [this](const QStringList& tagIds) {
                const QString threadId = m_impl->effectiveThreadId();
                if (m_impl->client && !threadId.isEmpty()) {
                    m_impl->client->setThreadTags(threadId, tagIds);
                }
            });

    connect(m_impl->composer, &ComposerWidget::configureEndpointRequested,
            this, [this]() {
                emit openSettingsRequested(kEngineSettingsPaneIndex);
            });

    connect(m_impl->composer, &ComposerWidget::popOutTerminalRequested,
            this, &AgentChatPane::popOutTerminalRequested);

    connect(m_impl->composer, &ComposerWidget::expandedToggled,
            this, [this](bool /*expanded*/) {
                if (!m_impl->splitter || !m_impl->composer) {
                    return;
                }
                const QList<int> sizes = m_impl->splitter->sizes();
                const int compIdx = m_impl->splitter->indexOf(m_impl->composer);
                if (compIdx >= 0 && compIdx < sizes.size() && sizes.at(compIdx) >= kMinComposerHeightFloor) {
                    m_impl->savedComposerHeight = sizes.at(compIdx);
                } else {
                    m_impl->savedComposerHeight = std::max(
                        m_impl->minComposerHeight(),
                        m_impl->composer->sizeHint().height());
                }
            });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &AgentChatPane::applyTheme);

    // Initialize default state and apply Spell IDE monochrome theme.
    m_impl->timeline->setEmptyStateContext(false, false);
    m_impl->composer->setEndpointStatus(false, QString(), false);
    applyTheme();
}

AgentChatPane::~AgentChatPane() {
    if (m_impl && m_impl->busySpinnerTimer) {
        m_impl->busySpinnerTimer->stop();
    }
    for (const auto& conn : m_impl->clientConnections) {
        QObject::disconnect(conn);
    }
    if (m_impl->terminalDestroyedConn) {
        QObject::disconnect(m_impl->terminalDestroyedConn);
    }
}

void AgentChatPane::bindClient(SpellCoreClient* client) {
    if (m_impl->client == client) {
        return;
    }

    for (const auto& conn : m_impl->clientConnections) {
        QObject::disconnect(conn);
    }
    m_impl->clientConnections.clear();

    m_impl->client = client;
    if (!client) {
        return;
    }

    m_impl->clientConnections.append(
        connect(client, &SpellCoreClient::snapshotUpdated,
                this, &AgentChatPane::updateSnapshot));

    m_impl->clientConnections.append(
        connect(client, &SpellCoreClient::busyChanged,
                this, [this](bool busy) {
                    m_impl->lastSnapshot.isAgentBusy = busy;
                    if (m_impl->composer) {
                        m_impl->composer->setBusy(busy);
                    }
                    m_impl->syncTabBar();
                }));

    const WorkspaceSnapshot snap = client->currentSnapshot();
    const bool hasContent = !snap.raw.isEmpty()
        || !snap.threads.isEmpty()
        || !snap.projects.isEmpty()
        || snap.endpointConfigured
        || !snap.modelId.trimmed().isEmpty();
    if (hasContent) {
        updateSnapshot(snap);
    } else if (m_impl->composer) {
        m_impl->composer->setBusy(client->isBusy());
    }
}

void AgentChatPane::updateSnapshot(const WorkspaceSnapshot& snap) {
    m_impl->lastSnapshot = snap;
    if (!snap.threads.isEmpty() || snap.raw.contains(QStringLiteral("threads"))) {
        m_impl->hasSeenThreadsInSnapshot = true;
    }

    // 1. Sync open conversation tabs against existing threads.
    if (m_impl->hasSeenThreadsInSnapshot) {
        QSet<QString> existingIds;
        existingIds.reserve(snap.threads.size());
        for (const auto& t : snap.threads) {
            if (!t.id.isEmpty()) {
                existingIds.insert(t.id);
            }
        }

        QStringList filtered;
        filtered.reserve(m_impl->openTabIds.size());
        for (const QString& id : m_impl->openTabIds) {
            if (existingIds.contains(id)) {
                filtered.append(id);
            }
        }
        m_impl->openTabIds = std::move(filtered);
        if (!m_impl->activeTabId.isEmpty() && !existingIds.contains(m_impl->activeTabId)) {
            m_impl->activeTabId.clear();
        }
    }

    const QString selectedId = snap.selectedThreadId.trimmed();
    m_impl->lastRequestedThreadId = selectedId;

    const bool suppressedByManualClose = !selectedId.isEmpty()
        && selectedId == m_impl->lastClosedThreadId
        && m_impl->openTabIds.isEmpty();

    if (!selectedId.isEmpty() && !suppressedByManualClose) {
        m_impl->lastClosedThreadId.clear();
        if (!m_impl->openTabIds.contains(selectedId)) {
            m_impl->openTabIds.append(selectedId);
        }
        m_impl->activeTabId = selectedId;
    } else if (selectedId.isEmpty() && m_impl->hasSeenThreadsInSnapshot && m_impl->openTabIds.isEmpty()) {
        m_impl->activeTabId.clear();
    }

    m_impl->syncTabBar();

    // 2. Active subagents indicator pill overlay (hidden when 0 active).
    if (m_impl->activeSubagents) {
        m_impl->activeSubagents->setActiveSubagents(snap.activeSubagents);
        m_impl->positionActiveSubagentsOverlay();
    }

    // 3. Timeline events & empty-state context (empty state renders only the witch hat logo).
    const bool endpointReady = snap.endpointConfigured && !snap.modelId.trimmed().isEmpty();
    if (m_impl->timeline) {
        m_impl->timeline->setEmptyStateContext(!snap.projects.isEmpty(), endpointReady);
        if (suppressedByManualClose) {
            m_impl->timeline->clear();
        } else {
            m_impl->timeline->updateEvents(snap.activeEvents, snap.pendingDiffs, m_impl->orderedProjectRoots());
        }
    }

    // 4. Composer state.
    if (m_impl->composer) {
        m_impl->composer->setBusy(snap.isAgentBusy);
        m_impl->composer->setEndpointStatus(snap.endpointConfigured, snap.modelId, snap.telemetry.engineOnline);
        if (const ThreadSummary* activeThread = m_impl->findThread(m_impl->effectiveThreadId())) {
            m_impl->composer->setThreadProfile(activeThread->profile);
        }
        m_impl->composer->setStageWriteMode(snap.stageWriteMode);
        m_impl->composer->setAvailableTags(snap.tags, snap.activeThreadTagIds);
        m_impl->composer->setTerminalVisible(isDockedTerminalVisible());
    }
}

void AgentChatPane::setDockedTerminal(SpellTerminalPanel* panel, bool visible) {
    const bool effectiveVisible = (panel != nullptr) && visible;

    if (m_impl->dockedTerminal && m_impl->dockedTerminal != panel) {
        m_impl->dockedTerminal->removeEventFilter(this);
        if (m_impl->terminalDestroyedConn) {
            QObject::disconnect(m_impl->terminalDestroyedConn);
        }
        if (m_impl->dockedTerminal->parentWidget() == m_impl->splitter) {
            m_impl->dockedTerminal->hide();
        }
        m_impl->dockedTerminal.clear();
    }

    if (!panel) {
        if (m_impl->composer) {
            m_impl->composer->setTerminalVisible(false);
        }
        m_impl->rebalanceSplitterForTerminal(false);
        return;
    }

    const bool wasMounted = (m_impl->splitter->indexOf(panel) >= 0);
    const bool wasVisible = wasMounted && !panel->isHidden();

    if (wasVisible && !effectiveVisible && panel->height() >= 90) {
        m_impl->savedTerminalHeight = panel->height();
    }

    m_impl->dockedTerminal = panel;

    if (!wasMounted) {
        panel->setMinimumHeight(kMinTerminalHeight);
        const int composerIdx = m_impl->splitter->indexOf(m_impl->composer);
        const int insertIdx = (composerIdx >= 0) ? composerIdx : 1;
        m_impl->splitter->insertWidget(insertIdx, panel);
        m_impl->splitter->setCollapsible(insertIdx, false);
        m_impl->splitter->setStretchFactor(0, 4);
        m_impl->splitter->setStretchFactor(insertIdx, 2);
        const int newComposerIdx = m_impl->splitter->indexOf(m_impl->composer);
        if (newComposerIdx >= 0) {
            m_impl->splitter->setStretchFactor(newComposerIdx, 0);
        }
        panel->installEventFilter(this);
        if (m_impl->terminalDestroyedConn) {
            QObject::disconnect(m_impl->terminalDestroyedConn);
        }
        m_impl->terminalDestroyedConn = connect(panel, &QObject::destroyed, this, [this]() {
            m_impl->dockedTerminal.clear();
            if (m_impl->composer) {
                m_impl->composer->setTerminalVisible(false);
            }
        });
        panel->applyTheme();
    }

    panel->setVisible(effectiveVisible);
    if (m_impl->composer) {
        m_impl->composer->setTerminalVisible(effectiveVisible);
    }

    if (wasVisible != effectiveVisible || !wasMounted) {
        m_impl->rebalanceSplitterForTerminal(effectiveVisible);
    }
}

void AgentChatPane::openConversationTab(const QString& threadId) {
    const QString tid = threadId.trimmed();
    if (tid.isEmpty()) {
        return;
    }

    m_impl->lastClosedThreadId.clear();
    if (!m_impl->openTabIds.contains(tid)) {
        m_impl->openTabIds.append(tid);
    }
    m_impl->activeTabId = tid;
    m_impl->syncTabBar();
    if (m_impl->composer) {
        if (const ThreadSummary* activeThread = m_impl->findThread(tid)) {
            m_impl->composer->setThreadProfile(activeThread->profile);
        }
    }

    if (m_impl->client && m_impl->lastRequestedThreadId != tid) {
        m_impl->lastRequestedThreadId = tid;
        m_impl->client->selectThread(tid);
    }
}

void AgentChatPane::closeConversationTab(const QString& threadId) {
    const qsizetype idx = m_impl->openTabIds.indexOf(threadId);
    if (idx < 0) {
        return;
    }

    m_impl->openTabIds.removeAt(idx);

    const bool closingActive = (m_impl->activeTabId == threadId);
    if (closingActive) {
        if (!m_impl->openTabIds.isEmpty()) {
            const qsizetype nextIdx = std::min(idx, m_impl->openTabIds.size() - 1);
            const QString nextId = m_impl->openTabIds.at(nextIdx);
            m_impl->activeTabId = nextId;
            m_impl->lastClosedThreadId.clear();
            m_impl->syncTabBar();
            if (m_impl->client && m_impl->lastRequestedThreadId != nextId) {
                m_impl->lastRequestedThreadId = nextId;
                m_impl->client->selectThread(nextId);
            }
            return;
        }

        m_impl->activeTabId.clear();
        m_impl->lastClosedThreadId = threadId;
        if (m_impl->timeline) {
            m_impl->timeline->clear();
        }
    }

    m_impl->syncTabBar();
}

void AgentChatPane::focusComposer() {
    if (m_impl->composer) {
        m_impl->composer->focusInput();
    }
}

void AgentChatPane::applyTheme() {
    const UiPalette u = ThemeManager::instance().ui();

    QPalette pal = palette();
    pal.setColor(QPalette::Window, u.background);
    pal.setColor(QPalette::WindowText, u.textPrimary);
    setPalette(pal);

    if (m_impl->tabScrollArea) {
        m_impl->tabScrollArea->setStyleSheet(QStringLiteral(
            "QScrollArea#conversationTabBar {"
            "  background-color: %1;"
            "  border: none;"
            "}"
        ).arg(qssColor(u.background)));
        if (QWidget* vp = m_impl->tabScrollArea->viewport()) {
            vp->setStyleSheet(QStringLiteral("background-color: %1; border: none;")
                                  .arg(qssColor(u.background)));
        }
    }

    if (m_impl->tabRowWidget) {
        m_impl->tabRowWidget->setStyleSheet(QStringLiteral(
            "QWidget#conversationTabRow {"
            "  background-color: %1;"
            "  border: none;"
            "}"
        ).arg(qssColor(u.background)));
    }

    for (ConversationTabWidget* tab : m_impl->tabWidgets) {
        if (tab) {
            tab->applyTheme();
        }
    }

    if (m_impl->headerHairline) {
        m_impl->headerHairline->applyTheme();
    }
    if (m_impl->splitter) {
        m_impl->splitter->applyTheme();
    }
    if (m_impl->timeline) {
        m_impl->timeline->applyTheme();
    }
    if (m_impl->activeSubagents) {
        m_impl->activeSubagents->applyTheme();
        m_impl->activeSubagents->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        m_impl->positionActiveSubagentsOverlay();
    }
    if (m_impl->dockedTerminal) {
        m_impl->dockedTerminal->applyTheme();
    }
    if (m_impl->composer) {
        m_impl->composer->applyTheme();
    }
}

QStringList AgentChatPane::openConversationTabIds() const {
    return m_impl->openTabIds;
}

QString AgentChatPane::activeConversationTabId() const {
    return m_impl->activeTabId;
}

bool AgentChatPane::isDockedTerminalVisible() const {
    if (!m_impl->dockedTerminal || !m_impl->splitter) {
        return false;
    }
    return m_impl->splitter->indexOf(m_impl->dockedTerminal) >= 0
        && !m_impl->dockedTerminal->isHidden();
}

bool AgentChatPane::isEndpointBannerVisible() const {
    return false;
}

ChatTimelineWidget* AgentChatPane::timelineWidget() const {
    return m_impl->timeline;
}

ComposerWidget* AgentChatPane::composerWidget() const {
    return m_impl->composer;
}

ActiveSubagentsWidget* AgentChatPane::activeSubagentsWidget() const {
    return m_impl->activeSubagents;
}

SpellSplitter* AgentChatPane::verticalSplitter() const {
    return m_impl->splitter;
}

bool AgentChatPane::eventFilter(QObject* watched, QEvent* event) {
    if (!event) {
        return QWidget::eventFilter(watched, event);
    }

    if (watched == m_impl->timeline) {
        if (event->type() == QEvent::Resize ||
            event->type() == QEvent::Show ||
            event->type() == QEvent::LayoutRequest) {
            m_impl->positionActiveSubagentsOverlay();
        }
    } else if (watched == m_impl->activeSubagents) {
        if (event->type() == QEvent::Show ||
            event->type() == QEvent::LayoutRequest) {
            m_impl->positionActiveSubagentsOverlay();
        }
    } else if (m_impl->dockedTerminal && watched == m_impl->dockedTerminal) {
        if (event->type() == QEvent::Show || event->type() == QEvent::Hide) {
            if (m_impl->composer) {
                m_impl->composer->setTerminalVisible(isDockedTerminalVisible());
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

} // namespace spell
