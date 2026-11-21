#include "settings/OnlineResearchSettingsPane.h"

#include "core/SpellCoreClient.h"
#include "core/SpellSettingsStore.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace spell {

namespace {

constexpr int kModeOfflineId = 0;
constexpr int kModeCuratedId = 1;
constexpr int kModeOpenWebId = 2;

constexpr int kMinFetchesPerTurn = 1;
constexpr int kMaxFetchesPerTurn = 50;
constexpr int kDefaultFetchesPerTurn = 6;

constexpr int kMinTimeoutSecs = 5;
constexpr int kMaxTimeoutSecs = 120;
constexpr int kDefaultTimeoutSecs = 20;

enum class SecretSlot : std::size_t {
    GitHub = 0,
    HuggingFace = 1,
    OpenAlex = 2,
    SearchApi = 3,
    Count = 4
};

struct SearchProviderSpec {
    const char* wireKey;
    const char* displayName;
    const char* defaultEndpoint;
};

constexpr std::array<SearchProviderSpec, 6> kSearchProviders = {{
    {"none", "None (Zero-Key Tier 1 APIs)", ""},
    {"brave", "Brave Search", "https://api.search.brave.com/res/v1/web/search"},
    {"exa", "Exa Neural Search", "https://api.exa.ai/search"},
    {"tavily", "Tavily", "https://api.tavily.com/search"},
    {"kagi", "Kagi Search", "https://kagi.com/api/v0/search"},
    {"custom", "Custom Endpoint", "https://search.example.com/v1/search"},
}};

struct CuratedGroupInfo {
    const char* title;
    const char* hostsSummary;
};

constexpr std::array<CuratedGroupInfo, 3> kCuratedGroups = {{
    {"Scholarly",
     "arxiv.org · ar5iv.labs.arxiv.org · openalex.org · semanticscholar.org · "
     "openreview.net · aclanthology.org · proceedings.mlr.press · doi.org"},
    {"Code & Model Hubs",
     "github.com · gitlab.com · codeberg.org · huggingface.co"},
    {"Docs & Packages",
     "crates.io · docs.rs · pypi.org · deps.dev · developer.apple.com · stackoverflow.com"},
}};

QString modeToBadgeText(const QString& mode) {
    if (mode == QLatin1StringView("curated")) {
        return QStringLiteral("CURATED ALLOWLIST");
    }
    if (mode == QLatin1StringView("open_web")) {
        return QStringLiteral("OPEN WEB");
    }
    return QStringLiteral("AIR-GAPPED");
}

BadgeTone modeToBadgeTone(const QString& mode) {
    if (mode == QLatin1StringView("curated")) {
        return BadgeTone::Success;
    }
    if (mode == QLatin1StringView("open_web")) {
        return BadgeTone::Warning;
    }
    return BadgeTone::Neutral;
}

QString modeToDescription(const QString& mode) {
    if (mode == QLatin1StringView("curated")) {
        return QStringLiteral(
            "Allows vetted HTTPS:443 academic, code repository, and package documentation "
            "hosts plus your custom allowlist, with DNS-pinning anti-SSRF.");
    }
    if (mode == QLatin1StringView("open_web")) {
        return QStringLiteral(
            "Allows public HTTPS:443 domains not listed in the denylist. Loopback, RFC1918 "
            "private IPs, anti-SSRF DNS pinning, and anti-bot guards still apply.");
    }
    return QStringLiteral(
        "Zero outbound DNS or TCP sockets; all online research tools fail closed before "
        "name resolution.");
}

int modeToRadioId(const QString& mode) {
    if (mode == QLatin1StringView("curated")) {
        return kModeCuratedId;
    }
    if (mode == QLatin1StringView("open_web")) {
        return kModeOpenWebId;
    }
    return kModeOfflineId;
}

QString radioIdToMode(int id) {
    switch (id) {
    case kModeCuratedId:
        return QStringLiteral("curated");
    case kModeOpenWebId:
        return QStringLiteral("open_web");
    case kModeOfflineId:
    default:
        return QStringLiteral("offline");
    }
}

QString normalizeSearchProviderKey(const QString& raw) {
    const QString cleaned = raw.trimmed().toLower();
    if (cleaned.isEmpty()) {
        return QStringLiteral("none");
    }
    return cleaned;
}

QString searchProviderDisplayName(const QString& providerKey) {
    for (const auto& spec : kSearchProviders) {
        if (providerKey == QLatin1StringView(spec.wireKey)) {
            return QString::fromLatin1(spec.displayName);
        }
    }
    return providerKey;
}

QString searchProviderEndpointPlaceholder(const QString& providerKey) {
    for (const auto& spec : kSearchProviders) {
        if (providerKey == QLatin1StringView(spec.wireKey)) {
            if (spec.defaultEndpoint[0] == '\0') {
                return QStringLiteral("Default Tier 1 academic & registry APIs (no search endpoint)");
            }
            return QString::fromLatin1(spec.defaultEndpoint);
        }
    }
    return QStringLiteral("https://search.example.com/v1/search");
}

QString searchKeyPlaceholder(const QString& providerKey) {
    if (providerKey == QLatin1StringView("none")) {
        return QStringLiteral("Select Brave / Exa / Tavily / Kagi / Custom");
    }
    return QStringLiteral("%1 API key…").arg(searchProviderDisplayName(providerKey));
}

/**
 * Normalizes a user-supplied domain into a strict lowercase public FQDN.
 * Rejects wildcards, raw IPv4/IPv6 literals, userinfo, non-443 ports,
 * single-label hostnames, and local/reserved TLDs.
 */
std::optional<QString> normalizePublicDomain(const QString& raw) {
    QString candidate = raw.trimmed().toLower();
    if (candidate.isEmpty()) {
        return std::nullopt;
    }

    if (candidate.startsWith(QLatin1StringView("https://"))) {
        candidate.remove(0, 8);
    } else if (candidate.startsWith(QLatin1StringView("http://"))) {
        candidate.remove(0, 7);
    }

    for (qsizetype i = 0; i < candidate.size(); ++i) {
        const QChar ch = candidate.at(i);
        if (ch == QLatin1Char('/') || ch == QLatin1Char('?') || ch == QLatin1Char('#')) {
            candidate.truncate(i);
            break;
        }
    }

    if (candidate.contains(QLatin1Char('@')) ||
        candidate.contains(QLatin1Char('[')) ||
        candidate.contains(QLatin1Char(']'))) {
        return std::nullopt;
    }

    const qsizetype colonIdx = candidate.indexOf(QLatin1Char(':'));
    if (colonIdx >= 0) {
        const QString portPart = candidate.mid(colonIdx + 1);
        if (portPart.contains(QLatin1Char(':')) ||
            (!portPart.isEmpty() && portPart != QLatin1StringView("443"))) {
            return std::nullopt;
        }
        candidate.truncate(colonIdx);
    }

    while (candidate.endsWith(QLatin1Char('.'))) {
        candidate.chop(1);
    }

    if (candidate.isEmpty() || candidate.size() > 253 ||
        candidate.contains(QLatin1Char('*')) || !candidate.contains(QLatin1Char('.'))) {
        return std::nullopt;
    }

    static constexpr std::array<QLatin1StringView, 7> kForbiddenSuffixes = {
        QLatin1StringView(".local"),
        QLatin1StringView(".localhost"),
        QLatin1StringView(".internal"),
        QLatin1StringView(".home.arpa"),
        QLatin1StringView(".onion"),
        QLatin1StringView(".invalid"),
        QLatin1StringView(".test"),
    };
    if (candidate == QLatin1StringView("localhost")) {
        return std::nullopt;
    }
    for (const QLatin1StringView suffix : kForbiddenSuffixes) {
        if (candidate.endsWith(suffix)) {
            return std::nullopt;
        }
    }

    const QStringList labels = candidate.split(QLatin1Char('.'), Qt::KeepEmptyParts);
    if (labels.size() < 2) {
        return std::nullopt;
    }

    bool allNumeric = true;
    for (const QString& label : labels) {
        if (label.isEmpty() || label.size() > 63 ||
            label.startsWith(QLatin1Char('-')) || label.endsWith(QLatin1Char('-'))) {
            return std::nullopt;
        }
        for (const QChar ch : label) {
            const ushort u = ch.unicode();
            const bool isLowerAlpha = (u >= 'a' && u <= 'z');
            const bool isDigit = (u >= '0' && u <= '9');
            const bool isHyphen = (u == '-');
            if (!isLowerAlpha && !isDigit && !isHyphen) {
                return std::nullopt;
            }
            if (!isDigit) {
                allNumeric = false;
            }
        }
    }

    if (allNumeric) {
        return std::nullopt;
    }

    return candidate;
}

QStringList normalizeDomainList(const QStringList& rawDomains) {
    QStringList out;
    out.reserve(rawDomains.size());
    for (const QString& raw : rawDomains) {
        if (const auto normalized = normalizePublicDomain(raw)) {
            if (!out.contains(*normalized)) {
                out.append(*normalized);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

QStringList itemsFromListWidget(const QListWidget* list) {
    QStringList out;
    if (!list) {
        return out;
    }
    out.reserve(list->count());
    for (int i = 0; i < list->count(); ++i) {
        if (const QListWidgetItem* item = list->item(i)) {
            out.append(item->text());
        }
    }
    return out;
}

void repolishWidget(QWidget* w) {
    if (!w || !w->style()) {
        return;
    }
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

} // namespace

struct OnlineResearchSettingsPane::Impl {
    struct SecretRowWidgets {
        QString account;
        QString wireKey;
        QString displayTitle;
        QString defaultPlaceholder;
        QLabel* titleLabel = nullptr;
        SpellBadge* statusBadge = nullptr;
        QLineEdit* inputEdit = nullptr;
        QPushButton* saveButton = nullptr;
        QPushButton* clearButton = nullptr;
        bool snapshotHasSecret = false;
        bool fallbackHasSecret = false;
    };

    OnlineResearchSettingsPane* owner = nullptr;
    SpellCoreClient* client = nullptr;
    SpellSettingsStore* store = nullptr;

    OnlineResearchSettings settings;
    bool syncing = false;
    bool curatedHostsExpanded = false;

    // Theme-tracked chrome collections
    QVector<QFrame*> cards;
    QVector<SpellSectionHeader*> sectionHeaders;
    QVector<SpellHairline*> hairlines;
    QVector<QLabel*> secondaryLabels;
    QVector<QLabel*> mutedLabels;
    QVector<QLabel*> monoLabels;
    QVector<QListWidget*> domainLists;

    // Card 1: Network posture & kill-switch
    SpellBadge* postureBadge = nullptr;
    SpellChipButton* airGapButton = nullptr;
    QButtonGroup* modeGroup = nullptr;
    QRadioButton* modeOfflineRadio = nullptr;
    QRadioButton* modeCuratedRadio = nullptr;
    QRadioButton* modeOpenWebRadio = nullptr;
    QLabel* modeDescriptionLabel = nullptr;
    QLabel* postureFootnoteLabel = nullptr;

    // Card 2: Domain allowlist & denylist + built-in curated reference
    QToolButton* curatedToggleButton = nullptr;
    QWidget* curatedDetailsWidget = nullptr;

    SpellBadge* allowlistCountBadge = nullptr;
    QLineEdit* allowlistInput = nullptr;
    QPushButton* allowlistAddButton = nullptr;
    QListWidget* allowlistList = nullptr;
    QPushButton* allowlistRemoveButton = nullptr;
    QPushButton* allowlistClearButton = nullptr;

    SpellBadge* denylistCountBadge = nullptr;
    QLineEdit* denylistInput = nullptr;
    QPushButton* denylistAddButton = nullptr;
    QListWidget* denylistList = nullptr;
    QPushButton* denylistRemoveButton = nullptr;
    QPushButton* denylistClearButton = nullptr;

    QLabel* domainFeedbackLabel = nullptr;
    bool domainFeedbackIsError = false;

    // Card 3: BYOK search provider, endpoint & fetch limits
    QComboBox* searchProviderCombo = nullptr;
    QLineEdit* searchEndpointEdit = nullptr;
    QSpinBox* maxFetchesSpin = nullptr;
    QSpinBox* timeoutSecsSpin = nullptr;

    // Card 4: OS keyring credentials
    std::array<SecretRowWidgets, static_cast<std::size_t>(SecretSlot::Count)> secretRows;
    QLabel* keyringStatusLabel = nullptr;
    bool keyringStatusIsError = false;

    QFrame* createCard(QVBoxLayout* parentLayout,
                       const QString& title,
                       const QString& iconName,
                       QVBoxLayout*& outCardLayout) {
        auto* header = new SpellSectionHeader(title, iconName, BadgeTone::Accent, owner);
        parentLayout->addWidget(header);
        sectionHeaders.append(header);

        auto* card = new QFrame(owner);
        card->setObjectName(QStringLiteral("spellSettingsCard"));
        card->setFrameShape(QFrame::NoFrame);
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        outCardLayout = new QVBoxLayout(card);
        outCardLayout->setContentsMargins(14, 12, 14, 12);
        outCardLayout->setSpacing(10);

        parentLayout->addWidget(card);
        cards.append(card);
        return card;
    }

    QLabel* createSecondaryLabel(const QString& text, QWidget* parent) {
        auto* lbl = new QLabel(text, parent);
        lbl->setWordWrap(true);
        secondaryLabels.append(lbl);
        return lbl;
    }

    QLabel* createMutedLabel(const QString& text, QWidget* parent) {
        auto* lbl = new QLabel(text, parent);
        lbl->setWordWrap(true);
        mutedLabels.append(lbl);
        return lbl;
    }

    SpellHairline* createHairline(QWidget* parent) {
        auto* line = new SpellHairline(Qt::Horizontal, parent);
        hairlines.append(line);
        return line;
    }

    void setupUi() {
        owner->setObjectName(QStringLiteral("onlineResearchPane"));
        owner->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        auto* root = new QVBoxLayout(owner);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(12);

        setupPostureCard(root);
        setupDomainRulesCard(root);
        setupSearchAndLimitsCard(root);
        setupKeyringCard(root);

        root->addStretch(1);
    }

    void setupPostureCard(QVBoxLayout* root) {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(
            root,
            QStringLiteral("Network Posture & Kill-Switch"),
            QStringLiteral("globe"),
            cardLayout);

        auto* topRow = new QHBoxLayout();
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(8);

        postureBadge = new SpellBadge(
            modeToBadgeText(settings.mode),
            modeToBadgeTone(settings.mode),
            true,
            card);
        postureBadge->setObjectName(QStringLiteral("postureBadge"));
        topRow->addWidget(postureBadge);
        topRow->addStretch(1);

        airGapButton = new SpellChipButton(
            QStringLiteral("Air-Gap Now"),
            QStringLiteral("lock"),
            BadgeTone::Neutral,
            card);
        airGapButton->setObjectName(QStringLiteral("airGapNowButton"));
        airGapButton->setCheckable(false);
        airGapButton->setAutoDefault(false);
        airGapButton->setDefault(false);
        airGapButton->setToolTip(QStringLiteral(
            "Immediately switches to Offline (Air-Gapped), clears session domain approvals, "
            "and blocks all outbound sockets."));
        QObject::connect(airGapButton, &QPushButton::clicked, owner, [this]() {
            triggerAirGapNow();
        });
        topRow->addWidget(airGapButton);
        cardLayout->addLayout(topRow);

        cardLayout->addWidget(createHairline(card));

        modeGroup = new QButtonGroup(owner);
        modeGroup->setExclusive(true);

        modeOfflineRadio = new QRadioButton(QStringLiteral("Offline (Air-Gapped)"), card);
        modeOfflineRadio->setObjectName(QStringLiteral("modeOfflineRadio"));
        modeCuratedRadio = new QRadioButton(QStringLiteral("Curated Allowlist"), card);
        modeCuratedRadio->setObjectName(QStringLiteral("modeCuratedRadio"));
        modeOpenWebRadio = new QRadioButton(QStringLiteral("Open Web"), card);
        modeOpenWebRadio->setObjectName(QStringLiteral("modeOpenWebRadio"));

        modeGroup->addButton(modeOfflineRadio, kModeOfflineId);
        modeGroup->addButton(modeCuratedRadio, kModeCuratedId);
        modeGroup->addButton(modeOpenWebRadio, kModeOpenWebId);
        modeOfflineRadio->setChecked(true);

        auto* radioLayout = new QVBoxLayout();
        radioLayout->setContentsMargins(0, 2, 0, 2);
        radioLayout->setSpacing(6);
        radioLayout->addWidget(modeOfflineRadio);
        radioLayout->addWidget(modeCuratedRadio);
        radioLayout->addWidget(modeOpenWebRadio);
        cardLayout->addLayout(radioLayout);

        QObject::connect(modeGroup, &QButtonGroup::idClicked, owner, [this](int id) {
            if (syncing) {
                return;
            }
            const QString nextMode = radioIdToMode(id);
            if (settings.mode == nextMode) {
                return;
            }
            settings.mode = nextMode;
            refreshModeWidgets();
            commitSettings();
        });

        modeDescriptionLabel = createSecondaryLabel(modeToDescription(settings.mode), card);
        cardLayout->addWidget(modeDescriptionLabel);

        postureFootnoteLabel = createMutedLabel(
            QStringLiteral("Outbound research enforces HTTPS:443, DNS-pinning anti-SSRF, "
                           "untrusted content provenance tags, and strict local endpoint isolation."),
            card);
        cardLayout->addWidget(postureFootnoteLabel);
    }

    void setupDomainRulesCard(QVBoxLayout* root) {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(
            root,
            QStringLiteral("Domain Allowlist & Denylist"),
            QStringLiteral("shield"),
            cardLayout);

        // Collapsible reference listing built-in vetted hosts for Curated Allowlist mode
        curatedToggleButton = new QToolButton(card);
        curatedToggleButton->setObjectName(QStringLiteral("curatedHostsToggleButton"));
        curatedToggleButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        curatedToggleButton->setText(QStringLiteral("Built-in curated hosts (18 vetted domains)"));
        curatedToggleButton->setAutoRaise(true);
        curatedToggleButton->setCursor(Qt::PointingHandCursor);
        QObject::connect(curatedToggleButton, &QToolButton::clicked, owner, [this]() {
            curatedHostsExpanded = !curatedHostsExpanded;
            curatedDetailsWidget->setVisible(curatedHostsExpanded);
            updateCuratedToggleIcon();
        });
        cardLayout->addWidget(curatedToggleButton, 0, Qt::AlignLeft);

        curatedDetailsWidget = new QWidget(card);
        curatedDetailsWidget->setVisible(false);
        auto* curatedLayout = new QVBoxLayout(curatedDetailsWidget);
        curatedLayout->setContentsMargins(18, 2, 0, 4);
        curatedLayout->setSpacing(6);
        for (const auto& group : kCuratedGroups) {
            auto* titleLbl = createSecondaryLabel(QString::fromLatin1(group.title), curatedDetailsWidget);
            QFont boldFont = titleLbl->font();
            boldFont.setWeight(QFont::DemiBold);
            titleLbl->setFont(boldFont);
            curatedLayout->addWidget(titleLbl);

            auto* hostsLbl = createMutedLabel(QString::fromLatin1(group.hostsSummary), curatedDetailsWidget);
            monoLabels.append(hostsLbl);
            curatedLayout->addWidget(hostsLbl);
        }
        cardLayout->addWidget(curatedDetailsWidget);

        cardLayout->addWidget(createHairline(card));

        // --- Allowlist editor ---
        auto* allowHeader = new QHBoxLayout();
        allowHeader->setContentsMargins(0, 0, 0, 0);
        allowHeader->setSpacing(8);
        auto* allowTitle = createSecondaryLabel(QStringLiteral("Custom Allowed Domains"), card);
        QFont subTitleFont = allowTitle->font();
        subTitleFont.setWeight(QFont::DemiBold);
        allowTitle->setFont(subTitleFont);
        allowHeader->addWidget(allowTitle);
        allowlistCountBadge = new SpellBadge(QStringLiteral("0"), BadgeTone::Neutral, false, card);
        allowHeader->addWidget(allowlistCountBadge);
        allowHeader->addStretch(1);
        cardLayout->addLayout(allowHeader);

        auto* allowInputRow = new QHBoxLayout();
        allowInputRow->setContentsMargins(0, 0, 0, 0);
        allowInputRow->setSpacing(6);
        allowlistInput = new QLineEdit(card);
        allowlistInput->setObjectName(QStringLiteral("allowlistInput"));
        allowlistInput->setPlaceholderText(QStringLiteral("Add public domain (e.g. pytorch.org)"));
        allowInputRow->addWidget(allowlistInput, 1);

        allowlistAddButton = new QPushButton(QStringLiteral("Add"), card);
        allowlistAddButton->setObjectName(QStringLiteral("allowlistAddButton"));
        allowlistAddButton->setProperty("primary", true);
        allowlistAddButton->setAutoDefault(false);
        allowlistAddButton->setDefault(false);
        allowlistAddButton->setEnabled(false);
        allowInputRow->addWidget(allowlistAddButton);
        cardLayout->addLayout(allowInputRow);

        allowlistList = new QListWidget(card);
        allowlistList->setObjectName(QStringLiteral("allowlistList"));
        allowlistList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        allowlistList->setAlternatingRowColors(false);
        allowlistList->setUniformItemSizes(true);
        allowlistList->setMinimumHeight(82);
        allowlistList->setMaximumHeight(124);
        domainLists.append(allowlistList);
        cardLayout->addWidget(allowlistList);

        auto* allowActionRow = new QHBoxLayout();
        allowActionRow->setContentsMargins(0, 0, 0, 0);
        allowActionRow->setSpacing(6);
        allowActionRow->addStretch(1);
        allowlistRemoveButton = new QPushButton(QStringLiteral("Remove Selected"), card);
        allowlistRemoveButton->setObjectName(QStringLiteral("allowlistRemoveButton"));
        allowlistRemoveButton->setAutoDefault(false);
        allowlistRemoveButton->setDefault(false);
        allowlistRemoveButton->setEnabled(false);
        allowlistClearButton = new QPushButton(QStringLiteral("Clear All"), card);
        allowlistClearButton->setObjectName(QStringLiteral("allowlistClearButton"));
        allowlistClearButton->setAutoDefault(false);
        allowlistClearButton->setDefault(false);
        allowlistClearButton->setEnabled(false);
        allowActionRow->addWidget(allowlistRemoveButton);
        allowActionRow->addWidget(allowlistClearButton);
        cardLayout->addLayout(allowActionRow);

        cardLayout->addWidget(createHairline(card));

        // --- Denylist editor ---
        auto* denyHeader = new QHBoxLayout();
        denyHeader->setContentsMargins(0, 0, 0, 0);
        denyHeader->setSpacing(8);
        auto* denyTitle = createSecondaryLabel(QStringLiteral("Blocked Domains (Denylist)"), card);
        denyTitle->setFont(subTitleFont);
        denyHeader->addWidget(denyTitle);
        denylistCountBadge = new SpellBadge(QStringLiteral("0"), BadgeTone::Neutral, false, card);
        denyHeader->addWidget(denylistCountBadge);
        denyHeader->addStretch(1);
        cardLayout->addLayout(denyHeader);

        auto* denyInputRow = new QHBoxLayout();
        denyInputRow->setContentsMargins(0, 0, 0, 0);
        denyInputRow->setSpacing(6);
        denylistInput = new QLineEdit(card);
        denylistInput->setObjectName(QStringLiteral("denylistInput"));
        denylistInput->setPlaceholderText(QStringLiteral("Block public domain (e.g. example.com)"));
        denyInputRow->addWidget(denylistInput, 1);

        denylistAddButton = new QPushButton(QStringLiteral("Block"), card);
        denylistAddButton->setObjectName(QStringLiteral("denylistAddButton"));
        denylistAddButton->setProperty("danger", true);
        denylistAddButton->setAutoDefault(false);
        denylistAddButton->setDefault(false);
        denylistAddButton->setEnabled(false);
        denyInputRow->addWidget(denylistAddButton);
        cardLayout->addLayout(denyInputRow);

        denylistList = new QListWidget(card);
        denylistList->setObjectName(QStringLiteral("denylistList"));
        denylistList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        denylistList->setAlternatingRowColors(false);
        denylistList->setUniformItemSizes(true);
        denylistList->setMinimumHeight(82);
        denylistList->setMaximumHeight(124);
        domainLists.append(denylistList);
        cardLayout->addWidget(denylistList);

        auto* denyActionRow = new QHBoxLayout();
        denyActionRow->setContentsMargins(0, 0, 0, 0);
        denyActionRow->setSpacing(6);
        denyActionRow->addStretch(1);
        denylistRemoveButton = new QPushButton(QStringLiteral("Remove Selected"), card);
        denylistRemoveButton->setObjectName(QStringLiteral("denylistRemoveButton"));
        denylistRemoveButton->setAutoDefault(false);
        denylistRemoveButton->setDefault(false);
        denylistRemoveButton->setEnabled(false);
        denylistClearButton = new QPushButton(QStringLiteral("Clear All"), card);
        denylistClearButton->setObjectName(QStringLiteral("denylistClearButton"));
        denylistClearButton->setAutoDefault(false);
        denylistClearButton->setDefault(false);
        denylistClearButton->setEnabled(false);
        denyActionRow->addWidget(denylistRemoveButton);
        denyActionRow->addWidget(denylistClearButton);
        cardLayout->addLayout(denyActionRow);

        domainFeedbackLabel = new QLabel(card);
        domainFeedbackLabel->setObjectName(QStringLiteral("domainFeedbackLabel"));
        domainFeedbackLabel->setWordWrap(true);
        domainFeedbackLabel->setVisible(false);
        cardLayout->addWidget(domainFeedbackLabel);

        // Wire Allowlist signals
        QObject::connect(allowlistInput, &QLineEdit::textChanged, owner, [this](const QString& text) {
            allowlistAddButton->setEnabled(!text.trimmed().isEmpty());
        });
        QObject::connect(allowlistInput, &QLineEdit::returnPressed, owner, [this]() {
            addDomainToList(true);
        });
        QObject::connect(allowlistAddButton, &QPushButton::clicked, owner, [this]() {
            addDomainToList(true);
        });
        QObject::connect(allowlistList, &QListWidget::itemSelectionChanged, owner, [this]() {
            allowlistRemoveButton->setEnabled(!allowlistList->selectedItems().isEmpty());
        });
        QObject::connect(allowlistRemoveButton, &QPushButton::clicked, owner, [this]() {
            removeSelectedDomains(true);
        });
        QObject::connect(allowlistClearButton, &QPushButton::clicked, owner, [this]() {
            clearDomainList(true);
        });
        auto* allowDeleteShortcut = new QShortcut(QKeySequence::Delete, allowlistList);
        allowDeleteShortcut->setContext(Qt::WidgetShortcut);
        QObject::connect(allowDeleteShortcut, &QShortcut::activated, owner, [this]() {
            removeSelectedDomains(true);
        });
        auto* allowBackspaceShortcut = new QShortcut(QKeySequence(Qt::Key_Backspace), allowlistList);
        allowBackspaceShortcut->setContext(Qt::WidgetShortcut);
        QObject::connect(allowBackspaceShortcut, &QShortcut::activated, owner, [this]() {
            removeSelectedDomains(true);
        });

        // Wire Denylist signals
        QObject::connect(denylistInput, &QLineEdit::textChanged, owner, [this](const QString& text) {
            denylistAddButton->setEnabled(!text.trimmed().isEmpty());
        });
        QObject::connect(denylistInput, &QLineEdit::returnPressed, owner, [this]() {
            addDomainToList(false);
        });
        QObject::connect(denylistAddButton, &QPushButton::clicked, owner, [this]() {
            addDomainToList(false);
        });
        QObject::connect(denylistList, &QListWidget::itemSelectionChanged, owner, [this]() {
            denylistRemoveButton->setEnabled(!denylistList->selectedItems().isEmpty());
        });
        QObject::connect(denylistRemoveButton, &QPushButton::clicked, owner, [this]() {
            removeSelectedDomains(false);
        });
        QObject::connect(denylistClearButton, &QPushButton::clicked, owner, [this]() {
            clearDomainList(false);
        });
        auto* denyDeleteShortcut = new QShortcut(QKeySequence::Delete, denylistList);
        denyDeleteShortcut->setContext(Qt::WidgetShortcut);
        QObject::connect(denyDeleteShortcut, &QShortcut::activated, owner, [this]() {
            removeSelectedDomains(false);
        });
        auto* denyBackspaceShortcut = new QShortcut(QKeySequence(Qt::Key_Backspace), denylistList);
        denyBackspaceShortcut->setContext(Qt::WidgetShortcut);
        QObject::connect(denyBackspaceShortcut, &QShortcut::activated, owner, [this]() {
            removeSelectedDomains(false);
        });
    }

    void setupSearchAndLimitsCard(QVBoxLayout* root) {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(
            root,
            QStringLiteral("BYOK Search Provider & Fetch Limits"),
            QStringLiteral("search"),
            cardLayout);

        auto* grid = new QGridLayout();
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(9);
        grid->setColumnStretch(0, 0);
        grid->setColumnStretch(1, 1);

        // Row 0: Search Provider
        auto* providerLabel = createSecondaryLabel(QStringLiteral("Web search provider"), card);
        searchProviderCombo = new QComboBox(card);
        searchProviderCombo->setObjectName(QStringLiteral("searchProviderCombo"));
        searchProviderCombo->setMinimumWidth(220);
        for (const auto& spec : kSearchProviders) {
            searchProviderCombo->addItem(
                QString::fromLatin1(spec.displayName),
                QString::fromLatin1(spec.wireKey));
        }
        grid->addWidget(providerLabel, 0, 0);
        grid->addWidget(searchProviderCombo, 0, 1);

        // Row 1: Search Endpoint Override
        auto* endpointLabel = createSecondaryLabel(QStringLiteral("Search endpoint"), card);
        searchEndpointEdit = new QLineEdit(card);
        searchEndpointEdit->setObjectName(QStringLiteral("searchEndpointEdit"));
        searchEndpointEdit->setClearButtonEnabled(true);
        searchEndpointEdit->setPlaceholderText(
            searchProviderEndpointPlaceholder(QStringLiteral("none")));
        searchEndpointEdit->setEnabled(false);
        grid->addWidget(endpointLabel, 1, 0);
        grid->addWidget(searchEndpointEdit, 1, 1);

        // Row 2: Max Fetches per Turn
        auto* maxFetchesLabel = createSecondaryLabel(QStringLiteral("Max fetches per turn"), card);
        maxFetchesSpin = new QSpinBox(card);
        maxFetchesSpin->setObjectName(QStringLiteral("maxFetchesSpin"));
        maxFetchesSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        maxFetchesSpin->setRange(kMinFetchesPerTurn, kMaxFetchesPerTurn);
        maxFetchesSpin->setValue(kDefaultFetchesPerTurn);
        maxFetchesSpin->setSuffix(QStringLiteral(" fetches"));
        maxFetchesSpin->setKeyboardTracking(false);
        maxFetchesSpin->setFixedWidth(140);
        grid->addWidget(maxFetchesLabel, 2, 0);
        grid->addWidget(maxFetchesSpin, 2, 1, Qt::AlignLeft);

        // Row 3: Timeout
        auto* timeoutLabel = createSecondaryLabel(QStringLiteral("Request timeout"), card);
        timeoutSecsSpin = new QSpinBox(card);
        timeoutSecsSpin->setObjectName(QStringLiteral("timeoutSecsSpin"));
        timeoutSecsSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        timeoutSecsSpin->setRange(kMinTimeoutSecs, kMaxTimeoutSecs);
        timeoutSecsSpin->setValue(kDefaultTimeoutSecs);
        timeoutSecsSpin->setSuffix(QStringLiteral(" s"));
        timeoutSecsSpin->setKeyboardTracking(false);
        timeoutSecsSpin->setFixedWidth(140);
        grid->addWidget(timeoutLabel, 3, 0);
        grid->addWidget(timeoutSecsSpin, 3, 1, Qt::AlignLeft);

        cardLayout->addLayout(grid);

        auto* note = createMutedLabel(
            QStringLiteral("Leave Search endpoint blank to use the selected provider's default "
                           "HTTPS endpoint. Zero-key Tier 1 APIs (arXiv, OpenAlex, Crossref, "
                           "crates.io, PyPI) work without a search API key."),
            card);
        cardLayout->addWidget(note);

        QObject::connect(searchProviderCombo, &QComboBox::currentIndexChanged, owner, [this](int idx) {
            if (syncing || idx < 0) {
                return;
            }
            const QString providerKey = normalizeSearchProviderKey(
                searchProviderCombo->itemData(idx).toString());
            if (settings.searchProvider == providerKey) {
                return;
            }
            settings.searchProvider = providerKey;
            refreshSearchProviderDependents();
            commitSettings();
        });

        QObject::connect(searchEndpointEdit, &QLineEdit::editingFinished, owner, [this]() {
            if (syncing) {
                return;
            }
            const QString trimmed = searchEndpointEdit->text().trimmed();
            if (trimmed == settings.searchEndpoint) {
                return;
            }
            settings.searchEndpoint = trimmed;
            commitSettings();
        });

        QObject::connect(maxFetchesSpin, &QSpinBox::valueChanged, owner, [this](int val) {
            if (syncing) {
                return;
            }
            if (settings.maxFetchesPerTurn == val) {
                return;
            }
            settings.maxFetchesPerTurn = val;
            commitSettings();
        });

        QObject::connect(timeoutSecsSpin, &QSpinBox::valueChanged, owner, [this](int val) {
            if (syncing) {
                return;
            }
            if (settings.timeoutSecs == val) {
                return;
            }
            settings.timeoutSecs = val;
            commitSettings();
        });
    }

    void setupKeyringCard(QVBoxLayout* root) {
        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCard(
            root,
            QStringLiteral("OS Keyring API Vault"),
            QStringLiteral("lock"),
            cardLayout);

        struct SecretInitSpec {
            SecretSlot slot;
            const char* account;
            const char* wireKey;
            const char* title;
            const char* placeholder;
            const char* objectPrefix;
        };

        const std::array<SecretInitSpec, static_cast<std::size_t>(SecretSlot::Count)> specs = {{
            {SecretSlot::GitHub,
             SpellSettingsStore::kAccountGithubToken,
             "github_token",
             "GitHub Token",
             "ghp_… or github_pat_…",
             "githubToken"},
            {SecretSlot::HuggingFace,
             SpellSettingsStore::kAccountHuggingFaceToken,
             "huggingface_token",
             "Hugging Face Token",
             "hf_…",
             "hfToken"},
            {SecretSlot::OpenAlex,
             SpellSettingsStore::kAccountOpenAlexApiKey,
             "openalex_api_key",
             "OpenAlex API Key",
             "Optional (10x daily quota)",
             "openAlexKey"},
            {SecretSlot::SearchApi,
             SpellSettingsStore::kAccountSearchApiKey,
             "search_api_key",
             "Search API Key",
             "Select Brave / Exa / Tavily / Kagi / Custom",
             "searchApiKey"},
        }};

        for (std::size_t i = 0; i < specs.size(); ++i) {
            const auto& spec = specs[i];
            SecretRowWidgets& row = secretRows[i];
            row.account = QString::fromLatin1(spec.account);
            row.wireKey = QString::fromLatin1(spec.wireKey);
            row.displayTitle = QString::fromLatin1(spec.title);
            row.defaultPlaceholder = QString::fromLatin1(spec.placeholder);

            auto* rowLayout = new QHBoxLayout();
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(8);

            auto* leftCol = new QVBoxLayout();
            leftCol->setContentsMargins(0, 0, 0, 0);
            leftCol->setSpacing(3);

            row.titleLabel = createSecondaryLabel(row.displayTitle, card);
            row.titleLabel->setWordWrap(false);
            row.titleLabel->setMinimumWidth(140);
            leftCol->addWidget(row.titleLabel);

            row.statusBadge = new SpellBadge(
                QStringLiteral("NOT SET"),
                BadgeTone::Neutral,
                false,
                card);
            leftCol->addWidget(row.statusBadge, 0, Qt::AlignLeft);

            rowLayout->addLayout(leftCol);

            row.inputEdit = new QLineEdit(card);
            row.inputEdit->setObjectName(QString::fromLatin1(spec.objectPrefix) + QStringLiteral("Edit"));
            row.inputEdit->setEchoMode(QLineEdit::Password);
            row.inputEdit->setPlaceholderText(row.defaultPlaceholder);
            row.inputEdit->setMinimumWidth(170);
            rowLayout->addWidget(row.inputEdit, 1);

            row.saveButton = new QPushButton(QStringLiteral("Save"), card);
            row.saveButton->setObjectName(QString::fromLatin1(spec.objectPrefix) + QStringLiteral("SaveButton"));
            row.saveButton->setProperty("primary", true);
            row.saveButton->setAutoDefault(false);
            row.saveButton->setDefault(false);
            row.saveButton->setEnabled(false);
            rowLayout->addWidget(row.saveButton);

            row.clearButton = new QPushButton(QStringLiteral("Clear"), card);
            row.clearButton->setObjectName(QString::fromLatin1(spec.objectPrefix) + QStringLiteral("ClearButton"));
            row.clearButton->setProperty("danger", true);
            row.clearButton->setAutoDefault(false);
            row.clearButton->setDefault(false);
            row.clearButton->setEnabled(false);
            rowLayout->addWidget(row.clearButton);

            cardLayout->addLayout(rowLayout);

            if (i + 1 < specs.size()) {
                cardLayout->addWidget(createHairline(card));
            }

            QObject::connect(row.inputEdit, &QLineEdit::textChanged, owner, [this, i](const QString& text) {
                secretRows[i].saveButton->setEnabled(!text.trimmed().isEmpty());
            });
            QObject::connect(row.inputEdit, &QLineEdit::returnPressed, owner, [this, i]() {
                saveSecretAt(i);
            });
            QObject::connect(row.saveButton, &QPushButton::clicked, owner, [this, i]() {
                saveSecretAt(i);
            });
            QObject::connect(row.clearButton, &QPushButton::clicked, owner, [this, i]() {
                clearSecretAt(i);
            });
        }

        keyringStatusLabel = new QLabel(card);
        keyringStatusLabel->setObjectName(QStringLiteral("keyringStatusLabel"));
        keyringStatusLabel->setWordWrap(true);
        keyringStatusLabel->setVisible(false);
        monoLabels.append(keyringStatusLabel);
        cardLayout->addWidget(keyringStatusLabel);

        auto* vaultFootnote = createMutedLabel(
            QStringLiteral("Stored securely in the OS keyring (com.spell.SpellIDE.credentials) "
                           "and never written to settings files or logs. Credentials are "
                           "host-pinned and stripped on cross-origin redirects."),
            card);
        cardLayout->addWidget(vaultFootnote);
    }

    void updateCuratedToggleIcon() {
        if (!curatedToggleButton) {
            return;
        }
        const UiPalette u = ThemeManager::instance().ui();
        const QString iconName = curatedHostsExpanded
                                     ? QStringLiteral("chevron-down")
                                     : QStringLiteral("chevron-right");
        curatedToggleButton->setIcon(icons::vectorIcon(iconName, u.textMuted, 12));
    }

    void setDomainFeedback(const QString& message, bool isError) {
        domainFeedbackIsError = isError;
        domainFeedbackLabel->setText(message);
        domainFeedbackLabel->setVisible(!message.isEmpty());
        applyFeedbackStyles();
    }

    void setKeyringStatus(const QString& message, bool isError) {
        keyringStatusIsError = isError;
        keyringStatusLabel->setText(message);
        keyringStatusLabel->setVisible(!message.isEmpty());
        applyFeedbackStyles();
    }

    void applyFeedbackStyles() {
        const UiPalette u = ThemeManager::instance().ui();
        if (domainFeedbackLabel) {
            const QColor col = domainFeedbackIsError ? u.danger : u.textSecondary;
            domainFeedbackLabel->setStyleSheet(
                QStringLiteral("QLabel { color: %1; background: transparent; font-size: 11px; }")
                    .arg(qssColor(col)));
        }
        if (keyringStatusLabel) {
            const QColor col = keyringStatusIsError ? u.danger : u.textSecondary;
            keyringStatusLabel->setStyleSheet(
                QStringLiteral("QLabel { color: %1; background: transparent; font-size: 11px; }")
                    .arg(qssColor(col)));
        }
    }

    void refreshModeWidgets() {
        const QString mode = detail::normalizeOnlineMode(settings.mode);
        settings.mode = mode;

        if (modeGroup) {
            QSignalBlocker blocker(modeGroup);
            if (QAbstractButton* btn = modeGroup->button(modeToRadioId(mode))) {
                btn->setChecked(true);
            }
        }

        if (postureBadge) {
            postureBadge->setText(modeToBadgeText(mode));
            postureBadge->setTone(modeToBadgeTone(mode));
            postureBadge->setShowDot(true);
        }

        if (airGapButton) {
            const bool isOffline = (mode == QLatin1StringView("offline"));
            airGapButton->setTone(isOffline ? BadgeTone::Neutral : BadgeTone::Danger);
        }

        if (modeDescriptionLabel) {
            modeDescriptionLabel->setText(modeToDescription(mode));
        }
    }

    void refreshSearchProviderDependents() {
        const QString providerKey = normalizeSearchProviderKey(settings.searchProvider);
        settings.searchProvider = providerKey;

        if (searchEndpointEdit) {
            searchEndpointEdit->setPlaceholderText(searchProviderEndpointPlaceholder(providerKey));
            const bool enableEndpoint =
                (providerKey != QLatin1StringView("none")) || !searchEndpointEdit->text().trimmed().isEmpty();
            searchEndpointEdit->setEnabled(enableEndpoint);
        }

        auto& searchRow = secretRows[static_cast<std::size_t>(SecretSlot::SearchApi)];
        if (searchRow.inputEdit) {
            searchRow.inputEdit->setPlaceholderText(searchKeyPlaceholder(providerKey));
        }
    }

    void syncDomainListWidget(QListWidget* listWidget,
                              SpellBadge* countBadge,
                              QPushButton* removeBtn,
                              QPushButton* clearBtn,
                              const QStringList& domains) {
        const QStringList normalized = normalizeDomainList(domains);
        if (itemsFromListWidget(listWidget) != normalized) {
            QSignalBlocker blocker(listWidget);
            listWidget->clear();
            for (const QString& domain : normalized) {
                listWidget->addItem(domain);
            }
        }
        if (countBadge) {
            countBadge->setText(QString::number(normalized.size()));
            countBadge->setTone(normalized.isEmpty() ? BadgeTone::Neutral : BadgeTone::Accent);
        }
        if (removeBtn) {
            removeBtn->setEnabled(!listWidget->selectedItems().isEmpty());
        }
        if (clearBtn) {
            clearBtn->setEnabled(!normalized.isEmpty());
        }
    }

    void addDomainToList(bool isAllowlist) {
        QLineEdit* input = isAllowlist ? allowlistInput : denylistInput;
        const QString raw = input->text().trimmed();
        if (raw.isEmpty()) {
            return;
        }

        const auto normalizedOpt = normalizePublicDomain(raw);
        if (!normalizedOpt.has_value()) {
            setDomainFeedback(
                QStringLiteral("Enter a valid public domain (e.g. pytorch.org) — wildcards, "
                               "IP literals, non-443 ports, and localhost are blocked."),
                true);
            return;
        }

        const QString host = *normalizedOpt;
        QStringList& targetList = isAllowlist ? settings.allowlist : settings.denylist;
        QStringList& oppositeList = isAllowlist ? settings.denylist : settings.allowlist;

        if (targetList.contains(host)) {
            input->clear();
            setDomainFeedback(
                QStringLiteral("'%1' is already in the %2.")
                    .arg(host, isAllowlist ? QStringLiteral("allowlist") : QStringLiteral("denylist")),
                false);
            return;
        }

        const bool movedFromOpposite = oppositeList.removeAll(host) > 0;
        targetList.append(host);
        targetList = normalizeDomainList(targetList);
        oppositeList = normalizeDomainList(oppositeList);

        input->clear();
        syncDomainListWidget(allowlistList, allowlistCountBadge, allowlistRemoveButton, allowlistClearButton, settings.allowlist);
        syncDomainListWidget(denylistList, denylistCountBadge, denylistRemoveButton, denylistClearButton, settings.denylist);

        if (movedFromOpposite) {
            setDomainFeedback(
                QStringLiteral("Moved '%1' from %2 to %3.")
                    .arg(host,
                         isAllowlist ? QStringLiteral("denylist") : QStringLiteral("allowlist"),
                         isAllowlist ? QStringLiteral("allowlist") : QStringLiteral("denylist")),
                false);
        } else {
            setDomainFeedback(QString(), false);
        }

        commitSettings();
    }

    void removeSelectedDomains(bool isAllowlist) {
        QListWidget* listWidget = isAllowlist ? allowlistList : denylistList;
        const QList<QListWidgetItem*> selected = listWidget->selectedItems();
        if (selected.isEmpty()) {
            return;
        }

        QStringList& targetList = isAllowlist ? settings.allowlist : settings.denylist;
        for (const QListWidgetItem* item : selected) {
            if (item) {
                targetList.removeAll(item->text());
            }
        }
        targetList = normalizeDomainList(targetList);

        syncDomainListWidget(allowlistList, allowlistCountBadge, allowlistRemoveButton, allowlistClearButton, settings.allowlist);
        syncDomainListWidget(denylistList, denylistCountBadge, denylistRemoveButton, denylistClearButton, settings.denylist);
        setDomainFeedback(QString(), false);
        commitSettings();
    }

    void clearDomainList(bool isAllowlist) {
        QStringList& targetList = isAllowlist ? settings.allowlist : settings.denylist;
        if (targetList.isEmpty()) {
            return;
        }
        targetList.clear();

        syncDomainListWidget(allowlistList, allowlistCountBadge, allowlistRemoveButton, allowlistClearButton, settings.allowlist);
        syncDomainListWidget(denylistList, denylistCountBadge, denylistRemoveButton, denylistClearButton, settings.denylist);
        setDomainFeedback(QString(), false);
        commitSettings();
    }

    void triggerAirGapNow() {
        settings.mode = QStringLiteral("offline");
        refreshModeWidgets();
        setDomainFeedback(QString(), false);

        if (client) {
            client->airGapNow();
        }
    }

    bool isSecretConfigured(std::size_t idx) const {
        const auto& row = secretRows[idx];
        if (store && store->hasSecret(row.account)) {
            return true;
        }
        return row.snapshotHasSecret || row.fallbackHasSecret;
    }

    void refreshCredentialBadges() {
        for (std::size_t i = 0; i < secretRows.size(); ++i) {
            auto& row = secretRows[i];
            const bool configured = isSecretConfigured(i);
            if (row.statusBadge) {
                row.statusBadge->setText(configured ? QStringLiteral("KEYRING SET") : QStringLiteral("NOT SET"));
                row.statusBadge->setTone(configured ? BadgeTone::Success : BadgeTone::Neutral);
                row.statusBadge->setShowDot(configured);
            }
            if (row.clearButton) {
                row.clearButton->setEnabled(configured);
            }
        }

        settings.hasGithubToken = isSecretConfigured(static_cast<std::size_t>(SecretSlot::GitHub));
        settings.hasHfToken = isSecretConfigured(static_cast<std::size_t>(SecretSlot::HuggingFace));
        settings.hasOpenAlexKey = isSecretConfigured(static_cast<std::size_t>(SecretSlot::OpenAlex));
        settings.hasSearchKey = isSecretConfigured(static_cast<std::size_t>(SecretSlot::SearchApi));
    }

    void syncSecretToCoreSession(const QString& wireKey, const std::optional<QString>& secretValue) {
        if (!client || wireKey.isEmpty()) {
            return;
        }
        QJsonObject cmd;
        cmd.insert(QStringLiteral("cmd"), QStringLiteral("configure_online_research"));
        if (secretValue.has_value()) {
            cmd.insert(wireKey, *secretValue);
        } else {
            cmd.insert(wireKey, QJsonValue::Null);
        }
        client->dispatchCommand(cmd);
    }

    void saveSecretAt(std::size_t idx) {
        if (idx >= secretRows.size()) {
            return;
        }
        auto& row = secretRows[idx];
        const QString trimmed = row.inputEdit->text().trimmed();
        if (trimmed.isEmpty()) {
            return;
        }

        const QString account = row.account;
        const QString wireKey = row.wireKey;
        const QString displayTitle = row.displayTitle;
        QPointer<OnlineResearchSettingsPane> self(owner);

        auto onCompleted = [self, idx, trimmed, wireKey, displayTitle](bool ok) {
            if (!self) {
                return;
            }
            auto& impl = *self->m_impl;
            auto& targetRow = impl.secretRows[idx];
            if (ok) {
                targetRow.snapshotHasSecret = true;
                targetRow.fallbackHasSecret = true;
                targetRow.inputEdit->clear();
                impl.refreshCredentialBadges();
                impl.syncSecretToCoreSession(wireKey, trimmed);
                impl.setKeyringStatus(
                    QStringLiteral("%1 saved to OS keyring.").arg(displayTitle),
                    false);
            } else {
                impl.setKeyringStatus(
                    QStringLiteral("Failed to save %1 to OS keyring.").arg(displayTitle),
                    true);
            }
        };

        if (store) {
            store->saveSecret(account, trimmed, std::move(onCompleted));
        } else if (client) {
            client->setSecret(account, trimmed, std::move(onCompleted));
        } else {
            setKeyringStatus(QStringLiteral("OS keyring service is unavailable."), true);
        }
    }

    void clearSecretAt(std::size_t idx) {
        if (idx >= secretRows.size()) {
            return;
        }
        auto& row = secretRows[idx];
        const QString account = row.account;
        const QString wireKey = row.wireKey;
        const QString displayTitle = row.displayTitle;
        QPointer<OnlineResearchSettingsPane> self(owner);

        auto onCompleted = [self, idx, wireKey, displayTitle](bool ok) {
            if (!self) {
                return;
            }
            auto& impl = *self->m_impl;
            auto& targetRow = impl.secretRows[idx];
            if (ok) {
                targetRow.snapshotHasSecret = false;
                targetRow.fallbackHasSecret = false;
                targetRow.inputEdit->clear();
                impl.refreshCredentialBadges();
                impl.syncSecretToCoreSession(wireKey, std::nullopt);
                impl.setKeyringStatus(
                    QStringLiteral("%1 removed from OS keyring.").arg(displayTitle),
                    false);
            } else {
                impl.setKeyringStatus(
                    QStringLiteral("Failed to remove %1 from OS keyring.").arg(displayTitle),
                    true);
            }
        };

        if (store) {
            store->removeSecret(account, std::move(onCompleted));
        } else if (client) {
            client->deleteSecret(account, std::move(onCompleted));
        } else {
            setKeyringStatus(QStringLiteral("OS keyring service is unavailable."), true);
        }
    }

    void commitSettings() {
        settings.mode = detail::normalizeOnlineMode(settings.mode);
        settings.allowlist = normalizeDomainList(settings.allowlist);
        settings.denylist = normalizeDomainList(settings.denylist);
        settings.searchProvider = normalizeSearchProviderKey(settings.searchProvider);
        settings.searchEndpoint = searchEndpointEdit ? searchEndpointEdit->text().trimmed() : settings.searchEndpoint.trimmed();
        settings.maxFetchesPerTurn = std::clamp(
            maxFetchesSpin ? maxFetchesSpin->value() : settings.maxFetchesPerTurn,
            kMinFetchesPerTurn,
            kMaxFetchesPerTurn);
        settings.timeoutSecs = std::clamp(
            timeoutSecsSpin ? timeoutSecsSpin->value() : settings.timeoutSecs,
            kMinTimeoutSecs,
            kMaxTimeoutSecs);
        settings.hasGithubToken = isSecretConfigured(static_cast<std::size_t>(SecretSlot::GitHub));
        settings.hasHfToken = isSecretConfigured(static_cast<std::size_t>(SecretSlot::HuggingFace));
        settings.hasOpenAlexKey = isSecretConfigured(static_cast<std::size_t>(SecretSlot::OpenAlex));
        settings.hasSearchKey = isSecretConfigured(static_cast<std::size_t>(SecretSlot::SearchApi));

        if (client) {
            client->setOnlineResearchSettings(settings);
        }
    }

    void probeSecretsInitial() {
        QPointer<OnlineResearchSettingsPane> self(owner);
        if (store) {
            store->loadAllSecrets([self]() {
                if (self) {
                    self->m_impl->refreshCredentialBadges();
                }
            });
            return;
        }
        if (client) {
            for (std::size_t i = 0; i < secretRows.size(); ++i) {
                const QString account = secretRows[i].account;
                client->getSecret(account, [self, i](QString value) {
                    if (!self) {
                        return;
                    }
                    self->m_impl->secretRows[i].fallbackHasSecret = !value.trimmed().isEmpty();
                    self->m_impl->refreshCredentialBadges();
                });
            }
        }
    }

    void selectSearchProviderInCombo(const QString& rawProvider) {
        const QString key = normalizeSearchProviderKey(rawProvider);
        int matchIdx = -1;
        for (int i = 0; i < searchProviderCombo->count(); ++i) {
            if (searchProviderCombo->itemData(i).toString() == key) {
                matchIdx = i;
                break;
            }
        }
        if (matchIdx < 0) {
            searchProviderCombo->addItem(key, key);
            matchIdx = searchProviderCombo->count() - 1;
        }
        QSignalBlocker blocker(searchProviderCombo);
        searchProviderCombo->setCurrentIndex(matchIdx);
    }

    void applyTheme() {
        const ThemeManager& tm = ThemeManager::instance();
        const UiPalette u = tm.ui();
        const QColor solidBorder = compositeOver(u.subtleBorder, u.background);
        const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
        const QColor solidSelected = compositeOver(u.selectedFill, u.background);

        const QString cardQss = QStringLiteral(
            "QFrame#spellSettingsCard {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 8px;"
            "}")
            .arg(qssColor(u.surface), qssColor(solidBorder));

        for (QFrame* card : cards) {
            card->setStyleSheet(cardQss);
        }

        for (SpellSectionHeader* header : sectionHeaders) {
            header->applyTheme();
        }
        for (SpellHairline* line : hairlines) {
            line->applyTheme();
        }

        if (postureBadge) {
            postureBadge->applyTheme();
        }
        if (airGapButton) {
            airGapButton->applyTheme();
        }
        if (allowlistCountBadge) {
            allowlistCountBadge->applyTheme();
        }
        if (denylistCountBadge) {
            denylistCountBadge->applyTheme();
        }

        const QString secondaryQss = QStringLiteral(
            "QLabel { color: %1; background: transparent; font-size: 12px; }")
            .arg(qssColor(u.textSecondary));
        for (QLabel* lbl : secondaryLabels) {
            lbl->setStyleSheet(secondaryQss);
        }

        const QString mutedQss = QStringLiteral(
            "QLabel { color: %1; background: transparent; font-size: 11px; }")
            .arg(qssColor(u.textMuted));
        for (QLabel* lbl : mutedLabels) {
            lbl->setStyleSheet(mutedQss);
        }

        QFont monoFont = tm.editorFont();
        monoFont.setPointSizeF(11.0);
        for (QLabel* lbl : monoLabels) {
            lbl->setFont(monoFont);
        }
        if (allowlistInput) {
            allowlistInput->setFont(monoFont);
        }
        if (denylistInput) {
            denylistInput->setFont(monoFont);
        }
        if (searchEndpointEdit) {
            searchEndpointEdit->setFont(monoFont);
        }

        const QString listQss = QStringLiteral(
            "QListWidget {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 6px;"
            "  padding: 3px;"
            "  outline: 0;"
            "}"
            "QListWidget::item {"
            "  padding: 3px 6px;"
            "  border-radius: 4px;"
            "}"
            "QListWidget::item:hover {"
            "  background-color: %4;"
            "}"
            "QListWidget::item:selected {"
            "  background-color: %5;"
            "  color: %2;"
            "}")
            .arg(qssColor(u.surface),
                 qssColor(u.textPrimary),
                 qssColor(solidBorder),
                 qssColor(solidElevated),
                 qssColor(solidSelected));

        for (QListWidget* list : domainLists) {
            list->setFont(monoFont);
            list->setStyleSheet(listQss);
        }

        if (curatedToggleButton) {
            curatedToggleButton->setStyleSheet(
                QStringLiteral(
                    "QToolButton {"
                    "  background: transparent;"
                    "  border: none;"
                    "  color: %1;"
                    "  font-size: 11.5px;"
                    "  font-weight: 600;"
                    "  padding: 2px 4px;"
                    "}"
                    "QToolButton:hover {"
                    "  color: %2;"
                    "}")
                    .arg(qssColor(u.textSecondary), qssColor(u.textPrimary)));
            updateCuratedToggleIcon();
        }

        for (auto& row : secretRows) {
            if (row.statusBadge) {
                row.statusBadge->applyTheme();
            }
            if (row.inputEdit) {
                row.inputEdit->setFont(monoFont);
            }
            if (row.saveButton) {
                row.saveButton->setIcon(icons::vectorIcon(QStringLiteral("save"), u.textPrimary, 13));
                repolishWidget(row.saveButton);
            }
            if (row.clearButton) {
                row.clearButton->setIcon(icons::vectorIcon(QStringLiteral("trash"), u.danger, 13));
                repolishWidget(row.clearButton);
            }
        }

        if (allowlistAddButton) {
            allowlistAddButton->setIcon(icons::vectorIcon(QStringLiteral("plus"), u.textPrimary, 13));
            repolishWidget(allowlistAddButton);
        }
        if (denylistAddButton) {
            denylistAddButton->setIcon(icons::vectorIcon(QStringLiteral("shield"), u.danger, 13));
            repolishWidget(denylistAddButton);
        }

        applyFeedbackStyles();
    }
};

OnlineResearchSettingsPane::OnlineResearchSettingsPane(SpellCoreClient* client,
                                                       SpellSettingsStore* store,
                                                       QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;
    m_impl->client = client;
    m_impl->store = store;

    m_impl->setupUi();
    m_impl->refreshModeWidgets();
    m_impl->refreshSearchProviderDependents();
    m_impl->refreshCredentialBadges();
    m_impl->applyTheme();

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });

    if (m_impl->store) {
        connect(m_impl->store, &SpellSettingsStore::secretChanged, this, [this](const QString& account) {
            for (auto& row : m_impl->secretRows) {
                if (row.account == account && !m_impl->store->hasSecret(account)) {
                    row.snapshotHasSecret = false;
                    row.fallbackHasSecret = false;
                }
            }
            m_impl->refreshCredentialBadges();
        });
    }

    if (m_impl->client) {
        connect(m_impl->client, &SpellCoreClient::snapshotUpdated, this,
                [this](const WorkspaceSnapshot& snap) {
                    updateSnapshot(snap);
                });
        updateSnapshot(m_impl->client->currentSnapshot());
    }

    m_impl->probeSecretsInitial();
}

OnlineResearchSettingsPane::~OnlineResearchSettingsPane() = default;

void OnlineResearchSettingsPane::updateSnapshot(const WorkspaceSnapshot& snap) {
    QScopedValueRollback<bool> guard(m_impl->syncing, true);

    const OnlineResearchSettings& incoming = snap.onlineResearch;
    m_impl->settings.mode = detail::normalizeOnlineMode(incoming.mode);
    m_impl->settings.allowlist = normalizeDomainList(incoming.allowlist);
    m_impl->settings.denylist = normalizeDomainList(incoming.denylist);
    m_impl->settings.searchProvider = normalizeSearchProviderKey(incoming.searchProvider);
    m_impl->settings.searchEndpoint = incoming.searchEndpoint.trimmed();
    m_impl->settings.maxFetchesPerTurn = std::clamp(
        incoming.maxFetchesPerTurn > 0 ? incoming.maxFetchesPerTurn : kDefaultFetchesPerTurn,
        kMinFetchesPerTurn,
        kMaxFetchesPerTurn);
    m_impl->settings.timeoutSecs = std::clamp(
        incoming.timeoutSecs > 0 ? incoming.timeoutSecs : kDefaultTimeoutSecs,
        kMinTimeoutSecs,
        kMaxTimeoutSecs);
    m_impl->settings.hasGithubToken = incoming.hasGithubToken;
    m_impl->settings.hasHfToken = incoming.hasHfToken;
    m_impl->settings.hasOpenAlexKey = incoming.hasOpenAlexKey;
    m_impl->settings.hasSearchKey = incoming.hasSearchKey;

    m_impl->refreshModeWidgets();

    m_impl->syncDomainListWidget(
        m_impl->allowlistList,
        m_impl->allowlistCountBadge,
        m_impl->allowlistRemoveButton,
        m_impl->allowlistClearButton,
        m_impl->settings.allowlist);
    m_impl->syncDomainListWidget(
        m_impl->denylistList,
        m_impl->denylistCountBadge,
        m_impl->denylistRemoveButton,
        m_impl->denylistClearButton,
        m_impl->settings.denylist);

    m_impl->selectSearchProviderInCombo(m_impl->settings.searchProvider);
    if (m_impl->searchEndpointEdit && !m_impl->searchEndpointEdit->hasFocus()) {
        if (m_impl->searchEndpointEdit->text() != m_impl->settings.searchEndpoint) {
            QSignalBlocker blocker(m_impl->searchEndpointEdit);
            m_impl->searchEndpointEdit->setText(m_impl->settings.searchEndpoint);
        }
    }
    m_impl->refreshSearchProviderDependents();

    if (m_impl->maxFetchesSpin && !m_impl->maxFetchesSpin->hasFocus()) {
        if (m_impl->maxFetchesSpin->value() != m_impl->settings.maxFetchesPerTurn) {
            QSignalBlocker blocker(m_impl->maxFetchesSpin);
            m_impl->maxFetchesSpin->setValue(m_impl->settings.maxFetchesPerTurn);
        }
    }

    if (m_impl->timeoutSecsSpin && !m_impl->timeoutSecsSpin->hasFocus()) {
        if (m_impl->timeoutSecsSpin->value() != m_impl->settings.timeoutSecs) {
            QSignalBlocker blocker(m_impl->timeoutSecsSpin);
            m_impl->timeoutSecsSpin->setValue(m_impl->settings.timeoutSecs);
        }
    }

    m_impl->secretRows[static_cast<std::size_t>(SecretSlot::GitHub)].snapshotHasSecret =
        incoming.hasGithubToken;
    m_impl->secretRows[static_cast<std::size_t>(SecretSlot::HuggingFace)].snapshotHasSecret =
        incoming.hasHfToken;
    m_impl->secretRows[static_cast<std::size_t>(SecretSlot::OpenAlex)].snapshotHasSecret =
        incoming.hasOpenAlexKey;
    m_impl->secretRows[static_cast<std::size_t>(SecretSlot::SearchApi)].snapshotHasSecret =
        incoming.hasSearchKey;
    m_impl->refreshCredentialBadges();
}

void OnlineResearchSettingsPane::applyTheme() {
    m_impl->applyTheme();
}

} // namespace spell
