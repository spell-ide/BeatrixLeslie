#include "settings/StatisticsSettingsPane.h"

#include "core/SpellCoreClient.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace spell {

namespace {

constexpr QChar kEmDash(0x2014);

QString emDash() {
    return QString(kEmDash);
}

QString formatTokenCount(quint64 value) {
    QString digits = QString::number(value);
    const qsizetype len = digits.size();
    if (len <= 3) {
        return digits;
    }
    QString out;
    out.reserve(len + (len - 1) / 3);
    for (qsizetype i = 0; i < len; ++i) {
        if (i > 0 && ((len - i) % 3 == 0)) {
            out.append(QLatin1Char(','));
        }
        out.append(digits.at(i));
    }
    return out;
}

QString formatSignedTokenCount(qint64 value) {
    return formatTokenCount(value > 0 ? static_cast<quint64>(value) : 0ULL);
}

QString formatBytes(quint64 bytes) {
    if (bytes == 0) {
        return emDash();
    }
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = kKiB * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;
    constexpr double kTiB = kGiB * 1024.0;
    const double val = static_cast<double>(bytes);
    if (val >= kTiB) {
        return QStringLiteral("%1 TB").arg(val / kTiB, 0, 'f', 2);
    }
    if (val >= kGiB) {
        return QStringLiteral("%1 GB").arg(val / kGiB, 0, 'f', 1);
    }
    if (val >= kMiB) {
        return QStringLiteral("%1 MB").arg(val / kMiB, 0, 'f', 1);
    }
    if (val >= kKiB) {
        return QStringLiteral("%1 KB").arg(val / kKiB, 0, 'f', 1);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

double sanitizeRate(double value) {
    return (std::isfinite(value) && value > 0.0) ? value : 0.0;
}

QString formatOsSummary(const SystemInfo& info) {
    const QString os = info.os.trimmed();
    const QString ver = info.osVersion.trimmed();
    if (!os.isEmpty() && !ver.isEmpty()) {
        if (ver.startsWith(os, Qt::CaseInsensitive)) {
            return ver;
        }
        return QStringLiteral("%1 %2").arg(os, ver);
    }
    if (!os.isEmpty()) {
        return os;
    }
    if (!ver.isEmpty()) {
        return ver;
    }
    return emDash();
}

struct StatTileWidgets {
    QFrame* card = nullptr;
    QLabel* titleLabel = nullptr;
    QLabel* valueLabel = nullptr;
    QLabel* unitLabel = nullptr;
};

StatTileWidgets makeStatTile(const QString& title,
                             const QString& initialValue,
                             const QString& unit,
                             QWidget* parent) {
    StatTileWidgets tile;
    tile.card = new QFrame(parent);
    tile.card->setObjectName(QStringLiteral("spellStatTile"));
    tile.card->setFrameShape(QFrame::NoFrame);
    tile.card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* vbox = new QVBoxLayout(tile.card);
    vbox->setContentsMargins(12, 10, 12, 10);
    vbox->setSpacing(4);

    tile.titleLabel = new QLabel(title.toUpper(), tile.card);
    tile.titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    vbox->addWidget(tile.titleLabel);

    auto* valueRow = new QHBoxLayout();
    valueRow->setContentsMargins(0, 0, 0, 0);
    valueRow->setSpacing(4);

    tile.valueLabel = new QLabel(initialValue, tile.card);
    tile.valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    valueRow->addWidget(tile.valueLabel, 0, Qt::AlignBottom);

    tile.unitLabel = new QLabel(unit, tile.card);
    tile.unitLabel->setVisible(!unit.isEmpty());
    valueRow->addWidget(tile.unitLabel, 0, Qt::AlignBottom);
    valueRow->addStretch(1);

    vbox->addLayout(valueRow);
    return tile;
}

} // namespace

struct StatisticsSettingsPane::Impl {
    SpellCoreClient* client = nullptr;

    // Top telemetry KPI tiles
    StatTileWidgets decodeTile;
    StatTileWidgets ewmaTile;
    StatTileWidgets latencyTile;
    StatTileWidgets epochTile;

    // Card 1: Client-measured Throughput & Token Usage
    SpellSectionHeader* throughputHeader = nullptr;
    SpellBadge* endpointStatusBadge = nullptr;
    QFrame* throughputCard = nullptr;
    SpellValueRow* decodeSpeedRow = nullptr;
    SpellValueRow* ewmaSpeedRow = nullptr;
    SpellValueRow* endpointLatencyRow = nullptr;
    SpellHairline* throughputDivider = nullptr;
    SpellValueRow* activePromptTokensRow = nullptr;
    SpellValueRow* totalPromptTokensRow = nullptr;
    SpellValueRow* totalCompletionTokensRow = nullptr;
    SpellValueRow* totalCachedTokensRow = nullptr;
    QLabel* endpointErrorLabel = nullptr;
    QLabel* throughputFootnoteLabel = nullptr;

    // Card 2: Context Cache
    SpellSectionHeader* contextHeader = nullptr;
    SpellBadge* contextLocalBadge = nullptr;
    QFrame* contextCard = nullptr;
    SpellValueRow* contextEpochRow = nullptr;
    SpellValueRow* activeThreadRow = nullptr;
    QLabel* contextFootnoteLabel = nullptr;
    QPushButton* resetContextButton = nullptr;
    QLabel* contextStatusLabel = nullptr;

    // Card 3: Machine Hardware (Informational)
    SpellSectionHeader* hardwareHeader = nullptr;
    SpellBadge* hardwareInfoBadge = nullptr;
    QFrame* hardwareCard = nullptr;
    SpellValueRow* osRow = nullptr;
    SpellValueRow* archRow = nullptr;
    SpellValueRow* cpuBrandRow = nullptr;
    SpellValueRow* cpuCoresRow = nullptr;
    SpellHairline* hardwareDivider = nullptr;
    SpellValueRow* totalRamRow = nullptr;
    SpellValueRow* availableRamRow = nullptr;
    SpellValueRow* hostnameRow = nullptr;
    QPushButton* refreshHardwareButton = nullptr;
    QLabel* hardwareFootnoteLabel = nullptr;

    // Cached state
    WorkspaceSnapshot lastSnapshot;
    SystemInfo lastSystemInfo;
    bool hasSystemInfo = false;
    QString selectedThreadId;
    int compactionEpoch = 0;
    bool awaitingContextReset = false;
    int pendingResetFromEpoch = 0;
    bool contextStatusIsError = false;
    bool refreshingHardware = false;

    void applyTileColors(const UiPalette& u) {
        const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
        const QColor solidBorder = compositeOver(u.subtleBorder, u.background);
        const QColor solidMuted = compositeOver(u.textMuted, u.background);
        const QColor solidSecondary = compositeOver(u.textSecondary, u.background);

        const QString tileStyle = QStringLiteral(
            "QFrame#spellStatTile {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 8px;"
            "}"
        ).arg(qssColor(solidElevated), qssColor(solidBorder));

        QFont titleFont;
        titleFont.setPointSizeF(9.5);
        titleFont.setWeight(QFont::DemiBold);
        titleFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.7);

        QFont valueFont(ThemeManager::instance().editorFontFamily());
        valueFont.setStyleHint(QFont::Monospace);
        valueFont.setPointSizeF(19.0);
        valueFont.setWeight(QFont::DemiBold);

        QFont unitFont(ThemeManager::instance().editorFontFamily());
        unitFont.setStyleHint(QFont::Monospace);
        unitFont.setPointSizeF(10.0);

        const TelemetrySnapshot& t = lastSnapshot.telemetry;
        const bool hasDecode = sanitizeRate(t.decodeTokS) > 0.0;
        const bool hasEwma = sanitizeRate(t.decodeTokSEwma) > 0.0;
        const bool hasLatency = t.endpointLatencyMs >= 0;

        auto styleTile = [&](StatTileWidgets& tile, bool active) {
            if (!tile.card) {
                return;
            }
            tile.card->setStyleSheet(tileStyle);
            tile.titleLabel->setFont(titleFont);
            tile.titleLabel->setStyleSheet(
                QStringLiteral("background: transparent; color: %1;").arg(qssColor(solidMuted)));
            tile.valueLabel->setFont(valueFont);
            tile.valueLabel->setStyleSheet(
                QStringLiteral("background: transparent; color: %1;")
                    .arg(qssColor(active ? u.textPrimary : solidSecondary)));
            tile.unitLabel->setFont(unitFont);
            tile.unitLabel->setStyleSheet(
                QStringLiteral("background: transparent; color: %1;").arg(qssColor(solidMuted)));
        };

        styleTile(decodeTile, hasDecode);
        styleTile(ewmaTile, hasEwma);
        styleTile(latencyTile, hasLatency);
        styleTile(epochTile, true);
    }

    void styleStatusLabel(const UiPalette& u) {
        if (!contextStatusLabel) {
            return;
        }
        const QColor color = contextStatusIsError
                                 ? u.danger
                                 : compositeOver(u.textSecondary, u.background);
        QFont monoFont(ThemeManager::instance().editorFontFamily());
        monoFont.setStyleHint(QFont::Monospace);
        monoFont.setPointSizeF(10.5);
        contextStatusLabel->setFont(monoFont);
        contextStatusLabel->setStyleSheet(
            QStringLiteral("background: transparent; color: %1;").arg(qssColor(color)));
    }

    void requestHardwareRefresh(StatisticsSettingsPane* owner, bool userInitiated) {
        if (!client) {
            return;
        }
        if (userInitiated && refreshingHardware) {
            return;
        }
        if (userInitiated) {
            refreshingHardware = true;
            if (refreshHardwareButton) {
                refreshHardwareButton->setEnabled(false);
                refreshHardwareButton->setText(QStringLiteral("Refreshing…"));
            }
        }
        QPointer<StatisticsSettingsPane> self(owner);
        client->fetchSystemInfo([self, userInitiated](const SystemInfo& info) {
            if (!self) {
                return;
            }
            if (userInitiated) {
                self->m_impl->refreshingHardware = false;
                if (self->m_impl->refreshHardwareButton) {
                    self->m_impl->refreshHardwareButton->setEnabled(self->m_impl->client != nullptr);
                    self->m_impl->refreshHardwareButton->setText(QStringLiteral("Refresh Hardware Info"));
                }
                self->updateSystemInfo(info);
            } else if (!self->m_impl->hasSystemInfo) {
                self->updateSystemInfo(info);
            }
        });
    }
};

StatisticsSettingsPane::StatisticsSettingsPane(SpellCoreClient* client, QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->client = client;

    setObjectName(QStringLiteral("spellStatisticsPane"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(14);

    // -------------------------------------------------------------------------
    // Top KPI Tiles
    // -------------------------------------------------------------------------
    auto* tilesLayout = new QHBoxLayout();
    tilesLayout->setContentsMargins(0, 0, 0, 0);
    tilesLayout->setSpacing(10);

    m_impl->decodeTile = makeStatTile(
        QStringLiteral("Decode"), QStringLiteral("0.0"), QStringLiteral("tok/s"), this);
    m_impl->ewmaTile = makeStatTile(
        QStringLiteral("EWMA"), QStringLiteral("0.0"), QStringLiteral("tok/s"), this);
    m_impl->latencyTile = makeStatTile(
        QStringLiteral("Latency"), emDash(), QStringLiteral("ms"), this);
    m_impl->latencyTile.unitLabel->setVisible(false);
    m_impl->epochTile = makeStatTile(
        QStringLiteral("Epoch"), QStringLiteral("0"), QString(), this);

    tilesLayout->addWidget(m_impl->decodeTile.card, 1);
    tilesLayout->addWidget(m_impl->ewmaTile.card, 1);
    tilesLayout->addWidget(m_impl->latencyTile.card, 1);
    tilesLayout->addWidget(m_impl->epochTile.card, 1);
    mainLayout->addLayout(tilesLayout);

    // Helper to build a titled card matching the Spell IDE monochrome aesthetic
    auto createCardSection = [this, mainLayout](const QString& title,
                                                const QString& iconName,
                                                SpellSectionHeader*& outHeader,
                                                SpellBadge* trailingBadge,
                                                QFrame*& outCard) -> QVBoxLayout* {
        auto* sectionLayout = new QVBoxLayout();
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(6);

        auto* headerRow = new QHBoxLayout();
        headerRow->setContentsMargins(2, 0, 2, 0);
        headerRow->setSpacing(8);

        outHeader = new SpellSectionHeader(title, iconName, BadgeTone::Accent, this);
        headerRow->addWidget(outHeader);
        headerRow->addStretch(1);
        if (trailingBadge) {
            headerRow->addWidget(trailingBadge);
        }
        sectionLayout->addLayout(headerRow);

        outCard = new QFrame(this);
        outCard->setObjectName(QStringLiteral("spellSettingsCard"));
        outCard->setFrameShape(QFrame::NoFrame);
        outCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        auto* cardLayout = new QVBoxLayout(outCard);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(8);

        sectionLayout->addWidget(outCard);
        mainLayout->addLayout(sectionLayout);
        return cardLayout;
    };

    // -------------------------------------------------------------------------
    // Card 1: Client-measured Throughput & Token Usage
    // -------------------------------------------------------------------------
    m_impl->endpointStatusBadge = new SpellBadge(
        QStringLiteral("NOT CONFIGURED"), BadgeTone::Neutral, true, this);
    auto* throughputLayout = createCardSection(
        QStringLiteral("Client-Measured Throughput & Token Usage"),
        QStringLiteral("speedometer"),
        m_impl->throughputHeader,
        m_impl->endpointStatusBadge,
        m_impl->throughputCard);

    m_impl->decodeSpeedRow = new SpellValueRow(
        QStringLiteral("Decode speed"), QStringLiteral("0.0 tok/s"), m_impl->throughputCard);
    m_impl->decodeSpeedRow->setValueIsPrimary(true);
    throughputLayout->addWidget(m_impl->decodeSpeedRow);

    m_impl->ewmaSpeedRow = new SpellValueRow(
        QStringLiteral("EWMA speed"), QStringLiteral("0.0 tok/s"), m_impl->throughputCard);
    m_impl->ewmaSpeedRow->setValueIsPrimary(true);
    throughputLayout->addWidget(m_impl->ewmaSpeedRow);

    m_impl->endpointLatencyRow = new SpellValueRow(
        QStringLiteral("Endpoint latency"), emDash(), m_impl->throughputCard);
    m_impl->endpointLatencyRow->setValueIsPrimary(true);
    throughputLayout->addWidget(m_impl->endpointLatencyRow);

    m_impl->throughputDivider = new SpellHairline(Qt::Horizontal, m_impl->throughputCard);
    throughputLayout->addWidget(m_impl->throughputDivider);

    m_impl->activePromptTokensRow = new SpellValueRow(
        QStringLiteral("Active thread prompt tokens"), QStringLiteral("0"), m_impl->throughputCard);
    m_impl->activePromptTokensRow->setValueIsPrimary(true);
    throughputLayout->addWidget(m_impl->activePromptTokensRow);

    m_impl->totalPromptTokensRow = new SpellValueRow(
        QStringLiteral("Session cumulative prompt tokens"),
        QStringLiteral("0"),
        BadgeTone::Info,
        m_impl->throughputCard);
    m_impl->totalPromptTokensRow->setValueIsPrimary(true);
    throughputLayout->addWidget(m_impl->totalPromptTokensRow);

    m_impl->totalCompletionTokensRow = new SpellValueRow(
        QStringLiteral("Session cumulative completion tokens"),
        QStringLiteral("0"),
        BadgeTone::Info,
        m_impl->throughputCard);
    m_impl->totalCompletionTokensRow->setValueIsPrimary(true);
    throughputLayout->addWidget(m_impl->totalCompletionTokensRow);

    m_impl->totalCachedTokensRow = new SpellValueRow(
        QStringLiteral("Session cumulative cached tokens"),
        QStringLiteral("0"),
        BadgeTone::Info,
        m_impl->throughputCard);
    m_impl->totalCachedTokensRow->setValueIsPrimary(true);
    throughputLayout->addWidget(m_impl->totalCachedTokensRow);

    m_impl->endpointErrorLabel = new QLabel(m_impl->throughputCard);
    m_impl->endpointErrorLabel->setWordWrap(true);
    m_impl->endpointErrorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_impl->endpointErrorLabel->setVisible(false);
    throughputLayout->addWidget(m_impl->endpointErrorLabel);

    m_impl->throughputFootnoteLabel = new QLabel(
        QStringLiteral("Measured client-side from streaming responses on your configured endpoint. "
                       "Values remain 0 until a completion turn finishes."),
        m_impl->throughputCard);
    m_impl->throughputFootnoteLabel->setWordWrap(true);
    throughputLayout->addWidget(m_impl->throughputFootnoteLabel);

    // -------------------------------------------------------------------------
    // Card 2: Context Cache
    // -------------------------------------------------------------------------
    m_impl->contextLocalBadge = new SpellBadge(
        QStringLiteral("LOCAL EPOCH"), BadgeTone::Neutral, false, this);
    auto* contextLayout = createCardSection(
        QStringLiteral("Context Cache"),
        QStringLiteral("shield"),
        m_impl->contextHeader,
        m_impl->contextLocalBadge,
        m_impl->contextCard);

    m_impl->contextEpochRow = new SpellValueRow(
        QStringLiteral("Current context reset epoch"), QStringLiteral("0"), m_impl->contextCard);
    m_impl->contextEpochRow->setValueIsPrimary(true);
    contextLayout->addWidget(m_impl->contextEpochRow);

    m_impl->activeThreadRow = new SpellValueRow(
        QStringLiteral("Active thread"),
        QStringLiteral("None selected"),
        BadgeTone::Neutral,
        m_impl->contextCard);
    contextLayout->addWidget(m_impl->activeThreadRow);

    m_impl->contextFootnoteLabel = new QLabel(
        QStringLiteral("Advances the active thread's local context reset epoch, clears seen-file "
                       "pinning, and drops the cached prompt prefix locally with zero network I/O."),
        m_impl->contextCard);
    m_impl->contextFootnoteLabel->setWordWrap(true);
    contextLayout->addWidget(m_impl->contextFootnoteLabel);

    auto* contextActionRow = new QHBoxLayout();
    contextActionRow->setContentsMargins(0, 2, 0, 0);
    contextActionRow->setSpacing(8);

    m_impl->resetContextButton = new QPushButton(
        QStringLiteral("Reset Local Context Cache"), m_impl->contextCard);
    m_impl->resetContextButton->setObjectName(QStringLiteral("resetLocalContextCacheButton"));
    m_impl->resetContextButton->setCursor(Qt::PointingHandCursor);
    m_impl->resetContextButton->setEnabled(m_impl->client != nullptr);
    m_impl->resetContextButton->setToolTip(
        QStringLiteral("Increment the active thread's local context reset epoch and clear cached prefix state"));
    contextActionRow->addWidget(m_impl->resetContextButton);
    contextActionRow->addStretch(1);
    contextLayout->addLayout(contextActionRow);

    m_impl->contextStatusLabel = new QLabel(m_impl->contextCard);
    m_impl->contextStatusLabel->setWordWrap(true);
    m_impl->contextStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_impl->contextStatusLabel->setVisible(false);
    contextLayout->addWidget(m_impl->contextStatusLabel);

    connect(m_impl->resetContextButton, &QPushButton::clicked, this, [this]() {
        if (!m_impl->client) {
            return;
        }
        m_impl->pendingResetFromEpoch = m_impl->compactionEpoch;
        m_impl->awaitingContextReset = true;
        m_impl->contextStatusIsError = false;
        m_impl->contextStatusLabel->setText(QStringLiteral("Resetting local context cache…"));
        m_impl->contextStatusLabel->setVisible(true);
        m_impl->styleStatusLabel(ThemeManager::instance().ui());
        m_impl->client->requestLocalContextReset(m_impl->selectedThreadId);
    });

    // -------------------------------------------------------------------------
    // Card 3: Machine Hardware (Informational)
    // -------------------------------------------------------------------------
    m_impl->hardwareInfoBadge = new SpellBadge(
        QStringLiteral("INFORMATIONAL"), BadgeTone::Neutral, false, this);
    auto* hardwareLayout = createCardSection(
        QStringLiteral("Machine Hardware"),
        QStringLiteral("cpu"),
        m_impl->hardwareHeader,
        m_impl->hardwareInfoBadge,
        m_impl->hardwareCard);

    m_impl->osRow = new SpellValueRow(
        QStringLiteral("Operating system"), emDash(), m_impl->hardwareCard);
    m_impl->osRow->setValueIsPrimary(true);
    hardwareLayout->addWidget(m_impl->osRow);

    m_impl->archRow = new SpellValueRow(
        QStringLiteral("Architecture"), emDash(), m_impl->hardwareCard);
    m_impl->archRow->setValueIsPrimary(true);
    hardwareLayout->addWidget(m_impl->archRow);

    m_impl->cpuBrandRow = new SpellValueRow(
        QStringLiteral("CPU"), emDash(), m_impl->hardwareCard);
    m_impl->cpuBrandRow->setValueIsPrimary(true);
    hardwareLayout->addWidget(m_impl->cpuBrandRow);

    m_impl->cpuCoresRow = new SpellValueRow(
        QStringLiteral("CPU cores"), emDash(), m_impl->hardwareCard);
    m_impl->cpuCoresRow->setValueIsPrimary(true);
    hardwareLayout->addWidget(m_impl->cpuCoresRow);

    m_impl->hardwareDivider = new SpellHairline(Qt::Horizontal, m_impl->hardwareCard);
    hardwareLayout->addWidget(m_impl->hardwareDivider);

    m_impl->totalRamRow = new SpellValueRow(
        QStringLiteral("Total RAM"), emDash(), m_impl->hardwareCard);
    m_impl->totalRamRow->setValueIsPrimary(true);
    hardwareLayout->addWidget(m_impl->totalRamRow);

    m_impl->availableRamRow = new SpellValueRow(
        QStringLiteral("Available RAM"), emDash(), m_impl->hardwareCard);
    m_impl->availableRamRow->setValueIsPrimary(true);
    hardwareLayout->addWidget(m_impl->availableRamRow);

    m_impl->hostnameRow = new SpellValueRow(
        QStringLiteral("Hostname"), emDash(), BadgeTone::Neutral, m_impl->hardwareCard);
    m_impl->hostnameRow->setVisible(false);
    hardwareLayout->addWidget(m_impl->hostnameRow);

    auto* hardwareActionRow = new QHBoxLayout();
    hardwareActionRow->setContentsMargins(0, 2, 0, 0);
    hardwareActionRow->setSpacing(8);

    m_impl->refreshHardwareButton = new QPushButton(
        QStringLiteral("Refresh Hardware Info"), m_impl->hardwareCard);
    m_impl->refreshHardwareButton->setObjectName(QStringLiteral("refreshHardwareButton"));
    m_impl->refreshHardwareButton->setCursor(Qt::PointingHandCursor);
    m_impl->refreshHardwareButton->setEnabled(m_impl->client != nullptr);
    m_impl->refreshHardwareButton->setToolTip(
        QStringLiteral("Re-query host OS, CPU, and memory availability"));
    hardwareActionRow->addWidget(m_impl->refreshHardwareButton);
    hardwareActionRow->addStretch(1);
    hardwareLayout->addLayout(hardwareActionRow);

    m_impl->hardwareFootnoteLabel = new QLabel(
        QStringLiteral("Informational host summary only. BeatrixLeslie IDE connects to any OpenAI-compatible "
                       "local or remote endpoint with no local hardware tier requirements."),
        m_impl->hardwareCard);
    m_impl->hardwareFootnoteLabel->setWordWrap(true);
    hardwareLayout->addWidget(m_impl->hardwareFootnoteLabel);

    connect(m_impl->refreshHardwareButton, &QPushButton::clicked, this, [this]() {
        m_impl->requestHardwareRefresh(this, true);
    });

    mainLayout->addStretch(1);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &StatisticsSettingsPane::applyTheme);

    if (m_impl->client) {
        connect(m_impl->client, &SpellCoreClient::snapshotUpdated,
                this, &StatisticsSettingsPane::updateSnapshot);
        connect(m_impl->client, &SpellCoreClient::errorOccurred,
                this, [this](const QString& message) {
                    if (!m_impl->awaitingContextReset) {
                        return;
                    }
                    m_impl->awaitingContextReset = false;
                    m_impl->contextStatusIsError = true;
                    const QString trimmed = message.trimmed();
                    m_impl->contextStatusLabel->setText(
                        trimmed.isEmpty()
                            ? QStringLiteral("Failed to reset local context cache.")
                            : trimmed);
                    m_impl->contextStatusLabel->setVisible(true);
                    m_impl->styleStatusLabel(ThemeManager::instance().ui());
                });

        updateSnapshot(m_impl->client->currentSnapshot());
        m_impl->requestHardwareRefresh(this, false);
    }

    applyTheme();
}

StatisticsSettingsPane::~StatisticsSettingsPane() = default;

void StatisticsSettingsPane::updateSnapshot(const WorkspaceSnapshot& snap) {
    const QString previousThreadId = m_impl->selectedThreadId;
    m_impl->lastSnapshot = snap;
    m_impl->selectedThreadId = snap.selectedThreadId.trimmed();

    const TelemetrySnapshot& t = snap.telemetry;
    const double decodeTokS = sanitizeRate(t.decodeTokS);
    const double ewmaTokS = sanitizeRate(t.decodeTokSEwma);
    const int latencyMs = t.endpointLatencyMs;
    const int epoch = std::max(0, t.compactionEpoch);
    m_impl->compactionEpoch = epoch;

    // Top KPI tiles
    m_impl->decodeTile.valueLabel->setText(QString::number(decodeTokS, 'f', 1));
    m_impl->ewmaTile.valueLabel->setText(QString::number(ewmaTokS, 'f', 1));
    if (latencyMs >= 0) {
        m_impl->latencyTile.valueLabel->setText(QString::number(latencyMs));
        m_impl->latencyTile.unitLabel->setVisible(true);
    } else {
        m_impl->latencyTile.valueLabel->setText(emDash());
        m_impl->latencyTile.unitLabel->setVisible(false);
    }
    m_impl->epochTile.valueLabel->setText(QString::number(epoch));

    // Endpoint status badge
    const bool configured = snap.endpointConfigured
                            || t.endpointConfigured
                            || !snap.endpointBaseUrl.trimmed().isEmpty();
    if (!configured) {
        m_impl->endpointStatusBadge->setText(QStringLiteral("NOT CONFIGURED"));
        m_impl->endpointStatusBadge->setTone(BadgeTone::Neutral);
    } else if (t.engineOnline) {
        m_impl->endpointStatusBadge->setText(QStringLiteral("ONLINE"));
        m_impl->endpointStatusBadge->setTone(BadgeTone::Success);
    } else {
        m_impl->endpointStatusBadge->setText(QStringLiteral("OFFLINE"));
        m_impl->endpointStatusBadge->setTone(BadgeTone::Danger);
    }

    // Throughput & Token Usage rows
    m_impl->decodeSpeedRow->setValue(QStringLiteral("%1 tok/s").arg(decodeTokS, 0, 'f', 1));
    m_impl->ewmaSpeedRow->setValue(QStringLiteral("%1 tok/s").arg(ewmaTokS, 0, 'f', 1));
    m_impl->endpointLatencyRow->setValue(
        latencyMs >= 0 ? QStringLiteral("%1 ms").arg(latencyMs) : emDash());

    m_impl->activePromptTokensRow->setValue(formatSignedTokenCount(t.activePromptTokens));
    m_impl->totalPromptTokensRow->setValue(formatTokenCount(t.totalPromptTokens));
    m_impl->totalCompletionTokensRow->setValue(formatTokenCount(t.totalCompletionTokens));

    if (t.totalCachedTokens > 0 && t.totalPromptTokens > 0) {
        const double cachedPct = std::clamp(
            (static_cast<double>(t.totalCachedTokens) * 100.0)
                / static_cast<double>(t.totalPromptTokens),
            0.0,
            100.0);
        m_impl->totalCachedTokensRow->setValue(
            QStringLiteral("%1 (%2%)")
                .arg(formatTokenCount(t.totalCachedTokens))
                .arg(cachedPct, 0, 'f', 1));
    } else {
        m_impl->totalCachedTokensRow->setValue(formatTokenCount(t.totalCachedTokens));
    }

    const QString errText = t.engineError.trimmed();
    const bool showError = configured && !t.engineOnline && !errText.isEmpty();
    m_impl->endpointErrorLabel->setText(errText);
    m_impl->endpointErrorLabel->setVisible(showError);

    // Context Cache rows & confirmation state
    m_impl->contextEpochRow->setValue(QString::number(epoch));

    QString threadDisplay = QStringLiteral("None selected");
    if (!m_impl->selectedThreadId.isEmpty()) {
        threadDisplay = m_impl->selectedThreadId;
        for (const ThreadSummary& th : snap.threads) {
            if (th.id == m_impl->selectedThreadId) {
                const QString title = th.title.trimmed();
                if (!title.isEmpty()) {
                    threadDisplay = title;
                }
                break;
            }
        }
    }
    m_impl->activeThreadRow->setValue(threadDisplay);
    m_impl->activeThreadRow->setValueIsPrimary(!m_impl->selectedThreadId.isEmpty());
    m_impl->activeThreadRow->setToolTip(
        m_impl->selectedThreadId.isEmpty() ? QString() : threadDisplay);

    if (previousThreadId != m_impl->selectedThreadId && !m_impl->awaitingContextReset) {
        m_impl->contextStatusLabel->setVisible(false);
    }

    if (m_impl->awaitingContextReset && epoch > m_impl->pendingResetFromEpoch) {
        m_impl->awaitingContextReset = false;
        m_impl->contextStatusIsError = false;
        m_impl->contextStatusLabel->setText(
            QStringLiteral("Local context cache reset (epoch %1).").arg(epoch));
        m_impl->contextStatusLabel->setVisible(true);
        m_impl->styleStatusLabel(ThemeManager::instance().ui());
    }

    m_impl->applyTileColors(ThemeManager::instance().ui());
}

void StatisticsSettingsPane::updateSystemInfo(const SystemInfo& info) {
    m_impl->lastSystemInfo = info;
    m_impl->hasSystemInfo = true;

    const QString osSummary = formatOsSummary(info);
    m_impl->osRow->setValue(osSummary);
    m_impl->osRow->setToolTip(osSummary == emDash() ? QString() : osSummary);

    const QString archText = info.arch.trimmed().isEmpty() ? emDash() : info.arch.trimmed();
    m_impl->archRow->setValue(archText);

    const QString cpuBrandText = info.cpuBrand.trimmed().isEmpty() ? emDash() : info.cpuBrand.trimmed();
    m_impl->cpuBrandRow->setValue(cpuBrandText);
    m_impl->cpuBrandRow->setToolTip(cpuBrandText == emDash() ? QString() : cpuBrandText);

    const QString cpuCoresText = info.cpuCores > 0
                                     ? QString::number(info.cpuCores)
                                     : emDash();
    m_impl->cpuCoresRow->setValue(cpuCoresText);

    m_impl->totalRamRow->setValue(formatBytes(info.totalMemoryBytes));

    if (info.availableMemoryBytes > 0
        && info.totalMemoryBytes > 0
        && info.availableMemoryBytes <= info.totalMemoryBytes) {
        const double freePct = std::clamp(
            (static_cast<double>(info.availableMemoryBytes) * 100.0)
                / static_cast<double>(info.totalMemoryBytes),
            0.0,
            100.0);
        m_impl->availableRamRow->setValue(
            QStringLiteral("%1 (%2% free)")
                .arg(formatBytes(info.availableMemoryBytes))
                .arg(freePct, 0, 'f', 0));
    } else {
        m_impl->availableRamRow->setValue(formatBytes(info.availableMemoryBytes));
    }

    const QString hostText = info.hostname.trimmed();
    m_impl->hostnameRow->setValue(hostText.isEmpty() ? emDash() : hostText);
    m_impl->hostnameRow->setVisible(!hostText.isEmpty());
}

void StatisticsSettingsPane::applyTheme() {
    const UiPalette u = ThemeManager::instance().ui();
    const QColor solidBorder = compositeOver(u.subtleBorder, u.background);
    const QColor solidMuted = compositeOver(u.textMuted, u.background);

    const QString bgHex = qssColor(u.background);
    setStyleSheet(
        QStringLiteral("QWidget#spellStatisticsPane { background-color: %1; }").arg(bgHex));

    m_impl->applyTileColors(u);

    const QString cardStyle = QStringLiteral(
        "QFrame#spellSettingsCard {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 8px;"
        "}"
    ).arg(qssColor(u.surface), qssColor(solidBorder));

    for (QFrame* card : {m_impl->throughputCard, m_impl->contextCard, m_impl->hardwareCard}) {
        if (card) {
            card->setStyleSheet(cardStyle);
        }
    }

    for (SpellSectionHeader* header :
         {m_impl->throughputHeader, m_impl->contextHeader, m_impl->hardwareHeader}) {
        if (header) {
            header->applyTheme();
        }
    }

    for (SpellBadge* badge :
         {m_impl->endpointStatusBadge, m_impl->contextLocalBadge, m_impl->hardwareInfoBadge}) {
        if (badge) {
            badge->applyTheme();
        }
    }

    for (SpellValueRow* row : {m_impl->decodeSpeedRow,
                               m_impl->ewmaSpeedRow,
                               m_impl->endpointLatencyRow,
                               m_impl->activePromptTokensRow,
                               m_impl->totalPromptTokensRow,
                               m_impl->totalCompletionTokensRow,
                               m_impl->totalCachedTokensRow,
                               m_impl->contextEpochRow,
                               m_impl->activeThreadRow,
                               m_impl->osRow,
                               m_impl->archRow,
                               m_impl->cpuBrandRow,
                               m_impl->cpuCoresRow,
                               m_impl->totalRamRow,
                               m_impl->availableRamRow,
                               m_impl->hostnameRow}) {
        if (row) {
            row->applyTheme();
        }
    }

    for (SpellHairline* line : {m_impl->throughputDivider, m_impl->hardwareDivider}) {
        if (line) {
            line->applyTheme();
        }
    }

    QFont footnoteFont;
    footnoteFont.setPointSizeF(11.0);
    const QString footnoteStyle =
        QStringLiteral("background: transparent; color: %1;").arg(qssColor(solidMuted));
    for (QLabel* note :
         {m_impl->throughputFootnoteLabel, m_impl->contextFootnoteLabel, m_impl->hardwareFootnoteLabel}) {
        if (note) {
            note->setFont(footnoteFont);
            note->setStyleSheet(footnoteStyle);
        }
    }

    if (m_impl->endpointErrorLabel) {
        QFont monoFont(ThemeManager::instance().editorFontFamily());
        monoFont.setStyleHint(QFont::Monospace);
        monoFont.setPointSizeF(10.5);
        m_impl->endpointErrorLabel->setFont(monoFont);
        m_impl->endpointErrorLabel->setStyleSheet(
            QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.danger)));
    }

    m_impl->styleStatusLabel(u);

    const QIcon refreshIcon = icons::vectorIcon(QStringLiteral("refresh"), u.textPrimary, 14);
    if (m_impl->resetContextButton) {
        m_impl->resetContextButton->setIcon(refreshIcon);
        m_impl->resetContextButton->setIconSize(QSize(14, 14));
    }
    if (m_impl->refreshHardwareButton) {
        m_impl->refreshHardwareButton->setIcon(refreshIcon);
        m_impl->refreshHardwareButton->setIconSize(QSize(14, 14));
    }
}

} // namespace spell
