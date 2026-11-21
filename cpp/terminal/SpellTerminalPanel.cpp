#include "SpellTerminalPanel.h"
#include "shell/WindowChrome.h"

#include "../core/SpellCoreClient.h"
#include "../theme/SpellIcons.h"
#include "../theme/SpellTheme.h"
#include "../widgets/SpellBadges.h"

#include <QCloseEvent>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <optional>
#include <utility>

namespace spell {

namespace {

constexpr int kHeaderHeight = 34;
constexpr int kMaxApprovalScrollHeight = 220;

QString abbreviatePath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QString cleaned = QDir::cleanPath(path);
    const QString home = QDir::cleanPath(QDir::homePath());
    if (!home.isEmpty()) {
        if (cleaned == home) {
            return QStringLiteral("~");
        }
        if (cleaned.startsWith(home + QLatin1Char('/'))) {
            return QLatin1Char('~') + cleaned.mid(home.size());
        }
    }
    return cleaned;
}

QString formatVisibleCommand(const QString& raw) {
    QString out;
    out.reserve(raw.size());
    for (const QChar ch : raw) {
        const ushort code = ch.unicode();
        switch (code) {
        case 0x1B:
            out += QStringLiteral("\u241B");
            break;
        case 0x0D:
            out += QStringLiteral("\u23CE");
            break;
        case 0x0A:
            out += QStringLiteral("\u2424");
            break;
        case 0x09:
            out += QStringLiteral("\u21E5");
            break;
        case 0x7F:
            out += QStringLiteral("\u232B");
            break;
        default:
            if (code < 0x20) {
                out += QLatin1Char('^');
                out += QChar(static_cast<ushort>(code + 0x40));
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

QString normalizeCommandForExecution(const QString& raw) {
    QString cmd = raw;
    if (cmd.endsWith(QLatin1String("\r\n"))) {
        cmd.chop(2);
    } else if (cmd.endsWith(QLatin1Char('\n')) || cmd.endsWith(QLatin1Char('\r'))) {
        cmd.chop(1);
    }
    return cmd;
}

struct HeaderChrome {
    QWidget* bar = nullptr;
    QLabel* iconLabel = nullptr;
    QFrame* statusDot = nullptr;
    QLabel* titleLabel = nullptr;
    QLabel* dirLabel = nullptr;
    SpellBadge* reasonBadge = nullptr;
    SpellBadge* sizeBadge = nullptr;
    SpellIconButton* clearButton = nullptr;
    SpellIconButton* restartButton = nullptr;
    SpellIconButton* popOutButton = nullptr;
    QPushButton* dockBackButton = nullptr;
    SpellIconButton* closeButton = nullptr;
    SpellHairline* bottomDivider = nullptr;
};

struct ApprovalCardRefs {
    QFrame* card = nullptr;
    QLabel* iconLabel = nullptr;
    QLabel* headingLabel = nullptr;
    QLabel* reasonLabel = nullptr;
    QFrame* commandFrame = nullptr;
    QLabel* commandLabel = nullptr;
    SpellBadge* returnBadge = nullptr;
    QPushButton* approveButton = nullptr;
    QPushButton* denyButton = nullptr;
};

} // namespace

struct SpellTerminalPanel::Impl {
    SpellTerminalPanel* q = nullptr;
    QPointer<SpellCoreClient> client;
    QMetaObject::Connection clientApprovalConn;

    // Docked layout & chrome
    QVBoxLayout* rootLayout = nullptr;
    HeaderChrome dockedHeader;
    QWidget* dockedApprovalSlot = nullptr;
    QVBoxLayout* dockedApprovalSlotLayout = nullptr;
    QWidget* dockedExitNoticeSlot = nullptr;
    QVBoxLayout* dockedExitNoticeSlotLayout = nullptr;
    QWidget* dockedSurfaceHost = nullptr;
    QVBoxLayout* dockedSurfaceLayout = nullptr;

    // Placeholder displayed in the docked host while popped out
    QWidget* poppedOutPlaceholder = nullptr;
    QLabel* poppedOutIconLabel = nullptr;
    QLabel* poppedOutMessageLabel = nullptr;
    QPushButton* focusWindowButton = nullptr;
    QPushButton* placeholderDockBackButton = nullptr;

    // Standalone pop-out window (created lazily on first popOut())
    QWidget* popOutWindow = nullptr;
    QVBoxLayout* popOutRootLayout = nullptr;
    HeaderChrome popOutHeader;
    QWidget* popOutApprovalSlot = nullptr;
    QVBoxLayout* popOutApprovalSlotLayout = nullptr;
    QWidget* popOutExitNoticeSlot = nullptr;
    QVBoxLayout* popOutExitNoticeSlotLayout = nullptr;
    QWidget* popOutSurfaceHost = nullptr;
    QVBoxLayout* popOutSurfaceLayout = nullptr;

    // Movable terminal surface (stays alive across popOut / dockBack)
    SpellTerminalSurface* surface = nullptr;

    // Movable collapsible pending approval banner section
    QWidget* approvalSection = nullptr;
    QVBoxLayout* approvalSectionLayout = nullptr;
    QWidget* approvalHeaderBar = nullptr;
    QPushButton* approvalToggleButton = nullptr;
    SpellBadge* approvalCountBadge = nullptr;
    QScrollArea* approvalScrollArea = nullptr;
    QWidget* approvalCardsHost = nullptr;
    QVBoxLayout* approvalCardsLayout = nullptr;
    SpellHairline* approvalBottomDivider = nullptr;
    QVector<ApprovalCardRefs> approvalCards;

    // Movable non-blocking exited shell notice bar (keeps last output visible)
    QWidget* exitNoticeBar = nullptr;
    QLabel* exitNoticeIconLabel = nullptr;
    QLabel* exitNoticeTextLabel = nullptr;
    QPushButton* exitRestartButton = nullptr;
    SpellIconButton* exitDismissButton = nullptr;
    SpellHairline* exitBottomDivider = nullptr;

    // State
    QVector<TerminalApprovalRequest> pendingApprovals;
    QString agentReason;
    std::optional<int> lastExitCode;
    bool exitNoticeVisible = false;
    bool approvalsCollapsed = false;
    bool updatingApprovalHeight = false;
    bool poppedOut = false;
    bool destroying = false;

    explicit Impl(SpellTerminalPanel* owner)
        : q(owner) {}

    HeaderChrome buildHeaderBar(QWidget* parent, bool isPopOutBar) {
        HeaderChrome h;
        h.bar = new QWidget(parent);
        h.bar->setObjectName(isPopOutBar ? QStringLiteral("terminalPopOutHeader")
                                         : QStringLiteral("terminalDockedHeader"));
        h.bar->setAttribute(Qt::WA_StyledBackground, true);
        h.bar->setAutoFillBackground(true);
        h.bar->setFixedHeight(kHeaderHeight);

        auto* layout = new QHBoxLayout(h.bar);
        layout->setContentsMargins(10, 0, 8, 0);
        layout->setSpacing(6);

        h.iconLabel = new QLabel(h.bar);
        h.iconLabel->setFixedSize(16, 16);
        h.iconLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(h.iconLabel);

        h.statusDot = new QFrame(h.bar);
        h.statusDot->setFixedSize(7, 7);
        layout->addWidget(h.statusDot);

        h.titleLabel = new QLabel(QStringLiteral("Terminal"), h.bar);
        h.titleLabel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
        h.titleLabel->setMaximumWidth(220);
        layout->addWidget(h.titleLabel);

        h.dirLabel = new QLabel(h.bar);
        h.dirLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        h.dirLabel->setMinimumWidth(0);
        h.dirLabel->installEventFilter(q);
        layout->addWidget(h.dirLabel, 1);

        h.reasonBadge = new SpellBadge(QString(), BadgeTone::Info, h.bar);
        h.reasonBadge->setFilled(true);
        h.reasonBadge->setMaximumWidth(200);
        h.reasonBadge->hide();
        layout->addWidget(h.reasonBadge);

        h.sizeBadge = new SpellBadge(QStringLiteral("80\u00D724"), BadgeTone::Neutral, h.bar);
        h.sizeBadge->setFilled(false);
        h.sizeBadge->setToolTip(QStringLiteral("Terminal grid size (columns \u00D7 rows)"));
        layout->addWidget(h.sizeBadge);

        h.clearButton = new SpellIconButton(
            QStringLiteral("trash"),
            QStringLiteral("Clear terminal buffer and scrollback"),
            BadgeTone::Neutral,
            h.bar);
        h.clearButton->setObjectName(QStringLiteral("terminalClearButton"));
        h.clearButton->setAccessibleName(QStringLiteral("Clear"));
        QObject::connect(h.clearButton, &QToolButton::clicked, q, &SpellTerminalPanel::clearTerminal);
        layout->addWidget(h.clearButton);

        h.restartButton = new SpellIconButton(
            QStringLiteral("refresh"),
            QStringLiteral("Restart shell session"),
            BadgeTone::Neutral,
            h.bar);
        h.restartButton->setObjectName(QStringLiteral("terminalRestartButton"));
        h.restartButton->setAccessibleName(QStringLiteral("Restart"));
        QObject::connect(h.restartButton, &QToolButton::clicked, q, [this]() {
            q->restartShell();
        });
        layout->addWidget(h.restartButton);

        if (isPopOutBar) {
            h.dockBackButton = new QPushButton(QStringLiteral("Dock Back"), h.bar);
            h.dockBackButton->setObjectName(QStringLiteral("terminalDockBackButton"));
            h.dockBackButton->setCursor(Qt::PointingHandCursor);
            h.dockBackButton->setToolTip(QStringLiteral("Dock terminal back into the workspace"));
            h.dockBackButton->setFixedHeight(22);
            QObject::connect(h.dockBackButton, &QPushButton::clicked, q, &SpellTerminalPanel::dockBack);
            layout->addWidget(h.dockBackButton);
        } else {
            h.popOutButton = new SpellIconButton(
                QStringLiteral("popout"),
                QStringLiteral("Pop out into a standalone window"),
                BadgeTone::Accent,
                h.bar);
            h.popOutButton->setObjectName(QStringLiteral("terminalPopOutButton"));
            h.popOutButton->setAccessibleName(QStringLiteral("Pop Out"));
            QObject::connect(h.popOutButton, &QToolButton::clicked, q, &SpellTerminalPanel::togglePopOut);
            layout->addWidget(h.popOutButton);

            h.closeButton = new SpellIconButton(
                QStringLiteral("close"),
                QStringLiteral("Hide terminal panel (shell keeps running)"),
                BadgeTone::Neutral,
                h.bar);
            h.closeButton->setObjectName(QStringLiteral("terminalCloseButton"));
            h.closeButton->setAccessibleName(QStringLiteral("Close"));
            QObject::connect(h.closeButton, &QToolButton::clicked, q, &SpellTerminalPanel::closeRequested);
            layout->addWidget(h.closeButton);
        }

        h.bottomDivider = new SpellHairline(Qt::Horizontal, parent);
        return h;
    }

    void buildApprovalSection(QWidget* parent) {
        approvalSection = new QWidget(parent);
        approvalSection->setObjectName(QStringLiteral("terminalApprovalSection"));
        approvalSection->setAttribute(Qt::WA_StyledBackground, true);
        approvalSection->setAutoFillBackground(true);
        approvalSection->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

        approvalSectionLayout = new QVBoxLayout(approvalSection);
        approvalSectionLayout->setContentsMargins(0, 0, 0, 0);
        approvalSectionLayout->setSpacing(0);

        approvalHeaderBar = new QWidget(approvalSection);
        approvalHeaderBar->setObjectName(QStringLiteral("terminalApprovalHeaderBar"));
        approvalHeaderBar->setFixedHeight(28);
        auto* headerLayout = new QHBoxLayout(approvalHeaderBar);
        headerLayout->setContentsMargins(10, 2, 10, 2);
        headerLayout->setSpacing(6);

        approvalToggleButton = new QPushButton(approvalHeaderBar);
        approvalToggleButton->setObjectName(QStringLiteral("terminalApprovalToggleButton"));
        approvalToggleButton->setFlat(true);
        approvalToggleButton->setCursor(Qt::PointingHandCursor);
        approvalToggleButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        QObject::connect(approvalToggleButton, &QPushButton::clicked, q, [this]() {
            approvalsCollapsed = !approvalsCollapsed;
            updateApprovalCollapseState();
        });
        headerLayout->addWidget(approvalToggleButton, 1);

        approvalCountBadge = new SpellBadge(QStringLiteral("0 pending"), BadgeTone::Warning, true, approvalHeaderBar);
        approvalCountBadge->setFilled(true);
        headerLayout->addWidget(approvalCountBadge);

        approvalSectionLayout->addWidget(approvalHeaderBar);

        approvalScrollArea = new QScrollArea(approvalSection);
        approvalScrollArea->setObjectName(QStringLiteral("terminalApprovalScrollArea"));
        approvalScrollArea->setFrameShape(QFrame::NoFrame);
        approvalScrollArea->setWidgetResizable(true);
        approvalScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        approvalScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        approvalScrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        approvalCardsHost = new QWidget(approvalScrollArea);
        approvalCardsHost->setObjectName(QStringLiteral("terminalApprovalCardsHost"));
        approvalCardsHost->installEventFilter(q);
        approvalCardsLayout = new QVBoxLayout(approvalCardsHost);
        approvalCardsLayout->setContentsMargins(10, 4, 10, 8);
        approvalCardsLayout->setSpacing(6);

        approvalScrollArea->setWidget(approvalCardsHost);
        approvalSectionLayout->addWidget(approvalScrollArea);

        approvalBottomDivider = new SpellHairline(Qt::Horizontal, approvalSection);
        approvalSectionLayout->addWidget(approvalBottomDivider);

        approvalSection->hide();
    }

    void buildExitNoticeBar(QWidget* parent) {
        exitNoticeBar = new QWidget(parent);
        exitNoticeBar->setObjectName(QStringLiteral("terminalExitNoticeBar"));
        exitNoticeBar->setAttribute(Qt::WA_StyledBackground, true);
        exitNoticeBar->setAutoFillBackground(true);
        auto* outer = new QVBoxLayout(exitNoticeBar);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        auto* rowWidget = new QWidget(exitNoticeBar);
        rowWidget->setFixedHeight(30);
        auto* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(10, 2, 8, 2);
        row->setSpacing(8);

        exitNoticeIconLabel = new QLabel(rowWidget);
        exitNoticeIconLabel->setFixedSize(14, 14);
        exitNoticeIconLabel->setAlignment(Qt::AlignCenter);
        row->addWidget(exitNoticeIconLabel);

        exitNoticeTextLabel = new QLabel(rowWidget);
        exitNoticeTextLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        row->addWidget(exitNoticeTextLabel, 1);

        exitRestartButton = new QPushButton(QStringLiteral("Restart Shell"), rowWidget);
        exitRestartButton->setObjectName(QStringLiteral("terminalExitRestartButton"));
        exitRestartButton->setCursor(Qt::PointingHandCursor);
        exitRestartButton->setFixedHeight(22);
        QObject::connect(exitRestartButton, &QPushButton::clicked, q, [this]() {
            q->restartShell();
        });
        row->addWidget(exitRestartButton);

        exitDismissButton = new SpellIconButton(
            QStringLiteral("close"),
            QStringLiteral("Dismiss notice"),
            BadgeTone::Neutral,
            rowWidget);
        QObject::connect(exitDismissButton, &QToolButton::clicked, q, [this]() {
            exitNoticeVisible = false;
            if (exitNoticeBar) {
                exitNoticeBar->hide();
            }
        });
        row->addWidget(exitDismissButton);

        outer->addWidget(rowWidget);

        exitBottomDivider = new SpellHairline(Qt::Horizontal, exitNoticeBar);
        outer->addWidget(exitBottomDivider);

        exitNoticeBar->hide();
    }

    void buildPoppedOutPlaceholder(QWidget* parent) {
        poppedOutPlaceholder = new QWidget(parent);
        poppedOutPlaceholder->setObjectName(QStringLiteral("terminalPoppedOutPlaceholder"));
        poppedOutPlaceholder->setAttribute(Qt::WA_StyledBackground, true);
        poppedOutPlaceholder->setAutoFillBackground(true);
        auto* outer = new QVBoxLayout(poppedOutPlaceholder);
        outer->setContentsMargins(16, 16, 16, 16);
        outer->setSpacing(10);
        outer->addStretch(1);

        auto* centerRow = new QHBoxLayout();
        centerRow->setSpacing(8);
        centerRow->addStretch(1);

        poppedOutIconLabel = new QLabel(poppedOutPlaceholder);
        poppedOutIconLabel->setFixedSize(16, 16);
        poppedOutIconLabel->setAlignment(Qt::AlignCenter);
        centerRow->addWidget(poppedOutIconLabel);

        poppedOutMessageLabel = new QLabel(
            QStringLiteral("Terminal is open in a standalone window"),
            poppedOutPlaceholder);
        centerRow->addWidget(poppedOutMessageLabel);
        centerRow->addStretch(1);
        outer->addLayout(centerRow);

        auto* actionsRow = new QHBoxLayout();
        actionsRow->setSpacing(8);
        actionsRow->addStretch(1);

        focusWindowButton = new QPushButton(QStringLiteral("Focus Window"), poppedOutPlaceholder);
        focusWindowButton->setCursor(Qt::PointingHandCursor);
        QObject::connect(focusWindowButton, &QPushButton::clicked, q, [this]() {
            if (popOutWindow) {
                popOutWindow->show();
                popOutWindow->raise();
                popOutWindow->activateWindow();
            }
            if (surface) {
                surface->setFocus(Qt::OtherFocusReason);
            }
        });
        actionsRow->addWidget(focusWindowButton);

        placeholderDockBackButton = new QPushButton(QStringLiteral("Dock Back"), poppedOutPlaceholder);
        placeholderDockBackButton->setCursor(Qt::PointingHandCursor);
        placeholderDockBackButton->setProperty("primary", true);
        QObject::connect(placeholderDockBackButton, &QPushButton::clicked, q, &SpellTerminalPanel::dockBack);
        actionsRow->addWidget(placeholderDockBackButton);

        actionsRow->addStretch(1);
        outer->addLayout(actionsRow);
        outer->addStretch(1);

        poppedOutPlaceholder->hide();
    }

    void ensurePopOutWindowCreated() {
        if (popOutWindow) {
            return;
        }

        popOutWindow = new QWidget(nullptr, Qt::Window);
        popOutWindow->setObjectName(QStringLiteral("spellTerminalPopOutWindow"));
        popOutWindow->setAttribute(Qt::WA_StyledBackground, true);
        popOutWindow->setAutoFillBackground(true);
        popOutWindow->setWindowIcon(icons::appWindowIcon());
        popOutWindow->setMinimumSize(480, 260);
        popOutWindow->resize(760, 480);
        popOutWindow->installEventFilter(q);
        installTerminalWindowControls(popOutWindow);

        popOutRootLayout = new QVBoxLayout(popOutWindow);
        popOutRootLayout->setContentsMargins(0, 0, 0, 0);
        popOutRootLayout->setSpacing(0);

        popOutHeader = buildHeaderBar(popOutWindow, true);
        popOutRootLayout->addWidget(popOutHeader.bar);
        popOutRootLayout->addWidget(popOutHeader.bottomDivider);

        popOutApprovalSlot = new QWidget(popOutWindow);
        popOutApprovalSlot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        popOutApprovalSlotLayout = new QVBoxLayout(popOutApprovalSlot);
        popOutApprovalSlotLayout->setContentsMargins(0, 0, 0, 0);
        popOutApprovalSlotLayout->setSpacing(0);
        popOutRootLayout->addWidget(popOutApprovalSlot);

        popOutExitNoticeSlot = new QWidget(popOutWindow);
        popOutExitNoticeSlot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        popOutExitNoticeSlotLayout = new QVBoxLayout(popOutExitNoticeSlot);
        popOutExitNoticeSlotLayout->setContentsMargins(0, 0, 0, 0);
        popOutExitNoticeSlotLayout->setSpacing(0);
        popOutRootLayout->addWidget(popOutExitNoticeSlot);

        popOutSurfaceHost = new QWidget(popOutWindow);
        popOutSurfaceHost->setAttribute(Qt::WA_StyledBackground, true);
        popOutSurfaceHost->setAutoFillBackground(true);
        popOutSurfaceHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        popOutSurfaceLayout = new QVBoxLayout(popOutSurfaceHost);
        popOutSurfaceLayout->setContentsMargins(0, 0, 0, 0);
        popOutSurfaceLayout->setSpacing(0);
        popOutRootLayout->addWidget(popOutSurfaceHost, 1);

        applyHeaderTheme(popOutHeader);
        syncHeaderChrome();
        applyPopOutWindowTheme();
    }

    QString buildWindowTitle() const {
        QStringList parts;
        parts << QStringLiteral("Message Terminal");
        const QString oscTitle = surface ? surface->terminalTitle().trimmed() : QString();
        const QString cwd = abbreviatePath(surface ? surface->workingDirectory() : QString());
        if (!oscTitle.isEmpty()) {
            parts << oscTitle;
        } else if (!cwd.isEmpty()) {
            const QString leaf = QFileInfo(cwd).fileName();
            parts << (leaf.isEmpty() ? cwd : leaf);
        }
        const int cols = surface ? surface->gridCols() : 80;
        const int rows = surface ? surface->gridRows() : 24;
        parts << QStringLiteral("%1\u00D7%2").arg(cols).arg(rows);
        return parts.join(QStringLiteral(" \u2014 "));
    }

    void updateElidedDirectoryLabel(QLabel* label) const {
        if (!label) {
            return;
        }
        const QString fullCwd = surface ? surface->workingDirectory() : QString();
        const QString displayCwd = abbreviatePath(fullCwd);
        if (displayCwd.isEmpty()) {
            label->clear();
            label->setToolTip(QString());
            return;
        }

        const QString prefixed = QStringLiteral("\u2014 ") + displayCwd;
        const int availableWidth = label->width();
        if (availableWidth > 12) {
            const QFontMetrics fm(label->font());
            label->setText(fm.elidedText(prefixed, Qt::ElideLeft, availableWidth));
        } else {
            label->setText(prefixed);
        }
        label->setToolTip(fullCwd);
    }

    void syncOneHeader(HeaderChrome& h) const {
        if (!h.bar) {
            return;
        }

        const QString oscTitle = surface ? surface->terminalTitle().trimmed() : QString();
        const QString displayTitle = oscTitle.isEmpty() ? QStringLiteral("Terminal") : oscTitle;
        const QFontMetrics titleFm(h.titleLabel->font());
        h.titleLabel->setText(titleFm.elidedText(displayTitle, Qt::ElideRight, 200));
        h.titleLabel->setToolTip(displayTitle);

        updateElidedDirectoryLabel(h.dirLabel);

        const int cols = surface ? surface->gridCols() : 80;
        const int rows = surface ? surface->gridRows() : 24;
        h.sizeBadge->setText(QStringLiteral("%1\u00D7%2").arg(cols).arg(rows));

        if (agentReason.isEmpty()) {
            h.reasonBadge->hide();
        } else {
            const QFontMetrics badgeFm(h.reasonBadge->font());
            h.reasonBadge->setText(badgeFm.elidedText(agentReason, Qt::ElideRight, 180));
            h.reasonBadge->setToolTip(agentReason);
            h.reasonBadge->show();
        }

        if (h.popOutButton) {
            if (poppedOut) {
                h.popOutButton->setIconName(QStringLiteral("chevron-down"));
                h.popOutButton->setToolTip(QStringLiteral("Dock terminal back into the workspace"));
                h.popOutButton->setAccessibleName(QStringLiteral("Dock Back"));
            } else {
                h.popOutButton->setIconName(QStringLiteral("popout"));
                h.popOutButton->setToolTip(QStringLiteral("Pop out into a standalone window"));
                h.popOutButton->setAccessibleName(QStringLiteral("Pop Out"));
            }
        }

        updateStatusDotColor(h);
    }

    void updateStatusDotColor(const HeaderChrome& h) const {
        if (!h.statusDot) {
            return;
        }
        const UiPalette u = ThemeManager::instance().ui();
        const QColor mutedSolid = compositeOver(u.textMuted, u.background);
        const bool running = surface && surface->isShellRunning();
        QColor dotColor = mutedSolid;
        QString statusTip = QStringLiteral("Shell stopped");

        if (running) {
            dotColor = u.accent;
            statusTip = QStringLiteral("Shell running");
        } else if (lastExitCode.has_value() && *lastExitCode != 0) {
            dotColor = u.danger;
            statusTip = QStringLiteral("Shell exited with code %1").arg(*lastExitCode);
        } else if (lastExitCode.has_value()) {
            dotColor = mutedSolid;
            statusTip = QStringLiteral("Shell exited (0)");
        }

        h.statusDot->setStyleSheet(
            QStringLiteral("background-color: %1; border-radius: 3px; border: none;")
                .arg(qssColor(dotColor)));
        h.statusDot->setToolTip(statusTip);
    }

    void syncHeaderChrome() {
        syncOneHeader(dockedHeader);
        if (popOutWindow) {
            syncOneHeader(popOutHeader);
            popOutWindow->setWindowTitle(buildWindowTitle());
        }
    }

    void updateApprovalScrollHeight() {
        if (updatingApprovalHeight || !approvalScrollArea || !approvalCardsHost || !approvalCardsLayout) {
            return;
        }
        updatingApprovalHeight = true;
        approvalCardsLayout->activate();
        const int viewportW = approvalScrollArea->viewport() ? approvalScrollArea->viewport()->width() : 0;
        int contentHeight = -1;
        if (viewportW > 40 && approvalCardsHost->hasHeightForWidth()) {
            contentHeight = approvalCardsHost->heightForWidth(viewportW);
        }
        if (contentHeight <= 0) {
            contentHeight = approvalCardsHost->sizeHint().height();
        }
        const int clamped = std::clamp(contentHeight, 0, kMaxApprovalScrollHeight);
        if (approvalScrollArea->minimumHeight() != clamped || approvalScrollArea->maximumHeight() != clamped) {
            approvalScrollArea->setFixedHeight(clamped);
        }
        updatingApprovalHeight = false;
    }

    void updateApprovalCollapseState() {
        if (!approvalSection || !approvalToggleButton || !approvalScrollArea) {
            return;
        }
        const bool hasAny = !pendingApprovals.isEmpty();
        approvalSection->setVisible(hasAny);
        if (!hasAny) {
            return;
        }

        const UiPalette u = ThemeManager::instance().ui();
        const QColor textSec = compositeOver(u.textSecondary, u.background);
        const qsizetype count = pendingApprovals.size();
        const QString heading = (count == 1)
                                    ? QStringLiteral("Pending Terminal Approval")
                                    : QStringLiteral("Pending Terminal Approvals");
        approvalToggleButton->setText(heading);
        approvalToggleButton->setIcon(icons::vectorIcon(
            approvalsCollapsed ? QStringLiteral("chevron-right") : QStringLiteral("chevron-down"),
            textSec,
            13));
        approvalToggleButton->setIconSize(QSize(13, 13));
        approvalCountBadge->setText(QStringLiteral("%1 pending").arg(count));

        approvalScrollArea->setVisible(!approvalsCollapsed);
        if (!approvalsCollapsed) {
            updateApprovalScrollHeight();
        }
    }

    void rebuildApprovalCards() {
        if (!approvalCardsLayout || !approvalCardsHost) {
            return;
        }

        approvalCards.clear();
        while (QLayoutItem* item = approvalCardsLayout->takeAt(0)) {
            if (QWidget* w = item->widget()) {
                w->hide();
                w->setParent(nullptr);
                w->deleteLater();
            }
            delete item;
        }

        for (const TerminalApprovalRequest& req : pendingApprovals) {
            ApprovalCardRefs refs;
            refs.card = new QFrame(approvalCardsHost);
            refs.card->setObjectName(QStringLiteral("terminalApprovalCard"));
            refs.card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

            auto* cardLayout = new QHBoxLayout(refs.card);
            cardLayout->setContentsMargins(10, 8, 10, 8);
            cardLayout->setSpacing(10);

            // Left: info + command display
            auto* leftCol = new QVBoxLayout();
            leftCol->setContentsMargins(0, 0, 0, 0);
            leftCol->setSpacing(5);

            auto* headingRow = new QHBoxLayout();
            headingRow->setContentsMargins(0, 0, 0, 0);
            headingRow->setSpacing(6);

            refs.iconLabel = new QLabel(refs.card);
            refs.iconLabel->setFixedSize(14, 14);
            refs.iconLabel->setAlignment(Qt::AlignCenter);
            headingRow->addWidget(refs.iconLabel);

            refs.headingLabel = new QLabel(
                QStringLiteral("BeatrixLeslie IDE wants to run a command in your terminal"),
                refs.card);
            refs.headingLabel->setWordWrap(true);
            headingRow->addWidget(refs.headingLabel, 1);
            leftCol->addLayout(headingRow);

            if (!req.reason.trimmed().isEmpty()) {
                refs.reasonLabel = new QLabel(req.reason.trimmed(), refs.card);
                refs.reasonLabel->setWordWrap(true);
                leftCol->addWidget(refs.reasonLabel);
            }

            refs.commandFrame = new QFrame(refs.card);
            refs.commandFrame->setObjectName(QStringLiteral("terminalApprovalCommandBox"));
            auto* cmdLayout = new QHBoxLayout(refs.commandFrame);
            cmdLayout->setContentsMargins(8, 5, 8, 5);
            cmdLayout->setSpacing(6);

            const QString visibleCmd = formatVisibleCommand(req.command);
            refs.commandLabel = new QLabel(
                visibleCmd.isEmpty() ? QStringLiteral("(empty)") : visibleCmd,
                refs.commandFrame);
            refs.commandLabel->setWordWrap(true);
            refs.commandLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            refs.commandLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            cmdLayout->addWidget(refs.commandLabel, 1);

            refs.returnBadge = new SpellBadge(QStringLiteral("+ RETURN"), BadgeTone::Neutral, refs.commandFrame);
            refs.returnBadge->setFilled(false);
            cmdLayout->addWidget(refs.returnBadge, 0, Qt::AlignTop);

            leftCol->addWidget(refs.commandFrame);
            cardLayout->addLayout(leftCol, 1);

            // Right: Approve & Run / Deny buttons
            auto* btnCol = new QVBoxLayout();
            btnCol->setContentsMargins(0, 0, 0, 0);
            btnCol->setSpacing(5);

            const QString reqId = req.id;

            refs.approveButton = new QPushButton(QStringLiteral("Approve && Run"), refs.card);
            refs.approveButton->setObjectName(QStringLiteral("terminalApproveButton"));
            refs.approveButton->setAccessibleName(QStringLiteral("Approve & Run"));
            refs.approveButton->setCursor(Qt::PointingHandCursor);
            refs.approveButton->setToolTip(QStringLiteral("Execute this command in the live terminal"));
            QObject::connect(refs.approveButton, &QPushButton::clicked, q, [this, reqId]() {
                q->approveRequest(reqId);
            });
            btnCol->addWidget(refs.approveButton);

            refs.denyButton = new QPushButton(QStringLiteral("Deny"), refs.card);
            refs.denyButton->setObjectName(QStringLiteral("terminalDenyButton"));
            refs.denyButton->setCursor(Qt::PointingHandCursor);
            refs.denyButton->setToolTip(QStringLiteral("Reject this command without typing into the terminal"));
            QObject::connect(refs.denyButton, &QPushButton::clicked, q, [this, reqId]() {
                q->denyRequest(reqId);
            });
            btnCol->addWidget(refs.denyButton);
            btnCol->addStretch(1);

            cardLayout->addLayout(btnCol);

            approvalCardsLayout->addWidget(refs.card);
            approvalCards.append(refs);
        }

        applyApprovalTheme();
        updateApprovalCollapseState();
    }

    void applyHeaderTheme(HeaderChrome& h) const {
        if (!h.bar) {
            return;
        }
        const UiPalette u = ThemeManager::instance().ui();
        const QColor textSec = compositeOver(u.textSecondary, u.background);
        const QColor elevated = compositeOver(u.elevatedSurface, u.background);
        const QColor border = compositeOver(u.subtleBorder, u.background);

        h.bar->setStyleSheet(
            QStringLiteral("QWidget { background-color: %1; }").arg(qssColor(u.surface)));

        h.iconLabel->setPixmap(
            icons::vectorIcon(QStringLiteral("terminal"), u.textPrimary, 14).pixmap(14, 14));

        QFont titleFont = h.titleLabel->font();
        titleFont.setPointSizeF(11.5);
        titleFont.setWeight(QFont::DemiBold);
        h.titleLabel->setFont(titleFont);
        h.titleLabel->setStyleSheet(
            QStringLiteral("color: %1; background: transparent;").arg(qssColor(u.textPrimary)));

        QFont dirFont = ThemeManager::instance().terminalFont();
        dirFont.setPointSizeF(10.5);
        h.dirLabel->setFont(dirFont);
        h.dirLabel->setStyleSheet(
            QStringLiteral("color: %1; background: transparent;").arg(qssColor(textSec)));

        if (h.reasonBadge) {
            h.reasonBadge->applyTheme();
        }
        if (h.sizeBadge) {
            QFont mono = ThemeManager::instance().terminalFont();
            mono.setPointSizeF(10.0);
            h.sizeBadge->setFont(mono);
            h.sizeBadge->applyTheme();
        }
        if (h.clearButton) {
            h.clearButton->applyTheme();
        }
        if (h.restartButton) {
            h.restartButton->applyTheme();
        }
        if (h.popOutButton) {
            h.popOutButton->applyTheme();
        }
        if (h.closeButton) {
            h.closeButton->applyTheme();
        }
        if (h.dockBackButton) {
            h.dockBackButton->setIcon(icons::vectorIcon(QStringLiteral("chevron-down"), u.textPrimary, 12));
            h.dockBackButton->setIconSize(QSize(12, 12));
            h.dockBackButton->setStyleSheet(
                QStringLiteral(
                    "QPushButton {"
                    "  background-color: %1;"
                    "  color: %2;"
                    "  border: 1px solid %3;"
                    "  border-radius: 5px;"
                    "  padding: 2px 8px;"
                    "  font-size: 11px;"
                    "  font-weight: 600;"
                    "}"
                    "QPushButton:hover {"
                    "  background-color: %4;"
                    "  border-color: %5;"
                    "}")
                    .arg(qssColor(u.surface),
                         qssColor(u.textPrimary),
                         qssColor(border),
                         qssColor(elevated),
                         qssColor(u.selectedBorder)));
        }
        if (h.bottomDivider) {
            h.bottomDivider->applyTheme();
        }

        updateStatusDotColor(h);
    }

    void applyApprovalTheme() {
        if (!approvalSection) {
            return;
        }
        const UiPalette u = ThemeManager::instance().ui();
        const QColor elevated = compositeOver(u.elevatedSurface, u.background);
        const QColor border = compositeOver(u.subtleBorder, u.background);
        const QColor divider = compositeOver(u.divider, u.background);
        const QColor textSec = compositeOver(u.textSecondary, u.background);
        const QColor textMuted = compositeOver(u.textMuted, u.background);

        const QColor approveColor = u.diffAdded;
        const QColor approveFill = compositeOver(withAlpha(u.diffAdded, 0.14), u.background);
        const QColor approveHover = compositeOver(withAlpha(u.diffAdded, 0.24), u.background);
        const QColor approveBorder = compositeOver(withAlpha(u.diffAdded, 0.65), u.background);

        const QColor denyColor = u.danger;
        const QColor denyFill = compositeOver(withAlpha(u.danger, 0.14), u.background);
        const QColor denyHover = compositeOver(withAlpha(u.danger, 0.24), u.background);
        const QColor denyBorder = compositeOver(withAlpha(u.danger, 0.65), u.background);

        approvalSection->setStyleSheet(
            QStringLiteral("QWidget#terminalApprovalSection { background-color: %1; }")
                .arg(qssColor(elevated)));
        approvalHeaderBar->setStyleSheet(
            QStringLiteral("QWidget#terminalApprovalHeaderBar { background-color: transparent; }"));
        approvalScrollArea->setStyleSheet(
            QStringLiteral("QScrollArea#terminalApprovalScrollArea { background: transparent; border: none; }"));
        approvalCardsHost->setStyleSheet(
            QStringLiteral("QWidget#terminalApprovalCardsHost { background: transparent; }"));

        approvalToggleButton->setStyleSheet(
            QStringLiteral(
                "QPushButton#terminalApprovalToggleButton {"
                "  background: transparent;"
                "  border: none;"
                "  color: %1;"
                "  text-align: left;"
                "  padding: 2px 4px;"
                "  font-size: 11px;"
                "  font-weight: 600;"
                "}"
                "QPushButton#terminalApprovalToggleButton:hover {"
                "  color: %2;"
                "}")
                .arg(qssColor(u.textPrimary), qssColor(u.accent)));

        approvalCountBadge->applyTheme();
        approvalBottomDivider->applyTheme();

        QFont cmdFont = ThemeManager::instance().terminalFont();
        cmdFont.setPointSizeF(std::clamp(ThemeManager::instance().terminalFontSize() - 1.0, 10.0, 14.0));
        cmdFont.setWeight(QFont::DemiBold);

        for (ApprovalCardRefs& refs : approvalCards) {
            if (!refs.card) {
                continue;
            }
            refs.card->setStyleSheet(
                QStringLiteral(
                    "QFrame#terminalApprovalCard {"
                    "  background-color: %1;"
                    "  border: 1px solid %2;"
                    "  border-radius: 6px;"
                    "}")
                    .arg(qssColor(u.surface), qssColor(border)));

            refs.iconLabel->setPixmap(
                icons::vectorIcon(QStringLiteral("warning"), u.textPrimary, 13).pixmap(13, 13));

            refs.headingLabel->setStyleSheet(
                QStringLiteral("color: %1; background: transparent; font-size: 11px; font-weight: 600;")
                    .arg(qssColor(u.textPrimary)));

            if (refs.reasonLabel) {
                refs.reasonLabel->setStyleSheet(
                    QStringLiteral("color: %1; background: transparent; font-size: 10.5px;")
                        .arg(qssColor(textMuted)));
            }

            refs.commandFrame->setStyleSheet(
                QStringLiteral(
                    "QFrame#terminalApprovalCommandBox {"
                    "  background-color: %1;"
                    "  border: 1px solid %2;"
                    "  border-radius: 4px;"
                    "}")
                    .arg(qssColor(elevated), qssColor(divider)));

            refs.commandLabel->setFont(cmdFont);
            refs.commandLabel->setStyleSheet(
                QStringLiteral("color: %1; background: transparent;").arg(qssColor(u.textPrimary)));

            if (refs.returnBadge) {
                refs.returnBadge->applyTheme();
            }

            refs.approveButton->setIcon(icons::vectorIcon(QStringLiteral("check"), approveColor, 13));
            refs.approveButton->setIconSize(QSize(13, 13));
            refs.approveButton->setStyleSheet(
                QStringLiteral(
                    "QPushButton#terminalApproveButton {"
                    "  background-color: %1;"
                    "  color: %2;"
                    "  border: 1px solid %3;"
                    "  border-radius: 5px;"
                    "  padding: 4px 10px;"
                    "  font-size: 11px;"
                    "  font-weight: 600;"
                    "}"
                    "QPushButton#terminalApproveButton:hover {"
                    "  background-color: %4;"
                    "}")
                    .arg(qssColor(approveFill),
                         qssColor(approveColor),
                         qssColor(approveBorder),
                         qssColor(approveHover)));

            refs.denyButton->setIcon(icons::vectorIcon(QStringLiteral("close"), denyColor, 12));
            refs.denyButton->setIconSize(QSize(12, 12));
            refs.denyButton->setStyleSheet(
                QStringLiteral(
                    "QPushButton#terminalDenyButton {"
                    "  background-color: %1;"
                    "  color: %2;"
                    "  border: 1px solid %3;"
                    "  border-radius: 5px;"
                    "  padding: 4px 10px;"
                    "  font-size: 11px;"
                    "  font-weight: 600;"
                    "}"
                    "QPushButton#terminalDenyButton:hover {"
                    "  background-color: %4;"
                    "}")
                    .arg(qssColor(denyFill),
                         qssColor(denyColor),
                         qssColor(denyBorder),
                         qssColor(denyHover)));
        }

        if (!pendingApprovals.isEmpty()) {
            approvalToggleButton->setIcon(icons::vectorIcon(
                approvalsCollapsed ? QStringLiteral("chevron-right") : QStringLiteral("chevron-down"),
                textSec,
                13));
        }
    }

    void applyExitNoticeTheme() const {
        if (!exitNoticeBar) {
            return;
        }
        const UiPalette u = ThemeManager::instance().ui();
        const QColor elevated = compositeOver(u.elevatedSurface, u.background);
        const QColor border = compositeOver(u.subtleBorder, u.background);
        const QColor textSec = compositeOver(u.textSecondary, u.background);
        const bool isErrorExit = lastExitCode.has_value() && *lastExitCode != 0;
        const QColor accentColor = isErrorExit ? u.danger : textSec;

        exitNoticeBar->setStyleSheet(
            QStringLiteral("QWidget#terminalExitNoticeBar { background-color: %1; }")
                .arg(qssColor(elevated)));
        exitNoticeIconLabel->setPixmap(
            icons::vectorIcon(isErrorExit ? QStringLiteral("warning") : QStringLiteral("terminal"),
                              accentColor,
                              13)
                .pixmap(13, 13));
        exitNoticeTextLabel->setStyleSheet(
            QStringLiteral("color: %1; background: transparent; font-size: 11px; font-weight: 500;")
                .arg(qssColor(accentColor)));
        exitRestartButton->setIcon(icons::vectorIcon(QStringLiteral("refresh"), u.textPrimary, 12));
        exitRestartButton->setIconSize(QSize(12, 12));
        exitRestartButton->setStyleSheet(
            QStringLiteral(
                "QPushButton#terminalExitRestartButton {"
                "  background-color: %1;"
                "  color: %2;"
                "  border: 1px solid %3;"
                "  border-radius: 5px;"
                "  padding: 2px 8px;"
                "  font-size: 11px;"
                "  font-weight: 600;"
                "}"
                "QPushButton#terminalExitRestartButton:hover {"
                "  border-color: %4;"
                "}")
                .arg(qssColor(u.surface),
                     qssColor(u.textPrimary),
                     qssColor(border),
                     qssColor(u.selectedBorder)));
        exitDismissButton->applyTheme();
        exitBottomDivider->applyTheme();
    }

    void applyPlaceholderTheme() const {
        if (!poppedOutPlaceholder) {
            return;
        }
        const UiPalette u = ThemeManager::instance().ui();
        const QColor textSec = compositeOver(u.textSecondary, u.background);
        const QColor elevated = compositeOver(u.elevatedSurface, u.background);
        const QColor selected = compositeOver(u.selectedFill, u.background);
        const QColor border = compositeOver(u.subtleBorder, u.background);

        poppedOutPlaceholder->setStyleSheet(
            QStringLiteral("QWidget#terminalPoppedOutPlaceholder { background-color: %1; }")
                .arg(qssColor(u.background)));
        poppedOutIconLabel->setPixmap(
            icons::vectorIcon(QStringLiteral("popout"), textSec, 15).pixmap(15, 15));
        poppedOutMessageLabel->setStyleSheet(
            QStringLiteral("color: %1; background: transparent; font-size: 11.5px;")
                .arg(qssColor(textSec)));

        const QString secondaryBtnQss = QStringLiteral(
            "QPushButton {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 6px;"
            "  padding: 4px 10px;"
            "  font-size: 11px;"
            "  font-weight: 600;"
            "}"
            "QPushButton:hover {"
            "  background-color: %4;"
            "  border-color: %5;"
            "}")
            .arg(qssColor(u.surface),
                 qssColor(u.textPrimary),
                 qssColor(border),
                 qssColor(elevated),
                 qssColor(u.selectedBorder));
        focusWindowButton->setStyleSheet(secondaryBtnQss);

        const QString primaryBtnQss = QStringLiteral(
            "QPushButton {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 6px;"
            "  padding: 4px 10px;"
            "  font-size: 11px;"
            "  font-weight: 600;"
            "}"
            "QPushButton:hover {"
            "  background-color: %4;"
            "}")
            .arg(qssColor(selected),
                 qssColor(u.textPrimary),
                 qssColor(u.selectedBorder),
                 qssColor(compositeOver(withAlpha(u.accent, 0.18), u.background)));
        placeholderDockBackButton->setStyleSheet(primaryBtnQss);
    }

    void applyPopOutWindowTheme() const {
        if (!popOutWindow) {
            return;
        }
        const UiPalette u = ThemeManager::instance().ui();
        const TerminalPalette tp = ThemeManager::instance().terminal();
        QPalette pal = popOutWindow->palette();
        pal.setColor(QPalette::Window, u.background);
        pal.setColor(QPalette::WindowText, u.textPrimary);
        popOutWindow->setPalette(pal);
        popOutWindow->setStyleSheet(
            QStringLiteral("QWidget#spellTerminalPopOutWindow { background-color: %1; }")
                .arg(qssColor(u.background)));
        if (popOutSurfaceHost) {
            popOutSurfaceHost->setStyleSheet(
                QStringLiteral("background-color: %1;").arg(qssColor(tp.background)));
        }
    }

    void updateSurfaceHostBackground() const {
        const UiPalette u = ThemeManager::instance().ui();
        const TerminalPalette tp = ThemeManager::instance().terminal();
        if (dockedSurfaceHost) {
            const QColor bg = poppedOut ? u.background : tp.background;
            dockedSurfaceHost->setStyleSheet(
                QStringLiteral("background-color: %1;").arg(qssColor(bg)));
        }
        if (popOutSurfaceHost) {
            popOutSurfaceHost->setStyleSheet(
                QStringLiteral("background-color: %1;").arg(qssColor(tp.background)));
        }
    }

    void showExitNotice(int exitCode) {
        lastExitCode = exitCode;
        exitNoticeVisible = true;
        if (exitNoticeTextLabel) {
            exitNoticeTextLabel->setText(
                QStringLiteral("Shell exited with status %1").arg(exitCode));
        }
        applyExitNoticeTheme();
        if (exitNoticeBar) {
            exitNoticeBar->show();
        }
    }

    void hideExitNotice() {
        lastExitCode.reset();
        exitNoticeVisible = false;
        if (exitNoticeBar) {
            exitNoticeBar->hide();
        }
    }
};

SpellTerminalPanel::SpellTerminalPanel(SpellCoreClient* client, QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>(this)) {
    setObjectName(QStringLiteral("spellTerminalPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_impl->rootLayout = new QVBoxLayout(this);
    m_impl->rootLayout->setContentsMargins(0, 0, 0, 0);
    m_impl->rootLayout->setSpacing(0);

    // Header bar + hairline
    m_impl->dockedHeader = m_impl->buildHeaderBar(this, false);
    m_impl->rootLayout->addWidget(m_impl->dockedHeader.bar);
    m_impl->rootLayout->addWidget(m_impl->dockedHeader.bottomDivider);

    // Docked approval banner slot
    m_impl->dockedApprovalSlot = new QWidget(this);
    m_impl->dockedApprovalSlot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    m_impl->dockedApprovalSlotLayout = new QVBoxLayout(m_impl->dockedApprovalSlot);
    m_impl->dockedApprovalSlotLayout->setContentsMargins(0, 0, 0, 0);
    m_impl->dockedApprovalSlotLayout->setSpacing(0);
    m_impl->buildApprovalSection(m_impl->dockedApprovalSlot);
    m_impl->dockedApprovalSlotLayout->addWidget(m_impl->approvalSection);
    m_impl->rootLayout->addWidget(m_impl->dockedApprovalSlot);

    // Docked exited-shell notice slot
    m_impl->dockedExitNoticeSlot = new QWidget(this);
    m_impl->dockedExitNoticeSlot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    m_impl->dockedExitNoticeSlotLayout = new QVBoxLayout(m_impl->dockedExitNoticeSlot);
    m_impl->dockedExitNoticeSlotLayout->setContentsMargins(0, 0, 0, 0);
    m_impl->dockedExitNoticeSlotLayout->setSpacing(0);
    m_impl->buildExitNoticeBar(m_impl->dockedExitNoticeSlot);
    m_impl->dockedExitNoticeSlotLayout->addWidget(m_impl->exitNoticeBar);
    m_impl->rootLayout->addWidget(m_impl->dockedExitNoticeSlot);

    // Surface container (hosts either live SpellTerminalSurface or popped-out placeholder)
    m_impl->dockedSurfaceHost = new QWidget(this);
    m_impl->dockedSurfaceHost->setAttribute(Qt::WA_StyledBackground, true);
    m_impl->dockedSurfaceHost->setAutoFillBackground(true);
    m_impl->dockedSurfaceHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_impl->dockedSurfaceLayout = new QVBoxLayout(m_impl->dockedSurfaceHost);
    m_impl->dockedSurfaceLayout->setContentsMargins(0, 0, 0, 0);
    m_impl->dockedSurfaceLayout->setSpacing(0);

    m_impl->buildPoppedOutPlaceholder(m_impl->dockedSurfaceHost);
    m_impl->dockedSurfaceLayout->addWidget(m_impl->poppedOutPlaceholder);

    m_impl->surface = new SpellTerminalSurface(m_impl->dockedSurfaceHost);
    m_impl->dockedSurfaceLayout->addWidget(m_impl->surface, 1);
    m_impl->rootLayout->addWidget(m_impl->dockedSurfaceHost, 1);

    // Surface signal wiring
    connect(m_impl->surface, &SpellTerminalSurface::gridSizeChanged, this, [this](int, int) {
        if (!m_impl->destroying) {
            m_impl->syncHeaderChrome();
        }
    });
    connect(m_impl->surface, &SpellTerminalSurface::titleChanged, this, [this](const QString&) {
        if (!m_impl->destroying) {
            m_impl->syncHeaderChrome();
        }
    });
    connect(m_impl->surface, &SpellTerminalSurface::workingDirectoryChanged, this, [this](const QString&) {
        if (!m_impl->destroying) {
            m_impl->syncHeaderChrome();
        }
    });
    connect(m_impl->surface, &SpellTerminalSurface::shellStarted, this, [this](const QString&) {
        if (m_impl->destroying) {
            return;
        }
        m_impl->hideExitNotice();
        m_impl->syncHeaderChrome();
        emit shellStateChanged(true);
    });
    connect(m_impl->surface, &SpellTerminalSurface::shellExited, this, [this](int code) {
        if (m_impl->destroying) {
            return;
        }
        m_impl->showExitNotice(code);
        m_impl->syncHeaderChrome();
        emit shellStateChanged(false);
    });
    connect(m_impl->surface,
            &SpellTerminalSurface::spellHandOffRequested,
            this,
            &SpellTerminalPanel::spellHandOffRequested);

    // Live theme synchronization
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &SpellTerminalPanel::applyTheme);
    connect(&ThemeManager::instance(), &ThemeManager::terminalThemeChanged, this, &SpellTerminalPanel::applyTheme);

    attachClient(client);
    applyTheme();
    m_impl->syncHeaderChrome();
}

SpellTerminalPanel::~SpellTerminalPanel() {
    if (!m_impl) {
        return;
    }
    m_impl->destroying = true;

    if (m_impl->clientApprovalConn) {
        disconnect(m_impl->clientApprovalConn);
    }

    if (m_impl->surface) {
        m_impl->surface->stopShell();
    }

    if (m_impl->popOutWindow) {
        m_impl->popOutWindow->removeEventFilter(this);

        // Reclaim movable widgets back to this panel before deleting the top-level window
        if (m_impl->surface && m_impl->popOutWindow->isAncestorOf(m_impl->surface)) {
            if (m_impl->popOutSurfaceLayout) {
                m_impl->popOutSurfaceLayout->removeWidget(m_impl->surface);
            }
            m_impl->surface->setParent(this);
        }
        if (m_impl->approvalSection && m_impl->popOutWindow->isAncestorOf(m_impl->approvalSection)) {
            if (m_impl->popOutApprovalSlotLayout) {
                m_impl->popOutApprovalSlotLayout->removeWidget(m_impl->approvalSection);
            }
            m_impl->approvalSection->setParent(this);
        }
        if (m_impl->exitNoticeBar && m_impl->popOutWindow->isAncestorOf(m_impl->exitNoticeBar)) {
            if (m_impl->popOutExitNoticeSlotLayout) {
                m_impl->popOutExitNoticeSlotLayout->removeWidget(m_impl->exitNoticeBar);
            }
            m_impl->exitNoticeBar->setParent(this);
        }

        m_impl->popOutWindow->hide();
        delete m_impl->popOutWindow;
        m_impl->popOutWindow = nullptr;
    }
}

void SpellTerminalPanel::attachClient(SpellCoreClient* client) {
    if (m_impl->clientApprovalConn) {
        disconnect(m_impl->clientApprovalConn);
        m_impl->clientApprovalConn = {};
    }

    m_impl->client = client;
    if (m_impl->client) {
        m_impl->clientApprovalConn = connect(
            m_impl->client,
            &SpellCoreClient::terminalCommandRequested,
            this,
            &SpellTerminalPanel::showApprovalRequest);
    }
}

SpellTerminalSurface* SpellTerminalPanel::surface() const {
    return m_impl->surface;
}

QWidget* SpellTerminalPanel::popOutWindow() const {
    return m_impl->popOutWindow;
}

bool SpellTerminalPanel::isPoppedOut() const {
    return m_impl->poppedOut;
}

// -----------------------------------------------------------------------------
// Shell lifecycle delegation
// -----------------------------------------------------------------------------

void SpellTerminalPanel::startShell(const QString& cwd) {
    m_impl->hideExitNotice();
    if (m_impl->surface) {
        m_impl->surface->startShell(cwd);
    }
    m_impl->syncHeaderChrome();
}

void SpellTerminalPanel::ensureShellRunning(const QString& cwd) {
    if (m_impl->surface) {
        m_impl->surface->ensureShellRunning(cwd);
    }
    m_impl->syncHeaderChrome();
}

void SpellTerminalPanel::restartShell(const QString& cwd) {
    m_impl->hideExitNotice();
    if (m_impl->surface) {
        const QString targetCwd = cwd.isEmpty() ? m_impl->surface->workingDirectory() : cwd;
        m_impl->surface->restartShell(targetCwd);
    }
    m_impl->syncHeaderChrome();
}

void SpellTerminalPanel::stopShell() {
    if (m_impl->surface) {
        m_impl->surface->stopShell();
    }
    m_impl->syncHeaderChrome();
}

void SpellTerminalPanel::sendCommandText(const QString& text, bool pressEnter) {
    if (m_impl->surface) {
        m_impl->surface->sendCommandText(text, pressEnter);
    }
}

bool SpellTerminalPanel::isShellRunning() const {
    return m_impl->surface && m_impl->surface->isShellRunning();
}

QString SpellTerminalPanel::workingDirectory() const {
    return m_impl->surface ? m_impl->surface->workingDirectory() : QString();
}

QString SpellTerminalPanel::recentOutputText(int maxLines) const {
    return m_impl->surface ? m_impl->surface->recentOutputText(maxLines) : QString();
}

int SpellTerminalPanel::gridCols() const {
    return m_impl->surface ? m_impl->surface->gridCols() : 80;
}

int SpellTerminalPanel::gridRows() const {
    return m_impl->surface ? m_impl->surface->gridRows() : 24;
}

void SpellTerminalPanel::setAgentReason(const QString& reason) {
    const QString trimmed = reason.trimmed();
    if (m_impl->agentReason == trimmed) {
        return;
    }
    m_impl->agentReason = trimmed;
    m_impl->syncHeaderChrome();
}

QString SpellTerminalPanel::agentReason() const {
    return m_impl->agentReason;
}

// -----------------------------------------------------------------------------
// Pending agent terminal-command approval banners
// -----------------------------------------------------------------------------

void SpellTerminalPanel::showApprovalRequest(const TerminalApprovalRequest& request) {
    if (request.id.isEmpty() && request.command.isEmpty()) {
        return;
    }

    bool updatedExisting = false;
    if (!request.id.isEmpty()) {
        for (TerminalApprovalRequest& existing : m_impl->pendingApprovals) {
            if (existing.id == request.id) {
                existing = request;
                updatedExisting = true;
                break;
            }
        }
    }

    if (!updatedExisting) {
        m_impl->pendingApprovals.append(request);
        m_impl->approvalsCollapsed = false;
    }

    m_impl->rebuildApprovalCards();
}

void SpellTerminalPanel::clearApprovalRequest(const QString& approvalId) {
    if (approvalId.isEmpty()) {
        if (m_impl->pendingApprovals.isEmpty()) {
            return;
        }
        m_impl->pendingApprovals.clear();
        m_impl->rebuildApprovalCards();
        return;
    }

    const qsizetype before = m_impl->pendingApprovals.size();
    m_impl->pendingApprovals.erase(
        std::remove_if(m_impl->pendingApprovals.begin(),
                       m_impl->pendingApprovals.end(),
                       [&approvalId](const TerminalApprovalRequest& r) {
                           return r.id == approvalId;
                       }),
        m_impl->pendingApprovals.end());

    if (m_impl->pendingApprovals.size() != before) {
        m_impl->rebuildApprovalCards();
    }
}

void SpellTerminalPanel::setPendingApprovals(const QVector<TerminalApprovalRequest>& requests) {
    QVector<TerminalApprovalRequest> deduped;
    deduped.reserve(requests.size());
    for (const TerminalApprovalRequest& req : requests) {
        if (req.id.isEmpty() && req.command.isEmpty()) {
            continue;
        }
        bool replaced = false;
        if (!req.id.isEmpty()) {
            for (TerminalApprovalRequest& existing : deduped) {
                if (existing.id == req.id) {
                    existing = req;
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) {
            deduped.append(req);
        }
    }

    if (m_impl->pendingApprovals == deduped) {
        return;
    }

    // Auto-expand if a brand-new approval request arrived
    bool hasNewRequest = false;
    for (const TerminalApprovalRequest& req : deduped) {
        const bool alreadyPresent = std::any_of(
            m_impl->pendingApprovals.cbegin(),
            m_impl->pendingApprovals.cend(),
            [&req](const TerminalApprovalRequest& existing) {
                return !req.id.isEmpty() ? (existing.id == req.id) : (existing == req);
            });
        if (!alreadyPresent) {
            hasNewRequest = true;
            break;
        }
    }
    if (hasNewRequest) {
        m_impl->approvalsCollapsed = false;
    }

    m_impl->pendingApprovals = std::move(deduped);
    m_impl->rebuildApprovalCards();
}

QVector<TerminalApprovalRequest> SpellTerminalPanel::pendingApprovals() const {
    return m_impl->pendingApprovals;
}

bool SpellTerminalPanel::hasPendingApprovals() const {
    return !m_impl->pendingApprovals.isEmpty();
}

void SpellTerminalPanel::approveRequest(const QString& approvalId) {
    const auto it = std::find_if(
        m_impl->pendingApprovals.begin(),
        m_impl->pendingApprovals.end(),
        [&approvalId](const TerminalApprovalRequest& req) {
            return req.id == approvalId;
        });
    if (it == m_impl->pendingApprovals.end()) {
        return;
    }

    const TerminalApprovalRequest req = *it;
    m_impl->pendingApprovals.erase(it);
    m_impl->rebuildApprovalCards();

    if (m_impl->surface) {
        if (!m_impl->surface->isShellRunning()) {
            m_impl->surface->ensureShellRunning(m_impl->surface->workingDirectory());
        }
        const QString cmd = normalizeCommandForExecution(req.command);
        m_impl->surface->sendCommandText(cmd, true);
        if (m_impl->surface->isVisible()) {
            m_impl->surface->setFocus(Qt::OtherFocusReason);
        }
    }

    const QString output = m_impl->surface ? m_impl->surface->recentOutputText(120) : QString();
    if (m_impl->client) {
        m_impl->client->respondTerminalApproval(req.id, true, output);
    }

    emit approvalResponded(req.id, true, output);
}

void SpellTerminalPanel::denyRequest(const QString& approvalId) {
    const auto it = std::find_if(
        m_impl->pendingApprovals.begin(),
        m_impl->pendingApprovals.end(),
        [&approvalId](const TerminalApprovalRequest& req) {
            return req.id == approvalId;
        });
    if (it == m_impl->pendingApprovals.end()) {
        return;
    }

    const QString id = it->id;
    m_impl->pendingApprovals.erase(it);
    m_impl->rebuildApprovalCards();

    if (m_impl->client) {
        m_impl->client->respondTerminalApproval(id, false, QString());
    }

    emit approvalResponded(id, false, QString());
}

QSize SpellTerminalPanel::sizeHint() const {
    return QSize(560, 260);
}

QSize SpellTerminalPanel::minimumSizeHint() const {
    return QSize(260, 120);
}

// -----------------------------------------------------------------------------
// Public slots
// -----------------------------------------------------------------------------

void SpellTerminalPanel::clearTerminal() {
    if (m_impl->surface) {
        m_impl->surface->clearBuffer();
    }
}

void SpellTerminalPanel::popOut() {
    if (m_impl->poppedOut) {
        if (m_impl->popOutWindow) {
            m_impl->popOutWindow->show();
            m_impl->popOutWindow->raise();
            m_impl->popOutWindow->activateWindow();
        }
        if (m_impl->surface) {
            m_impl->surface->setFocus(Qt::OtherFocusReason);
        }
        return;
    }

    m_impl->ensurePopOutWindowCreated();
    m_impl->poppedOut = true;

    // Move approval banner into the pop-out window
    if (m_impl->approvalSection) {
        const bool showApprovals = !m_impl->pendingApprovals.isEmpty();
        m_impl->dockedApprovalSlotLayout->removeWidget(m_impl->approvalSection);
        m_impl->dockedApprovalSlot->hide();
        m_impl->approvalSection->setParent(m_impl->popOutApprovalSlot);
        m_impl->popOutApprovalSlotLayout->addWidget(m_impl->approvalSection);
        m_impl->popOutApprovalSlot->show();
        m_impl->approvalSection->setVisible(showApprovals);
    }

    // Move exited-shell notice bar into the pop-out window
    if (m_impl->exitNoticeBar) {
        m_impl->dockedExitNoticeSlotLayout->removeWidget(m_impl->exitNoticeBar);
        m_impl->dockedExitNoticeSlot->hide();
        m_impl->exitNoticeBar->setParent(m_impl->popOutExitNoticeSlot);
        m_impl->popOutExitNoticeSlotLayout->addWidget(m_impl->exitNoticeBar);
        m_impl->popOutExitNoticeSlot->show();
        m_impl->exitNoticeBar->setVisible(m_impl->exitNoticeVisible);
    }

    // Seamlessly reparent the live SpellTerminalSurface without interrupting its PTY
    if (m_impl->surface) {
        m_impl->dockedSurfaceLayout->removeWidget(m_impl->surface);
        m_impl->surface->setParent(m_impl->popOutSurfaceHost);
        m_impl->popOutSurfaceLayout->addWidget(m_impl->surface, 1);
        m_impl->surface->show();
    }

    if (m_impl->poppedOutPlaceholder) {
        m_impl->poppedOutPlaceholder->show();
    }

    m_impl->updateSurfaceHostBackground();
    m_impl->syncHeaderChrome();

    m_impl->popOutWindow->show();
    m_impl->popOutWindow->raise();
    m_impl->popOutWindow->activateWindow();

    if (m_impl->surface) {
        m_impl->surface->setFocus(Qt::OtherFocusReason);
    }

    if (!m_impl->pendingApprovals.isEmpty() && !m_impl->approvalsCollapsed) {
        m_impl->updateApprovalScrollHeight();
    }

    emit popOutToggled(true);
}

void SpellTerminalPanel::dockBack() {
    if (!m_impl->poppedOut) {
        return;
    }

    m_impl->poppedOut = false;

    if (m_impl->poppedOutPlaceholder) {
        m_impl->poppedOutPlaceholder->hide();
    }

    // Move approval banner back to the docked panel
    if (m_impl->approvalSection && m_impl->popOutApprovalSlotLayout) {
        const bool showApprovals = !m_impl->pendingApprovals.isEmpty();
        m_impl->popOutApprovalSlotLayout->removeWidget(m_impl->approvalSection);
        m_impl->popOutApprovalSlot->hide();
        m_impl->approvalSection->setParent(m_impl->dockedApprovalSlot);
        m_impl->dockedApprovalSlotLayout->addWidget(m_impl->approvalSection);
        m_impl->dockedApprovalSlot->show();
        m_impl->approvalSection->setVisible(showApprovals);
    }

    // Move exited-shell notice bar back to the docked panel
    if (m_impl->exitNoticeBar && m_impl->popOutExitNoticeSlotLayout) {
        m_impl->popOutExitNoticeSlotLayout->removeWidget(m_impl->exitNoticeBar);
        m_impl->popOutExitNoticeSlot->hide();
        m_impl->exitNoticeBar->setParent(m_impl->dockedExitNoticeSlot);
        m_impl->dockedExitNoticeSlotLayout->addWidget(m_impl->exitNoticeBar);
        m_impl->dockedExitNoticeSlot->show();
        m_impl->exitNoticeBar->setVisible(m_impl->exitNoticeVisible);
    }

    // Seamlessly reparent the live SpellTerminalSurface back into the docked container
    if (m_impl->surface && m_impl->popOutSurfaceLayout) {
        m_impl->popOutSurfaceLayout->removeWidget(m_impl->surface);
        m_impl->surface->setParent(m_impl->dockedSurfaceHost);
        m_impl->dockedSurfaceLayout->addWidget(m_impl->surface, 1);
        m_impl->surface->show();
    }

    if (m_impl->popOutWindow) {
        m_impl->popOutWindow->hide();
    }

    m_impl->updateSurfaceHostBackground();
    m_impl->syncHeaderChrome();

    if (isVisible() && m_impl->surface) {
        m_impl->surface->setFocus(Qt::OtherFocusReason);
    }

    if (!m_impl->pendingApprovals.isEmpty() && !m_impl->approvalsCollapsed) {
        m_impl->updateApprovalScrollHeight();
    }

    emit popOutToggled(false);
}

void SpellTerminalPanel::setPoppedOut(bool poppedOut) {
    if (poppedOut) {
        popOut();
    } else {
        dockBack();
    }
}

void SpellTerminalPanel::togglePopOut() {
    setPoppedOut(!m_impl->poppedOut);
}

void SpellTerminalPanel::applyTheme() {
    const UiPalette u = ThemeManager::instance().ui();

    QPalette pal = palette();
    pal.setColor(QPalette::Window, u.background);
    pal.setColor(QPalette::WindowText, u.textPrimary);
    setPalette(pal);
    setStyleSheet(
        QStringLiteral("QWidget#spellTerminalPanel { background-color: %1; }")
            .arg(qssColor(u.background)));

    m_impl->applyHeaderTheme(m_impl->dockedHeader);
    if (m_impl->popOutWindow) {
        m_impl->applyHeaderTheme(m_impl->popOutHeader);
        m_impl->applyPopOutWindowTheme();
    }
    m_impl->applyApprovalTheme();
    m_impl->applyExitNoticeTheme();
    m_impl->applyPlaceholderTheme();
    m_impl->updateSurfaceHostBackground();

    if (m_impl->surface) {
        m_impl->surface->applyTheme();
    }
}

// -----------------------------------------------------------------------------
// Protected event filter
// -----------------------------------------------------------------------------

bool SpellTerminalPanel::eventFilter(QObject* watched, QEvent* event) {
    if (!event || !m_impl || m_impl->destroying) {
        return QWidget::eventFilter(watched, event);
    }

    if (watched == m_impl->popOutWindow) {
        if (event->type() == QEvent::Close) {
            if (m_impl->poppedOut) {
                dockBack();
            } else if (m_impl->popOutWindow) {
                m_impl->popOutWindow->hide();
            }
            event->ignore();
            return true;
        }
        if (event->type() == QEvent::WindowActivate) {
            if (m_impl->poppedOut && m_impl->surface && m_impl->surface->isVisible()) {
                m_impl->surface->setFocus(Qt::OtherFocusReason);
            }
        }
    } else if (watched == m_impl->dockedHeader.dirLabel && event->type() == QEvent::Resize) {
        m_impl->updateElidedDirectoryLabel(m_impl->dockedHeader.dirLabel);
    } else if (m_impl->popOutWindow && watched == m_impl->popOutHeader.dirLabel && event->type() == QEvent::Resize) {
        m_impl->updateElidedDirectoryLabel(m_impl->popOutHeader.dirLabel);
    } else if (watched == m_impl->approvalCardsHost &&
               (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize)) {
        if (!m_impl->pendingApprovals.isEmpty() && !m_impl->approvalsCollapsed) {
            m_impl->updateApprovalScrollHeight();
        }
    }

    return QWidget::eventFilter(watched, event);
}

} // namespace spell
