#include "settings/SubagentsSettingsPane.h"

#include "core/SpellCoreClient.h"
#include "core/SpellSettingsStore.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace spell {

namespace {

constexpr int kManualSlotCount = 4;
constexpr int kDefaultSlotReadLimit = 200;
constexpr int kDefaultMaxTokens = 1024;

enum class FeedbackTone {
    Neutral,
    Success,
    Danger
};

QString protocolDescription(const QString& protocol) {
    const QString norm = detail::normalizeSubagentProtocol(protocol);
    if (norm == QLatin1StringView("native")) {
        return QStringLiteral(
            "Uses native OpenAI-compatible streaming tool calls for multi-step coding "
            "delegations, falling back automatically if repeated malformed tool calls occur.");
    }
    if (norm == QLatin1StringView("json")) {
        return QStringLiteral(
            "Drives multi-step coding delegations with single-action JSON objects from step 0. "
            "Ideal for endpoints or models that do not advertise native tool-call support.");
    }
    return QStringLiteral(
        "Starts with native streaming tool calls and switches automatically to structured "
        "JSON actions if the model emits JSON-in-content or malformed tool steps.");
}

QString normalizeSlotStateLabel(const QString& rawState) {
    const QString s = rawState.trimmed().toLower();
    if (s.isEmpty() || s == QLatin1StringView("idle") || s == QLatin1StringView("ready")) {
        return QStringLiteral("IDLE");
    }
    if (s == QLatin1StringView("running") || s == QLatin1StringView("busy") ||
        s == QLatin1StringView("working") || s == QLatin1StringView("active")) {
        return QStringLiteral("RUNNING");
    }
    if (s == QLatin1StringView("completed") || s == QLatin1StringView("done") ||
        s == QLatin1StringView("ok")) {
        return QStringLiteral("COMPLETED");
    }
    if (s == QLatin1StringView("error") || s == QLatin1StringView("failed")) {
        return QStringLiteral("ERROR");
    }
    return s.toUpper();
}

BadgeTone slotStateTone(const QString& rawState) {
    const QString s = rawState.trimmed().toLower();
    if (s == QLatin1StringView("error") || s == QLatin1StringView("failed")) {
        return BadgeTone::Danger;
    }
    if (s == QLatin1StringView("running") || s == QLatin1StringView("busy") ||
        s == QLatin1StringView("working") || s == QLatin1StringView("active")) {
        return BadgeTone::Accent;
    }
    if (s == QLatin1StringView("completed") || s == QLatin1StringView("done") ||
        s == QLatin1StringView("ok")) {
        return BadgeTone::Success;
    }
    return BadgeTone::Neutral;
}

bool isSlotStateActive(const QString& rawState) {
    const QString s = rawState.trimmed().toLower();
    return s == QLatin1StringView("running") || s == QLatin1StringView("busy") ||
           s == QLatin1StringView("working") || s == QLatin1StringView("active");
}

QString formatElapsedSince(qint64 startedAt) {
    if (startedAt <= 0) {
        return {};
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    // Accept either Unix seconds or Unix milliseconds.
    const qint64 startMs = (startedAt > 1'000'000'000'000LL) ? startedAt : (startedAt * 1000LL);
    const qint64 deltaSec = std::max<qint64>(0, (nowMs - startMs) / 1000LL);
    if (deltaSec < 60) {
        return QStringLiteral("%1s").arg(deltaSec);
    }
    const qint64 mins = deltaSec / 60;
    const qint64 secs = deltaSec % 60;
    if (mins < 60) {
        return QStringLiteral("%1m %2s").arg(mins).arg(secs);
    }
    const qint64 hrs = mins / 60;
    return QStringLiteral("%1h %2m").arg(hrs).arg(mins % 60);
}

QStringList collectUniqueModelIds(const QVector<ModelDescriptor>& models,
                                  const QString& primaryModel,
                                  const QString& subagentModel) {
    QStringList ids;
    auto addUnique = [&ids](const QString& raw) {
        const QString trimmed = raw.trimmed();
        if (!trimmed.isEmpty() && !ids.contains(trimmed)) {
            ids.append(trimmed);
        }
    };
    for (const ModelDescriptor& m : models) {
        addUnique(m.id);
    }
    addUnique(subagentModel);
    addUnique(primaryModel);
    return ids;
}

bool isComboFocused(const QComboBox* combo) {
    if (!combo) {
        return false;
    }
    if (combo->hasFocus()) {
        return true;
    }
    if (const QLineEdit* le = combo->lineEdit()) {
        return le->hasFocus();
    }
    return false;
}

} // namespace

struct SubagentsSettingsPane::Impl {
    QPointer<SpellCoreClient> client;
    QPointer<SpellSettingsStore> store;

    // Section 1: Secondary / Subagent Model ID
    SpellSectionHeader* modelHeader = nullptr;
    QFrame* modelCard = nullptr;
    QLabel* subagentModelCaption = nullptr;
    QComboBox* subagentModelCombo = nullptr;
    SpellIconButton* refreshModelsButton = nullptr;
    QPushButton* usePrimaryButton = nullptr;
    QPushButton* saveModelButton = nullptr;
    QLabel* modelHintLabel = nullptr;
    QLabel* modelStatusLabel = nullptr;
    SpellHairline* modelHairline = nullptr;
    SpellValueRow* primaryModelRow = nullptr;
    SpellValueRow* effectiveModelRow = nullptr;

    // Section 2: Delegation Protocol
    SpellSectionHeader* protocolHeader = nullptr;
    QFrame* protocolCard = nullptr;
    QLabel* protocolCaption = nullptr;
    QComboBox* protocolCombo = nullptr;
    QLabel* protocolDescLabel = nullptr;

    // Section 3: Gatekeeper Critic LLM Review
    SpellSectionHeader* criticHeader = nullptr;
    QFrame* criticCard = nullptr;
    QCheckBox* criticCheckBox = nullptr;
    SpellBadge* criticBadge = nullptr;
    QLabel* criticDescLabel = nullptr;

    // Section 4: Pool Status & Active Delegations
    SpellSectionHeader* poolHeader = nullptr;
    QFrame* poolCard = nullptr;
    QLabel* poolSummaryLabel = nullptr;
    QPushButton* refreshPoolButton = nullptr;
    std::array<QPushButton*, kManualSlotCount> poolSlotTiles{};
    std::array<QLabel*, kManualSlotCount> poolSlotTitleLabels{};
    std::array<SpellBadge*, kManualSlotCount> poolSlotBadges{};
    std::array<QLabel*, kManualSlotCount> poolSlotMetaLabels{};
    SpellHairline* poolHairline = nullptr;
    QLabel* delegationsTitleLabel = nullptr;
    QWidget* activeDelegationsContainer = nullptr;
    QVBoxLayout* activeDelegationsLayout = nullptr;

    // Section 5: Manual Subagent Slots (0..3) Test Console
    SpellSectionHeader* consoleHeader = nullptr;
    QFrame* consoleCard = nullptr;
    QLabel* slotPickerCaption = nullptr;
    QButtonGroup* slotButtonGroup = nullptr;
    std::array<QPushButton*, kManualSlotCount> slotPickerButtons{};
    SpellBadge* selectedSlotBadge = nullptr;
    QLabel* modeCaption = nullptr;
    QComboBox* modeCombo = nullptr;
    QLabel* maxTokensCaption = nullptr;
    QSpinBox* maxTokensSpin = nullptr;
    QPlainTextEdit* promptEdit = nullptr;
    QPushButton* sendButton = nullptr;
    QPushButton* readLinesButton = nullptr;
    QLabel* duplicateArrowLabel = nullptr;
    QComboBox* duplicateTargetCombo = nullptr;
    QPushButton* duplicateButton = nullptr;
    QPushButton* terminateButton = nullptr;
    QLabel* consoleStatusLabel = nullptr;
    SpellHairline* consoleHairline = nullptr;
    QLabel* outputTitleLabel = nullptr;
    QLabel* outputLineCountLabel = nullptr;
    QPushButton* copyOutputButton = nullptr;
    QPushButton* clearViewButton = nullptr;
    QPlainTextEdit* outputConsole = nullptr;

    // State
    WorkspaceSnapshot lastSnapshot;
    bool hasSnapshot = false;
    bool hasSnapshotSlots = false;
    QVector<ModelDescriptor> cachedModels;
    QStringList currentComboModelIds;
    QString lastSyncedSubagentModelId;
    bool subagentModelDirty = false;
    bool modelFetchPending = false;
    bool syncingUi = false;

    // Manual slot state (UI indices 0..3 mapped cleanly to wire IDs; defaults to 1..4 to match
    // spell_core's local_subagent_slots, and adapts automatically if a snapshot uses 0..3).
    std::array<int, kManualSlotCount> slotWireIds = {1, 2, 3, 4};
    std::array<SubagentSlotStatus, kManualSlotCount> slotSnapshots{};
    std::array<QStringList, kManualSlotCount> slotLinesCache{};
    std::array<bool, kManualSlotCount> slotHasExplicitLines{};
    int selectedSlotIndex = 0;
    bool slotBusy = false;
    bool awaitingReadLinesFeedback = false;

    FeedbackTone modelFeedbackTone = FeedbackTone::Neutral;
    FeedbackTone consoleFeedbackTone = FeedbackTone::Neutral;
    uint64_t copyOutputFeedbackGen = 0;

    int wireSlotForIndex(int idx) const {
        if (idx < 0 || idx >= kManualSlotCount) {
            return slotWireIds[0];
        }
        return slotWireIds[static_cast<std::size_t>(idx)];
    }

    int resolveSlotIndex(int incomingSlot) const {
        // Prefer exact match against the active slot's wire ID.
        if (incomingSlot == wireSlotForIndex(selectedSlotIndex)) {
            return selectedSlotIndex;
        }
        // When no snapshot has established wire IDs yet, accept 0-based active index as well.
        if (!hasSnapshotSlots && incomingSlot == selectedSlotIndex) {
            return selectedSlotIndex;
        }
        // Match against known wire IDs.
        for (int i = 0; i < kManualSlotCount; ++i) {
            if (slotWireIds[static_cast<std::size_t>(i)] == incomingSlot) {
                return i;
            }
        }
        // Fallback for 0-based slot IDs (0..3) when wire IDs are 1-based (1..4).
        if (incomingSlot >= 0 && incomingSlot < kManualSlotCount) {
            return incomingSlot;
        }
        return -1;
    }

    QString resolvedPrimaryModel() const {
        QString m = lastSnapshot.modelId.trimmed();
        if (m.isEmpty()) {
            m = lastSnapshot.telemetry.modelId.trimmed();
        }
        if (m.isEmpty() && store) {
            m = store->modelId().trimmed();
        }
        return m;
    }

    QString resolvedEffectiveSubagentModel() const {
        const QString overrideId = lastSyncedSubagentModelId.trimmed();
        if (!overrideId.isEmpty()) {
            return overrideId;
        }
        const QString snapEff = lastSnapshot.effectiveSubagentModelId.trimmed();
        if (!snapEff.isEmpty()) {
            return snapEff;
        }
        const QString telEff = lastSnapshot.telemetry.effectiveSubagentModelId.trimmed();
        if (!telEff.isEmpty()) {
            return telEff;
        }
        return resolvedPrimaryModel();
    }

    void setModelFeedback(const QString& text, FeedbackTone tone);
    void setConsoleFeedback(const QString& text, FeedbackTone tone);
    void applyFeedbackColors();

    void populateModelDropdown(const QVector<ModelDescriptor>& models);
    void updateModelSummaryRows();
    void updateCriticVisuals(bool enabled);
    void rebuildDuplicateTargetCombo();
    void updateSelectedSlotHeader();
    void refreshPoolSlotTiles();
    void rebuildActiveDelegationsList(const QVector<ActiveSubagentInfo>& active);
    void renderSelectedSlotConsole(bool scrollToBottom);
    void setConsoleBusy(bool busy);
};

void SubagentsSettingsPane::Impl::setModelFeedback(const QString& text, FeedbackTone tone) {
    modelFeedbackTone = tone;
    modelStatusLabel->setText(text);
    modelStatusLabel->setVisible(!text.trimmed().isEmpty());
    applyFeedbackColors();
}

void SubagentsSettingsPane::Impl::setConsoleFeedback(const QString& text, FeedbackTone tone) {
    consoleFeedbackTone = tone;
    consoleStatusLabel->setText(text);
    consoleStatusLabel->setVisible(!text.trimmed().isEmpty());
    applyFeedbackColors();
}

void SubagentsSettingsPane::Impl::applyFeedbackColors() {
    const UiPalette u = ThemeManager::instance().ui();
    const QColor solidSecondary = compositeOver(u.textSecondary, u.background);

    auto colorFor = [&](FeedbackTone tone) -> QColor {
        switch (tone) {
        case FeedbackTone::Success:
            return u.diffAdded;
        case FeedbackTone::Danger:
            return u.danger;
        case FeedbackTone::Neutral:
            return solidSecondary;
        }
        return solidSecondary;
    };

    if (modelStatusLabel) {
        modelStatusLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background: transparent; }")
                .arg(qssColor(colorFor(modelFeedbackTone))));
    }
    if (consoleStatusLabel) {
        consoleStatusLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background: transparent; }")
                .arg(qssColor(colorFor(consoleFeedbackTone))));
    }
}

void SubagentsSettingsPane::Impl::populateModelDropdown(const QVector<ModelDescriptor>& models) {
    const QString primary = resolvedPrimaryModel();
    const QString subOverride = lastSyncedSubagentModelId.trimmed();
    const QStringList nextIds = collectUniqueModelIds(models, primary, subOverride);
    if (nextIds == currentComboModelIds) {
        return;
    }

    currentComboModelIds = nextIds;

    const QString preservedText = subagentModelCombo->currentText();
    int cursorPos = 0;
    int selStart = -1;
    int selLength = 0;
    if (QLineEdit* le = subagentModelCombo->lineEdit()) {
        cursorPos = le->cursorPosition();
        selStart = le->selectionStart();
        selLength = le->selectedText().length();
    }

    const QSignalBlocker blocker(subagentModelCombo);
    subagentModelCombo->clear();
    for (const QString& id : nextIds) {
        subagentModelCombo->addItem(id, id);
    }
    subagentModelCombo->setEditText(preservedText);
    if (QLineEdit* le = subagentModelCombo->lineEdit()) {
        if (selStart >= 0 && selLength > 0) {
            le->setSelection(selStart, selLength);
        } else {
            le->setCursorPosition(std::min(cursorPos, static_cast<int>(preservedText.length())));
        }
    }
}

void SubagentsSettingsPane::Impl::updateModelSummaryRows() {
    const QString primary = resolvedPrimaryModel();
    primaryModelRow->setValue(primary.isEmpty() ? QStringLiteral("Not configured") : primary);
    primaryModelRow->setTone(primary.isEmpty() ? BadgeTone::Neutral : BadgeTone::Info);

    const QString overrideId = lastSyncedSubagentModelId.trimmed();
    const QString effective = resolvedEffectiveSubagentModel();
    if (effective.isEmpty()) {
        effectiveModelRow->setValue(QStringLiteral("Not configured"));
        effectiveModelRow->setTone(BadgeTone::Neutral);
    } else if (overrideId.isEmpty()) {
        effectiveModelRow->setValue(QStringLiteral("%1 (Primary)").arg(effective));
        effectiveModelRow->setTone(BadgeTone::Info);
    } else {
        effectiveModelRow->setValue(effective);
        effectiveModelRow->setTone(BadgeTone::Accent);
    }

    if (usePrimaryButton) {
        usePrimaryButton->setEnabled(!subagentModelCombo->currentText().trimmed().isEmpty() ||
                                     !lastSyncedSubagentModelId.trimmed().isEmpty());
    }
}

void SubagentsSettingsPane::Impl::updateCriticVisuals(bool enabled) {
    if (!criticBadge) {
        return;
    }
    criticBadge->setText(enabled ? QStringLiteral("LLM + PRECHECK")
                                 : QStringLiteral("PRECHECK ONLY"));
    criticBadge->setTone(enabled ? BadgeTone::Accent : BadgeTone::Neutral);
    criticBadge->setShowDot(enabled);
}

void SubagentsSettingsPane::Impl::rebuildDuplicateTargetCombo() {
    if (!duplicateTargetCombo) {
        return;
    }
    const int previousTarget = duplicateTargetCombo->currentData().isValid()
                                   ? duplicateTargetCombo->currentData().toInt()
                                   : -1;

    const QSignalBlocker blocker(duplicateTargetCombo);
    duplicateTargetCombo->clear();

    int restoreIdx = -1;
    for (int i = 0; i < kManualSlotCount; ++i) {
        if (i == selectedSlotIndex) {
            continue;
        }
        const int itemIdx = duplicateTargetCombo->count();
        duplicateTargetCombo->addItem(QStringLiteral("Slot %1").arg(i), i);
        if (i == previousTarget) {
            restoreIdx = itemIdx;
        }
    }
    if (restoreIdx >= 0) {
        duplicateTargetCombo->setCurrentIndex(restoreIdx);
    } else if (duplicateTargetCombo->count() > 0) {
        duplicateTargetCombo->setCurrentIndex(0);
    }
}

void SubagentsSettingsPane::Impl::updateSelectedSlotHeader() {
    const SubagentSlotStatus& st = slotSnapshots[static_cast<std::size_t>(selectedSlotIndex)];
    const QString stateLabel = slotBusy ? QStringLiteral("RUNNING")
                                        : normalizeSlotStateLabel(st.state);
    const BadgeTone tone = slotBusy ? BadgeTone::Accent : slotStateTone(st.state);
    selectedSlotBadge->setText(stateLabel);
    selectedSlotBadge->setTone(tone);
    selectedSlotBadge->setShowDot(slotBusy || isSlotStateActive(st.state));

    outputConsole->setPlaceholderText(
        QStringLiteral("No transcript output for Slot %1 yet. Send a prompt or click Read Lines.")
            .arg(selectedSlotIndex));
}

void SubagentsSettingsPane::Impl::refreshPoolSlotTiles() {
    int runningSlots = 0;
    const QString fallbackModel = resolvedEffectiveSubagentModel();

    for (int i = 0; i < kManualSlotCount; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        const SubagentSlotStatus& st = slotSnapshots[idx];
        const bool isSelected = (i == selectedSlotIndex);
        const bool isActive = (isSelected && slotBusy) || isSlotStateActive(st.state);
        if (isActive) {
            ++runningSlots;
        }

        const QString badgeText =
            (isSelected && slotBusy) ? QStringLiteral("RUNNING") : normalizeSlotStateLabel(st.state);
        const BadgeTone badgeTone =
            (isSelected && slotBusy) ? BadgeTone::Accent : slotStateTone(st.state);
        poolSlotBadges[idx]->setText(badgeText);
        poolSlotBadges[idx]->setTone(badgeTone);
        poolSlotBadges[idx]->setShowDot(isActive);

        const QString slotModel = st.modelId.trimmed().isEmpty() ? fallbackModel : st.modelId.trimmed();
        const int lineCount = slotHasExplicitLines[idx]
                                  ? static_cast<int>(slotLinesCache[idx].size())
                                  : std::max(st.turnCount, static_cast<int>(st.recentLines.size()));
        QString meta = slotModel.isEmpty() ? QStringLiteral("No model") : slotModel;
        if (meta.size() > 26) {
            meta = meta.left(25) + QStringLiteral("\u2026");
        }
        poolSlotMetaLabels[idx]->setText(
            QStringLiteral("%1  \u00B7  %2 %3")
                .arg(meta)
                .arg(lineCount)
                .arg(lineCount == 1 ? QStringLiteral("line") : QStringLiteral("lines")));
        poolSlotMetaLabels[idx]->setToolTip(
            slotModel.isEmpty() ? QStringLiteral("Configure a model in Settings \u2192 Engine")
                                : slotModel);

        if (poolSlotTiles[idx]) {
            poolSlotTiles[idx]->setChecked(isSelected);
        }
    }

    const int activeDelegations = static_cast<int>(lastSnapshot.activeSubagents.size());
    poolSummaryLabel->setText(
        QStringLiteral("%1 slots (%2 running)  \u00B7  %3 active %4")
            .arg(kManualSlotCount)
            .arg(runningSlots)
            .arg(activeDelegations)
            .arg(activeDelegations == 1 ? QStringLiteral("delegation") : QStringLiteral("delegations")));
}

void SubagentsSettingsPane::Impl::rebuildActiveDelegationsList(
    const QVector<ActiveSubagentInfo>& active) {
    while (QLayoutItem* item = activeDelegationsLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }

    const UiPalette u = ThemeManager::instance().ui();
    const QColor solidSecondary = compositeOver(u.textSecondary, u.background);
    const QColor solidMuted = compositeOver(u.textMuted, u.background);
    QFont monoFont = ThemeManager::instance().editorFont();
    monoFont.setPointSizeF(10.5);

    if (active.isEmpty()) {
        auto* emptyLabel = new QLabel(
            QStringLiteral("No active background delegations."), activeDelegationsContainer);
        emptyLabel->setObjectName(QStringLiteral("subagentsEmptyDelegationLabel"));
        emptyLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background: transparent; }")
                .arg(qssColor(solidMuted)));
        activeDelegationsLayout->addWidget(emptyLabel);
        return;
    }

    for (const ActiveSubagentInfo& info : active) {
        auto* rowWidget = new QWidget(activeDelegationsContainer);
        rowWidget->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 2, 0, 2);
        rowLayout->setSpacing(8);

        const QString titleText =
            !info.label.trimmed().isEmpty()
                ? info.label.trimmed()
                : (!info.role.trimmed().isEmpty() ? info.role.trimmed() : QStringLiteral("Subagent"));
        auto* titleLabel = new QLabel(titleText, rowWidget);
        QFont titleFont = titleLabel->font();
        titleFont.setWeight(QFont::DemiBold);
        titleFont.setPointSizeF(11.5);
        titleLabel->setFont(titleFont);
        titleLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background: transparent; }")
                .arg(qssColor(u.textPrimary)));
        rowLayout->addWidget(titleLabel, 0, Qt::AlignVCenter);

        if (!info.role.trimmed().isEmpty() &&
            info.role.trimmed().compare(titleText, Qt::CaseInsensitive) != 0) {
            auto* roleLabel = new QLabel(info.role.trimmed(), rowWidget);
            roleLabel->setFont(monoFont);
            roleLabel->setStyleSheet(
                QStringLiteral("QLabel { color: %1; background: transparent; }")
                    .arg(qssColor(solidMuted)));
            rowLayout->addWidget(roleLabel, 0, Qt::AlignVCenter);
        }

        rowLayout->addStretch(1);

        if (!info.modelId.trimmed().isEmpty()) {
            auto* modelLabel = new QLabel(info.modelId.trimmed(), rowWidget);
            modelLabel->setFont(monoFont);
            modelLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            modelLabel->setStyleSheet(
                QStringLiteral("QLabel { color: %1; background: transparent; }")
                    .arg(qssColor(solidSecondary)));
            rowLayout->addWidget(modelLabel, 0, Qt::AlignVCenter);
        }

        const QString elapsed = formatElapsedSince(info.startedAt);
        if (!elapsed.isEmpty()) {
            auto* elapsedLabel = new QLabel(elapsed, rowWidget);
            elapsedLabel->setFont(monoFont);
            elapsedLabel->setStyleSheet(
                QStringLiteral("QLabel { color: %1; background: transparent; }")
                    .arg(qssColor(solidMuted)));
            rowLayout->addWidget(elapsedLabel, 0, Qt::AlignVCenter);
        }

        auto* statusBadge = new SpellBadge(
            normalizeSlotStateLabel(info.status),
            slotStateTone(info.status),
            isSlotStateActive(info.status),
            rowWidget);
        rowLayout->addWidget(statusBadge, 0, Qt::AlignVCenter);

        activeDelegationsLayout->addWidget(rowWidget);
    }
}

void SubagentsSettingsPane::Impl::renderSelectedSlotConsole(bool scrollToBottom) {
    const auto idx = static_cast<std::size_t>(selectedSlotIndex);
    QStringList lines = slotLinesCache[idx];
    if (lines.isEmpty() && !slotHasExplicitLines[idx]) {
        const SubagentSlotStatus& st = slotSnapshots[idx];
        if (!st.recentLines.isEmpty()) {
            lines = st.recentLines;
        } else if (!st.lastReply.trimmed().isEmpty()) {
            lines = st.lastReply.split(QLatin1Char('\n'));
        }
    }

    const QString joined = lines.join(QLatin1Char('\n'));
    if (outputConsole->toPlainText() != joined) {
        outputConsole->setPlainText(joined);
    }
    if (scrollToBottom) {
        outputConsole->moveCursor(QTextCursor::End);
        if (QScrollBar* sb = outputConsole->verticalScrollBar()) {
            sb->setValue(sb->maximum());
        }
    }

    const int lineCount = lines.isEmpty() ? 0 : static_cast<int>(lines.size());
    outputLineCountLabel->setText(
        QStringLiteral("%1 %2")
            .arg(lineCount)
            .arg(lineCount == 1 ? QStringLiteral("line") : QStringLiteral("lines")));
    copyOutputButton->setEnabled(!joined.isEmpty());
    clearViewButton->setEnabled(!joined.isEmpty());
}

void SubagentsSettingsPane::Impl::setConsoleBusy(bool busy) {
    slotBusy = busy;
    sendButton->setEnabled(!busy);
    sendButton->setText(busy ? QStringLiteral("Sending\u2026") : QStringLiteral("Send"));
    duplicateButton->setEnabled(!busy && duplicateTargetCombo->count() > 0);
    terminateButton->setEnabled(!busy);
    updateSelectedSlotHeader();
    refreshPoolSlotTiles();
}

SubagentsSettingsPane::SubagentsSettingsPane(SpellCoreClient* client,
                                             SpellSettingsStore* store,
                                             QWidget* parent)
    : QWidget(parent),
      m_impl(std::make_unique<Impl>()) {
    m_impl->client = client;
    m_impl->store = store;

    for (int i = 0; i < kManualSlotCount; ++i) {
        auto idx = static_cast<std::size_t>(i);
        m_impl->slotSnapshots[idx].slot = m_impl->slotWireIds[idx];
        m_impl->slotSnapshots[idx].state = QStringLiteral("idle");
        m_impl->slotHasExplicitLines[idx] = false;
    }

    setObjectName(QStringLiteral("subagentsSettingsPane"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(14);

    // =========================================================================
    // 1. Secondary / Subagent Model ID
    // =========================================================================
    {
        auto* sectionLayout = new QVBoxLayout();
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(6);

        m_impl->modelHeader = new SpellSectionHeader(
            QStringLiteral("Subagent Model"), QStringLiteral("cpu"), BadgeTone::Accent, this);
        sectionLayout->addWidget(m_impl->modelHeader);

        m_impl->modelCard = new QFrame(this);
        m_impl->modelCard->setObjectName(QStringLiteral("subagentsModelCard"));
        m_impl->modelCard->setFrameShape(QFrame::NoFrame);

        auto* cardLayout = new QVBoxLayout(m_impl->modelCard);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(10);

        auto* modelInputRow = new QHBoxLayout();
        modelInputRow->setContentsMargins(0, 0, 0, 0);
        modelInputRow->setSpacing(8);

        m_impl->subagentModelCaption = new QLabel(
            QStringLiteral("Secondary / Subagent Model ID"), m_impl->modelCard);
        m_impl->subagentModelCaption->setObjectName(QStringLiteral("subagentsCaptionLabel"));
        modelInputRow->addWidget(m_impl->subagentModelCaption, 0, Qt::AlignVCenter);

        m_impl->subagentModelCombo = new QComboBox(m_impl->modelCard);
        m_impl->subagentModelCombo->setObjectName(QStringLiteral("subagentsModelCombo"));
        m_impl->subagentModelCombo->setEditable(true);
        m_impl->subagentModelCombo->setInsertPolicy(QComboBox::NoInsert);
        m_impl->subagentModelCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_impl->subagentModelCombo->setMinimumWidth(200);
        if (QLineEdit* le = m_impl->subagentModelCombo->lineEdit()) {
            le->setPlaceholderText(QStringLiteral("Blank = uses Primary Model ID"));
            le->setClearButtonEnabled(true);
        }
        modelInputRow->addWidget(m_impl->subagentModelCombo, 1);

        m_impl->refreshModelsButton = new SpellIconButton(
            QStringLiteral("refresh"),
            QStringLiteral("Fetch available models from configured endpoint"),
            BadgeTone::Neutral,
            m_impl->modelCard);
        m_impl->refreshModelsButton->setObjectName(QStringLiteral("subagentsRefreshModelsBtn"));
        modelInputRow->addWidget(m_impl->refreshModelsButton, 0, Qt::AlignVCenter);

        m_impl->usePrimaryButton = new QPushButton(QStringLiteral("Use Primary"), m_impl->modelCard);
        m_impl->usePrimaryButton->setObjectName(QStringLiteral("subagentsUsePrimaryBtn"));
        m_impl->usePrimaryButton->setToolTip(
            QStringLiteral("Clear override so subagents inherit the Primary Model ID"));
        modelInputRow->addWidget(m_impl->usePrimaryButton, 0, Qt::AlignVCenter);

        m_impl->saveModelButton = new QPushButton(QStringLiteral("Save"), m_impl->modelCard);
        m_impl->saveModelButton->setObjectName(QStringLiteral("subagentsSaveModelBtn"));
        m_impl->saveModelButton->setProperty("primary", true);
        modelInputRow->addWidget(m_impl->saveModelButton, 0, Qt::AlignVCenter);

        cardLayout->addLayout(modelInputRow);

        m_impl->modelHintLabel = new QLabel(
            QStringLiteral("Leave blank to use the Primary Model ID configured in Settings \u2192 Engine. "
                           "Enter or select another model ID from the endpoint to run coding delegations, "
                           "critic reviews, and manual slots on a dedicated secondary model."),
            m_impl->modelCard);
        m_impl->modelHintLabel->setObjectName(QStringLiteral("subagentsHintLabel"));
        m_impl->modelHintLabel->setWordWrap(true);
        cardLayout->addWidget(m_impl->modelHintLabel);

        m_impl->modelStatusLabel = new QLabel(m_impl->modelCard);
        m_impl->modelStatusLabel->setObjectName(QStringLiteral("subagentsModelStatusLabel"));
        m_impl->modelStatusLabel->setWordWrap(true);
        m_impl->modelStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_impl->modelStatusLabel->setVisible(false);
        cardLayout->addWidget(m_impl->modelStatusLabel);

        m_impl->modelHairline = new SpellHairline(Qt::Horizontal, m_impl->modelCard);
        cardLayout->addWidget(m_impl->modelHairline);

        m_impl->primaryModelRow = new SpellValueRow(
            QStringLiteral("Primary model"),
            QStringLiteral("Not configured"),
            BadgeTone::Neutral,
            m_impl->modelCard);
        m_impl->primaryModelRow->setValueIsPrimary(true);
        cardLayout->addWidget(m_impl->primaryModelRow);

        m_impl->effectiveModelRow = new SpellValueRow(
            QStringLiteral("Effective subagent model"),
            QStringLiteral("Not configured"),
            BadgeTone::Neutral,
            m_impl->modelCard);
        m_impl->effectiveModelRow->setValueIsPrimary(true);
        cardLayout->addWidget(m_impl->effectiveModelRow);

        sectionLayout->addWidget(m_impl->modelCard);
        rootLayout->addLayout(sectionLayout);
    }

    // =========================================================================
    // 2. Delegation Protocol
    // =========================================================================
    {
        auto* sectionLayout = new QVBoxLayout();
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(6);

        m_impl->protocolHeader = new SpellSectionHeader(
            QStringLiteral("Delegation Protocol"), QStringLiteral("repeat"), BadgeTone::Accent, this);
        sectionLayout->addWidget(m_impl->protocolHeader);

        m_impl->protocolCard = new QFrame(this);
        m_impl->protocolCard->setObjectName(QStringLiteral("subagentsProtocolCard"));
        m_impl->protocolCard->setFrameShape(QFrame::NoFrame);

        auto* cardLayout = new QVBoxLayout(m_impl->protocolCard);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(8);

        auto* protoRow = new QHBoxLayout();
        protoRow->setContentsMargins(0, 0, 0, 0);
        protoRow->setSpacing(12);

        m_impl->protocolCaption = new QLabel(
            QStringLiteral("Coding subagent protocol"), m_impl->protocolCard);
        m_impl->protocolCaption->setObjectName(QStringLiteral("subagentsCaptionLabel"));
        protoRow->addWidget(m_impl->protocolCaption, 0, Qt::AlignVCenter);
        protoRow->addStretch(1);

        m_impl->protocolCombo = new QComboBox(m_impl->protocolCard);
        m_impl->protocolCombo->setObjectName(QStringLiteral("subagentsProtocolCombo"));
        m_impl->protocolCombo->setMinimumWidth(180);
        m_impl->protocolCombo->addItem(QStringLiteral("Auto"), QStringLiteral("auto"));
        m_impl->protocolCombo->addItem(QStringLiteral("Native"), QStringLiteral("native"));
        m_impl->protocolCombo->addItem(QStringLiteral("JSON"), QStringLiteral("json"));
        protoRow->addWidget(m_impl->protocolCombo, 0, Qt::AlignVCenter);

        cardLayout->addLayout(protoRow);

        m_impl->protocolDescLabel = new QLabel(
            protocolDescription(QStringLiteral("auto")), m_impl->protocolCard);
        m_impl->protocolDescLabel->setObjectName(QStringLiteral("subagentsHintLabel"));
        m_impl->protocolDescLabel->setWordWrap(true);
        cardLayout->addWidget(m_impl->protocolDescLabel);

        sectionLayout->addWidget(m_impl->protocolCard);
        rootLayout->addLayout(sectionLayout);
    }

    // =========================================================================
    // 3. Gatekeeper Critic LLM Review
    // =========================================================================
    {
        auto* sectionLayout = new QVBoxLayout();
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(6);

        m_impl->criticHeader = new SpellSectionHeader(
            QStringLiteral("Gatekeeper Critic Review"),
            QStringLiteral("shield"),
            BadgeTone::Accent,
            this);
        sectionLayout->addWidget(m_impl->criticHeader);

        m_impl->criticCard = new QFrame(this);
        m_impl->criticCard->setObjectName(QStringLiteral("subagentsCriticCard"));
        m_impl->criticCard->setFrameShape(QFrame::NoFrame);

        auto* cardLayout = new QVBoxLayout(m_impl->criticCard);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(8);

        auto* criticRow = new QHBoxLayout();
        criticRow->setContentsMargins(0, 0, 0, 0);
        criticRow->setSpacing(10);

        m_impl->criticCheckBox = new QCheckBox(
            QStringLiteral("Gatekeeper Critic LLM Review"), m_impl->criticCard);
        m_impl->criticCheckBox->setObjectName(QStringLiteral("subagentsCriticCheckBox"));
        QFont checkFont = m_impl->criticCheckBox->font();
        checkFont.setWeight(QFont::DemiBold);
        m_impl->criticCheckBox->setFont(checkFont);
        criticRow->addWidget(m_impl->criticCheckBox, 0, Qt::AlignVCenter);
        criticRow->addStretch(1);

        m_impl->criticBadge = new SpellBadge(
            QStringLiteral("PRECHECK ONLY"), BadgeTone::Neutral, false, m_impl->criticCard);
        criticRow->addWidget(m_impl->criticBadge, 0, Qt::AlignVCenter);

        cardLayout->addLayout(criticRow);

        m_impl->criticDescLabel = new QLabel(
            QStringLiteral(
                "Deterministic prechecks (syntax sanity, workspace path boundaries, and staged diff "
                "integrity) always run before committing delegated edits. Enabling Gatekeeper Critic "
                "LLM Review adds an optional second-pass LLM review using your effective "
                "subagent model."),
            m_impl->criticCard);
        m_impl->criticDescLabel->setObjectName(QStringLiteral("subagentsHintLabel"));
        m_impl->criticDescLabel->setWordWrap(true);
        cardLayout->addWidget(m_impl->criticDescLabel);

        sectionLayout->addWidget(m_impl->criticCard);
        rootLayout->addLayout(sectionLayout);
    }

    // =========================================================================
    // 4. Active Subagent Pool & Running Delegations
    // =========================================================================
    {
        auto* sectionLayout = new QVBoxLayout();
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(6);

        m_impl->poolHeader = new SpellSectionHeader(
            QStringLiteral("Subagent Pool & Active Delegations"),
            QStringLiteral("sparkles"),
            BadgeTone::Accent,
            this);
        sectionLayout->addWidget(m_impl->poolHeader);

        m_impl->poolCard = new QFrame(this);
        m_impl->poolCard->setObjectName(QStringLiteral("subagentsPoolCard"));
        m_impl->poolCard->setFrameShape(QFrame::NoFrame);

        auto* cardLayout = new QVBoxLayout(m_impl->poolCard);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(10);

        auto* topRow = new QHBoxLayout();
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(8);

        m_impl->poolSummaryLabel = new QLabel(
            QStringLiteral("4 slots (0 running)  \u00B7  0 active delegations"), m_impl->poolCard);
        m_impl->poolSummaryLabel->setObjectName(QStringLiteral("subagentsCaptionLabel"));
        topRow->addWidget(m_impl->poolSummaryLabel, 1, Qt::AlignVCenter);

        m_impl->refreshPoolButton = new QPushButton(QStringLiteral("Refresh"), m_impl->poolCard);
        m_impl->refreshPoolButton->setObjectName(QStringLiteral("subagentsCompactButton"));
        topRow->addWidget(m_impl->refreshPoolButton, 0, Qt::AlignVCenter);
        cardLayout->addLayout(topRow);

        auto* grid = new QGridLayout();
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(8);

        for (int i = 0; i < kManualSlotCount; ++i) {
            const auto idx = static_cast<std::size_t>(i);
            auto* tile = new QPushButton(m_impl->poolCard);
            tile->setObjectName(QStringLiteral("subagentsSlotTile"));
            tile->setCheckable(true);
            tile->setChecked(i == 0);
            tile->setCursor(Qt::PointingHandCursor);
            tile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            tile->setMinimumHeight(56);

            auto* tileLayout = new QVBoxLayout(tile);
            tileLayout->setContentsMargins(10, 8, 10, 8);
            tileLayout->setSpacing(4);

            auto* tileTop = new QHBoxLayout();
            tileTop->setContentsMargins(0, 0, 0, 0);
            tileTop->setSpacing(6);

            auto* slotTitle = new QLabel(QStringLiteral("Slot %1").arg(i), tile);
            slotTitle->setObjectName(QStringLiteral("subagentsSlotTileTitle"));
            slotTitle->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            QFont titleFont = slotTitle->font();
            titleFont.setWeight(QFont::DemiBold);
            titleFont.setPointSizeF(11.5);
            slotTitle->setFont(titleFont);
            tileTop->addWidget(slotTitle, 0, Qt::AlignVCenter);
            tileTop->addStretch(1);

            auto* badge = new SpellBadge(QStringLiteral("IDLE"), BadgeTone::Neutral, false, tile);
            badge->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            tileTop->addWidget(badge, 0, Qt::AlignVCenter);
            tileLayout->addLayout(tileTop);

            auto* metaLabel = new QLabel(QStringLiteral("No model  \u00B7  0 lines"), tile);
            metaLabel->setObjectName(QStringLiteral("subagentsSlotTileMeta"));
            metaLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            tileLayout->addWidget(metaLabel);

            m_impl->poolSlotTiles[idx] = tile;
            m_impl->poolSlotTitleLabels[idx] = slotTitle;
            m_impl->poolSlotBadges[idx] = badge;
            m_impl->poolSlotMetaLabels[idx] = metaLabel;

            grid->addWidget(tile, i / 2, i % 2);
        }
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        cardLayout->addLayout(grid);

        m_impl->poolHairline = new SpellHairline(Qt::Horizontal, m_impl->poolCard);
        cardLayout->addWidget(m_impl->poolHairline);

        m_impl->delegationsTitleLabel = new QLabel(
            QStringLiteral("RUNNING DELEGATIONS"), m_impl->poolCard);
        m_impl->delegationsTitleLabel->setObjectName(QStringLiteral("subagentsSubHeaderLabel"));
        cardLayout->addWidget(m_impl->delegationsTitleLabel);

        m_impl->activeDelegationsContainer = new QWidget(m_impl->poolCard);
        m_impl->activeDelegationsContainer->setObjectName(
            QStringLiteral("subagentsActiveDelegationsContainer"));
        m_impl->activeDelegationsLayout = new QVBoxLayout(m_impl->activeDelegationsContainer);
        m_impl->activeDelegationsLayout->setContentsMargins(0, 0, 0, 0);
        m_impl->activeDelegationsLayout->setSpacing(4);
        cardLayout->addWidget(m_impl->activeDelegationsContainer);

        sectionLayout->addWidget(m_impl->poolCard);
        rootLayout->addLayout(sectionLayout);
    }

    // =========================================================================
    // 5. Manual Subagent Slots (0..3) Test Console
    // =========================================================================
    {
        auto* sectionLayout = new QVBoxLayout();
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(6);

        m_impl->consoleHeader = new SpellSectionHeader(
            QStringLiteral("Manual Subagent Slots (0\u20133) Test Console"),
            QStringLiteral("terminal"),
            BadgeTone::Accent,
            this);
        sectionLayout->addWidget(m_impl->consoleHeader);

        m_impl->consoleCard = new QFrame(this);
        m_impl->consoleCard->setObjectName(QStringLiteral("subagentsConsoleCard"));
        m_impl->consoleCard->setFrameShape(QFrame::NoFrame);

        auto* cardLayout = new QVBoxLayout(m_impl->consoleCard);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(10);

        // Row 1: Slot picker (0..3) + status badge
        auto* slotRow = new QHBoxLayout();
        slotRow->setContentsMargins(0, 0, 0, 0);
        slotRow->setSpacing(8);

        m_impl->slotPickerCaption = new QLabel(QStringLiteral("Slot"), m_impl->consoleCard);
        m_impl->slotPickerCaption->setObjectName(QStringLiteral("subagentsCaptionLabel"));
        slotRow->addWidget(m_impl->slotPickerCaption, 0, Qt::AlignVCenter);

        m_impl->slotButtonGroup = new QButtonGroup(this);
        m_impl->slotButtonGroup->setExclusive(true);

        auto* segRow = new QHBoxLayout();
        segRow->setContentsMargins(0, 0, 0, 0);
        segRow->setSpacing(4);
        for (int i = 0; i < kManualSlotCount; ++i) {
            auto* btn = new QPushButton(QStringLiteral("Slot %1").arg(i), m_impl->consoleCard);
            btn->setObjectName(QStringLiteral("subagentsSlotPickerBtn"));
            btn->setCheckable(true);
            btn->setChecked(i == 0);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setMinimumWidth(64);
            m_impl->slotButtonGroup->addButton(btn, i);
            m_impl->slotPickerButtons[static_cast<std::size_t>(i)] = btn;
            segRow->addWidget(btn);
        }
        slotRow->addLayout(segRow);

        slotRow->addStretch(1);

        m_impl->selectedSlotBadge = new SpellBadge(
            QStringLiteral("IDLE"), BadgeTone::Neutral, false, m_impl->consoleCard);
        slotRow->addWidget(m_impl->selectedSlotBadge, 0, Qt::AlignVCenter);
        cardLayout->addLayout(slotRow);

        // Row 2: Mode (`turn` / `prompt`) + max tokens
        auto* modeRow = new QHBoxLayout();
        modeRow->setContentsMargins(0, 0, 0, 0);
        modeRow->setSpacing(8);

        m_impl->modeCaption = new QLabel(QStringLiteral("Mode"), m_impl->consoleCard);
        m_impl->modeCaption->setObjectName(QStringLiteral("subagentsCaptionLabel"));
        modeRow->addWidget(m_impl->modeCaption, 0, Qt::AlignVCenter);

        m_impl->modeCombo = new QComboBox(m_impl->consoleCard);
        m_impl->modeCombo->setObjectName(QStringLiteral("subagentsModeCombo"));
        m_impl->modeCombo->setMinimumWidth(190);
        m_impl->modeCombo->addItem(QStringLiteral("turn (continue slot)"), QStringLiteral("turn"));
        m_impl->modeCombo->addItem(QStringLiteral("prompt (single prompt)"), QStringLiteral("prompt"));
        modeRow->addWidget(m_impl->modeCombo, 0, Qt::AlignVCenter);

        modeRow->addStretch(1);

        m_impl->maxTokensCaption = new QLabel(QStringLiteral("Max tokens"), m_impl->consoleCard);
        m_impl->maxTokensCaption->setObjectName(QStringLiteral("subagentsCaptionLabel"));
        modeRow->addWidget(m_impl->maxTokensCaption, 0, Qt::AlignVCenter);

        m_impl->maxTokensSpin = new QSpinBox(m_impl->consoleCard);
        m_impl->maxTokensSpin->setObjectName(QStringLiteral("subagentsMaxTokensSpin"));
        m_impl->maxTokensSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        m_impl->maxTokensSpin->setRange(64, 8192);
        m_impl->maxTokensSpin->setSingleStep(64);
        m_impl->maxTokensSpin->setValue(kDefaultMaxTokens);
        m_impl->maxTokensSpin->setMinimumWidth(92);
        modeRow->addWidget(m_impl->maxTokensSpin, 0, Qt::AlignVCenter);

        cardLayout->addLayout(modeRow);

        // Row 3: Prompt input
        m_impl->promptEdit = new QPlainTextEdit(m_impl->consoleCard);
        m_impl->promptEdit->setObjectName(QStringLiteral("subagentsPromptEdit"));
        m_impl->promptEdit->setPlaceholderText(
            QStringLiteral("Enter a prompt or steering message for the selected subagent slot\u2026 "
                           "(Ctrl/Cmd+Enter to send)"));
        m_impl->promptEdit->setFixedHeight(84);
        m_impl->promptEdit->setTabChangesFocus(true);
        cardLayout->addWidget(m_impl->promptEdit);

        // Row 4: Action buttons (Send / Read Lines / Duplicate -> target / Terminate)
        auto* actionsRow = new QHBoxLayout();
        actionsRow->setContentsMargins(0, 0, 0, 0);
        actionsRow->setSpacing(6);

        m_impl->sendButton = new QPushButton(QStringLiteral("Send"), m_impl->consoleCard);
        m_impl->sendButton->setObjectName(QStringLiteral("subagentsSendBtn"));
        m_impl->sendButton->setProperty("primary", true);
        actionsRow->addWidget(m_impl->sendButton);

        m_impl->readLinesButton = new QPushButton(QStringLiteral("Read Lines"), m_impl->consoleCard);
        m_impl->readLinesButton->setObjectName(QStringLiteral("subagentsReadLinesBtn"));
        actionsRow->addWidget(m_impl->readLinesButton);

        actionsRow->addStretch(1);

        m_impl->duplicateArrowLabel = new QLabel(QStringLiteral("\u2192"), m_impl->consoleCard);
        m_impl->duplicateArrowLabel->setObjectName(QStringLiteral("subagentsCaptionLabel"));
        actionsRow->addWidget(m_impl->duplicateArrowLabel, 0, Qt::AlignVCenter);

        m_impl->duplicateTargetCombo = new QComboBox(m_impl->consoleCard);
        m_impl->duplicateTargetCombo->setObjectName(QStringLiteral("subagentsDuplicateTargetCombo"));
        m_impl->duplicateTargetCombo->setMinimumWidth(84);
        actionsRow->addWidget(m_impl->duplicateTargetCombo);

        m_impl->duplicateButton = new QPushButton(QStringLiteral("Duplicate"), m_impl->consoleCard);
        m_impl->duplicateButton->setObjectName(QStringLiteral("subagentsDuplicateBtn"));
        m_impl->duplicateButton->setToolTip(
            QStringLiteral("Clone conversation history from the selected slot into the target slot"));
        actionsRow->addWidget(m_impl->duplicateButton);

        m_impl->terminateButton = new QPushButton(QStringLiteral("Terminate"), m_impl->consoleCard);
        m_impl->terminateButton->setObjectName(QStringLiteral("subagentsTerminateBtn"));
        m_impl->terminateButton->setProperty("danger", true);
        m_impl->terminateButton->setToolTip(
            QStringLiteral("Reset and clear the selected subagent slot"));
        actionsRow->addWidget(m_impl->terminateButton);

        cardLayout->addLayout(actionsRow);

        // Status / outcome feedback label
        m_impl->consoleStatusLabel = new QLabel(m_impl->consoleCard);
        m_impl->consoleStatusLabel->setObjectName(QStringLiteral("subagentsConsoleStatusLabel"));
        m_impl->consoleStatusLabel->setWordWrap(true);
        m_impl->consoleStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_impl->consoleStatusLabel->setVisible(false);
        cardLayout->addWidget(m_impl->consoleStatusLabel);

        m_impl->consoleHairline = new SpellHairline(Qt::Horizontal, m_impl->consoleCard);
        cardLayout->addWidget(m_impl->consoleHairline);

        // Output console header toolbar
        auto* outputHeaderRow = new QHBoxLayout();
        outputHeaderRow->setContentsMargins(0, 0, 0, 0);
        outputHeaderRow->setSpacing(8);

        m_impl->outputTitleLabel = new QLabel(
            QStringLiteral("TRANSCRIPT OUTPUT"), m_impl->consoleCard);
        m_impl->outputTitleLabel->setObjectName(QStringLiteral("subagentsSubHeaderLabel"));
        outputHeaderRow->addWidget(m_impl->outputTitleLabel, 0, Qt::AlignVCenter);

        m_impl->outputLineCountLabel = new QLabel(QStringLiteral("0 lines"), m_impl->consoleCard);
        m_impl->outputLineCountLabel->setObjectName(QStringLiteral("subagentsOutputCountLabel"));
        outputHeaderRow->addWidget(m_impl->outputLineCountLabel, 0, Qt::AlignVCenter);
        outputHeaderRow->addStretch(1);

        m_impl->copyOutputButton = new QPushButton(QStringLiteral("Copy"), m_impl->consoleCard);
        m_impl->copyOutputButton->setObjectName(QStringLiteral("subagentsCompactButton"));
        m_impl->copyOutputButton->setEnabled(false);
        outputHeaderRow->addWidget(m_impl->copyOutputButton, 0, Qt::AlignVCenter);

        m_impl->clearViewButton = new QPushButton(QStringLiteral("Clear View"), m_impl->consoleCard);
        m_impl->clearViewButton->setObjectName(QStringLiteral("subagentsCompactButton"));
        m_impl->clearViewButton->setEnabled(false);
        outputHeaderRow->addWidget(m_impl->clearViewButton, 0, Qt::AlignVCenter);

        cardLayout->addLayout(outputHeaderRow);

        // Monospaced output console
        m_impl->outputConsole = new QPlainTextEdit(m_impl->consoleCard);
        m_impl->outputConsole->setObjectName(QStringLiteral("subagentsOutputConsole"));
        m_impl->outputConsole->setReadOnly(true);
        m_impl->outputConsole->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        m_impl->outputConsole->setMinimumHeight(180);
        m_impl->outputConsole->setMaximumHeight(300);
        m_impl->outputConsole->setPlaceholderText(
            QStringLiteral("No transcript output for Slot 0 yet. Send a prompt or click Read Lines."));
        cardLayout->addWidget(m_impl->outputConsole);

        sectionLayout->addWidget(m_impl->consoleCard);
        rootLayout->addLayout(sectionLayout);
    }

    rootLayout->addStretch(1);

    // =========================================================================
    // Signal & Slot Wiring
    // =========================================================================

    auto saveSubagentModel = [this]() {
        const QString newSubagentModel = m_impl->subagentModelCombo->currentText().trimmed();

        QString baseUrl = m_impl->lastSnapshot.endpointBaseUrl.trimmed();
        if (baseUrl.isEmpty() && m_impl->client) {
            baseUrl = m_impl->client->currentSnapshot().endpointBaseUrl.trimmed();
        }
        if (baseUrl.isEmpty() && m_impl->store) {
            baseUrl = m_impl->store->endpointBaseUrl().trimmed();
        }

        QString primaryModel = m_impl->resolvedPrimaryModel();
        if (primaryModel.isEmpty() && m_impl->client) {
            primaryModel = m_impl->client->currentSnapshot().modelId.trimmed();
        }

        m_impl->lastSyncedSubagentModelId = newSubagentModel;
        m_impl->subagentModelDirty = false;
        if (m_impl->subagentModelCombo->currentText() != newSubagentModel) {
            m_impl->syncingUi = true;
            m_impl->subagentModelCombo->setEditText(newSubagentModel);
            m_impl->syncingUi = false;
        }
        m_impl->lastSnapshot.subagentModelId = newSubagentModel;
        m_impl->lastSnapshot.effectiveSubagentModelId =
            newSubagentModel.isEmpty() ? primaryModel : newSubagentModel;

        if (m_impl->store) {
            m_impl->store->setSubagentModelId(newSubagentModel);
            m_impl->store->sync();
        }
        if (m_impl->client) {
            m_impl->client->configureEndpoint(baseUrl, std::nullopt, primaryModel, newSubagentModel);
        }

        m_impl->populateModelDropdown(m_impl->cachedModels);
        m_impl->updateModelSummaryRows();
        m_impl->refreshPoolSlotTiles();

        if (newSubagentModel.isEmpty()) {
            const QString msg = primaryModel.isEmpty()
                                    ? QStringLiteral("Saved \u2014 subagents will inherit Primary Model ID.")
                                    : QStringLiteral("Saved \u2014 subagents now inherit Primary Model ID (%1).")
                                          .arg(primaryModel);
            m_impl->setModelFeedback(msg, FeedbackTone::Success);
        } else {
            m_impl->setModelFeedback(
                QStringLiteral("Saved secondary subagent model: %1").arg(newSubagentModel),
                FeedbackTone::Success);
        }
    };

    connect(m_impl->subagentModelCombo, &QComboBox::editTextChanged, this,
            [this](const QString& text) {
                if (m_impl->syncingUi) {
                    return;
                }
                m_impl->subagentModelDirty = (text.trimmed() != m_impl->lastSyncedSubagentModelId);
                m_impl->usePrimaryButton->setEnabled(!text.trimmed().isEmpty() ||
                                                     !m_impl->lastSyncedSubagentModelId.trimmed().isEmpty());
            });

    if (QLineEdit* le = m_impl->subagentModelCombo->lineEdit()) {
        connect(le, &QLineEdit::returnPressed, this, saveSubagentModel);
    }

    connect(m_impl->saveModelButton, &QPushButton::clicked, this, saveSubagentModel);

    connect(m_impl->usePrimaryButton, &QPushButton::clicked, this, [this, saveSubagentModel]() {
        m_impl->subagentModelCombo->setEditText(QString());
        saveSubagentModel();
    });

    connect(m_impl->refreshModelsButton, &QToolButton::clicked, this, [this]() {
        if (!m_impl->client) {
            m_impl->setModelFeedback(
                QStringLiteral("Core client unavailable; cannot fetch models."),
                FeedbackTone::Danger);
            return;
        }
        m_impl->modelFetchPending = true;
        m_impl->setModelFeedback(
            QStringLiteral("Fetching available models from configured endpoint\u2026"),
            FeedbackTone::Neutral);
        m_impl->client->fetchModels();
    });

    // Delegation protocol selector
    connect(m_impl->protocolCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) {
            return;
        }
        const QString proto =
            detail::normalizeSubagentProtocol(m_impl->protocolCombo->itemData(index).toString());
        m_impl->protocolDescLabel->setText(protocolDescription(proto));
        if (m_impl->syncingUi) {
            return;
        }
        m_impl->lastSnapshot.subagentProtocol = proto;
        if (m_impl->client) {
            m_impl->client->setSubagentProtocol(proto);
        }
    });

    // Gatekeeper Critic LLM Review toggle
    connect(m_impl->criticCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        m_impl->updateCriticVisuals(checked);
        if (m_impl->syncingUi) {
            return;
        }
        m_impl->lastSnapshot.slot2CriticEnabled = checked;
        if (m_impl->store) {
            m_impl->store->setSlot2CriticEnabled(checked);
            m_impl->store->sync();
        }
        if (m_impl->client) {
            m_impl->client->setSlot2CriticEnabled(checked);
        }
    });

    // Pool Refresh & Pool Slot Tile selection
    connect(m_impl->refreshPoolButton, &QPushButton::clicked, this, [this]() {
        if (m_impl->client) {
            m_impl->client->refreshSnapshot();
            m_impl->client->readSubagentSlotLines(
                m_impl->wireSlotForIndex(m_impl->selectedSlotIndex), kDefaultSlotReadLimit);
        }
    });

    auto selectManualSlot = [this](int slotIdx, bool requestLines) {
        if (slotIdx < 0 || slotIdx >= kManualSlotCount) {
            return;
        }
        m_impl->selectedSlotIndex = slotIdx;
        if (QAbstractButton* btn = m_impl->slotButtonGroup->button(slotIdx)) {
            const QSignalBlocker blocker(m_impl->slotButtonGroup);
            btn->setChecked(true);
        }
        m_impl->rebuildDuplicateTargetCombo();
        m_impl->updateSelectedSlotHeader();
        m_impl->refreshPoolSlotTiles();
        m_impl->renderSelectedSlotConsole(true);

        if (requestLines && m_impl->client) {
            m_impl->client->readSubagentSlotLines(
                m_impl->wireSlotForIndex(slotIdx), kDefaultSlotReadLimit);
        }
    };

    for (int i = 0; i < kManualSlotCount; ++i) {
        connect(m_impl->poolSlotTiles[static_cast<std::size_t>(i)], &QPushButton::clicked, this,
                [selectManualSlot, i]() { selectManualSlot(i, true); });
    }

    connect(m_impl->slotButtonGroup, &QButtonGroup::idClicked, this,
            [selectManualSlot](int id) { selectManualSlot(id, true); });

    // Send prompt to selected subagent slot
    auto sendSlotPrompt = [this]() {
        if (m_impl->slotBusy) {
            return;
        }
        const QString prompt = m_impl->promptEdit->toPlainText().trimmed();
        if (prompt.isEmpty()) {
            m_impl->setConsoleFeedback(
                QStringLiteral("Enter a prompt message before sending to Slot %1.")
                    .arg(m_impl->selectedSlotIndex),
                FeedbackTone::Danger);
            m_impl->promptEdit->setFocus();
            return;
        }
        if (!m_impl->client) {
            m_impl->setConsoleFeedback(
                QStringLiteral("Core client unavailable; cannot run subagent slot."),
                FeedbackTone::Danger);
            return;
        }

        const QString mode = m_impl->modeCombo->currentData().toString().trimmed();
        const QString effectiveMode = mode.isEmpty() ? QStringLiteral("turn") : mode;
        const int maxTokens = m_impl->maxTokensSpin->value();
        const int wireSlot = m_impl->wireSlotForIndex(m_impl->selectedSlotIndex);

        m_impl->setConsoleBusy(true);
        m_impl->setConsoleFeedback(
            QStringLiteral("Running Slot %1 (mode: %2, max %3 tokens)\u2026")
                .arg(m_impl->selectedSlotIndex)
                .arg(effectiveMode)
                .arg(maxTokens),
            FeedbackTone::Neutral);

        m_impl->client->runSubagentSlot(wireSlot, effectiveMode, prompt, maxTokens);
    };

    connect(m_impl->sendButton, &QPushButton::clicked, this, sendSlotPrompt);

    auto* sendReturnShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), m_impl->promptEdit);
    sendReturnShortcut->setContext(Qt::WidgetShortcut);
    connect(sendReturnShortcut, &QShortcut::activated, this, sendSlotPrompt);

    auto* sendEnterShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), m_impl->promptEdit);
    sendEnterShortcut->setContext(Qt::WidgetShortcut);
    connect(sendEnterShortcut, &QShortcut::activated, this, sendSlotPrompt);

    // Read Lines
    connect(m_impl->readLinesButton, &QPushButton::clicked, this, [this]() {
        if (!m_impl->client) {
            m_impl->setConsoleFeedback(
                QStringLiteral("Core client unavailable; cannot read slot transcript."),
                FeedbackTone::Danger);
            return;
        }
        const int wireSlot = m_impl->wireSlotForIndex(m_impl->selectedSlotIndex);
        m_impl->awaitingReadLinesFeedback = true;
        m_impl->setConsoleFeedback(
            QStringLiteral("Reading transcript lines for Slot %1\u2026")
                .arg(m_impl->selectedSlotIndex),
            FeedbackTone::Neutral);
        m_impl->client->readSubagentSlotLines(wireSlot, kDefaultSlotReadLimit);
    });

    // Duplicate
    connect(m_impl->duplicateButton, &QPushButton::clicked, this, [this]() {
        if (m_impl->slotBusy || m_impl->duplicateTargetCombo->count() == 0) {
            return;
        }
        const int dstIdx = m_impl->duplicateTargetCombo->currentData().toInt();
        if (dstIdx == m_impl->selectedSlotIndex || dstIdx < 0 || dstIdx >= kManualSlotCount) {
            return;
        }
        if (!m_impl->client) {
            m_impl->setConsoleFeedback(
                QStringLiteral("Core client unavailable; cannot duplicate slot."),
                FeedbackTone::Danger);
            return;
        }
        const int srcWire = m_impl->wireSlotForIndex(m_impl->selectedSlotIndex);
        const int dstWire = m_impl->wireSlotForIndex(dstIdx);
        m_impl->client->duplicateSubagentSlot(srcWire, dstWire);

        // Mirror cached lines optimistically into the destination slot until refreshed.
        m_impl->slotLinesCache[static_cast<std::size_t>(dstIdx)] =
            m_impl->slotLinesCache[static_cast<std::size_t>(m_impl->selectedSlotIndex)];
        m_impl->slotHasExplicitLines[static_cast<std::size_t>(dstIdx)] =
            m_impl->slotHasExplicitLines[static_cast<std::size_t>(m_impl->selectedSlotIndex)];

        m_impl->setConsoleFeedback(
            QStringLiteral("Duplicated Slot %1 \u2192 Slot %2.")
                .arg(m_impl->selectedSlotIndex)
                .arg(dstIdx),
            FeedbackTone::Success);
    });

    // Terminate
    connect(m_impl->terminateButton, &QPushButton::clicked, this, [this]() {
        if (m_impl->slotBusy) {
            return;
        }
        if (!m_impl->client) {
            m_impl->setConsoleFeedback(
                QStringLiteral("Core client unavailable; cannot terminate slot."),
                FeedbackTone::Danger);
            return;
        }
        const auto idx = static_cast<std::size_t>(m_impl->selectedSlotIndex);
        const int wireSlot = m_impl->wireSlotForIndex(m_impl->selectedSlotIndex);
        m_impl->client->terminateSubagentSlot(wireSlot);

        m_impl->slotLinesCache[idx].clear();
        m_impl->slotHasExplicitLines[idx] = true;
        m_impl->slotSnapshots[idx].state = QStringLiteral("idle");
        m_impl->slotSnapshots[idx].lastReply.clear();
        m_impl->slotSnapshots[idx].recentLines.clear();
        m_impl->slotSnapshots[idx].turnCount = 0;

        m_impl->updateSelectedSlotHeader();
        m_impl->refreshPoolSlotTiles();
        m_impl->renderSelectedSlotConsole(false);
        m_impl->setConsoleFeedback(
            QStringLiteral("Terminated and reset Slot %1 to idle.")
                .arg(m_impl->selectedSlotIndex),
            FeedbackTone::Success);
    });

    // Output console toolbar buttons
    connect(m_impl->copyOutputButton, &QPushButton::clicked, this, [this]() {
        const QString text = m_impl->outputConsole->toPlainText();
        if (text.isEmpty()) {
            return;
        }
        if (QClipboard* cb = QGuiApplication::clipboard()) {
            cb->setText(text);
        }
        const uint64_t gen = ++m_impl->copyOutputFeedbackGen;
        m_impl->copyOutputButton->setText(QStringLiteral("Copied"));
        QPointer<SubagentsSettingsPane> guard(this);
        QTimer::singleShot(1400, this, [guard, gen]() {
            if (!guard || guard->m_impl->copyOutputFeedbackGen != gen) {
                return;
            }
            guard->m_impl->copyOutputButton->setText(QStringLiteral("Copy"));
        });
    });

    connect(m_impl->clearViewButton, &QPushButton::clicked, this, [this]() {
        const auto idx = static_cast<std::size_t>(m_impl->selectedSlotIndex);
        m_impl->slotLinesCache[idx].clear();
        m_impl->slotHasExplicitLines[idx] = true;
        m_impl->renderSelectedSlotConsole(false);
    });

    // External signal synchronization
    if (client) {
        connect(client, &SpellCoreClient::modelsFetched, this,
                [this](const QVector<ModelDescriptor>& models, const QString& error) {
                    if (!models.isEmpty()) {
                        m_impl->cachedModels = models;
                        m_impl->populateModelDropdown(m_impl->cachedModels);
                    }
                    if (m_impl->modelFetchPending) {
                        m_impl->modelFetchPending = false;
                        if (!error.trimmed().isEmpty()) {
                            m_impl->setModelFeedback(
                                QStringLiteral("Model discovery failed: %1").arg(error.trimmed()),
                                FeedbackTone::Danger);
                        } else {
                            m_impl->setModelFeedback(
                                QStringLiteral("Loaded %1 %2 from endpoint.")
                                    .arg(models.size())
                                    .arg(models.size() == 1 ? QStringLiteral("model")
                                                            : QStringLiteral("models")),
                                FeedbackTone::Success);
                        }
                    }
                });
    }

    if (store) {
        connect(store, &SpellSettingsStore::endpointConfigChanged, this, [this]() {
            if (!m_impl->store) {
                return;
            }
            const QString storeSub = m_impl->store->subagentModelId().trimmed();
            if (!m_impl->subagentModelDirty && !isComboFocused(m_impl->subagentModelCombo)) {
                m_impl->syncingUi = true;
                m_impl->lastSyncedSubagentModelId = storeSub;
                m_impl->subagentModelCombo->setEditText(storeSub);
                m_impl->syncingUi = false;
            }
            m_impl->populateModelDropdown(m_impl->cachedModels);
            m_impl->updateModelSummaryRows();
            m_impl->refreshPoolSlotTiles();
        });
    }

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SubagentsSettingsPane::applyTheme);

    // =========================================================================
    // Initial Population
    // =========================================================================
    m_impl->syncingUi = true;
    if (store) {
        m_impl->lastSyncedSubagentModelId = store->subagentModelId().trimmed();
        m_impl->subagentModelCombo->setEditText(m_impl->lastSyncedSubagentModelId);
        const bool criticOn = store->slot2CriticEnabled();
        m_impl->criticCheckBox->setChecked(criticOn);
        m_impl->updateCriticVisuals(criticOn);
    } else {
        m_impl->updateCriticVisuals(false);
    }
    m_impl->syncingUi = false;

    m_impl->rebuildDuplicateTargetCombo();
    m_impl->updateModelSummaryRows();
    m_impl->updateSelectedSlotHeader();
    m_impl->refreshPoolSlotTiles();
    m_impl->rebuildActiveDelegationsList({});
    m_impl->renderSelectedSlotConsole(false);

    if (client) {
        const WorkspaceSnapshot initial = client->currentSnapshot();
        if (!initial.raw.isEmpty() || !initial.availableModels.isEmpty() ||
            !initial.subagentSlots.isEmpty() || !initial.modelId.isEmpty() ||
            !initial.endpointBaseUrl.isEmpty()) {
            updateSnapshot(initial);
        }
    }

    applyTheme();
}

SubagentsSettingsPane::~SubagentsSettingsPane() = default;

void SubagentsSettingsPane::updateSnapshot(const WorkspaceSnapshot& snap) {
    m_impl->lastSnapshot = snap;
    m_impl->hasSnapshot = true;

    // Merge discovered models while keeping previously fetched list if a transient snapshot is empty.
    if (!snap.availableModels.isEmpty()) {
        m_impl->cachedModels = snap.availableModels;
    }

    // Sync subagent model override (respecting unsaved user typing).
    const QString incomingSubModel = snap.subagentModelId.trimmed();
    if (!m_impl->subagentModelDirty && !isComboFocused(m_impl->subagentModelCombo)) {
        m_impl->syncingUi = true;
        m_impl->lastSyncedSubagentModelId = incomingSubModel;
        if (m_impl->subagentModelCombo->currentText().trimmed() != incomingSubModel) {
            m_impl->subagentModelCombo->setEditText(incomingSubModel);
        }
        m_impl->syncingUi = false;
    }

    m_impl->populateModelDropdown(m_impl->cachedModels);
    m_impl->updateModelSummaryRows();

    // Sync delegation protocol (`auto` / `native` / `json`).
    const QString normProto = detail::normalizeSubagentProtocol(snap.subagentProtocol);
    m_impl->syncingUi = true;
    const int protoIdx = m_impl->protocolCombo->findData(normProto);
    if (protoIdx >= 0 && m_impl->protocolCombo->currentIndex() != protoIdx) {
        m_impl->protocolCombo->setCurrentIndex(protoIdx);
    }
    m_impl->protocolDescLabel->setText(protocolDescription(normProto));

    // Sync Gatekeeper Critic LLM Review toggle (`slot2CriticEnabled`).
    const bool criticEnabled = snap.slot2CriticEnabled;
    if (m_impl->criticCheckBox->isChecked() != criticEnabled) {
        m_impl->criticCheckBox->setChecked(criticEnabled);
    }
    m_impl->updateCriticVisuals(criticEnabled);
    m_impl->syncingUi = false;

    // Sync local 4-slot pool statuses (adapting automatically to 1..4 or 0..3 wire slot IDs).
    if (!snap.subagentSlots.isEmpty()) {
        QVector<SubagentSlotStatus> sortedSlots = snap.subagentSlots;
        std::sort(sortedSlots.begin(), sortedSlots.end(),
                  [](const SubagentSlotStatus& a, const SubagentSlotStatus& b) {
                      return a.slot < b.slot;
                  });

        const bool usesZeroBased = std::any_of(
            sortedSlots.cbegin(), sortedSlots.cend(),
            [](const SubagentSlotStatus& s) { return s.slot == 0; });
        m_impl->hasSnapshotSlots = true;
        for (int i = 0; i < kManualSlotCount; ++i) {
            const auto idx = static_cast<std::size_t>(i);
            m_impl->slotWireIds[idx] = usesZeroBased ? i : (i + 1);
            m_impl->slotSnapshots[idx].slot = m_impl->slotWireIds[idx];
            m_impl->slotSnapshots[idx].state = QStringLiteral("idle");
        }

        for (const SubagentSlotStatus& st : sortedSlots) {
            const int mappedIdx = usesZeroBased ? st.slot : (st.slot - 1);
            if (mappedIdx >= 0 && mappedIdx < kManualSlotCount) {
                const auto idx = static_cast<std::size_t>(mappedIdx);
                m_impl->slotWireIds[idx] = st.slot;
                m_impl->slotSnapshots[idx] = st;
                if (!st.recentLines.isEmpty() && !m_impl->slotHasExplicitLines[idx]) {
                    m_impl->slotLinesCache[idx] = st.recentLines;
                }
            }
        }
    }

    m_impl->refreshPoolSlotTiles();
    m_impl->rebuildActiveDelegationsList(snap.activeSubagents);
    m_impl->updateSelectedSlotHeader();
    m_impl->renderSelectedSlotConsole(false);
}

void SubagentsSettingsPane::onSlotReply(int slot, const QString& reply, const QString& err) {
    m_impl->setConsoleBusy(false);

    const int slotIdx = m_impl->resolveSlotIndex(slot);
    const QString trimmedErr = err.trimmed();
    const QString displaySlotLabel =
        (slotIdx >= 0) ? QStringLiteral("Slot %1").arg(slotIdx) : QStringLiteral("Slot %1").arg(slot);

    if (!trimmedErr.isEmpty()) {
        if (slotIdx >= 0 && slotIdx < kManualSlotCount) {
            const auto idx = static_cast<std::size_t>(slotIdx);
            m_impl->slotSnapshots[idx].state = QStringLiteral("error");
            m_impl->slotLinesCache[idx].append(QStringLiteral("[error] %1").arg(trimmedErr));
            m_impl->slotHasExplicitLines[idx] = true;
        }
        m_impl->setConsoleFeedback(
            QStringLiteral("%1 error: %2").arg(displaySlotLabel, trimmedErr),
            FeedbackTone::Danger);
        m_impl->refreshPoolSlotTiles();
        m_impl->updateSelectedSlotHeader();
        if (slotIdx == m_impl->selectedSlotIndex) {
            m_impl->renderSelectedSlotConsole(true);
        }
        return;
    }

    if (slotIdx >= 0 && slotIdx < kManualSlotCount) {
        const auto idx = static_cast<std::size_t>(slotIdx);
        m_impl->slotSnapshots[idx].state = QStringLiteral("completed");
        m_impl->slotSnapshots[idx].lastReply = reply;
        if (!reply.isEmpty()) {
            const QStringList replyLines = reply.split(QLatin1Char('\n'));
            for (const QString& line : replyLines) {
                m_impl->slotLinesCache[idx].append(line);
            }
            m_impl->slotHasExplicitLines[idx] = true;
        }
    }

    const int charCount = static_cast<int>(reply.size());
    m_impl->setConsoleFeedback(
        QStringLiteral("%1 replied (%2 %3).")
            .arg(displaySlotLabel)
            .arg(charCount)
            .arg(charCount == 1 ? QStringLiteral("char") : QStringLiteral("chars")),
        FeedbackTone::Success);

    m_impl->refreshPoolSlotTiles();
    m_impl->updateSelectedSlotHeader();
    if (slotIdx == m_impl->selectedSlotIndex) {
        m_impl->renderSelectedSlotConsole(true);
    }

    // Fetch authoritative bounded line history from spell_core after completion.
    if (m_impl->client && slotIdx == m_impl->selectedSlotIndex) {
        m_impl->client->readSubagentSlotLines(
            m_impl->wireSlotForIndex(m_impl->selectedSlotIndex), kDefaultSlotReadLimit);
    }
}

void SubagentsSettingsPane::onSlotLines(int slot, const QStringList& lines) {
    const int slotIdx = m_impl->resolveSlotIndex(slot);
    if (slotIdx < 0 || slotIdx >= kManualSlotCount) {
        return;
    }

    const auto idx = static_cast<std::size_t>(slotIdx);
    m_impl->slotLinesCache[idx] = lines;
    m_impl->slotHasExplicitLines[idx] = true;
    m_impl->slotSnapshots[idx].recentLines = lines;

    m_impl->refreshPoolSlotTiles();
    if (slotIdx == m_impl->selectedSlotIndex) {
        m_impl->renderSelectedSlotConsole(true);
        if (m_impl->awaitingReadLinesFeedback) {
            m_impl->awaitingReadLinesFeedback = false;
            if (lines.isEmpty()) {
                m_impl->setConsoleFeedback(
                    QStringLiteral("Slot %1 has no transcript lines yet.").arg(slotIdx),
                    FeedbackTone::Neutral);
            } else {
                const int count = static_cast<int>(lines.size());
                m_impl->setConsoleFeedback(
                    QStringLiteral("Loaded %1 transcript %2 for Slot %3.")
                        .arg(count)
                        .arg(count == 1 ? QStringLiteral("line") : QStringLiteral("lines"))
                        .arg(slotIdx),
                    FeedbackTone::Success);
            }
        }
    }
}

void SubagentsSettingsPane::applyTheme() {
    const ThemeManager& tm = ThemeManager::instance();
    const UiPalette u = tm.ui();

    const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
    const QColor solidSelected = compositeOver(u.selectedFill, u.background);
    const QColor solidBorder = compositeOver(u.subtleBorder, u.background);
    const QColor solidDivider = compositeOver(u.divider, u.background);
    const QColor solidSecondary = compositeOver(u.textSecondary, u.background);
    const QColor solidMuted = compositeOver(u.textMuted, u.background);

    // Typography
    QFont monoUi = tm.editorFont();
    monoUi.setPointSizeF(std::clamp(tm.editorFontSize() - 1.5, 10.0, 12.0));
    m_impl->subagentModelCombo->setFont(monoUi);
    m_impl->promptEdit->setFont(monoUi);

    QFont consoleFont = tm.editorFont();
    consoleFont.setPointSizeF(std::clamp(tm.editorFontSize() - 2.0, 10.0, 12.0));
    m_impl->outputConsole->setFont(consoleFont);

    QFont smallMono = tm.editorFont();
    smallMono.setPointSizeF(10.0);
    m_impl->modelStatusLabel->setFont(smallMono);
    m_impl->consoleStatusLabel->setFont(smallMono);
    m_impl->outputLineCountLabel->setFont(smallMono);
    for (QLabel* meta : m_impl->poolSlotMetaLabels) {
        if (meta) {
            meta->setFont(smallMono);
        }
    }

    QFont subHeaderFont = tm.editorFont();
    subHeaderFont.setPointSizeF(9.5);
    subHeaderFont.setWeight(QFont::DemiBold);
    subHeaderFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    m_impl->delegationsTitleLabel->setFont(subHeaderFont);
    m_impl->outputTitleLabel->setFont(subHeaderFont);

    // Re-tint monochrome vector icons
    m_impl->saveModelButton->setIcon(icons::vectorIcon(QStringLiteral("save"), u.textPrimary, 13));
    m_impl->saveModelButton->setIconSize(QSize(13, 13));
    m_impl->refreshPoolButton->setIcon(icons::vectorIcon(QStringLiteral("refresh"), u.textPrimary, 12));
    m_impl->refreshPoolButton->setIconSize(QSize(12, 12));
    m_impl->sendButton->setIcon(icons::vectorIcon(QStringLiteral("send"), u.textPrimary, 13));
    m_impl->sendButton->setIconSize(QSize(13, 13));
    m_impl->readLinesButton->setIcon(icons::vectorIcon(QStringLiteral("refresh"), u.textPrimary, 13));
    m_impl->readLinesButton->setIconSize(QSize(13, 13));
    m_impl->duplicateButton->setIcon(icons::vectorIcon(QStringLiteral("copy"), u.textPrimary, 13));
    m_impl->duplicateButton->setIconSize(QSize(13, 13));
    m_impl->terminateButton->setIcon(icons::vectorIcon(QStringLiteral("stop"), u.danger, 13));
    m_impl->terminateButton->setIconSize(QSize(13, 13));
    m_impl->copyOutputButton->setIcon(icons::vectorIcon(QStringLiteral("copy"), u.textPrimary, 12));
    m_impl->copyOutputButton->setIconSize(QSize(12, 12));
    m_impl->clearViewButton->setIcon(icons::vectorIcon(QStringLiteral("close"), u.textPrimary, 12));
    m_impl->clearViewButton->setIconSize(QSize(12, 12));

    // Scoped monochrome QSS matching Spell IDE palette
    const QString style = QStringLiteral(
        "QWidget#subagentsSettingsPane {\n"
        "  background-color: %1;\n"
        "}\n"
        "QFrame#subagentsModelCard, QFrame#subagentsProtocolCard,\n"
        "QFrame#subagentsCriticCard, QFrame#subagentsPoolCard,\n"
        "QFrame#subagentsConsoleCard {\n"
        "  background-color: %2;\n"
        "  border: 1px solid %3;\n"
        "  border-radius: 8px;\n"
        "}\n"
        "QLabel#subagentsCaptionLabel {\n"
        "  color: %4;\n"
        "  background-color: transparent;\n"
        "  font-size: 12px;\n"
        "}\n"
        "QLabel#subagentsHintLabel {\n"
        "  color: %5;\n"
        "  background-color: transparent;\n"
        "  font-size: 11px;\n"
        "}\n"
        "QLabel#subagentsSubHeaderLabel, QLabel#subagentsOutputCountLabel {\n"
        "  color: %5;\n"
        "  background-color: transparent;\n"
        "}\n"
        "QPushButton#subagentsSlotTile {\n"
        "  background-color: %6;\n"
        "  border: 1px solid %3;\n"
        "  border-radius: 7px;\n"
        "  padding: 0px;\n"
        "  text-align: left;\n"
        "}\n"
        "QPushButton#subagentsSlotTile:hover {\n"
        "  background-color: %7;\n"
        "  border-color: %8;\n"
        "}\n"
        "QPushButton#subagentsSlotTile:checked {\n"
        "  background-color: %7;\n"
        "  border: 1px solid %8;\n"
        "}\n"
        "QLabel#subagentsSlotTileTitle {\n"
        "  color: %9;\n"
        "  background-color: transparent;\n"
        "}\n"
        "QLabel#subagentsSlotTileMeta {\n"
        "  color: %5;\n"
        "  background-color: transparent;\n"
        "}\n"
        "QPushButton#subagentsSlotPickerBtn {\n"
        "  background-color: %2;\n"
        "  color: %4;\n"
        "  border: 1px solid %3;\n"
        "  border-radius: 6px;\n"
        "  padding: 4px 10px;\n"
        "  font-size: 11.5px;\n"
        "  font-weight: 500;\n"
        "}\n"
        "QPushButton#subagentsSlotPickerBtn:hover {\n"
        "  background-color: %6;\n"
        "  color: %9;\n"
        "}\n"
        "QPushButton#subagentsSlotPickerBtn:checked {\n"
        "  background-color: %7;\n"
        "  border: 1px solid %8;\n"
        "  color: %9;\n"
        "  font-weight: 600;\n"
        "}\n"
        "QPushButton#subagentsCompactButton, QPushButton#subagentsUsePrimaryBtn {\n"
        "  background-color: %2;\n"
        "  color: %9;\n"
        "  border: 1px solid %3;\n"
        "  border-radius: 5px;\n"
        "  padding: 3px 9px;\n"
        "  font-size: 11px;\n"
        "  font-weight: 500;\n"
        "}\n"
        "QPushButton#subagentsCompactButton:hover, QPushButton#subagentsUsePrimaryBtn:hover {\n"
        "  background-color: %6;\n"
        "  border-color: %8;\n"
        "}\n"
        "QPushButton#subagentsCompactButton:disabled, QPushButton#subagentsUsePrimaryBtn:disabled {\n"
        "  color: %5;\n"
        "  border-color: %10;\n"
        "  background-color: transparent;\n"
        "}\n"
        "QPlainTextEdit#subagentsOutputConsole {\n"
        "  background-color: %2;\n"
        "  color: %9;\n"
        "  border: 1px solid %3;\n"
        "  border-radius: 6px;\n"
        "  padding: 8px 10px;\n"
        "  selection-background-color: %7;\n"
        "  selection-color: %9;\n"
        "}\n"
        "QWidget#subagentsActiveDelegationsContainer {\n"
        "  background-color: transparent;\n"
        "}\n"
    ).arg(
        qssColor(u.background),      // %1
        qssColor(u.surface),         // %2
        qssColor(solidBorder),       // %3
        qssColor(solidSecondary),    // %4
        qssColor(solidMuted),        // %5
        qssColor(solidElevated),     // %6
        qssColor(solidSelected),     // %7
        qssColor(u.selectedBorder),  // %8
        qssColor(u.textPrimary),     // %9
        qssColor(solidDivider)       // %10
    );
    setStyleSheet(style);

    // Propagate theme updates to reusable Spell widgets
    m_impl->modelHeader->applyTheme();
    m_impl->refreshModelsButton->applyTheme();
    m_impl->modelHairline->applyTheme();
    m_impl->primaryModelRow->applyTheme();
    m_impl->effectiveModelRow->applyTheme();

    m_impl->protocolHeader->applyTheme();

    m_impl->criticHeader->applyTheme();
    m_impl->criticBadge->applyTheme();

    m_impl->poolHeader->applyTheme();
    m_impl->poolHairline->applyTheme();
    for (SpellBadge* badge : m_impl->poolSlotBadges) {
        if (badge) {
            badge->applyTheme();
        }
    }

    m_impl->consoleHeader->applyTheme();
    m_impl->selectedSlotBadge->applyTheme();
    m_impl->consoleHairline->applyTheme();

    m_impl->rebuildActiveDelegationsList(m_impl->lastSnapshot.activeSubagents);
    m_impl->applyFeedbackColors();
}

} // namespace spell
