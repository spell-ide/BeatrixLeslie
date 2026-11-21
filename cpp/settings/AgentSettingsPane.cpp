#include "settings/AgentSettingsPane.h"

#include "core/SpellCoreClient.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace spell {

namespace {

constexpr int kMinAgentSteps = 1;
constexpr int kMaxAgentSteps = 40;
constexpr int kDefaultAgentSteps = 12;

int permissionModeToIndex(const QString& normalizedMode) {
    if (normalizedMode == QLatin1StringView("ask")) {
        return 1;
    }
    if (normalizedMode == QLatin1StringView("deny")) {
        return 2;
    }
    return 0;
}

int subagentProtocolToIndex(const QString& normalizedProtocol) {
    if (normalizedProtocol == QLatin1StringView("native")) {
        return 1;
    }
    if (normalizedProtocol == QLatin1StringView("json")) {
        return 2;
    }
    return 0;
}

QString subagentProtocolBadgeText(const QString& normalizedProtocol) {
    if (normalizedProtocol == QLatin1StringView("native")) {
        return QStringLiteral("NATIVE");
    }
    if (normalizedProtocol == QLatin1StringView("json")) {
        return QStringLiteral("JSON");
    }
    return QStringLiteral("AUTO");
}

QString subagentProtocolDescription(const QString& normalizedProtocol) {
    if (normalizedProtocol == QLatin1StringView("native")) {
        return QStringLiteral(
            "Dispatches coding subagent delegations exclusively via native OpenAI-compatible "
            "structured tool-calling schemas.");
    }
    if (normalizedProtocol == QLatin1StringView("json")) {
        return QStringLiteral(
            "Uses strict JSON action envelopes for endpoints or models that do not expose "
            "native tool-calling support.");
    }
    return QStringLiteral(
        "Automatically selects native tool calls when supported by the configured endpoint, "
        "falling back cleanly to structured JSON actions.");
}

QString onlineModeBadgeText(const QString& normalizedMode) {
    if (normalizedMode == QLatin1StringView("curated")) {
        return QStringLiteral("CURATED ALLOWLIST");
    }
    if (normalizedMode == QLatin1StringView("open_web")) {
        return QStringLiteral("OPEN WEB (ASK PER DOMAIN)");
    }
    return QStringLiteral("OFFLINE (AIR-GAPPED)");
}

BadgeTone onlineModeBadgeTone(const QString& normalizedMode) {
    if (normalizedMode == QLatin1StringView("curated")) {
        return BadgeTone::Success;
    }
    if (normalizedMode == QLatin1StringView("open_web")) {
        return BadgeTone::Warning;
    }
    return BadgeTone::Neutral;
}

QString onlineModeSummaryText(const OnlineResearchSettings& settings,
                              const QString& normalizedMode) {
    if (normalizedMode == QLatin1StringView("offline")) {
        return QStringLiteral(
            "Outbound web research sockets are blocked. Local and configured inference "
            "endpoints remain unaffected.");
    }

    QStringList parts;
    if (normalizedMode == QLatin1StringView("curated")) {
        parts.append(QStringLiteral("Vetted HTTPS research allowlist active"));
    } else {
        parts.append(QStringLiteral("Prompts for domain approval before fetching unvetted HTTPS hosts"));
    }

    const QString provider = settings.searchProvider.trimmed();
    if (!provider.isEmpty() && provider.compare(QLatin1StringView("none"), Qt::CaseInsensitive) != 0) {
        parts.append(QStringLiteral("Search: %1").arg(provider));
    }
    if (!settings.allowlist.isEmpty()) {
        const qsizetype count = settings.allowlist.size();
        parts.append(QStringLiteral("%1 custom %2")
                         .arg(count)
                         .arg(count == 1 ? QStringLiteral("domain") : QStringLiteral("domains")));
    }
    if (settings.maxFetchesPerTurn > 0) {
        parts.append(QStringLiteral("Up to %1 fetches/turn").arg(settings.maxFetchesPerTurn));
    }
    return parts.join(QStringLiteral(" · ")) + QStringLiteral(".");
}

bool isTerminalForcedAskTool(const QString& toolName) {
    return toolName.compare(QLatin1StringView("type_in_terminal"), Qt::CaseInsensitive) == 0;
}

QFrame* createSettingsCard(QWidget* parent, QVBoxLayout** outLayout) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("spellSettingsCard"));
    card->setFrameShape(QFrame::NoFrame);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(10);
    if (outLayout) {
        *outLayout = layout;
    }
    return card;
}

QWidget* createSectionBar(QWidget* parent,
                          SpellSectionHeader* header,
                          SpellBadge* trailingBadge = nullptr) {
    auto* bar = new QWidget(parent);
    bar->setObjectName(QStringLiteral("agentSectionBar"));
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(header);
    row->addStretch(1);
    if (trailingBadge) {
        row->addWidget(trailingBadge);
    }
    return bar;
}

} // namespace

struct AgentSettingsPane::Impl {
    struct ToolRow {
        QString toolName;
        QString category;
        QString description;
        QString lastMode;
        QWidget* container = nullptr;
        SpellHairline* topDivider = nullptr;
        QLabel* nameLabel = nullptr;
        QLabel* categoryLabel = nullptr;
        QLabel* descLabel = nullptr;
        QComboBox* modeCombo = nullptr;
    };

    SpellCoreClient* client = nullptr;
    bool syncing = false;
    bool agentBusy = false;
    qsizetype pendingDiffCount = 0;

    // Tracked theme-aware custom widgets
    std::vector<SpellSectionHeader*> sectionHeaders;
    std::vector<SpellHairline*> staticHairlines;
    std::vector<SpellBadge*> badges;
    std::vector<SpellValueRow*> valueRows;

    // 1. Autonomous Loop card
    SpellBadge* loopStatusBadge = nullptr;
    QCheckBox* autonomousLoopCheck = nullptr;
    QSpinBox* maxStepsSpin = nullptr;

    // 2. Stage-Write Mode card
    SpellBadge* writeModeBadge = nullptr;
    SpellBadge* pendingDiffsBadge = nullptr;
    QCheckBox* stageWriteCheck = nullptr;
    QLabel* stageWriteCaption = nullptr;

    // 3. Subagent Protocol card
    SpellBadge* protocolBadge = nullptr;
    QComboBox* subagentProtocolCombo = nullptr;
    QLabel* protocolDescriptionLabel = nullptr;
    SpellValueRow* subagentModelRow = nullptr;
    SpellValueRow* criticRow = nullptr;
    SpellValueRow* activeDelegationsRow = nullptr;

    // 4. Online Research Posture card
    SpellBadge* onlineStatusBadge = nullptr;
    QPushButton* airGapButton = nullptr;
    QPushButton* openOnlineResearchButton = nullptr;
    QLabel* onlineSummaryLabel = nullptr;

    // 5. Tool Permission Matrix card
    SpellBadge* toolCountBadge = nullptr;
    QLineEdit* toolFilterEdit = nullptr;
    QWidget* toolListContainer = nullptr;
    QVBoxLayout* toolListLayout = nullptr;
    QLabel* emptyToolsLabel = nullptr;
    QLabel* noMatchingToolsLabel = nullptr;
    std::vector<ToolRow> toolRows;

    void refreshLoopStatusBadge() {
        if (!loopStatusBadge || !autonomousLoopCheck) {
            return;
        }
        if (agentBusy) {
            loopStatusBadge->setText(QStringLiteral("RUNNING"));
            loopStatusBadge->setTone(BadgeTone::Info);
        } else if (autonomousLoopCheck->isChecked()) {
            loopStatusBadge->setText(QStringLiteral("ENABLED"));
            loopStatusBadge->setTone(BadgeTone::Success);
        } else {
            loopStatusBadge->setText(QStringLiteral("SINGLE-STEP"));
            loopStatusBadge->setTone(BadgeTone::Neutral);
        }
    }

    void refreshWriteModeVisuals(bool staged, qsizetype pendingCount) {
        if (writeModeBadge) {
            writeModeBadge->setText(staged ? QStringLiteral("STAGED") : QStringLiteral("DIRECT"));
            writeModeBadge->setTone(staged ? BadgeTone::Info : BadgeTone::Neutral);
        }
        if (pendingDiffsBadge) {
            if (pendingCount > 0) {
                pendingDiffsBadge->setText(QStringLiteral("%1 PENDING").arg(pendingCount));
                pendingDiffsBadge->setTone(BadgeTone::Warning);
                pendingDiffsBadge->setVisible(true);
            } else {
                pendingDiffsBadge->setVisible(false);
            }
        }
        if (stageWriteCaption) {
            stageWriteCaption->setText(
                staged
                    ? QStringLiteral(
                          "File mutations are staged as unified diffs in the review queue until "
                          "you explicitly approve or reject each change.")
                    : QStringLiteral(
                          "Approved file-edit tools write directly to disk in writable projects "
                          "(read-only projects remain protected)."));
        }
    }

    void refreshProtocolVisuals(const QString& normalizedProtocol) {
        if (protocolBadge) {
            protocolBadge->setText(subagentProtocolBadgeText(normalizedProtocol));
            protocolBadge->setTone(BadgeTone::Info);
        }
        if (protocolDescriptionLabel) {
            protocolDescriptionLabel->setText(subagentProtocolDescription(normalizedProtocol));
        }
    }

    void applyToolFilter() {
        const QString query = toolFilterEdit ? toolFilterEdit->text().trimmed() : QString();
        const qsizetype total = static_cast<qsizetype>(toolRows.size());
        qsizetype visibleCount = 0;
        bool firstVisible = true;

        for (auto& row : toolRows) {
            const bool matches =
                query.isEmpty() ||
                row.toolName.contains(query, Qt::CaseInsensitive) ||
                row.category.contains(query, Qt::CaseInsensitive) ||
                row.description.contains(query, Qt::CaseInsensitive);

            if (row.container) {
                row.container->setVisible(matches);
            }
            if (matches) {
                if (row.topDivider) {
                    row.topDivider->setVisible(!firstVisible);
                }
                firstVisible = false;
                ++visibleCount;
            }
        }

        if (toolFilterEdit) {
            toolFilterEdit->setVisible(total > 0);
        }
        if (emptyToolsLabel) {
            emptyToolsLabel->setVisible(total == 0);
        }
        if (noMatchingToolsLabel) {
            noMatchingToolsLabel->setVisible(total > 0 && visibleCount == 0);
        }
        if (toolCountBadge) {
            if (query.isEmpty()) {
                toolCountBadge->setText(
                    QStringLiteral("%1 %2")
                        .arg(total)
                        .arg(total == 1 ? QStringLiteral("TOOL") : QStringLiteral("TOOLS")));
            } else {
                toolCountBadge->setText(
                    QStringLiteral("%1 / %2 TOOLS").arg(visibleCount).arg(total));
            }
            toolCountBadge->setTone(total > 0 ? BadgeTone::Info : BadgeTone::Neutral);
        }
    }

    void updateToolRowFonts() {
        QFont monoFont = ThemeManager::instance().editorFont();
        monoFont.setPointSizeF(11.5);
        monoFont.setWeight(QFont::DemiBold);

        QFont catFont = ThemeManager::instance().editorFont();
        catFont.setPointSizeF(9.5);
        catFont.setWeight(QFont::Medium);

        for (auto& row : toolRows) {
            if (row.nameLabel) {
                row.nameLabel->setFont(monoFont);
            }
            if (row.categoryLabel) {
                row.categoryLabel->setFont(catFont);
            }
            if (row.topDivider) {
                row.topDivider->applyTheme();
            }
        }
    }

    void syncToolPermissions(AgentSettingsPane* owner,
                             const QVector<ToolPermissionEntry>& entries) {
        QVector<ToolPermissionEntry> sorted = entries;
        std::sort(sorted.begin(), sorted.end(),
                  [](const ToolPermissionEntry& a, const ToolPermissionEntry& b) {
                      const int cmp = a.toolName.compare(b.toolName, Qt::CaseInsensitive);
                      if (cmp != 0) {
                          return cmp < 0;
                      }
                      return a.toolName < b.toolName;
                  });

        bool sameStructure = (static_cast<qsizetype>(toolRows.size()) == sorted.size());
        if (sameStructure) {
            for (qsizetype i = 0; i < sorted.size(); ++i) {
                const auto& existing = toolRows[static_cast<std::size_t>(i)];
                const auto& incoming = sorted[i];
                if (existing.toolName != incoming.toolName ||
                    existing.category != incoming.category ||
                    existing.description != incoming.description) {
                    sameStructure = false;
                    break;
                }
            }
        }

        if (sameStructure) {
            for (qsizetype i = 0; i < sorted.size(); ++i) {
                auto& row = toolRows[static_cast<std::size_t>(i)];
                const QString effectiveMode =
                    isTerminalForcedAskTool(row.toolName)
                        ? QStringLiteral("ask")
                        : detail::normalizePermissionMode(sorted[i].mode);
                if (row.lastMode != effectiveMode) {
                    row.lastMode = effectiveMode;
                    if (row.modeCombo) {
                        const QSignalBlocker blocker(row.modeCombo);
                        row.modeCombo->setCurrentIndex(permissionModeToIndex(effectiveMode));
                    }
                }
            }
            applyToolFilter();
            return;
        }

        // Rebuild rows cleanly without deleting any widget synchronously on a signal stack.
        for (auto& row : toolRows) {
            if (row.container) {
                row.container->hide();
                if (toolListLayout) {
                    toolListLayout->removeWidget(row.container);
                }
                row.container->deleteLater();
            }
        }
        toolRows.clear();
        toolRows.reserve(static_cast<std::size_t>(sorted.size()));

        for (const ToolPermissionEntry& entry : sorted) {
            const bool forcedAsk = isTerminalForcedAskTool(entry.toolName);
            const QString effectiveMode =
                forcedAsk ? QStringLiteral("ask") : detail::normalizePermissionMode(entry.mode);

            ToolRow row;
            row.toolName = entry.toolName;
            row.category = entry.category;
            row.description = entry.description;
            row.lastMode = effectiveMode;

            row.container = new QWidget(toolListContainer);
            row.container->setObjectName(QStringLiteral("toolRowContainer"));
            row.container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            auto* outerLayout = new QVBoxLayout(row.container);
            outerLayout->setContentsMargins(0, 0, 0, 0);
            outerLayout->setSpacing(0);

            row.topDivider = new SpellHairline(Qt::Horizontal, row.container);
            outerLayout->addWidget(row.topDivider);

            auto* body = new QWidget(row.container);
            body->setObjectName(QStringLiteral("toolRowBody"));
            auto* bodyLayout = new QHBoxLayout(body);
            bodyLayout->setContentsMargins(0, 7, 0, 7);
            bodyLayout->setSpacing(12);

            auto* infoCol = new QVBoxLayout();
            infoCol->setContentsMargins(0, 0, 0, 0);
            infoCol->setSpacing(2);

            auto* titleRow = new QHBoxLayout();
            titleRow->setContentsMargins(0, 0, 0, 0);
            titleRow->setSpacing(6);

            row.nameLabel = new QLabel(entry.toolName, body);
            row.nameLabel->setProperty("role", QStringLiteral("toolName"));
            row.nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            titleRow->addWidget(row.nameLabel);

            if (!entry.category.trimmed().isEmpty()) {
                row.categoryLabel = new QLabel(entry.category.trimmed().toUpper(), body);
                row.categoryLabel->setProperty("role", QStringLiteral("toolCategory"));
                titleRow->addWidget(row.categoryLabel);
            } else if (forcedAsk) {
                row.categoryLabel = new QLabel(QStringLiteral("ALWAYS ASK"), body);
                row.categoryLabel->setProperty("role", QStringLiteral("toolCategory"));
                titleRow->addWidget(row.categoryLabel);
            }
            titleRow->addStretch(1);
            infoCol->addLayout(titleRow);

            const QString descText =
                !entry.description.trimmed().isEmpty()
                    ? entry.description.trimmed()
                    : (forcedAsk
                           ? QStringLiteral(
                                 "Always requires explicit user approval before typing into the "
                                 "integrated terminal.")
                           : QString());
            if (!descText.isEmpty()) {
                row.descLabel = new QLabel(descText, body);
                row.descLabel->setProperty("role", QStringLiteral("toolDesc"));
                row.descLabel->setWordWrap(true);
                infoCol->addWidget(row.descLabel);
            }

            bodyLayout->addLayout(infoCol, 1);

            row.modeCombo = new QComboBox(body);
            row.modeCombo->setProperty("toolName", entry.toolName);
            row.modeCombo->addItem(QStringLiteral("Allow"), QStringLiteral("allow"));
            row.modeCombo->addItem(QStringLiteral("Ask"), QStringLiteral("ask"));
            row.modeCombo->addItem(QStringLiteral("Deny"), QStringLiteral("deny"));
            row.modeCombo->setFixedWidth(112);
            {
                const QSignalBlocker blocker(row.modeCombo);
                row.modeCombo->setCurrentIndex(permissionModeToIndex(effectiveMode));
            }

            if (forcedAsk) {
                row.modeCombo->setEnabled(false);
                row.modeCombo->setToolTip(QStringLiteral(
                    "Typing into the integrated terminal (type_in_terminal) always asks for "
                    "explicit approval."));
            } else {
                row.modeCombo->setToolTip(
                    QStringLiteral("Permission policy for %1 (Allow, Ask, or Deny).")
                        .arg(entry.toolName));
                const QString capturedTool = entry.toolName;
                QComboBox* capturedCombo = row.modeCombo;
                QObject::connect(
                    capturedCombo,
                    &QComboBox::currentIndexChanged,
                    owner,
                    [this, capturedTool, capturedCombo](int) {
                        if (syncing) {
                            return;
                        }
                        const QString mode = capturedCombo->currentData().toString();
                        for (auto& existing : toolRows) {
                            if (existing.toolName == capturedTool) {
                                existing.lastMode = mode;
                                break;
                            }
                        }
                        if (client) {
                            client->setToolPermission(capturedTool, mode);
                        }
                    });
            }

            bodyLayout->addWidget(row.modeCombo, 0, Qt::AlignVCenter);
            outerLayout->addWidget(body);

            if (toolListLayout) {
                toolListLayout->addWidget(row.container);
            }
            toolRows.push_back(std::move(row));
        }

        updateToolRowFonts();
        applyToolFilter();
        if (toolListContainer) {
            toolListContainer->updateGeometry();
        }
        if (owner) {
            owner->updateGeometry();
        }
    }
};

AgentSettingsPane::AgentSettingsPane(SpellCoreClient* client, QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->client = client;
    setObjectName(QStringLiteral("agentSettingsPane"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(14);

    // =========================================================================
    // Card 1: Autonomous Loop
    // =========================================================================
    {
        auto* group = new QWidget(this);
        group->setObjectName(QStringLiteral("agentGroupBox"));
        group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* groupLayout = new QVBoxLayout(group);
        groupLayout->setContentsMargins(0, 0, 0, 0);
        groupLayout->setSpacing(6);

        auto* header = new SpellSectionHeader(
            QStringLiteral("Autonomous Loop"), QStringLiteral("repeat"), BadgeTone::Accent, group);
        m_impl->sectionHeaders.push_back(header);

        m_impl->loopStatusBadge = new SpellBadge(
            QStringLiteral("ENABLED"), BadgeTone::Success, true, group);
        m_impl->badges.push_back(m_impl->loopStatusBadge);
        groupLayout->addWidget(createSectionBar(group, header, m_impl->loopStatusBadge));

        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createSettingsCard(group, &cardLayout);

        m_impl->autonomousLoopCheck = new QCheckBox(
            QStringLiteral("Enable autonomous multi-step loop"), card);
        m_impl->autonomousLoopCheck->setObjectName(QStringLiteral("autonomousLoopCheckBox"));
        m_impl->autonomousLoopCheck->setChecked(true);
        cardLayout->addWidget(m_impl->autonomousLoopCheck);

        auto* divider = new SpellHairline(Qt::Horizontal, card);
        m_impl->staticHairlines.push_back(divider);
        cardLayout->addWidget(divider);

        auto* stepsRow = new QHBoxLayout();
        stepsRow->setContentsMargins(0, 0, 0, 0);
        stepsRow->setSpacing(12);

        auto* stepsLabel = new QLabel(QStringLiteral("Maximum steps per turn"), card);
        stepsLabel->setProperty("role", QStringLiteral("settingLabel"));
        stepsRow->addWidget(stepsLabel);
        stepsRow->addStretch(1);

        m_impl->maxStepsSpin = new QSpinBox(card);
        m_impl->maxStepsSpin->setObjectName(QStringLiteral("maxStepsSpinBox"));
        m_impl->maxStepsSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        m_impl->maxStepsSpin->setRange(kMinAgentSteps, kMaxAgentSteps);
        m_impl->maxStepsSpin->setValue(kDefaultAgentSteps);
        m_impl->maxStepsSpin->setSuffix(QStringLiteral(" steps"));
        m_impl->maxStepsSpin->setKeyboardTracking(false);
        m_impl->maxStepsSpin->setFixedWidth(124);
        m_impl->maxStepsSpin->setToolTip(QStringLiteral(
            "Maximum autonomous tool-call steps allowed per user turn (1–40)."));
        stepsRow->addWidget(m_impl->maxStepsSpin);
        cardLayout->addLayout(stepsRow);

        auto* loopCaption = new QLabel(
            QStringLiteral(
                "Allows the agent to plan, call tools, verify outputs, and iterate up to the "
                "step cap before yielding control back to you."),
            card);
        loopCaption->setProperty("role", QStringLiteral("caption"));
        loopCaption->setWordWrap(true);
        cardLayout->addWidget(loopCaption);

        groupLayout->addWidget(card);
        rootLayout->addWidget(group);

        connect(m_impl->autonomousLoopCheck, &QCheckBox::toggled, this, [this](bool checked) {
            if (m_impl->maxStepsSpin) {
                m_impl->maxStepsSpin->setEnabled(checked);
            }
            m_impl->refreshLoopStatusBadge();
            if (m_impl->syncing) {
                return;
            }
            if (m_impl->client) {
                m_impl->client->setAutonomousLoop(checked, m_impl->maxStepsSpin->value());
            }
        });

        connect(m_impl->maxStepsSpin, &QSpinBox::valueChanged, this, [this](int steps) {
            if (m_impl->syncing) {
                return;
            }
            if (m_impl->client) {
                m_impl->client->setAutonomousLoop(
                    m_impl->autonomousLoopCheck->isChecked(),
                    std::clamp(steps, kMinAgentSteps, kMaxAgentSteps));
            }
        });
    }

    // =========================================================================
    // Card 2: File Writes (Stage-Write Mode)
    // =========================================================================
    {
        auto* group = new QWidget(this);
        group->setObjectName(QStringLiteral("agentGroupBox"));
        group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* groupLayout = new QVBoxLayout(group);
        groupLayout->setContentsMargins(0, 0, 0, 0);
        groupLayout->setSpacing(6);

        auto* header = new SpellSectionHeader(
            QStringLiteral("File Writes"), QStringLiteral("edit"), BadgeTone::Accent, group);
        m_impl->sectionHeaders.push_back(header);

        m_impl->writeModeBadge = new SpellBadge(
            QStringLiteral("DIRECT"), BadgeTone::Neutral, true, group);
        m_impl->badges.push_back(m_impl->writeModeBadge);
        groupLayout->addWidget(createSectionBar(group, header, m_impl->writeModeBadge));

        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createSettingsCard(group, &cardLayout);

        auto* toggleRow = new QHBoxLayout();
        toggleRow->setContentsMargins(0, 0, 0, 0);
        toggleRow->setSpacing(8);

        m_impl->stageWriteCheck = new QCheckBox(
            QStringLiteral("Stage-Write Mode (stage diffs for approval before writing to disk)"),
            card);
        m_impl->stageWriteCheck->setObjectName(QStringLiteral("stageWriteModeCheckBox"));
        m_impl->stageWriteCheck->setChecked(false);
        toggleRow->addWidget(m_impl->stageWriteCheck, 1);

        m_impl->pendingDiffsBadge = new SpellBadge(
            QStringLiteral("0 PENDING"), BadgeTone::Warning, false, card);
        m_impl->pendingDiffsBadge->setVisible(false);
        m_impl->badges.push_back(m_impl->pendingDiffsBadge);
        toggleRow->addWidget(m_impl->pendingDiffsBadge);
        cardLayout->addLayout(toggleRow);

        m_impl->stageWriteCaption = new QLabel(card);
        m_impl->stageWriteCaption->setProperty("role", QStringLiteral("caption"));
        m_impl->stageWriteCaption->setWordWrap(true);
        cardLayout->addWidget(m_impl->stageWriteCaption);

        m_impl->refreshWriteModeVisuals(false, 0);

        groupLayout->addWidget(card);
        rootLayout->addWidget(group);

        connect(m_impl->stageWriteCheck, &QCheckBox::toggled, this, [this](bool checked) {
            m_impl->refreshWriteModeVisuals(checked, m_impl->pendingDiffCount);
            if (m_impl->syncing) {
                return;
            }
            if (m_impl->client) {
                m_impl->client->setStageWriteMode(checked);
            }
        });
    }

    // =========================================================================
    // Card 3: Subagent Protocol Summary & Selector
    // =========================================================================
    {
        auto* group = new QWidget(this);
        group->setObjectName(QStringLiteral("agentGroupBox"));
        group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* groupLayout = new QVBoxLayout(group);
        groupLayout->setContentsMargins(0, 0, 0, 0);
        groupLayout->setSpacing(6);

        auto* header = new SpellSectionHeader(
            QStringLiteral("Subagent Protocol"), QStringLiteral("cpu"), BadgeTone::Accent, group);
        m_impl->sectionHeaders.push_back(header);

        m_impl->protocolBadge = new SpellBadge(
            QStringLiteral("AUTO"), BadgeTone::Info, false, group);
        m_impl->badges.push_back(m_impl->protocolBadge);
        groupLayout->addWidget(createSectionBar(group, header, m_impl->protocolBadge));

        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createSettingsCard(group, &cardLayout);

        auto* protoRow = new QHBoxLayout();
        protoRow->setContentsMargins(0, 0, 0, 0);
        protoRow->setSpacing(12);

        auto* protoLabel = new QLabel(QStringLiteral("Delegation protocol"), card);
        protoLabel->setProperty("role", QStringLiteral("settingLabel"));
        protoRow->addWidget(protoLabel);
        protoRow->addStretch(1);

        m_impl->subagentProtocolCombo = new QComboBox(card);
        m_impl->subagentProtocolCombo->setObjectName(QStringLiteral("subagentProtocolComboBox"));
        m_impl->subagentProtocolCombo->addItem(
            QStringLiteral("Auto (Recommended)"), QStringLiteral("auto"));
        m_impl->subagentProtocolCombo->addItem(
            QStringLiteral("Native Tool Calls"), QStringLiteral("native"));
        m_impl->subagentProtocolCombo->addItem(
            QStringLiteral("JSON Action Loop"), QStringLiteral("json"));
        m_impl->subagentProtocolCombo->setFixedWidth(210);
        protoRow->addWidget(m_impl->subagentProtocolCombo);
        cardLayout->addLayout(protoRow);

        m_impl->protocolDescriptionLabel = new QLabel(card);
        m_impl->protocolDescriptionLabel->setProperty("role", QStringLiteral("caption"));
        m_impl->protocolDescriptionLabel->setWordWrap(true);
        cardLayout->addWidget(m_impl->protocolDescriptionLabel);

        auto* divider = new SpellHairline(Qt::Horizontal, card);
        m_impl->staticHairlines.push_back(divider);
        cardLayout->addWidget(divider);

        m_impl->subagentModelRow = new SpellValueRow(
            QStringLiteral("Subagent model"),
            QStringLiteral("Inherits primary model"),
            BadgeTone::Neutral,
            card);
        m_impl->valueRows.push_back(m_impl->subagentModelRow);
        cardLayout->addWidget(m_impl->subagentModelRow);

        m_impl->criticRow = new SpellValueRow(
            QStringLiteral("Gatekeeper Critic review"),
            QStringLiteral("Disabled"),
            BadgeTone::Neutral,
            card);
        m_impl->valueRows.push_back(m_impl->criticRow);
        cardLayout->addWidget(m_impl->criticRow);

        m_impl->activeDelegationsRow = new SpellValueRow(
            QStringLiteral("Active delegations"),
            QStringLiteral("None"),
            BadgeTone::Neutral,
            card);
        m_impl->valueRows.push_back(m_impl->activeDelegationsRow);
        cardLayout->addWidget(m_impl->activeDelegationsRow);

        m_impl->refreshProtocolVisuals(QStringLiteral("auto"));

        groupLayout->addWidget(card);
        rootLayout->addWidget(group);

        connect(m_impl->subagentProtocolCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            const QString proto = m_impl->subagentProtocolCombo->currentData().toString();
            m_impl->refreshProtocolVisuals(proto);
            if (m_impl->syncing) {
                return;
            }
            if (m_impl->client) {
                m_impl->client->setSubagentProtocol(proto);
            }
        });
    }

    // =========================================================================
    // Card 4: Online Research Posture Shortcut
    // =========================================================================
    {
        auto* group = new QWidget(this);
        group->setObjectName(QStringLiteral("agentGroupBox"));
        group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* groupLayout = new QVBoxLayout(group);
        groupLayout->setContentsMargins(0, 0, 0, 0);
        groupLayout->setSpacing(6);

        auto* header = new SpellSectionHeader(
            QStringLiteral("Online Research Posture"), QStringLiteral("globe"), BadgeTone::Accent, group);
        m_impl->sectionHeaders.push_back(header);
        groupLayout->addWidget(createSectionBar(group, header));

        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createSettingsCard(group, &cardLayout);

        auto* statusRow = new QHBoxLayout();
        statusRow->setContentsMargins(0, 0, 0, 0);
        statusRow->setSpacing(8);

        m_impl->onlineStatusBadge = new SpellBadge(
            QStringLiteral("OFFLINE (AIR-GAPPED)"), BadgeTone::Neutral, true, card);
        m_impl->onlineStatusBadge->setObjectName(QStringLiteral("onlineResearchStatusBadge"));
        m_impl->badges.push_back(m_impl->onlineStatusBadge);
        statusRow->addWidget(m_impl->onlineStatusBadge);
        statusRow->addStretch(1);

        m_impl->airGapButton = new QPushButton(QStringLiteral("Air-Gap Now"), card);
        m_impl->airGapButton->setObjectName(QStringLiteral("airGapNowButton"));
        m_impl->airGapButton->setProperty("danger", true);
        m_impl->airGapButton->setToolTip(QStringLiteral(
            "Immediately switch to Offline (Air-Gapped) mode and block all outbound web research."));
        m_impl->airGapButton->setVisible(false);
        statusRow->addWidget(m_impl->airGapButton);

        m_impl->openOnlineResearchButton = new QPushButton(
            QStringLiteral("Online Research…"), card);
        m_impl->openOnlineResearchButton->setObjectName(QStringLiteral("openOnlineResearchButton"));
        m_impl->openOnlineResearchButton->setToolTip(QStringLiteral(
            "Open the Online Research pane to configure domain allowlists, search providers, and "
            "API credentials."));
        statusRow->addWidget(m_impl->openOnlineResearchButton);

        cardLayout->addLayout(statusRow);

        m_impl->onlineSummaryLabel = new QLabel(
            onlineModeSummaryText(OnlineResearchSettings{}, QStringLiteral("offline")), card);
        m_impl->onlineSummaryLabel->setProperty("role", QStringLiteral("caption"));
        m_impl->onlineSummaryLabel->setWordWrap(true);
        cardLayout->addWidget(m_impl->onlineSummaryLabel);

        groupLayout->addWidget(card);
        rootLayout->addWidget(group);

        connect(m_impl->airGapButton, &QPushButton::clicked, this, [this]() {
            if (m_impl->client) {
                m_impl->client->airGapNow();
            }
        });

        connect(m_impl->openOnlineResearchButton, &QPushButton::clicked, this, [this]() {
            emit openOnlineResearchRequested();
        });
    }

    // =========================================================================
    // Card 5: Tool Permission Matrix
    // =========================================================================
    {
        auto* group = new QWidget(this);
        group->setObjectName(QStringLiteral("agentGroupBox"));
        group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* groupLayout = new QVBoxLayout(group);
        groupLayout->setContentsMargins(0, 0, 0, 0);
        groupLayout->setSpacing(6);

        auto* header = new SpellSectionHeader(
            QStringLiteral("Tool Permissions"), QStringLiteral("shield"), BadgeTone::Accent, group);
        m_impl->sectionHeaders.push_back(header);

        m_impl->toolCountBadge = new SpellBadge(
            QStringLiteral("0 TOOLS"), BadgeTone::Neutral, false, group);
        m_impl->badges.push_back(m_impl->toolCountBadge);
        groupLayout->addWidget(createSectionBar(group, header, m_impl->toolCountBadge));

        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createSettingsCard(group, &cardLayout);

        m_impl->toolFilterEdit = new QLineEdit(card);
        m_impl->toolFilterEdit->setObjectName(QStringLiteral("toolPermissionFilterEdit"));
        m_impl->toolFilterEdit->setPlaceholderText(
            QStringLiteral("Filter tools by name, category, or description…"));
        m_impl->toolFilterEdit->setClearButtonEnabled(true);
        m_impl->toolFilterEdit->setVisible(false);
        cardLayout->addWidget(m_impl->toolFilterEdit);

        m_impl->toolListContainer = new QWidget(card);
        m_impl->toolListContainer->setObjectName(QStringLiteral("toolListContainer"));
        m_impl->toolListContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_impl->toolListLayout = new QVBoxLayout(m_impl->toolListContainer);
        m_impl->toolListLayout->setContentsMargins(0, 0, 0, 0);
        m_impl->toolListLayout->setSpacing(0);
        cardLayout->addWidget(m_impl->toolListContainer);

        m_impl->emptyToolsLabel = new QLabel(
            QStringLiteral("No tools reported by core yet."), card);
        m_impl->emptyToolsLabel->setProperty("role", QStringLiteral("caption"));
        m_impl->emptyToolsLabel->setWordWrap(true);
        cardLayout->addWidget(m_impl->emptyToolsLabel);

        m_impl->noMatchingToolsLabel = new QLabel(
            QStringLiteral("No tools match the current filter."), card);
        m_impl->noMatchingToolsLabel->setProperty("role", QStringLiteral("caption"));
        m_impl->noMatchingToolsLabel->setWordWrap(true);
        m_impl->noMatchingToolsLabel->setVisible(false);
        cardLayout->addWidget(m_impl->noMatchingToolsLabel);

        auto* divider = new SpellHairline(Qt::Horizontal, card);
        m_impl->staticHairlines.push_back(divider);
        cardLayout->addWidget(divider);

        auto* footnote = new QLabel(
            QStringLiteral(
                "Typing into the integrated terminal (type_in_terminal) always asks for explicit "
                "approval regardless of policy overrides."),
            card);
        footnote->setProperty("role", QStringLiteral("caption"));
        footnote->setWordWrap(true);
        cardLayout->addWidget(footnote);

        groupLayout->addWidget(card);
        rootLayout->addWidget(group);

        connect(m_impl->toolFilterEdit, &QLineEdit::textChanged, this, [this](const QString&) {
            m_impl->applyToolFilter();
            if (m_impl->toolListContainer) {
                m_impl->toolListContainer->updateGeometry();
            }
            updateGeometry();
        });
    }

    rootLayout->addStretch(1);

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &AgentSettingsPane::applyTheme);

    applyTheme();

    if (m_impl->client) {
        updateSnapshot(m_impl->client->currentSnapshot());
    }
}

AgentSettingsPane::~AgentSettingsPane() = default;

void AgentSettingsPane::updateSnapshot(const WorkspaceSnapshot& snap) {
    struct SyncGuard {
        bool& flag;
        explicit SyncGuard(bool& f) : flag(f) { flag = true; }
        ~SyncGuard() { flag = false; }
    } guard(m_impl->syncing);

    // 1. Autonomous loop & step cap
    m_impl->agentBusy = snap.isAgentBusy;
    if (m_impl->autonomousLoopCheck) {
        const QSignalBlocker blocker(m_impl->autonomousLoopCheck);
        m_impl->autonomousLoopCheck->setChecked(snap.autonomousLoopEnabled);
    }
    if (m_impl->maxStepsSpin) {
        const int clampedSteps = std::clamp(snap.maxAgentSteps, kMinAgentSteps, kMaxAgentSteps);
        const QSignalBlocker blocker(m_impl->maxStepsSpin);
        m_impl->maxStepsSpin->setValue(clampedSteps);
        m_impl->maxStepsSpin->setEnabled(snap.autonomousLoopEnabled);
    }
    m_impl->refreshLoopStatusBadge();

    // 2. Stage-Write Mode & pending staged diffs
    m_impl->pendingDiffCount = snap.pendingDiffs.size();
    if (m_impl->stageWriteCheck) {
        const QSignalBlocker blocker(m_impl->stageWriteCheck);
        m_impl->stageWriteCheck->setChecked(snap.stageWriteMode);
    }
    m_impl->refreshWriteModeVisuals(snap.stageWriteMode, m_impl->pendingDiffCount);

    // 3. Subagent delegation protocol & summary
    const QString normProto = detail::normalizeSubagentProtocol(snap.subagentProtocol);
    if (m_impl->subagentProtocolCombo) {
        const QSignalBlocker blocker(m_impl->subagentProtocolCombo);
        m_impl->subagentProtocolCombo->setCurrentIndex(subagentProtocolToIndex(normProto));
    }
    m_impl->refreshProtocolVisuals(normProto);

    if (m_impl->subagentModelRow) {
        const QString effModel = snap.effectiveSubagentModelId.trimmed();
        const QString subModel = snap.subagentModelId.trimmed();
        const QString primaryModel = snap.modelId.trimmed();
        const bool hasModel = !effModel.isEmpty() || !subModel.isEmpty() || !primaryModel.isEmpty();
        const QString modelText =
            !effModel.isEmpty()
                ? effModel
                : (!subModel.isEmpty()
                       ? subModel
                       : (!primaryModel.isEmpty()
                              ? QStringLiteral("%1 (primary)").arg(primaryModel)
                              : QStringLiteral("Inherits primary model")));
        m_impl->subagentModelRow->setValue(modelText);
        m_impl->subagentModelRow->setValueIsPrimary(hasModel);
        m_impl->subagentModelRow->setTone(hasModel ? BadgeTone::Info : BadgeTone::Neutral);
    }
    if (m_impl->criticRow) {
        m_impl->criticRow->setValue(
            snap.criticEnabled ? QStringLiteral("Enabled") : QStringLiteral("Disabled"));
        m_impl->criticRow->setTone(
            snap.criticEnabled ? BadgeTone::Success : BadgeTone::Neutral);
    }
    if (m_impl->activeDelegationsRow) {
        const qsizetype activeCount = snap.activeSubagents.size();
        m_impl->activeDelegationsRow->setValue(
            activeCount > 0
                ? QStringLiteral("%1 running").arg(activeCount)
                : QStringLiteral("None"));
        m_impl->activeDelegationsRow->setTone(
            activeCount > 0 ? BadgeTone::Info : BadgeTone::Neutral);
    }

    // 4. Online research posture shortcut
    const QString normOnline = detail::normalizeOnlineMode(snap.onlineResearch.mode);
    if (m_impl->onlineStatusBadge) {
        m_impl->onlineStatusBadge->setText(onlineModeBadgeText(normOnline));
        m_impl->onlineStatusBadge->setTone(onlineModeBadgeTone(normOnline));
    }
    if (m_impl->airGapButton) {
        m_impl->airGapButton->setVisible(normOnline != QLatin1StringView("offline"));
    }
    if (m_impl->onlineSummaryLabel) {
        m_impl->onlineSummaryLabel->setText(
            onlineModeSummaryText(snap.onlineResearch, normOnline));
    }

    // 5. Tool permission matrix
    m_impl->syncToolPermissions(this, snap.toolPermissions);
}

void AgentSettingsPane::applyTheme() {
    const UiPalette u = ThemeManager::instance().ui();
    const QColor solidBorder = compositeOver(u.subtleBorder, u.background);
    const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
    const QColor solidSecondary = compositeOver(u.textSecondary, u.background);
    const QColor solidMuted = compositeOver(u.textMuted, u.background);

    const QString paneStyle = QStringLiteral(
        "QWidget#agentSettingsPane, QWidget#agentGroupBox, QWidget#agentSectionBar,\n"
        "QWidget#toolListContainer, QWidget#toolRowContainer, QWidget#toolRowBody {\n"
        "  background-color: transparent;\n"
        "}\n"
        "QFrame#spellSettingsCard {\n"
        "  background-color: %1;\n"
        "  border: 1px solid %2;\n"
        "  border-radius: 8px;\n"
        "}\n"
        "QLabel[role=\"settingLabel\"] {\n"
        "  color: %3;\n"
        "  background-color: transparent;\n"
        "  font-size: 12px;\n"
        "}\n"
        "QLabel[role=\"caption\"] {\n"
        "  color: %4;\n"
        "  background-color: transparent;\n"
        "  font-size: 11px;\n"
        "}\n"
        "QLabel[role=\"toolName\"] {\n"
        "  color: %5;\n"
        "  background-color: transparent;\n"
        "}\n"
        "QLabel[role=\"toolCategory\"] {\n"
        "  color: %4;\n"
        "  background-color: %6;\n"
        "  border: 1px solid %2;\n"
        "  border-radius: 4px;\n"
        "  padding: 1px 5px;\n"
        "}\n"
        "QLabel[role=\"toolDesc\"] {\n"
        "  color: %4;\n"
        "  background-color: transparent;\n"
        "  font-size: 11px;\n"
        "}\n")
                                  .arg(qssColor(u.surface),
                                       qssColor(solidBorder),
                                       qssColor(solidSecondary),
                                       qssColor(solidMuted),
                                       qssColor(u.textPrimary),
                                       qssColor(solidElevated));
    setStyleSheet(paneStyle);

    if (m_impl->airGapButton) {
        m_impl->airGapButton->setIcon(
            icons::vectorIcon(QStringLiteral("lock"), u.danger, 14));
        m_impl->airGapButton->setIconSize(QSize(14, 14));
    }
    if (m_impl->openOnlineResearchButton) {
        m_impl->openOnlineResearchButton->setIcon(
            icons::vectorIcon(QStringLiteral("globe"), u.textPrimary, 14));
        m_impl->openOnlineResearchButton->setIconSize(QSize(14, 14));
    }

    for (SpellSectionHeader* header : m_impl->sectionHeaders) {
        if (header) {
            header->applyTheme();
        }
    }
    for (SpellHairline* hairline : m_impl->staticHairlines) {
        if (hairline) {
            hairline->applyTheme();
        }
    }
    for (SpellBadge* badge : m_impl->badges) {
        if (badge) {
            badge->applyTheme();
        }
    }
    for (SpellValueRow* row : m_impl->valueRows) {
        if (row) {
            row->applyTheme();
        }
    }
    m_impl->updateToolRowFonts();
}

} // namespace spell
