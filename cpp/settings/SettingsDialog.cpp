#include "settings/SettingsDialog.h"

#include "core/SpellCoreClient.h"
#include "core/SpellSettingsStore.h"
#include "settings/AboutSettingsPane.h"
#include "settings/AgentSettingsPane.h"
#include "settings/ConversationSettingsPane.h"
#include "settings/DiagnosticsSettingsPane.h"
#include "settings/EngineSettingsPane.h"
#include "settings/GeneralSettingsPane.h"
#include "settings/OnlineResearchSettingsPane.h"
#include "settings/StatisticsSettingsPane.h"
#include "settings/SubagentsSettingsPane.h"
#include "settings/TerminalSettingsPane.h"
#include "settings/TimersSettingsPane.h"
#include "shell/WindowChrome.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <QButtonGroup>
#include <QCoreApplication>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>

namespace spell {

namespace {

constexpr int kSidebarWidthPx = 195;
constexpr int kMinDialogWidthPx = 840;
constexpr int kMinDialogHeightPx = 580;
constexpr int kDefaultDialogWidthPx = 960;
constexpr int kDefaultDialogHeightPx = 660;
constexpr int kNavIconSizePx = 14;
constexpr int kFooterHeightPx = 48;
constexpr int kHatMarkSizePx = 22;
constexpr int kSaveCloseButtonSizePx = 30;
constexpr int kSaveCloseIconSizePx = 16;

SettingsPaneId parseLaunchPaneArgument() {
    if (QCoreApplication::instance() == nullptr) {
        return SettingsPaneId::General;
    }
    const QStringList args = QCoreApplication::arguments();
    constexpr QLatin1StringView kPrefix("--settings-pane=");
    for (int i = 1; i < args.size(); ++i) {
        const QString& arg = args.at(i);
        if (arg == QLatin1StringView("--settings-pane") && i + 1 < args.size()) {
            return settingsPaneFromName(args.at(i + 1));
        }
        if (arg.startsWith(kPrefix)) {
            return settingsPaneFromName(arg.mid(kPrefix.size()));
        }
    }
    return SettingsPaneId::General;
}

QString paneObjectKey(SettingsPaneId pane) {
    QString key = settingsPaneTitle(pane);
    key.remove(QLatin1Char(' '));
    return key;
}

QString navButtonObjectName(SettingsPaneId pane) {
    return QStringLiteral("spellSettingsNav_") + paneObjectKey(pane);
}

} // namespace

struct SettingsDialog::Impl {
    SpellCoreClient* client = nullptr;
    SpellSettingsStore* store = nullptr;

    QWidget* sidebar = nullptr;
    QButtonGroup* navGroup = nullptr;
    std::array<QPushButton*, kSettingsPaneCount> navButtons{};
    SpellHairline* footerHairline = nullptr;
    QWidget* footerWidget = nullptr;
    QLabel* hatMarkLabel = nullptr;
    QPushButton* saveCloseButton = nullptr;

    SpellHairline* verticalHairline = nullptr;

    QStackedWidget* stack = nullptr;
    std::array<QScrollArea*, kSettingsPaneCount> paneScrollAreas{};
    std::array<QLabel*, kSettingsPaneCount> paneTitleLabels{};

    GeneralSettingsPane* generalPane = nullptr;
    EngineSettingsPane* enginePane = nullptr;
    StatisticsSettingsPane* statisticsPane = nullptr;
    SubagentsSettingsPane* subagentsPane = nullptr;
    ConversationSettingsPane* conversationPane = nullptr;
    AgentSettingsPane* agentPane = nullptr;
    OnlineResearchSettingsPane* onlineResearchPane = nullptr;
    TimersSettingsPane* timersPane = nullptr;
    TerminalSettingsPane* terminalPane = nullptr;
    DiagnosticsSettingsPane* diagnosticsPane = nullptr;
    AboutSettingsPane* aboutPane = nullptr;

    int currentIndex = 0;
    bool initialized = false;
    bool applyingTheme = false;
    bool systemInfoInFlight = false;
    bool hasSnapshot = false;
    WorkspaceSnapshot lastSnapshot;
    bool hasSystemInfo = false;
    SystemInfo lastSystemInfo;
    bool terminalDocked = false;
    bool terminalPoppedOut = false;

    void updateSidebarIcons(const UiPalette& u) const {
        for (int i = 0; i < kSettingsPaneCount; ++i) {
            QPushButton* btn = navButtons[static_cast<std::size_t>(i)];
            if (!btn) {
                continue;
            }
            const auto pane = static_cast<SettingsPaneId>(i);
            const QColor tint = (i == currentIndex) ? u.textPrimary : u.textSecondary;
            btn->setIcon(icons::vectorIcon(settingsPaneIconName(pane), tint, kNavIconSizePx));
        }
    }

    void requestSystemInfo(SettingsDialog* owner) {
        if (!client || systemInfoInFlight) {
            return;
        }
        systemInfoInFlight = true;
        QPointer<SettingsDialog> guard(owner);
        client->fetchSystemInfo([guard](const SystemInfo& info) {
            if (!guard) {
                return;
            }
            guard->m_impl->systemInfoInFlight = false;
            guard->updateSystemInfo(info);
        });
    }
};

SettingsDialog::SettingsDialog(SpellCoreClient* client,
                               SpellSettingsStore* store,
                               QWidget* parent)
    : QDialog(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->client = client;
    m_impl->store = store;

    setObjectName(QStringLiteral("spellSettingsDialog"));
    setWindowTitle(QStringLiteral("Settings — BeatrixLeslie IDE"));
    setWindowIcon(icons::appWindowIcon());
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumSize(kMinDialogWidthPx, kMinDialogHeightPx);
    resize(kDefaultDialogWidthPx, kDefaultDialogHeightPx);
    installFramelessSheetChrome(this);

    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(1, 1, 1, 1);
    rootLayout->setSpacing(0);

    // -------------------------------------------------------------------------
    // Left 195px sidebar: 11 pane rows + horizontal hairline + 48px icon footer
    // -------------------------------------------------------------------------
    m_impl->sidebar = new QWidget(this);
    m_impl->sidebar->setObjectName(QStringLiteral("spellSettingsSidebar"));
    m_impl->sidebar->setFixedWidth(kSidebarWidthPx);
    m_impl->sidebar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    installWindowDragRegion(m_impl->sidebar);

    auto* sidebarLayout = new QVBoxLayout(m_impl->sidebar);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);

    auto* navContainer = new QWidget(m_impl->sidebar);
    navContainer->setObjectName(QStringLiteral("spellSettingsNavContainer"));
    installWindowDragRegion(navContainer);

    auto* navLayout = new QVBoxLayout(navContainer);
    navLayout->setContentsMargins(8, 14, 8, 10);
    navLayout->setSpacing(2);

    m_impl->navGroup = new QButtonGroup(this);
    m_impl->navGroup->setExclusive(true);

    for (int i = 0; i < kSettingsPaneCount; ++i) {
        const auto pane = static_cast<SettingsPaneId>(i);
        const QString title = settingsPaneTitle(pane);

        auto* btn = new QPushButton(title, navContainer);
        btn->setObjectName(navButtonObjectName(pane));
        btn->setProperty("spellNavItem", true);
        btn->setAccessibleName(title);
        btn->setCheckable(true);
        btn->setAutoDefault(false);
        btn->setDefault(false);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setIconSize(QSize(kNavIconSizePx, kNavIconSizePx));
        btn->setMinimumHeight(30);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_impl->navGroup->addButton(btn, i);
        m_impl->navButtons[static_cast<std::size_t>(i)] = btn;
        navLayout->addWidget(btn);
    }

    navLayout->addStretch(1);
    sidebarLayout->addWidget(navContainer, 1);

    m_impl->footerHairline = new SpellHairline(Qt::Horizontal, m_impl->sidebar);
    sidebarLayout->addWidget(m_impl->footerHairline);

    m_impl->footerWidget = new QWidget(m_impl->sidebar);
    m_impl->footerWidget->setObjectName(QStringLiteral("spellSettingsFooter"));
    m_impl->footerWidget->setFixedHeight(kFooterHeightPx);
    installWindowDragRegion(m_impl->footerWidget);

    auto* footerLayout = new QHBoxLayout(m_impl->footerWidget);
    footerLayout->setContentsMargins(14, 0, 14, 0);
    footerLayout->setSpacing(0);

    m_impl->hatMarkLabel = new QLabel(m_impl->footerWidget);
    m_impl->hatMarkLabel->setObjectName(QStringLiteral("spellSettingsHatMark"));
    m_impl->hatMarkLabel->setFixedSize(kHatMarkSizePx, kHatMarkSizePx);
    m_impl->hatMarkLabel->setAlignment(Qt::AlignCenter);
    m_impl->hatMarkLabel->setToolTip(QStringLiteral("BeatrixLeslie IDE"));

    m_impl->saveCloseButton = new QPushButton(m_impl->footerWidget);
    m_impl->saveCloseButton->setText(QString());
    m_impl->saveCloseButton->setObjectName(QStringLiteral("spellSettingsSaveCloseButton"));
    m_impl->saveCloseButton->setAccessibleName(QStringLiteral("Save and close"));
    m_impl->saveCloseButton->setToolTip(QStringLiteral("Save and close"));
    m_impl->saveCloseButton->setCursor(Qt::PointingHandCursor);
    m_impl->saveCloseButton->setAutoDefault(false);
    m_impl->saveCloseButton->setDefault(false);
    m_impl->saveCloseButton->setFlat(true);
    m_impl->saveCloseButton->setFocusPolicy(Qt::NoFocus);
    m_impl->saveCloseButton->setFixedSize(kSaveCloseButtonSizePx, kSaveCloseButtonSizePx);
    m_impl->saveCloseButton->setIconSize(QSize(kSaveCloseIconSizePx, kSaveCloseIconSizePx));

    footerLayout->addWidget(m_impl->hatMarkLabel, 0, Qt::AlignVCenter);
    footerLayout->addStretch(1);
    footerLayout->addWidget(m_impl->saveCloseButton, 0, Qt::AlignVCenter);
    sidebarLayout->addWidget(m_impl->footerWidget);

    rootLayout->addWidget(m_impl->sidebar);

    // Vertical hairline divider between sidebar and right content stack
    m_impl->verticalHairline = new SpellHairline(Qt::Vertical, this);
    rootLayout->addWidget(m_impl->verticalHairline);

    // -------------------------------------------------------------------------
    // Right content area: QStackedWidget hosting 11 full-height QScrollAreas
    // -------------------------------------------------------------------------
    m_impl->stack = new QStackedWidget(this);
    m_impl->stack->setObjectName(QStringLiteral("spellSettingsPaneStack"));
    m_impl->stack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto makePanePage = [this](SettingsPaneId pane, auto&& createPane) {
        const int idx = static_cast<int>(pane);
        const QString key = paneObjectKey(pane);

        auto* scroll = new QScrollArea(m_impl->stack);
        scroll->setObjectName(QStringLiteral("spellSettingsScrollArea_") + key);
        scroll->setProperty("spellSettingsScrollArea", true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        if (QWidget* vp = scroll->viewport()) {
            vp->setObjectName(QStringLiteral("spellSettingsScrollViewport_") + key);
            vp->setProperty("spellSettingsScrollViewport", true);
            vp->setAttribute(Qt::WA_StyledBackground, true);
        }

        auto* pageContainer = new QWidget(scroll);
        pageContainer->setObjectName(QStringLiteral("spellSettingsScrollContent_") + key);
        pageContainer->setProperty("spellSettingsScrollContent", true);
        pageContainer->setAttribute(Qt::WA_StyledBackground, true);
        pageContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        // Keep effective page margins at (24, 20, 24, 24) while placing the top 20px
        // inside titleStrip so grabbing anywhere along the top header strip drags smoothly.
        auto* pageLayout = new QVBoxLayout(pageContainer);
        pageLayout->setContentsMargins(24, 0, 24, 24);
        pageLayout->setSpacing(14);

        auto* titleStrip = new QWidget(pageContainer);
        titleStrip->setObjectName(QStringLiteral("spellSettingsTitleStrip_") + key);
        titleStrip->setProperty("spellSettingsTitleStrip", true);
        titleStrip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        auto* titleLayout = new QHBoxLayout(titleStrip);
        titleLayout->setContentsMargins(0, 20, 0, 0);
        titleLayout->setSpacing(0);

        auto* titleLabel = new QLabel(settingsPaneTitle(pane), titleStrip);
        titleLabel->setObjectName(QStringLiteral("spellSettingsHeaderTitle_") + key);
        titleLabel->setProperty("spellSettingsHeaderTitle", true);
        titleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
        QFont titleFont = titleLabel->font();
        titleFont.setPointSizeF(18.0);
        titleFont.setWeight(QFont::Bold);
        titleLabel->setFont(titleFont);

        titleLayout->addWidget(titleLabel, 0, Qt::AlignVCenter);
        titleLayout->addStretch(1);

        installWindowDragRegion(titleStrip);
        installWindowDragRegion(titleLabel);

        pageLayout->addWidget(titleStrip);

        auto* paneWidget = createPane(pageContainer);
        paneWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        pageLayout->addWidget(paneWidget);
        pageLayout->addStretch(1);

        scroll->setWidget(pageContainer);

        m_impl->paneScrollAreas[static_cast<std::size_t>(idx)] = scroll;
        m_impl->paneTitleLabels[static_cast<std::size_t>(idx)] = titleLabel;
        m_impl->stack->addWidget(scroll);
        return paneWidget;
    };

    // Instantiate and add all 11 panes strictly in SettingsPaneId order (0..10)
    m_impl->generalPane = makePanePage(SettingsPaneId::General, [](QWidget* pageParent) {
        return new GeneralSettingsPane(pageParent);
    });
    m_impl->enginePane = makePanePage(SettingsPaneId::Engine, [client, store](QWidget* pageParent) {
        return new EngineSettingsPane(client, store, pageParent);
    });
    m_impl->statisticsPane = makePanePage(SettingsPaneId::Statistics, [client](QWidget* pageParent) {
        return new StatisticsSettingsPane(client, pageParent);
    });
    m_impl->subagentsPane = makePanePage(SettingsPaneId::Subagents, [client, store](QWidget* pageParent) {
        return new SubagentsSettingsPane(client, store, pageParent);
    });
    m_impl->conversationPane = makePanePage(SettingsPaneId::Conversation, [client](QWidget* pageParent) {
        return new ConversationSettingsPane(client, pageParent);
    });
    m_impl->agentPane = makePanePage(SettingsPaneId::Agent, [client](QWidget* pageParent) {
        return new AgentSettingsPane(client, pageParent);
    });
    m_impl->onlineResearchPane = makePanePage(
        SettingsPaneId::OnlineResearch, [client, store](QWidget* pageParent) {
            return new OnlineResearchSettingsPane(client, store, pageParent);
        });
    m_impl->timersPane = makePanePage(SettingsPaneId::Timers, [client](QWidget* pageParent) {
        return new TimersSettingsPane(client, pageParent);
    });
    m_impl->terminalPane = makePanePage(SettingsPaneId::Terminal, [client](QWidget* pageParent) {
        return new TerminalSettingsPane(client, pageParent);
    });
    m_impl->diagnosticsPane = makePanePage(
        SettingsPaneId::Diagnostics, [client, store](QWidget* pageParent) {
            return new DiagnosticsSettingsPane(client, store, pageParent);
        });
    m_impl->aboutPane = makePanePage(SettingsPaneId::About, [](QWidget* pageParent) {
        return new AboutSettingsPane(pageParent);
    });

    rootLayout->addWidget(m_impl->stack, 1);

    // -------------------------------------------------------------------------
    // Signal wiring
    // -------------------------------------------------------------------------
    connect(m_impl->navGroup, &QButtonGroup::idClicked, this, [this](int id) {
        selectPaneIndex(id);
    });

    connect(m_impl->saveCloseButton, &QPushButton::clicked, this, [this]() {
        accept();
    });

    connect(this, &QDialog::finished, this, [this](int) {
        if (m_impl->store) {
            m_impl->store->sync();
        }
    });

    connect(m_impl->agentPane, &AgentSettingsPane::openOnlineResearchRequested, this, [this]() {
        selectPane(SettingsPaneId::OnlineResearch);
    });

    connect(m_impl->terminalPane, &TerminalSettingsPane::toggleDockedRequested,
            this, &SettingsDialog::toggleDockedTerminalRequested);
    connect(m_impl->terminalPane, &TerminalSettingsPane::togglePopOutRequested,
            this, &SettingsDialog::togglePopOutTerminalRequested);
    connect(m_impl->terminalPane, &TerminalSettingsPane::clearTerminalRequested,
            this, &SettingsDialog::clearTerminalRequested);
    connect(m_impl->terminalPane, &TerminalSettingsPane::restartTerminalRequested,
            this, &SettingsDialog::restartTerminalRequested);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });

    connect(&ThemeManager::instance(), &ThemeManager::terminalThemeChanged, this, [this]() {
        if (m_impl->applyingTheme) {
            return;
        }
        if (m_impl->generalPane) {
            m_impl->generalPane->applyTheme();
        }
        if (m_impl->terminalPane) {
            m_impl->terminalPane->applyTheme();
        }
    });

    if (client) {
        connect(client, &SpellCoreClient::snapshotUpdated, this,
                [this](const WorkspaceSnapshot& snap) {
                    if (m_impl->hasSnapshot && m_impl->lastSnapshot == snap) {
                        return;
                    }
                    updateSnapshot(snap);
                });

        connect(client, &SpellCoreClient::modelsFetched, this,
                [this](const QVector<ModelDescriptor>& models, const QString& error) {
                    if (m_impl->enginePane) {
                        m_impl->enginePane->onModelsFetched(models, error);
                    }
                });

        connect(client, &SpellCoreClient::subagentSlotReply, this,
                [this](int slot, const QString& reply, const QString& error) {
                    if (m_impl->subagentsPane) {
                        m_impl->subagentsPane->onSlotReply(slot, reply, error);
                    }
                });

        connect(client, &SpellCoreClient::subagentSlotLines, this,
                [this](int slot, const QStringList& lines) {
                    if (m_impl->subagentsPane) {
                        m_impl->subagentsPane->onSlotLines(slot, lines);
                    }
                });
    }

    // Initial state synchronization
    if (store) {
        m_impl->terminalDocked = store->terminalDocked();
        m_impl->terminalPane->setTerminalState(m_impl->terminalDocked, m_impl->terminalPoppedOut);
    }

    selectPane(parseLaunchPaneArgument());
    applyTheme();
    m_impl->initialized = true;

    if (client) {
        updateSnapshot(client->currentSnapshot());
        m_impl->requestSystemInfo(this);
    }
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::selectPane(SettingsPaneId pane) {
    selectPaneIndex(static_cast<int>(pane));
}

void SettingsDialog::selectPaneIndex(int idx) {
    const int clamped = std::clamp(idx, 0, kSettingsPaneCount - 1);
    const bool paneChanged = (clamped != m_impl->currentIndex);
    m_impl->currentIndex = clamped;

    if (QPushButton* btn = m_impl->navButtons[static_cast<std::size_t>(clamped)]) {
        btn->setChecked(true);
    }

    if (m_impl->stack) {
        m_impl->stack->setCurrentIndex(clamped);
    }

    const UiPalette u = ThemeManager::instance().ui();
    m_impl->updateSidebarIcons(u);

    if (QScrollArea* scroll = m_impl->paneScrollAreas[static_cast<std::size_t>(clamped)]) {
        if (QScrollBar* vbar = scroll->verticalScrollBar()) {
            vbar->setValue(0);
        }
    }

    if (m_impl->initialized && paneChanged) {
        const auto pane = static_cast<SettingsPaneId>(clamped);
        if (pane == SettingsPaneId::Statistics || pane == SettingsPaneId::Diagnostics) {
            m_impl->requestSystemInfo(this);
        }
    }
}

SettingsPaneId SettingsDialog::currentPane() const {
    return static_cast<SettingsPaneId>(currentPaneIndex());
}

int SettingsDialog::currentPaneIndex() const {
    return m_impl->currentIndex;
}

void SettingsDialog::updateSnapshot(const WorkspaceSnapshot& snap) {
    m_impl->lastSnapshot = snap;
    m_impl->hasSnapshot = true;

    if (m_impl->enginePane) {
        m_impl->enginePane->updateSnapshot(snap);
    }
    if (m_impl->statisticsPane) {
        m_impl->statisticsPane->updateSnapshot(snap);
    }
    if (m_impl->subagentsPane) {
        m_impl->subagentsPane->updateSnapshot(snap);
    }
    if (m_impl->conversationPane) {
        m_impl->conversationPane->updateSnapshot(snap);
    }
    if (m_impl->agentPane) {
        m_impl->agentPane->updateSnapshot(snap);
    }
    if (m_impl->onlineResearchPane) {
        m_impl->onlineResearchPane->updateSnapshot(snap);
    }
    if (m_impl->timersPane) {
        m_impl->timersPane->updateSnapshot(snap);
    }
    if (m_impl->terminalPane) {
        m_impl->terminalPane->updateSnapshot(snap);
    }
    if (m_impl->diagnosticsPane) {
        m_impl->diagnosticsPane->updateSnapshot(snap);
    }
}

void SettingsDialog::updateSystemInfo(const SystemInfo& info) {
    m_impl->lastSystemInfo = info;
    m_impl->hasSystemInfo = true;

    if (m_impl->statisticsPane) {
        m_impl->statisticsPane->updateSystemInfo(info);
    }
    if (m_impl->diagnosticsPane) {
        m_impl->diagnosticsPane->updateSystemInfo(info);
    }
}

void SettingsDialog::setTerminalState(bool docked, bool poppedOut) {
    m_impl->terminalDocked = docked;
    m_impl->terminalPoppedOut = poppedOut;

    if (m_impl->terminalPane) {
        m_impl->terminalPane->setTerminalState(docked, poppedOut);
    }
}

void SettingsDialog::applyTheme() {
    if (m_impl->applyingTheme) {
        return;
    }
    m_impl->applyingTheme = true;

    const UiPalette u = ThemeManager::instance().ui();
    const QColor solidElevated  = compositeOver(u.elevatedSurface, u.background);
    const QColor solidSelected  = compositeOver(u.selectedFill, u.background);
    const QColor solidBorder    = compositeOver(u.subtleBorder, u.background);
    const QColor solidSecondary = compositeOver(u.textSecondary, u.background);

    const QString qss = QStringLiteral(
        "QDialog#spellSettingsDialog {\n"
        "  background-color: %1;\n"
        "  color: %2;\n"
        "  border: 1px solid %6;\n"
        "}\n"
        "QWidget#spellSettingsSidebar,\n"
        "QWidget#spellSettingsNavContainer,\n"
        "QWidget#spellSettingsFooter {\n"
        "  background-color: %1;\n"
        "}\n"
        "QWidget#spellSettingsSidebar QPushButton[spellNavItem=\"true\"] {\n"
        "  background-color: transparent;\n"
        "  color: %3;\n"
        "  border: 1px solid transparent;\n"
        "  border-radius: 6px;\n"
        "  padding: 6px 10px;\n"
        "  text-align: left;\n"
        "  font-size: 13px;\n"
        "  font-weight: 400;\n"
        "  outline: none;\n"
        "}\n"
        "QWidget#spellSettingsSidebar QPushButton[spellNavItem=\"true\"]:hover {\n"
        "  background-color: %4;\n"
        "  border: 1px solid transparent;\n"
        "  color: %2;\n"
        "}\n"
        "QWidget#spellSettingsSidebar QPushButton[spellNavItem=\"true\"]:pressed {\n"
        "  background-color: %5;\n"
        "  border: 1px solid transparent;\n"
        "  color: %2;\n"
        "}\n"
        "QWidget#spellSettingsSidebar QPushButton[spellNavItem=\"true\"]:checked {\n"
        "  background-color: %5;\n"
        "  border: 1px solid transparent;\n"
        "  color: %2;\n"
        "  font-weight: 600;\n"
        "}\n"
        "QWidget#spellSettingsSidebar QPushButton[spellNavItem=\"true\"]:checked:hover {\n"
        "  background-color: %5;\n"
        "  border: 1px solid transparent;\n"
        "  color: %2;\n"
        "}\n"
        "QPushButton#spellSettingsSaveCloseButton {\n"
        "  background-color: transparent;\n"
        "  border: none;\n"
        "  border-radius: 6px;\n"
        "  padding: 0px;\n"
        "  outline: none;\n"
        "}\n"
        "QPushButton#spellSettingsSaveCloseButton:hover {\n"
        "  background-color: %4;\n"
        "  border: none;\n"
        "}\n"
        "QPushButton#spellSettingsSaveCloseButton:pressed {\n"
        "  background-color: %5;\n"
        "  border: none;\n"
        "}\n"
        "QStackedWidget#spellSettingsPaneStack {\n"
        "  background-color: %1;\n"
        "  border: none;\n"
        "}\n"
        "QScrollArea[spellSettingsScrollArea=\"true\"] {\n"
        "  background-color: %1;\n"
        "  border: none;\n"
        "}\n"
        "QWidget[spellSettingsScrollViewport=\"true\"],\n"
        "QWidget[spellSettingsScrollContent=\"true\"] {\n"
        "  background-color: %1;\n"
        "}\n"
        "QWidget[spellSettingsTitleStrip=\"true\"] {\n"
        "  background-color: transparent;\n"
        "}\n"
        "QLabel[spellSettingsHeaderTitle=\"true\"] {\n"
        "  background-color: transparent;\n"
        "  color: %2;\n"
        "  font-size: 18pt;\n"
        "  font-weight: 700;\n"
        "}\n"
        "QLabel#spellSettingsHatMark {\n"
        "  background-color: transparent;\n"
        "}\n")
        .arg(qssColor(u.background),
             qssColor(u.textPrimary),
             qssColor(solidSecondary),
             qssColor(solidElevated),
             qssColor(solidSelected),
             qssColor(solidBorder));

    setStyleSheet(qss);

    m_impl->updateSidebarIcons(u);

    for (QLabel* titleLabel : m_impl->paneTitleLabels) {
        if (!titleLabel) {
            continue;
        }
        QPalette pal = titleLabel->palette();
        pal.setColor(QPalette::WindowText, u.textPrimary);
        titleLabel->setPalette(pal);
    }

    if (m_impl->hatMarkLabel) {
        m_impl->hatMarkLabel->setPixmap(icons::hatMarkPixmap(kHatMarkSizePx, u.textPrimary));
    }
    if (m_impl->saveCloseButton) {
        m_impl->saveCloseButton->setIcon(
            icons::vectorIcon(QStringLiteral("arrow-left"), u.textPrimary, kSaveCloseIconSizePx));
    }

    if (m_impl->footerHairline) {
        m_impl->footerHairline->applyTheme();
    }
    if (m_impl->verticalHairline) {
        m_impl->verticalHairline->applyTheme();
    }

    if (m_impl->generalPane) {
        m_impl->generalPane->applyTheme();
    }
    if (m_impl->enginePane) {
        m_impl->enginePane->applyTheme();
    }
    if (m_impl->statisticsPane) {
        m_impl->statisticsPane->applyTheme();
    }
    if (m_impl->subagentsPane) {
        m_impl->subagentsPane->applyTheme();
    }
    if (m_impl->conversationPane) {
        m_impl->conversationPane->applyTheme();
    }
    if (m_impl->agentPane) {
        m_impl->agentPane->applyTheme();
    }
    if (m_impl->onlineResearchPane) {
        m_impl->onlineResearchPane->applyTheme();
    }
    if (m_impl->timersPane) {
        m_impl->timersPane->applyTheme();
    }
    if (m_impl->terminalPane) {
        m_impl->terminalPane->applyTheme();
    }
    if (m_impl->diagnosticsPane) {
        m_impl->diagnosticsPane->applyTheme();
    }
    if (m_impl->aboutPane) {
        m_impl->aboutPane->applyTheme();
    }

    m_impl->applyingTheme = false;
}

} // namespace spell
