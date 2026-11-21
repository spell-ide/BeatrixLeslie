#include "settings/EngineSettingsPane.h"

#include "core/SpellCoreClient.h"
#include "core/SpellSettingsStore.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace spell {

namespace {

constexpr const char* kPresetOpenAiUrl     = "https://api.openai.com/v1";
constexpr const char* kPresetOpenRouterUrl = "https://openrouter.ai/api/v1";
constexpr const char* kPresetOllamaUrl     = "http://localhost:11434/v1";
constexpr const char* kPresetLmStudioUrl   = "http://localhost:1234/v1";

struct EndpointValidation {
    bool isEmpty = false;
    bool isValid = false;
    bool autoAppendsV1 = false;
    bool strippedSuffix = false;
    QString normalizedBaseUrl;
    QString resolvedModelsUrl;
    QString resolvedChatUrl;
    QString errorMessage;
};

bool isAsciiHostChar(QChar ch) {
    const ushort u = ch.unicode();
    return (u >= 'a' && u <= 'z') ||
           (u >= 'A' && u <= 'Z') ||
           (u >= '0' && u <= '9') ||
           u == '.' || u == '-' || u == ':';
}

EndpointValidation validateAndNormalizeEndpoint(const QString& rawInput) {
    EndpointValidation out;
    const QString trimmed = rawInput.trimmed();
    if (trimmed.isEmpty()) {
        out.isEmpty = true;
        out.isValid = true;
        return out;
    }

    for (const QChar ch : trimmed) {
        if (ch.isSpace() || ch.category() == QChar::Other_Control) {
            out.errorMessage = QStringLiteral(
                "Endpoint URL must not contain spaces or control characters.");
            return out;
        }
    }

    // Optional unix:// socket target (supported by core transport for local IPC/tests)
    if (trimmed.startsWith(QLatin1StringView("unix://"), Qt::CaseInsensitive) ||
        trimmed.startsWith(QLatin1StringView("http+unix://"), Qt::CaseInsensitive)) {
        out.isValid = true;
        out.normalizedBaseUrl = trimmed;
        out.resolvedModelsUrl = QStringLiteral("http://localhost/v1/models");
        out.resolvedChatUrl = QStringLiteral("http://localhost/v1/chat/completions");
        return out;
    }

    QString scheme;
    QString rest;
    if (trimmed.startsWith(QLatin1StringView("https://"), Qt::CaseInsensitive)) {
        scheme = QStringLiteral("https");
        rest = trimmed.mid(8);
    } else if (trimmed.startsWith(QLatin1StringView("http://"), Qt::CaseInsensitive)) {
        scheme = QStringLiteral("http");
        rest = trimmed.mid(7);
    } else {
        out.errorMessage = QStringLiteral(
            "Endpoint URL must start with http:// or https://.");
        return out;
    }

    if (rest.isEmpty()) {
        out.errorMessage = QStringLiteral("Endpoint URL is missing a hostname.");
        return out;
    }
    if (rest.contains(QLatin1Char('?')) || rest.contains(QLatin1Char('#'))) {
        out.errorMessage = QStringLiteral(
            "Endpoint base URL must not contain query parameters (?) or fragments (#).");
        return out;
    }

    const qsizetype slashIdx = rest.indexOf(QLatin1Char('/'));
    const QString authority = (slashIdx >= 0) ? rest.left(slashIdx) : rest;
    QString path = (slashIdx >= 0) ? rest.mid(slashIdx) : QString();

    if (authority.isEmpty()) {
        out.errorMessage = QStringLiteral("Endpoint URL is missing a hostname.");
        return out;
    }
    if (authority.contains(QLatin1Char('@'))) {
        out.errorMessage = QStringLiteral(
            "Do not embed credentials (@) in the URL. Use the OS Keychain API Key field instead.");
        return out;
    }

    QString host;
    if (authority.startsWith(QLatin1Char('['))) {
        const qsizetype closeIdx = authority.indexOf(QLatin1Char(']'));
        if (closeIdx <= 1) {
            out.errorMessage = QStringLiteral("Malformed IPv6 host in endpoint URL.");
            return out;
        }
        host = authority.mid(1, closeIdx - 1);
        const QString suffix = authority.mid(closeIdx + 1);
        if (!suffix.isEmpty()) {
            if (!suffix.startsWith(QLatin1Char(':'))) {
                out.errorMessage = QStringLiteral("Unexpected characters after IPv6 host.");
                return out;
            }
            bool ok = false;
            const int port = suffix.mid(1).toInt(&ok);
            if (!ok || port < 1 || port > 65535) {
                out.errorMessage = QStringLiteral("Port number must be between 1 and 65535.");
                return out;
            }
        }
    } else {
        const qsizetype colonIdx = authority.lastIndexOf(QLatin1Char(':'));
        if (colonIdx >= 0) {
            host = authority.left(colonIdx);
            if (host.isEmpty() || host.contains(QLatin1Char(':'))) {
                out.errorMessage = QStringLiteral("Malformed host:port authority in endpoint URL.");
                return out;
            }
            bool ok = false;
            const int port = authority.mid(colonIdx + 1).toInt(&ok);
            if (!ok || port < 1 || port > 65535) {
                out.errorMessage = QStringLiteral("Port number must be between 1 and 65535.");
                return out;
            }
        } else {
            host = authority;
        }
    }

    if (host.isEmpty() ||
        !std::all_of(host.cbegin(), host.cend(), isAsciiHostChar)) {
        out.errorMessage = QStringLiteral("Endpoint URL contains an invalid hostname.");
        return out;
    }

    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }

    for (;;) {
        const QString lower = path.toLower();
        if (lower.endsWith(QLatin1StringView("/chat/completions"))) {
            path.chop(17);
            out.strippedSuffix = true;
            while (path.endsWith(QLatin1Char('/'))) {
                path.chop(1);
            }
        } else if (lower.endsWith(QLatin1StringView("/models"))) {
            path.chop(7);
            out.strippedSuffix = true;
            while (path.endsWith(QLatin1Char('/'))) {
                path.chop(1);
            }
        } else {
            break;
        }
    }

    out.isValid = true;
    out.normalizedBaseUrl = scheme + QStringLiteral("://") + authority + path;
    if (path.isEmpty()) {
        out.autoAppendsV1 = true;
        out.resolvedModelsUrl = out.normalizedBaseUrl + QStringLiteral("/v1/models");
        out.resolvedChatUrl = out.normalizedBaseUrl + QStringLiteral("/v1/chat/completions");
    } else {
        out.autoAppendsV1 = false;
        out.resolvedModelsUrl = out.normalizedBaseUrl + QStringLiteral("/models");
        out.resolvedChatUrl = out.normalizedBaseUrl + QStringLiteral("/chat/completions");
    }
    return out;
}

QString formatContextWindow(qint64 tokens) {
    if (tokens <= 0) {
        return {};
    }
    if (tokens >= 1000000) {
        return QStringLiteral("%1M tokens").arg(static_cast<double>(tokens) / 1000000.0, 0, 'f', 1);
    }
    if (tokens >= 1000) {
        const qint64 k = (tokens + 500) / 1000;
        return QStringLiteral("%1K tokens").arg(k);
    }
    return QStringLiteral("%1 tokens").arg(tokens);
}

QString formatModelTooltip(const ModelDescriptor& model) {
    QStringList parts;
    parts.append(model.id);
    if (!model.ownedBy.isEmpty()) {
        parts.append(QStringLiteral("provider: %1").arg(model.ownedBy));
    }
    const QString ctx = formatContextWindow(model.contextWindow);
    if (!ctx.isEmpty()) {
        parts.append(QStringLiteral("context: %1").arg(ctx));
    }
    return parts.join(QStringLiteral("  ·  "));
}

void repolish(QWidget* w) {
    if (!w) {
        return;
    }
    if (QStyle* st = w->style()) {
        st->unpolish(w);
        st->polish(w);
    }
    w->update();
}

void setRoleProperty(QLabel* label, const char* role) {
    if (!label) {
        return;
    }
    label->setProperty("spellRole", QString::fromLatin1(role));
    repolish(label);
}

QFrame* createCardFrame(QWidget* parent, QVBoxLayout** outLayout) {
    auto* frame = new QFrame(parent);
    frame->setProperty("spellCard", true);
    frame->setFrameShape(QFrame::NoFrame);
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(9);
    if (outLayout) {
        *outLayout = layout;
    }
    return frame;
}

QString canonicalCompareUrl(const QString& url) {
    QString s = url.trimmed().toLower();
    while (s.endsWith(QLatin1Char('/'))) {
        s.chop(1);
    }
    return s;
}

bool isComboFocused(const QComboBox* combo) {
    if (!combo) {
        return false;
    }
    return combo->hasFocus() || (combo->lineEdit() && combo->lineEdit()->hasFocus());
}

} // namespace

struct EngineSettingsPane::Impl {
    SpellCoreClient* client = nullptr;
    SpellSettingsStore* store = nullptr;

    // Section headers & hairlines for theme updates
    QVector<SpellSectionHeader*> sectionHeaders;
    QVector<SpellHairline*> hairlines;
    QVector<SpellValueRow*> valueRows;

    // Connection status card
    SpellBadge* statusBadge = nullptr;
    QLabel* latencyLabel = nullptr;
    SpellIconButton* refreshStatusButton = nullptr;
    SpellValueRow* activeEndpointRow = nullptr;
    SpellValueRow* activeModelRow = nullptr;
    SpellValueRow* activeSubagentModelRow = nullptr;
    QLabel* errorLabel = nullptr;

    // Endpoint & presets card
    QPushButton* presetOpenAiButton = nullptr;
    QPushButton* presetOpenRouterButton = nullptr;
    QPushButton* presetOllamaButton = nullptr;
    QPushButton* presetLmStudioButton = nullptr;
    QPushButton* presetClearButton = nullptr;
    QLineEdit* baseUrlEdit = nullptr;
    QLabel* urlHelpLabel = nullptr;
    QLabel* normalizedPreviewLabel = nullptr;
    QPushButton* testConnectionButton = nullptr;
    QLabel* testFeedbackLabel = nullptr;

    // API key (OS Keychain) card
    SpellBadge* apiKeyStatusBadge = nullptr;
    QLineEdit* apiKeyEdit = nullptr;
    QPushButton* showHideKeyButton = nullptr;
    QPushButton* saveKeyButton = nullptr;
    QPushButton* clearKeyButton = nullptr;
    QLabel* apiKeyFeedbackLabel = nullptr;
    QLabel* apiKeyNoteLabel = nullptr;

    // Model & critic card
    SpellBadge* discoveredModelsBadge = nullptr;
    QComboBox* primaryModelCombo = nullptr;
    QLabel* primaryModelMetaLabel = nullptr;
    QComboBox* subagentModelCombo = nullptr;
    QLabel* subagentModelHintLabel = nullptr;
    QCheckBox* slot2CriticCheck = nullptr;
    QLabel* slot2CriticHintLabel = nullptr;
    QPushButton* saveEndpointButton = nullptr;
    QLabel* saveFeedbackLabel = nullptr;

    // State tracking
    WorkspaceSnapshot lastSnapshot;
    QVector<ModelDescriptor> discoveredModels;
    QElapsedTimer testTimer;
    bool testInFlight = false;
    int lastMeasuredLatencyMs = -1;

    bool hasInitializedFromSnapshot = false;
    bool baseUrlDirty = false;
    bool primaryModelDirty = false;
    bool subagentModelDirty = false;
    QString lastAppliedBaseUrl;
    QString lastAppliedPrimaryModel;
    QString lastAppliedSubagentModel;
    bool lastAppliedSlot2Critic = false;

    void setErrorText(const QString& text) {
        const QString trimmed = text.trimmed();
        errorLabel->setText(trimmed);
        errorLabel->setVisible(!trimmed.isEmpty());
    }

    void setFeedback(QLabel* label, const QString& text, const char* role) {
        if (!label) {
            return;
        }
        label->setText(text);
        label->setVisible(!text.isEmpty());
        setRoleProperty(label, role);
    }

    void updateConnectionBadge(const QString& text, BadgeTone tone) {
        if (!statusBadge) {
            return;
        }
        statusBadge->setText(text);
        statusBadge->setTone(tone);
        statusBadge->setShowDot(true);
    }

    void updateLatencyDisplay(int latencyMs) {
        if (!latencyLabel) {
            return;
        }
        if (latencyMs >= 0) {
            latencyLabel->setText(QStringLiteral("%1 ms").arg(latencyMs));
        } else {
            latencyLabel->setText(QStringLiteral("—"));
        }
    }

    void refreshApiKeyStatusUi() {
        const bool hasKey = (store != nullptr) && store->hasEndpointApiKey();
        if (apiKeyStatusBadge) {
            apiKeyStatusBadge->setText(hasKey ? QStringLiteral("SAVED IN KEYCHAIN")
                                              : QStringLiteral("NOT SET"));
            apiKeyStatusBadge->setTone(hasKey ? BadgeTone::Success : BadgeTone::Neutral);
            apiKeyStatusBadge->setShowDot(true);
        }
        if (apiKeyEdit) {
            apiKeyEdit->setPlaceholderText(
                hasKey
                    ? QStringLiteral("Saved in OS Keychain — enter a new key to replace…")
                    : QStringLiteral("sk-... (optional for keyless local servers)"));
        }
        if (clearKeyButton) {
            clearKeyButton->setEnabled(hasKey || (apiKeyEdit && !apiKeyEdit->text().trimmed().isEmpty()));
        }
    }

    void updatePresetHighlights() {
        const QString cur = canonicalCompareUrl(baseUrlEdit ? baseUrlEdit->text() : QString());
        auto applyActive = [](QPushButton* btn, bool active) {
            if (!btn) {
                return;
            }
            if (btn->property("presetActive").toBool() != active) {
                btn->setProperty("presetActive", active);
                repolish(btn);
            }
        };

        applyActive(presetOpenAiButton,
                    cur == QLatin1StringView("https://api.openai.com/v1") ||
                        cur == QLatin1StringView("https://api.openai.com"));
        applyActive(presetOpenRouterButton,
                    cur == QLatin1StringView("https://openrouter.ai/api/v1"));
        applyActive(presetOllamaButton,
                    cur == QLatin1StringView("http://localhost:11434/v1") ||
                        cur == QLatin1StringView("http://localhost:11434") ||
                        cur == QLatin1StringView("http://127.0.0.1:11434/v1") ||
                        cur == QLatin1StringView("http://127.0.0.1:11434"));
        applyActive(presetLmStudioButton,
                    cur == QLatin1StringView("http://localhost:1234/v1") ||
                        cur == QLatin1StringView("http://localhost:1234") ||
                        cur == QLatin1StringView("http://127.0.0.1:1234/v1") ||
                        cur == QLatin1StringView("http://127.0.0.1:1234"));
        applyActive(presetClearButton, cur.isEmpty());
    }

    void updateNormalizedPreview() {
        if (!normalizedPreviewLabel || !baseUrlEdit) {
            return;
        }
        const EndpointValidation v = validateAndNormalizeEndpoint(baseUrlEdit->text());
        if (v.isEmpty) {
            normalizedPreviewLabel->setText(QStringLiteral(
                "Unconfigured — BeatrixLeslie IDE performs zero network requests until an endpoint is saved or tested."));
            setRoleProperty(normalizedPreviewLabel, "hint");
            return;
        }
        if (!v.isValid) {
            normalizedPreviewLabel->setText(v.errorMessage);
            setRoleProperty(normalizedPreviewLabel, "previewError");
            return;
        }

        QString note;
        if (v.autoAppendsV1) {
            note = QStringLiteral("  (/v1 auto-appended)");
        } else if (v.strippedSuffix) {
            note = QStringLiteral("  (endpoint suffix normalized)");
        }
        normalizedPreviewLabel->setText(
            QStringLiteral("Resolves to: GET %1%2").arg(v.resolvedModelsUrl, note));
        setRoleProperty(normalizedPreviewLabel, "preview");
    }

    void updateSelectedModelMetadata() {
        if (!primaryModelMetaLabel || !primaryModelCombo) {
            return;
        }
        const QString selected = primaryModelCombo->currentText().trimmed();
        if (selected.isEmpty()) {
            primaryModelMetaLabel->setText(QStringLiteral(
                "Choose a discovered model or type any custom model ID supported by your endpoint."));
            setRoleProperty(primaryModelMetaLabel, "hint");
            return;
        }

        for (const ModelDescriptor& m : discoveredModels) {
            if (m.id == selected) {
                QStringList parts;
                if (!m.ownedBy.isEmpty()) {
                    parts.append(QStringLiteral("Provider: %1").arg(m.ownedBy));
                }
                const QString ctx = formatContextWindow(m.contextWindow);
                if (!ctx.isEmpty()) {
                    parts.append(QStringLiteral("Context: %1").arg(ctx));
                }
                if (parts.isEmpty()) {
                    primaryModelMetaLabel->setText(
                        QStringLiteral("Verified on endpoint via GET /models."));
                } else {
                    primaryModelMetaLabel->setText(parts.join(QStringLiteral("  ·  ")));
                }
                setRoleProperty(primaryModelMetaLabel, "preview");
                return;
            }
        }

        if (!discoveredModels.isEmpty()) {
            primaryModelMetaLabel->setText(QStringLiteral(
                "Custom model ID (manual entry — not listed in last GET /models response)."));
        } else {
            primaryModelMetaLabel->setText(QStringLiteral(
                "Custom model ID — click Test Connection & Fetch Models to discover server models."));
        }
        setRoleProperty(primaryModelMetaLabel, "hint");
    }

    void populateModelCombo(QComboBox* combo, const QVector<ModelDescriptor>& models) {
        if (!combo) {
            return;
        }
        const QString preservedText = combo->currentText();
        const bool hadFocus = isComboFocused(combo);
        const int cursorPos = combo->lineEdit() ? combo->lineEdit()->cursorPosition() : -1;

        const QSignalBlocker comboBlocker(combo);
        std::optional<QSignalBlocker> editBlocker;
        if (combo->lineEdit()) {
            editBlocker.emplace(combo->lineEdit());
        }

        combo->clear();
        QStringList seen;
        seen.reserve(models.size());
        for (const ModelDescriptor& m : models) {
            const QString id = m.id.trimmed();
            if (id.isEmpty() || seen.contains(id)) {
                continue;
            }
            seen.append(id);
            combo->addItem(id);
            const int idx = combo->count() - 1;
            combo->setItemData(idx, formatModelTooltip(m), Qt::ToolTipRole);
        }

        const int matchIdx = combo->findText(preservedText, Qt::MatchExactly);
        if (matchIdx >= 0) {
            combo->setCurrentIndex(matchIdx);
        } else {
            combo->setCurrentIndex(-1);
            combo->setEditText(preservedText);
        }
        if (hadFocus && combo->lineEdit() && cursorPos >= 0) {
            combo->lineEdit()->setCursorPosition(
                std::min(cursorPos, static_cast<int>(preservedText.size())));
        }
    }

    void updateDiscoveredModelsUi(const QVector<ModelDescriptor>& models, bool autoSelectPrimaryIfEmpty) {
        discoveredModels = models;
        populateModelCombo(primaryModelCombo, models);
        populateModelCombo(subagentModelCombo, models);

        if (autoSelectPrimaryIfEmpty && primaryModelCombo &&
            primaryModelCombo->currentText().trimmed().isEmpty() && !models.isEmpty()) {
            const QString firstId = models.first().id.trimmed();
            if (!firstId.isEmpty()) {
                const QSignalBlocker comboBlocker(primaryModelCombo);
                const int idx = primaryModelCombo->findText(firstId, Qt::MatchExactly);
                if (idx >= 0) {
                    primaryModelCombo->setCurrentIndex(idx);
                } else {
                    primaryModelCombo->setEditText(firstId);
                }
                primaryModelDirty = true;
            }
        }

        if (discoveredModelsBadge) {
            const int count = primaryModelCombo ? primaryModelCombo->count() : 0;
            if (count == 0) {
                discoveredModelsBadge->setText(QStringLiteral("MANUAL ENTRY"));
                discoveredModelsBadge->setTone(BadgeTone::Neutral);
            } else if (count == 1) {
                discoveredModelsBadge->setText(QStringLiteral("1 MODEL"));
                discoveredModelsBadge->setTone(BadgeTone::Info);
            } else {
                discoveredModelsBadge->setText(QStringLiteral("%1 MODELS").arg(count));
                discoveredModelsBadge->setTone(BadgeTone::Info);
            }
        }

        updateSelectedModelMetadata();
    }

    void updateSummaryRows(const WorkspaceSnapshot& snap) {
        const QString activeUrl = snap.endpointBaseUrl.trimmed();
        if (activeEndpointRow) {
            activeEndpointRow->setValue(activeUrl.isEmpty() ? QStringLiteral("Not configured") : activeUrl);
            activeEndpointRow->setTone(activeUrl.isEmpty() ? BadgeTone::Neutral : BadgeTone::Info);
            activeEndpointRow->setValueIsPrimary(!activeUrl.isEmpty());
        }

        const QString activeModel = snap.modelId.trimmed();
        if (activeModelRow) {
            activeModelRow->setValue(activeModel.isEmpty() ? QStringLiteral("Not selected") : activeModel);
            activeModelRow->setTone(activeModel.isEmpty() ? BadgeTone::Neutral : BadgeTone::Info);
            activeModelRow->setValueIsPrimary(!activeModel.isEmpty());
        }

        if (activeSubagentModelRow) {
            const QString subOverride = snap.subagentModelId.trimmed();
            const QString effective = snap.effectiveSubagentModelId.trimmed();
            if (!subOverride.isEmpty()) {
                activeSubagentModelRow->setValue(subOverride);
                activeSubagentModelRow->setTone(BadgeTone::Info);
                activeSubagentModelRow->setValueIsPrimary(true);
            } else if (!effective.isEmpty()) {
                activeSubagentModelRow->setValue(QStringLiteral("%1 (inherited)").arg(effective));
                activeSubagentModelRow->setTone(BadgeTone::Neutral);
                activeSubagentModelRow->setValueIsPrimary(false);
            } else if (!activeModel.isEmpty()) {
                activeSubagentModelRow->setValue(QStringLiteral("%1 (inherited)").arg(activeModel));
                activeSubagentModelRow->setTone(BadgeTone::Neutral);
                activeSubagentModelRow->setValueIsPrimary(false);
            } else {
                activeSubagentModelRow->setValue(QStringLiteral("Inherited from primary"));
                activeSubagentModelRow->setTone(BadgeTone::Neutral);
                activeSubagentModelRow->setValueIsPrimary(false);
            }
        }
    }

    std::optional<QString> resolveEffectiveApiKeyForProbe() const {
        if (apiKeyEdit) {
            const QString typed = apiKeyEdit->text().trimmed();
            if (!typed.isEmpty()) {
                return typed;
            }
        }
        if (store && store->hasEndpointApiKey()) {
            const QString cached = store->cachedEndpointApiKey().trimmed();
            if (!cached.isEmpty()) {
                return cached;
            }
        }
        return std::nullopt;
    }

    QString safeBaseUrlForKeyDispatch() const {
        if (baseUrlEdit) {
            const EndpointValidation v = validateAndNormalizeEndpoint(baseUrlEdit->text());
            if (v.isValid) {
                return v.normalizedBaseUrl;
            }
        }
        if (store) {
            return store->endpointBaseUrl().trimmed();
        }
        return lastSnapshot.endpointBaseUrl.trimmed();
    }

    void syncFieldsFromStoreIfClean() {
        if (!store) {
            return;
        }
        if (!baseUrlDirty && baseUrlEdit && !baseUrlEdit->hasFocus()) {
            const QSignalBlocker blocker(baseUrlEdit);
            baseUrlEdit->setText(store->endpointBaseUrl().trimmed());
            updateNormalizedPreview();
            updatePresetHighlights();
        }
        if (!primaryModelDirty && primaryModelCombo && !isComboFocused(primaryModelCombo)) {
            const QSignalBlocker blocker(primaryModelCombo);
            primaryModelCombo->setEditText(store->modelId().trimmed());
            updateSelectedModelMetadata();
        }
        if (!subagentModelDirty && subagentModelCombo && !isComboFocused(subagentModelCombo)) {
            const QSignalBlocker blocker(subagentModelCombo);
            subagentModelCombo->setEditText(store->subagentModelId().trimmed());
        }
    }

    void applyPreset(const QString& label, const QString& url, bool localKeyless) {
        if (baseUrlEdit) {
            const QSignalBlocker blocker(baseUrlEdit);
            baseUrlEdit->setText(url);
        }
        baseUrlDirty = true;
        setErrorText(QString());
        updateNormalizedPreview();
        updatePresetHighlights();

        const QString hint = localKeyless
            ? QStringLiteral("Preset filled: %1 (local — API key optional). Click Test Connection & Fetch Models or Save Endpoint & Model.").arg(label)
            : QStringLiteral("Preset filled: %1. Save your API key below, then click Test Connection & Fetch Models or Save Endpoint & Model.").arg(label);
        setFeedback(testFeedbackLabel, hint, "feedbackInfo");
    }

    void clearEndpointConfig() {
        testInFlight = false;
        lastMeasuredLatencyMs = -1;
        if (testConnectionButton) {
            testConnectionButton->setEnabled(true);
            testConnectionButton->setText(QStringLiteral("Test Connection & Fetch Models"));
        }

        if (baseUrlEdit) {
            const QSignalBlocker blocker(baseUrlEdit);
            baseUrlEdit->clear();
        }
        if (primaryModelCombo) {
            const QSignalBlocker blocker(primaryModelCombo);
            primaryModelCombo->setCurrentIndex(-1);
            primaryModelCombo->setEditText(QString());
        }
        if (subagentModelCombo) {
            const QSignalBlocker blocker(subagentModelCombo);
            subagentModelCombo->setCurrentIndex(-1);
            subagentModelCombo->setEditText(QString());
        }

        baseUrlDirty = false;
        primaryModelDirty = false;
        subagentModelDirty = false;
        lastAppliedBaseUrl.clear();
        lastAppliedPrimaryModel.clear();
        lastAppliedSubagentModel.clear();
        updateDiscoveredModelsUi({}, false);

        if (store) {
            store->setEndpointConfig(QString(), QString(), QString());
            store->sync();
        }
        if (client) {
            client->configureEndpoint(QString(), std::nullopt, QString(), QString());
        }

        updateConnectionBadge(QStringLiteral("NOT CONFIGURED"), BadgeTone::Neutral);
        updateLatencyDisplay(-1);
        setErrorText(QString());

        WorkspaceSnapshot cleared = lastSnapshot;
        cleared.endpointConfigured = false;
        cleared.endpointBaseUrl.clear();
        cleared.modelId.clear();
        cleared.subagentModelId.clear();
        cleared.effectiveSubagentModelId.clear();
        cleared.telemetry.endpointConfigured = false;
        cleared.telemetry.engineOnline = false;
        cleared.telemetry.engineError.clear();
        cleared.telemetry.endpointLatencyMs = -1;
        lastSnapshot = cleared;
        updateSummaryRows(cleared);

        if (refreshStatusButton) {
            refreshStatusButton->setEnabled(false);
        }

        updateNormalizedPreview();
        updatePresetHighlights();
        updateSelectedModelMetadata();
        setFeedback(testFeedbackLabel, QString(), "feedbackInfo");
        setFeedback(saveFeedbackLabel,
                    QStringLiteral("Endpoint cleared (unconfigured). BeatrixLeslie IDE will not make network requests."),
                    "feedbackInfo");
    }

    void saveApiKeyToKeychain(EngineSettingsPane* owner) {
        if (!apiKeyEdit) {
            return;
        }
        const QString key = apiKeyEdit->text().trimmed();
        if (key.isEmpty()) {
            setFeedback(apiKeyFeedbackLabel,
                        QStringLiteral("Enter an API key above before saving to OS Keychain."),
                        "feedbackError");
            return;
        }

        const QString baseUrl = safeBaseUrlForKeyDispatch();
        const QString modelId = primaryModelCombo ? primaryModelCombo->currentText().trimmed()
                                                  : lastAppliedPrimaryModel;
        const QString subModelId = subagentModelCombo ? subagentModelCombo->currentText().trimmed()
                                                      : lastAppliedSubagentModel;

        if (saveKeyButton) {
            saveKeyButton->setEnabled(false);
        }
        setFeedback(apiKeyFeedbackLabel, QStringLiteral("Saving to OS Keychain…"), "feedbackInfo");

        if (client) {
            client->configureEndpoint(baseUrl, std::optional<QString>(key), modelId, subModelId);
        }

        if (store) {
            QPointer<EngineSettingsPane> guard(owner);
            store->setEndpointApiKey(key, [guard](bool ok) {
                if (!guard || !guard->m_impl) {
                    return;
                }
                auto& impl = *guard->m_impl;
                if (impl.saveKeyButton) {
                    impl.saveKeyButton->setEnabled(true);
                }
                if (ok) {
                    if (impl.apiKeyEdit) {
                        impl.apiKeyEdit->clear();
                        impl.apiKeyEdit->setEchoMode(QLineEdit::Password);
                    }
                    if (impl.showHideKeyButton) {
                        impl.showHideKeyButton->setChecked(false);
                    }
                    impl.refreshApiKeyStatusUi();
                    impl.setFeedback(impl.apiKeyFeedbackLabel,
                                     QStringLiteral("API key saved to OS Keychain."),
                                     "feedbackOk");
                } else {
                    impl.refreshApiKeyStatusUi();
                    impl.setFeedback(impl.apiKeyFeedbackLabel,
                                     QStringLiteral("Could not write API key to OS Keychain; active for this session only."),
                                     "feedbackError");
                }
            });
        } else {
            if (saveKeyButton) {
                saveKeyButton->setEnabled(true);
            }
            apiKeyEdit->clear();
            apiKeyEdit->setEchoMode(QLineEdit::Password);
            if (showHideKeyButton) {
                showHideKeyButton->setChecked(false);
            }
            refreshApiKeyStatusUi();
            setFeedback(apiKeyFeedbackLabel,
                        QStringLiteral("API key applied for this session."),
                        "feedbackOk");
        }
    }

    void clearApiKeyFromKeychain(EngineSettingsPane* owner) {
        if (apiKeyEdit) {
            apiKeyEdit->clear();
            apiKeyEdit->setEchoMode(QLineEdit::Password);
        }
        if (showHideKeyButton) {
            showHideKeyButton->setChecked(false);
        }

        const QString baseUrl = safeBaseUrlForKeyDispatch();
        const QString modelId = primaryModelCombo ? primaryModelCombo->currentText().trimmed()
                                                  : lastAppliedPrimaryModel;
        const QString subModelId = subagentModelCombo ? subagentModelCombo->currentText().trimmed()
                                                      : lastAppliedSubagentModel;

        if (client) {
            // Passing empty QString clears any active API key override inside spell_core.
            client->configureEndpoint(baseUrl, std::optional<QString>(QString()), modelId, subModelId);
        }

        if (store) {
            if (clearKeyButton) {
                clearKeyButton->setEnabled(false);
            }
            QPointer<EngineSettingsPane> guard(owner);
            store->clearEndpointApiKey([guard](bool ok) {
                if (!guard || !guard->m_impl) {
                    return;
                }
                auto& impl = *guard->m_impl;
                impl.refreshApiKeyStatusUi();
                impl.setFeedback(
                    impl.apiKeyFeedbackLabel,
                    ok ? QStringLiteral("API key cleared from OS Keychain.")
                       : QStringLiteral("API key cleared from memory."),
                    "feedbackInfo");
            });
        } else {
            refreshApiKeyStatusUi();
            setFeedback(apiKeyFeedbackLabel,
                        QStringLiteral("API key cleared."),
                        "feedbackInfo");
        }
    }

    void testConnectionAndFetchModels() {
        if (!baseUrlEdit) {
            return;
        }
        const EndpointValidation v = validateAndNormalizeEndpoint(baseUrlEdit->text());
        if (v.isEmpty) {
            updateConnectionBadge(QStringLiteral("NOT CONFIGURED"), BadgeTone::Neutral);
            updateLatencyDisplay(-1);
            setErrorText(QStringLiteral(
                "Enter an OpenAI-compatible Base URL (or pick a provider preset) before testing."));
            setFeedback(testFeedbackLabel, QString(), "feedbackInfo");
            return;
        }
        if (!v.isValid) {
            setErrorText(v.errorMessage);
            setFeedback(testFeedbackLabel, QStringLiteral("Invalid endpoint URL."), "feedbackError");
            return;
        }

        if (!client) {
            setErrorText(QStringLiteral("Core client is unavailable."));
            return;
        }

        testInFlight = true;
        lastMeasuredLatencyMs = -1;
        lastSnapshot.telemetry.endpointLatencyMs = -1;
        testTimer.restart();

        if (testConnectionButton) {
            testConnectionButton->setEnabled(false);
            testConnectionButton->setText(QStringLiteral("Testing…"));
        }
        updateConnectionBadge(QStringLiteral("TESTING…"), BadgeTone::Info);
        updateLatencyDisplay(-1);
        setErrorText(QString());
        setFeedback(testFeedbackLabel,
                    QStringLiteral("Probing GET %1…").arg(v.resolvedModelsUrl),
                    "feedbackInfo");

        client->fetchModels(v.normalizedBaseUrl, resolveEffectiveApiKeyForProbe());
    }

    void saveEndpointAndModel(EngineSettingsPane* owner) {
        const QString rawUrl = baseUrlEdit ? baseUrlEdit->text() : QString();
        const EndpointValidation v = validateAndNormalizeEndpoint(rawUrl);
        if (!v.isValid) {
            setErrorText(v.errorMessage);
            setFeedback(saveFeedbackLabel, QStringLiteral("Fix the Base URL before saving."), "feedbackError");
            return;
        }

        const QString normalizedUrl = v.normalizedBaseUrl;
        const QString primaryModel = primaryModelCombo ? primaryModelCombo->currentText().trimmed() : QString();
        const QString subagentModel = subagentModelCombo ? subagentModelCombo->currentText().trimmed() : QString();
        const QString typedKey = apiKeyEdit ? apiKeyEdit->text().trimmed() : QString();

        if (baseUrlEdit && baseUrlEdit->text().trimmed() != normalizedUrl) {
            const QSignalBlocker blocker(baseUrlEdit);
            baseUrlEdit->setText(normalizedUrl);
        }

        baseUrlDirty = false;
        primaryModelDirty = false;
        subagentModelDirty = false;
        lastAppliedBaseUrl = normalizedUrl;
        lastAppliedPrimaryModel = primaryModel;
        lastAppliedSubagentModel = subagentModel;

        if (store) {
            store->setEndpointConfig(normalizedUrl, primaryModel, subagentModel);
            store->sync();
        }

        std::optional<QString> keyToDispatch;
        bool savedTypedKey = false;
        if (!typedKey.isEmpty()) {
            keyToDispatch = typedKey;
            savedTypedKey = true;
            if (store) {
                QPointer<EngineSettingsPane> guard(owner);
                store->setEndpointApiKey(typedKey, [guard](bool ok) {
                    if (!guard || !guard->m_impl) {
                        return;
                    }
                    auto& impl = *guard->m_impl;
                    if (ok && impl.apiKeyEdit) {
                        impl.apiKeyEdit->clear();
                        impl.apiKeyEdit->setEchoMode(QLineEdit::Password);
                    }
                    if (ok && impl.showHideKeyButton) {
                        impl.showHideKeyButton->setChecked(false);
                    }
                    impl.refreshApiKeyStatusUi();
                    impl.setFeedback(
                        impl.apiKeyFeedbackLabel,
                        ok ? QStringLiteral("API key saved to OS Keychain.")
                           : QStringLiteral("API key applied for this session (Keychain unavailable)."),
                        ok ? "feedbackOk" : "feedbackError");
                });
            } else {
                if (apiKeyEdit) {
                    apiKeyEdit->clear();
                    apiKeyEdit->setEchoMode(QLineEdit::Password);
                }
                if (showHideKeyButton) {
                    showHideKeyButton->setChecked(false);
                }
                refreshApiKeyStatusUi();
            }
        } else if (store && store->hasEndpointApiKey()) {
            const QString cached = store->cachedEndpointApiKey().trimmed();
            if (!cached.isEmpty()) {
                keyToDispatch = cached;
            }
        }

        if (client) {
            client->configureEndpoint(normalizedUrl, keyToDispatch, primaryModel, subagentModel);
            if (!normalizedUrl.isEmpty()) {
                client->refreshEndpointStatus();
            }
        }

        setErrorText(QString());
        updateNormalizedPreview();
        updatePresetHighlights();
        updateSelectedModelMetadata();

        WorkspaceSnapshot optimistic = lastSnapshot;
        optimistic.endpointBaseUrl = normalizedUrl;
        optimistic.endpointConfigured = !normalizedUrl.isEmpty();
        optimistic.modelId = primaryModel;
        optimistic.subagentModelId = subagentModel;
        optimistic.effectiveSubagentModelId = subagentModel.isEmpty() ? primaryModel : subagentModel;
        if (normalizedUrl.isEmpty()) {
            optimistic.telemetry.endpointConfigured = false;
            optimistic.telemetry.engineOnline = false;
            optimistic.telemetry.engineError.clear();
            optimistic.telemetry.endpointLatencyMs = -1;
            updateConnectionBadge(QStringLiteral("NOT CONFIGURED"), BadgeTone::Neutral);
            updateLatencyDisplay(-1);
        }
        lastSnapshot = optimistic;
        updateSummaryRows(optimistic);

        if (refreshStatusButton) {
            refreshStatusButton->setEnabled(!normalizedUrl.isEmpty());
        }

        if (normalizedUrl.isEmpty()) {
            setFeedback(saveFeedbackLabel,
                        QStringLiteral("Saved unconfigured state. BeatrixLeslie IDE will make zero network requests."),
                        "feedbackInfo");
        } else if (savedTypedKey) {
            setFeedback(saveFeedbackLabel,
                        QStringLiteral("Endpoint, model, and API key saved."),
                        "feedbackOk");
        } else {
            setFeedback(saveFeedbackLabel,
                        QStringLiteral("Endpoint and model saved."),
                        "feedbackOk");
        }
    }
};

EngineSettingsPane::EngineSettingsPane(SpellCoreClient* client,
                                       SpellSettingsStore* store,
                                       QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->client = client;
    m_impl->store = store;

    setObjectName(QStringLiteral("EngineSettingsPane"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(14);

    auto addSection = [&](const QString& title, const QString& iconName) -> QVBoxLayout* {
        auto* sectionBox = new QVBoxLayout();
        sectionBox->setContentsMargins(0, 0, 0, 0);
        sectionBox->setSpacing(6);

        auto* header = new SpellSectionHeader(title, iconName, BadgeTone::Accent, this);
        m_impl->sectionHeaders.append(header);
        sectionBox->addWidget(header);

        QVBoxLayout* cardLayout = nullptr;
        QFrame* card = createCardFrame(this, &cardLayout);
        sectionBox->addWidget(card);
        root->addLayout(sectionBox);
        return cardLayout;
    };

    auto makeActionButton = [](const QString& text, QWidget* p) -> QPushButton* {
        auto* btn = new QPushButton(text, p);
        btn->setAutoDefault(false);
        btn->setDefault(false);
        btn->setCursor(Qt::PointingHandCursor);
        return btn;
    };

    // =========================================================================
    // Section 1: Connection Status
    // =========================================================================
    {
        QVBoxLayout* card = addSection(QStringLiteral("Connection Status"), QStringLiteral("server"));

        auto* statusRow = new QHBoxLayout();
        statusRow->setContentsMargins(0, 0, 0, 0);
        statusRow->setSpacing(8);

        auto* statusCaption = new QLabel(QStringLiteral("Endpoint status"), this);
        setRoleProperty(statusCaption, "fieldLabel");
        statusRow->addWidget(statusCaption);
        statusRow->addStretch(1);

        m_impl->statusBadge = new SpellBadge(
            QStringLiteral("NOT CONFIGURED"), BadgeTone::Neutral, true, this);
        m_impl->statusBadge->setObjectName(QStringLiteral("engineStatusBadge"));
        statusRow->addWidget(m_impl->statusBadge);

        m_impl->latencyLabel = new QLabel(QStringLiteral("—"), this);
        m_impl->latencyLabel->setObjectName(QStringLiteral("engineLatencyLabel"));
        m_impl->latencyLabel->setToolTip(QStringLiteral("Latest GET /models round-trip latency"));
        setRoleProperty(m_impl->latencyLabel, "latency");
        statusRow->addWidget(m_impl->latencyLabel);

        m_impl->refreshStatusButton = new SpellIconButton(
            QStringLiteral("refresh"),
            QStringLiteral("Re-probe configured endpoint (GET /models)"),
            BadgeTone::Neutral,
            this);
        m_impl->refreshStatusButton->setEnabled(false);
        statusRow->addWidget(m_impl->refreshStatusButton);

        card->addLayout(statusRow);

        auto* divider = new SpellHairline(Qt::Horizontal, this);
        m_impl->hairlines.append(divider);
        card->addWidget(divider);

        m_impl->activeEndpointRow = new SpellValueRow(
            QStringLiteral("Active endpoint"), QStringLiteral("Not configured"), BadgeTone::Neutral, this);
        m_impl->activeModelRow = new SpellValueRow(
            QStringLiteral("Primary model"), QStringLiteral("Not selected"), BadgeTone::Neutral, this);
        m_impl->activeSubagentModelRow = new SpellValueRow(
            QStringLiteral("Subagent model"), QStringLiteral("Inherited from primary"), BadgeTone::Neutral, this);

        m_impl->valueRows.append(m_impl->activeEndpointRow);
        m_impl->valueRows.append(m_impl->activeModelRow);
        m_impl->valueRows.append(m_impl->activeSubagentModelRow);

        card->addWidget(m_impl->activeEndpointRow);
        card->addWidget(m_impl->activeModelRow);
        card->addWidget(m_impl->activeSubagentModelRow);

        m_impl->errorLabel = new QLabel(this);
        m_impl->errorLabel->setObjectName(QStringLiteral("engineErrorLabel"));
        m_impl->errorLabel->setWordWrap(true);
        m_impl->errorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        setRoleProperty(m_impl->errorLabel, "errorBox");
        m_impl->errorLabel->setVisible(false);
        card->addWidget(m_impl->errorLabel);
    }

    // =========================================================================
    // Section 2: OpenAI-Compatible Endpoint & Provider Presets
    // =========================================================================
    {
        QVBoxLayout* card = addSection(QStringLiteral("OpenAI-Compatible Endpoint"), QStringLiteral("globe"));

        auto makePresetBtn = [&](const QString& text, const QString& tip) -> QPushButton* {
            auto* btn = makeActionButton(text, this);
            btn->setProperty("preset", true);
            btn->setProperty("presetActive", false);
            btn->setToolTip(tip);
            return btn;
        };

        auto* presetsHeaderRow = new QHBoxLayout();
        presetsHeaderRow->setContentsMargins(0, 0, 0, 0);
        presetsHeaderRow->setSpacing(8);

        auto* presetsCaption = new QLabel(QStringLiteral("Quick-fill provider presets"), this);
        setRoleProperty(presetsCaption, "fieldLabel");
        presetsHeaderRow->addWidget(presetsCaption);
        presetsHeaderRow->addStretch(1);

        m_impl->presetClearButton = makePresetBtn(
            QStringLiteral("Clear / Unconfigured"),
            QStringLiteral("Clear endpoint URL and model selection (disables all inference requests)"));
        presetsHeaderRow->addWidget(m_impl->presetClearButton);
        card->addLayout(presetsHeaderRow);

        auto* presetsRow = new QHBoxLayout();
        presetsRow->setContentsMargins(0, 0, 0, 0);
        presetsRow->setSpacing(6);

        m_impl->presetOpenAiButton = makePresetBtn(
            QStringLiteral("OpenAI"),
            QStringLiteral("Quick-fill %1").arg(QLatin1StringView(kPresetOpenAiUrl)));
        m_impl->presetOpenRouterButton = makePresetBtn(
            QStringLiteral("OpenRouter"),
            QStringLiteral("Quick-fill %1").arg(QLatin1StringView(kPresetOpenRouterUrl)));
        m_impl->presetOllamaButton = makePresetBtn(
            QStringLiteral("Ollama"),
            QStringLiteral("Quick-fill %1 (local server, API key optional)")
                .arg(QLatin1StringView(kPresetOllamaUrl)));
        m_impl->presetLmStudioButton = makePresetBtn(
            QStringLiteral("LM Studio"),
            QStringLiteral("Quick-fill %1 (local server, API key optional)")
                .arg(QLatin1StringView(kPresetLmStudioUrl)));

        presetsRow->addWidget(m_impl->presetOpenAiButton);
        presetsRow->addWidget(m_impl->presetOpenRouterButton);
        presetsRow->addWidget(m_impl->presetOllamaButton);
        presetsRow->addWidget(m_impl->presetLmStudioButton);
        presetsRow->addStretch(1);
        card->addLayout(presetsRow);

        auto* divider = new SpellHairline(Qt::Horizontal, this);
        m_impl->hairlines.append(divider);
        card->addWidget(divider);

        auto* urlLabel = new QLabel(QStringLiteral("Base URL"), this);
        setRoleProperty(urlLabel, "fieldLabel");
        card->addWidget(urlLabel);

        m_impl->baseUrlEdit = new QLineEdit(this);
        m_impl->baseUrlEdit->setObjectName(QStringLiteral("engineBaseUrlEdit"));
        m_impl->baseUrlEdit->setPlaceholderText(
            QStringLiteral("https://api.openai.com/v1  or  http://localhost:11434/v1"));
        m_impl->baseUrlEdit->setClearButtonEnabled(true);
        card->addWidget(m_impl->baseUrlEdit);

        m_impl->urlHelpLabel = new QLabel(
            QStringLiteral("Supports any OpenAI-compatible http:// or https:// server (hosted APIs, Ollama, "
                           "LM Studio, vLLM, llama.cpp, or custom proxies). Bare host URLs automatically "
                           "normalize with /v1 (e.g. http://localhost:11434 -> /v1/models); explicit paths "
                           "like /v1 or /api/v1 are preserved and trailing /chat/completions or /models "
                           "suffixes are stripped."),
            this);
        m_impl->urlHelpLabel->setWordWrap(true);
        setRoleProperty(m_impl->urlHelpLabel, "hint");
        card->addWidget(m_impl->urlHelpLabel);

        m_impl->normalizedPreviewLabel = new QLabel(this);
        m_impl->normalizedPreviewLabel->setObjectName(QStringLiteral("engineNormalizedPreviewLabel"));
        m_impl->normalizedPreviewLabel->setWordWrap(true);
        m_impl->normalizedPreviewLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        setRoleProperty(m_impl->normalizedPreviewLabel, "hint");
        card->addWidget(m_impl->normalizedPreviewLabel);

        auto* testRow = new QHBoxLayout();
        testRow->setContentsMargins(0, 2, 0, 0);
        testRow->setSpacing(8);

        m_impl->testConnectionButton = makeActionButton(
            QStringLiteral("Test Connection & Fetch Models"), this);
        m_impl->testConnectionButton->setObjectName(QStringLiteral("engineTestConnectionButton"));
        m_impl->testConnectionButton->setToolTip(
            QStringLiteral("Sends GET /models to verify reachability, measure latency, and populate available models"));
        testRow->addWidget(m_impl->testConnectionButton);
        testRow->addStretch(1);
        card->addLayout(testRow);

        m_impl->testFeedbackLabel = new QLabel(this);
        m_impl->testFeedbackLabel->setWordWrap(true);
        m_impl->testFeedbackLabel->setVisible(false);
        setRoleProperty(m_impl->testFeedbackLabel, "feedbackInfo");
        card->addWidget(m_impl->testFeedbackLabel);
    }

    // =========================================================================
    // Section 3: API Key (OS Keychain)
    // =========================================================================
    {
        QVBoxLayout* card = addSection(QStringLiteral("API Key (OS Keychain)"), QStringLiteral("lock"));

        auto* keyHeaderRow = new QHBoxLayout();
        keyHeaderRow->setContentsMargins(0, 0, 0, 0);
        keyHeaderRow->setSpacing(8);

        auto* keyCaption = new QLabel(QStringLiteral("Bearer API key"), this);
        setRoleProperty(keyCaption, "fieldLabel");
        keyHeaderRow->addWidget(keyCaption);
        keyHeaderRow->addStretch(1);

        m_impl->apiKeyStatusBadge = new SpellBadge(
            QStringLiteral("NOT SET"), BadgeTone::Neutral, true, this);
        m_impl->apiKeyStatusBadge->setObjectName(QStringLiteral("engineApiKeyStatusBadge"));
        keyHeaderRow->addWidget(m_impl->apiKeyStatusBadge);
        card->addLayout(keyHeaderRow);

        auto* keyInputRow = new QHBoxLayout();
        keyInputRow->setContentsMargins(0, 0, 0, 0);
        keyInputRow->setSpacing(6);

        m_impl->apiKeyEdit = new QLineEdit(this);
        m_impl->apiKeyEdit->setObjectName(QStringLiteral("engineApiKeyEdit"));
        m_impl->apiKeyEdit->setEchoMode(QLineEdit::Password);
        m_impl->apiKeyEdit->setPlaceholderText(
            QStringLiteral("sk-... (optional for keyless local servers)"));
        keyInputRow->addWidget(m_impl->apiKeyEdit, 1);

        m_impl->showHideKeyButton = makeActionButton(QStringLiteral("Show"), this);
        m_impl->showHideKeyButton->setObjectName(QStringLiteral("engineToggleApiKeyVisibilityButton"));
        m_impl->showHideKeyButton->setCheckable(true);
        m_impl->showHideKeyButton->setToolTip(QStringLiteral("Show or hide the API key input"));
        keyInputRow->addWidget(m_impl->showHideKeyButton);

        card->addLayout(keyInputRow);

        auto* keyActionsRow = new QHBoxLayout();
        keyActionsRow->setContentsMargins(0, 0, 0, 0);
        keyActionsRow->setSpacing(8);

        m_impl->saveKeyButton = makeActionButton(QStringLiteral("Save to OS Keychain"), this);
        m_impl->saveKeyButton->setObjectName(QStringLiteral("engineSaveApiKeyButton"));
        m_impl->saveKeyButton->setToolTip(
            QStringLiteral("Securely stores the API key in your system keychain (com.spell.SpellIDE.credentials)"));
        keyActionsRow->addWidget(m_impl->saveKeyButton);

        m_impl->clearKeyButton = makeActionButton(QStringLiteral("Clear Key"), this);
        m_impl->clearKeyButton->setObjectName(QStringLiteral("engineClearApiKeyButton"));
        m_impl->clearKeyButton->setProperty("danger", true);
        m_impl->clearKeyButton->setToolTip(
            QStringLiteral("Removes the stored API key from your OS Keychain and active session"));
        keyActionsRow->addWidget(m_impl->clearKeyButton);
        keyActionsRow->addStretch(1);
        card->addLayout(keyActionsRow);

        m_impl->apiKeyFeedbackLabel = new QLabel(this);
        m_impl->apiKeyFeedbackLabel->setWordWrap(true);
        m_impl->apiKeyFeedbackLabel->setVisible(false);
        setRoleProperty(m_impl->apiKeyFeedbackLabel, "feedbackInfo");
        card->addWidget(m_impl->apiKeyFeedbackLabel);

        m_impl->apiKeyNoteLabel = new QLabel(
            QStringLiteral("Optional for keyless local servers (Ollama, LM Studio, local vLLM/llama.cpp). "
                           "Secrets are stored strictly in your OS Keychain and never written to plaintext "
                           "config files. Environment fallback: SPELL_API_KEY or OPENAI_API_KEY."),
            this);
        m_impl->apiKeyNoteLabel->setWordWrap(true);
        setRoleProperty(m_impl->apiKeyNoteLabel, "hint");
        card->addWidget(m_impl->apiKeyNoteLabel);
    }

    // =========================================================================
    // Section 4: Model Selection & Gatekeeper Critic
    // =========================================================================
    {
        QVBoxLayout* card = addSection(QStringLiteral("Model & Critic Configuration"), QStringLiteral("cpu"));

        auto* primaryHeaderRow = new QHBoxLayout();
        primaryHeaderRow->setContentsMargins(0, 0, 0, 0);
        primaryHeaderRow->setSpacing(8);

        auto* primaryLabel = new QLabel(QStringLiteral("Primary Model ID"), this);
        setRoleProperty(primaryLabel, "fieldLabel");
        primaryHeaderRow->addWidget(primaryLabel);
        primaryHeaderRow->addStretch(1);

        m_impl->discoveredModelsBadge = new SpellBadge(
            QStringLiteral("MANUAL ENTRY"), BadgeTone::Neutral, false, this);
        primaryHeaderRow->addWidget(m_impl->discoveredModelsBadge);
        card->addLayout(primaryHeaderRow);

        m_impl->primaryModelCombo = new QComboBox(this);
        m_impl->primaryModelCombo->setObjectName(QStringLiteral("enginePrimaryModelCombo"));
        m_impl->primaryModelCombo->setProperty("editableModel", true);
        m_impl->primaryModelCombo->setEditable(true);
        m_impl->primaryModelCombo->setInsertPolicy(QComboBox::NoInsert);
        m_impl->primaryModelCombo->setSizeAdjustPolicy(
            QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_impl->primaryModelCombo->setMinimumContentsLength(20);
        m_impl->primaryModelCombo->setMinimumWidth(0);
        m_impl->primaryModelCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (m_impl->primaryModelCombo->lineEdit()) {
            m_impl->primaryModelCombo->lineEdit()->setPlaceholderText(
                QStringLiteral("Select a fetched model or type a custom model ID…"));
            m_impl->primaryModelCombo->lineEdit()->setClearButtonEnabled(true);
        }
        card->addWidget(m_impl->primaryModelCombo);

        m_impl->primaryModelMetaLabel = new QLabel(this);
        m_impl->primaryModelMetaLabel->setWordWrap(true);
        setRoleProperty(m_impl->primaryModelMetaLabel, "hint");
        card->addWidget(m_impl->primaryModelMetaLabel);

        auto* subLabel = new QLabel(
            QStringLiteral("Subagent Model Override (optional)"), this);
        setRoleProperty(subLabel, "fieldLabel");
        card->addWidget(subLabel);

        m_impl->subagentModelCombo = new QComboBox(this);
        m_impl->subagentModelCombo->setObjectName(QStringLiteral("engineSubagentModelCombo"));
        m_impl->subagentModelCombo->setProperty("editableModel", true);
        m_impl->subagentModelCombo->setEditable(true);
        m_impl->subagentModelCombo->setInsertPolicy(QComboBox::NoInsert);
        m_impl->subagentModelCombo->setSizeAdjustPolicy(
            QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_impl->subagentModelCombo->setMinimumContentsLength(20);
        m_impl->subagentModelCombo->setMinimumWidth(0);
        m_impl->subagentModelCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (m_impl->subagentModelCombo->lineEdit()) {
            m_impl->subagentModelCombo->lineEdit()->setPlaceholderText(
                QStringLiteral("Leave blank to inherit Primary Model ID"));
            m_impl->subagentModelCombo->lineEdit()->setClearButtonEnabled(true);
        }
        card->addWidget(m_impl->subagentModelCombo);

        m_impl->subagentModelHintLabel = new QLabel(
            QStringLiteral("Optional model used for delegated coding subagent slots and Gatekeeper Critic audits. "
                           "Leave blank to reuse the Primary Model ID."),
            this);
        m_impl->subagentModelHintLabel->setWordWrap(true);
        setRoleProperty(m_impl->subagentModelHintLabel, "hint");
        card->addWidget(m_impl->subagentModelHintLabel);

        auto* divider1 = new SpellHairline(Qt::Horizontal, this);
        m_impl->hairlines.append(divider1);
        card->addWidget(divider1);

        m_impl->slot2CriticCheck = new QCheckBox(
            QStringLiteral("Enable Gatekeeper Critic review"), this);
        m_impl->slot2CriticCheck->setObjectName(QStringLiteral("engineSlot2CriticCheck"));
        card->addWidget(m_impl->slot2CriticCheck);

        m_impl->slot2CriticHintLabel = new QLabel(
            QStringLiteral("When enabled, staged file mutations undergo an isolated LLM critic pass before "
                           "auto-commit using the configured endpoint and subagent model."),
            this);
        m_impl->slot2CriticHintLabel->setWordWrap(true);
        setRoleProperty(m_impl->slot2CriticHintLabel, "hint");
        card->addWidget(m_impl->slot2CriticHintLabel);

        auto* divider2 = new SpellHairline(Qt::Horizontal, this);
        m_impl->hairlines.append(divider2);
        card->addWidget(divider2);

        auto* saveRow = new QHBoxLayout();
        saveRow->setContentsMargins(0, 2, 0, 0);
        saveRow->setSpacing(8);

        m_impl->saveEndpointButton = makeActionButton(
            QStringLiteral("Save Endpoint & Model"), this);
        m_impl->saveEndpointButton->setObjectName(QStringLiteral("engineSaveEndpointButton"));
        m_impl->saveEndpointButton->setProperty("primary", true);
        m_impl->saveEndpointButton->setToolTip(
            QStringLiteral("Persists endpoint URL, primary model, and subagent model override to BeatrixLeslie IDE"));
        saveRow->addWidget(m_impl->saveEndpointButton);
        saveRow->addStretch(1);
        card->addLayout(saveRow);

        m_impl->saveFeedbackLabel = new QLabel(this);
        m_impl->saveFeedbackLabel->setWordWrap(true);
        m_impl->saveFeedbackLabel->setVisible(false);
        setRoleProperty(m_impl->saveFeedbackLabel, "feedbackInfo");
        card->addWidget(m_impl->saveFeedbackLabel);
    }

    root->addStretch(1);

    // =========================================================================
    // Signal Wiring
    // =========================================================================
    connect(m_impl->presetOpenAiButton, &QPushButton::clicked, this, [this]() {
        m_impl->applyPreset(QStringLiteral("OpenAI"),
                            QString::fromLatin1(kPresetOpenAiUrl),
                            false);
    });
    connect(m_impl->presetOpenRouterButton, &QPushButton::clicked, this, [this]() {
        m_impl->applyPreset(QStringLiteral("OpenRouter"),
                            QString::fromLatin1(kPresetOpenRouterUrl),
                            false);
    });
    connect(m_impl->presetOllamaButton, &QPushButton::clicked, this, [this]() {
        m_impl->applyPreset(QStringLiteral("Ollama"),
                            QString::fromLatin1(kPresetOllamaUrl),
                            true);
    });
    connect(m_impl->presetLmStudioButton, &QPushButton::clicked, this, [this]() {
        m_impl->applyPreset(QStringLiteral("LM Studio"),
                            QString::fromLatin1(kPresetLmStudioUrl),
                            true);
    });
    connect(m_impl->presetClearButton, &QPushButton::clicked, this, [this]() {
        m_impl->clearEndpointConfig();
    });

    connect(m_impl->baseUrlEdit, &QLineEdit::textEdited, this, [this](const QString&) {
        m_impl->baseUrlDirty = true;
    });
    connect(m_impl->baseUrlEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        m_impl->updateNormalizedPreview();
        m_impl->updatePresetHighlights();
    });
    connect(m_impl->baseUrlEdit, &QLineEdit::returnPressed, this, [this]() {
        m_impl->testConnectionAndFetchModels();
    });

    connect(m_impl->refreshStatusButton, &QToolButton::clicked, this, [this]() {
        if (!m_impl->client) {
            return;
        }
        m_impl->setErrorText(QString());
        m_impl->updateConnectionBadge(QStringLiteral("TESTING…"), BadgeTone::Info);
        m_impl->client->refreshEndpointStatus();
    });

    connect(m_impl->testConnectionButton, &QPushButton::clicked, this, [this]() {
        m_impl->testConnectionAndFetchModels();
    });

    connect(m_impl->apiKeyEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        m_impl->refreshApiKeyStatusUi();
    });
    connect(m_impl->apiKeyEdit, &QLineEdit::returnPressed, this, [this]() {
        if (!m_impl->apiKeyEdit->text().trimmed().isEmpty()) {
            m_impl->saveApiKeyToKeychain(this);
        }
    });
    connect(m_impl->showHideKeyButton, &QPushButton::toggled, this, [this](bool visible) {
        m_impl->apiKeyEdit->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
        m_impl->showHideKeyButton->setText(visible ? QStringLiteral("Hide") : QStringLiteral("Show"));
        const UiPalette u = ThemeManager::instance().ui();
        m_impl->showHideKeyButton->setIcon(
            icons::vectorIcon(visible ? QStringLiteral("unlock") : QStringLiteral("lock"),
                              u.textPrimary, 14));
    });
    connect(m_impl->saveKeyButton, &QPushButton::clicked, this, [this]() {
        m_impl->saveApiKeyToKeychain(this);
    });
    connect(m_impl->clearKeyButton, &QPushButton::clicked, this, [this]() {
        m_impl->clearApiKeyFromKeychain(this);
    });

    connect(m_impl->primaryModelCombo, &QComboBox::editTextChanged, this, [this](const QString&) {
        m_impl->primaryModelDirty = true;
        m_impl->updateSelectedModelMetadata();
    });
    connect(m_impl->primaryModelCombo, &QComboBox::activated, this, [this](int) {
        m_impl->primaryModelDirty = true;
        m_impl->updateSelectedModelMetadata();
    });
    connect(m_impl->subagentModelCombo, &QComboBox::editTextChanged, this, [this](const QString&) {
        m_impl->subagentModelDirty = true;
    });
    connect(m_impl->subagentModelCombo, &QComboBox::activated, this, [this](int) {
        m_impl->subagentModelDirty = true;
    });

    connect(m_impl->slot2CriticCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        m_impl->lastAppliedSlot2Critic = enabled;
        if (m_impl->store) {
            m_impl->store->setSlot2CriticEnabled(enabled);
            m_impl->store->sync();
        }
        if (m_impl->client) {
            m_impl->client->setSlot2CriticEnabled(enabled);
        }
    });

    connect(m_impl->saveEndpointButton, &QPushButton::clicked, this, [this]() {
        m_impl->saveEndpointAndModel(this);
    });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });

    if (m_impl->store) {
        connect(m_impl->store, &SpellSettingsStore::apiKeyChanged, this, [this]() {
            m_impl->refreshApiKeyStatusUi();
        });
        connect(m_impl->store, &SpellSettingsStore::endpointConfigChanged, this, [this]() {
            m_impl->syncFieldsFromStoreIfClean();
        });
    }

    if (m_impl->client) {
        connect(m_impl->client, &SpellCoreClient::modelsFetched, this,
                [this](const QVector<ModelDescriptor>& models, const QString& error) {
                    onModelsFetched(models, error);
                });
        connect(m_impl->client, &SpellCoreClient::snapshotUpdated, this,
                [this](const WorkspaceSnapshot& snap) {
                    updateSnapshot(snap);
                });
    }

    // =========================================================================
    // Initial Hydration from Store + Client Snapshot
    // =========================================================================
    if (m_impl->store) {
        {
            const QSignalBlocker blocker(m_impl->baseUrlEdit);
            m_impl->baseUrlEdit->setText(m_impl->store->endpointBaseUrl().trimmed());
        }
        {
            const QSignalBlocker blocker(m_impl->primaryModelCombo);
            m_impl->primaryModelCombo->setEditText(m_impl->store->modelId().trimmed());
        }
        {
            const QSignalBlocker blocker(m_impl->subagentModelCombo);
            m_impl->subagentModelCombo->setEditText(m_impl->store->subagentModelId().trimmed());
        }
        {
            const QSignalBlocker blocker(m_impl->slot2CriticCheck);
            m_impl->slot2CriticCheck->setChecked(m_impl->store->slot2CriticEnabled());
            m_impl->lastAppliedSlot2Critic = m_impl->store->slot2CriticEnabled();
        }
        m_impl->lastAppliedBaseUrl = m_impl->store->endpointBaseUrl().trimmed();
        m_impl->lastAppliedPrimaryModel = m_impl->store->modelId().trimmed();
        m_impl->lastAppliedSubagentModel = m_impl->store->subagentModelId().trimmed();

        WorkspaceSnapshot storeSeed;
        storeSeed.endpointBaseUrl = m_impl->lastAppliedBaseUrl;
        storeSeed.endpointConfigured = !m_impl->lastAppliedBaseUrl.isEmpty();
        storeSeed.modelId = m_impl->lastAppliedPrimaryModel;
        storeSeed.subagentModelId = m_impl->lastAppliedSubagentModel;
        storeSeed.effectiveSubagentModelId = m_impl->lastAppliedSubagentModel.isEmpty()
                                                 ? m_impl->lastAppliedPrimaryModel
                                                 : m_impl->lastAppliedSubagentModel;
        storeSeed.slot2CriticEnabled = m_impl->lastAppliedSlot2Critic;
        m_impl->lastSnapshot = storeSeed;
        m_impl->updateSummaryRows(storeSeed);
        if (m_impl->refreshStatusButton) {
            m_impl->refreshStatusButton->setEnabled(storeSeed.endpointConfigured);
        }
    }

    m_impl->refreshApiKeyStatusUi();
    m_impl->updateNormalizedPreview();
    m_impl->updatePresetHighlights();
    m_impl->updateSelectedModelMetadata();

    if (m_impl->client) {
        const WorkspaceSnapshot initialSnap = m_impl->client->currentSnapshot();
        const bool snapPopulated = !initialSnap.raw.isEmpty() ||
                                   initialSnap.endpointConfigured ||
                                   !initialSnap.endpointBaseUrl.trimmed().isEmpty() ||
                                   initialSnap.telemetry.engineOnline ||
                                   !initialSnap.telemetry.engineError.trimmed().isEmpty() ||
                                   !initialSnap.availableModels.isEmpty();
        if (snapPopulated || !m_impl->store) {
            updateSnapshot(initialSnap);
        }
    }

    applyTheme();
}

EngineSettingsPane::~EngineSettingsPane() = default;

void EngineSettingsPane::updateSnapshot(const WorkspaceSnapshot& snap) {
    if (!m_impl) {
        return;
    }

    m_impl->lastSnapshot = snap;

    // Sync discovered models list without clobbering user text
    if (!snap.availableModels.isEmpty() && snap.availableModels != m_impl->discoveredModels) {
        m_impl->updateDiscoveredModelsUi(snap.availableModels, false);
    }

    // Sync editable inputs only when untouched by the user and not actively focused
    const QString snapUrl = snap.endpointBaseUrl.trimmed();
    if (!m_impl->baseUrlDirty && m_impl->baseUrlEdit && !m_impl->baseUrlEdit->hasFocus()) {
        if (!snapUrl.isEmpty() ||
            (m_impl->hasInitializedFromSnapshot && snapUrl != m_impl->lastAppliedBaseUrl)) {
            const QSignalBlocker blocker(m_impl->baseUrlEdit);
            m_impl->baseUrlEdit->setText(snapUrl);
            m_impl->updateNormalizedPreview();
            m_impl->updatePresetHighlights();
        }
    }

    const QString snapModel = snap.modelId.trimmed();
    if (!m_impl->primaryModelDirty && m_impl->primaryModelCombo && !isComboFocused(m_impl->primaryModelCombo)) {
        if (!snapModel.isEmpty() ||
            (m_impl->hasInitializedFromSnapshot && snapModel != m_impl->lastAppliedPrimaryModel)) {
            const QSignalBlocker blocker(m_impl->primaryModelCombo);
            const int idx = m_impl->primaryModelCombo->findText(snapModel, Qt::MatchExactly);
            if (idx >= 0) {
                m_impl->primaryModelCombo->setCurrentIndex(idx);
            } else {
                m_impl->primaryModelCombo->setCurrentIndex(-1);
                m_impl->primaryModelCombo->setEditText(snapModel);
            }
            m_impl->updateSelectedModelMetadata();
        }
    }

    const QString snapSubModel = snap.subagentModelId.trimmed();
    if (!m_impl->subagentModelDirty && m_impl->subagentModelCombo && !isComboFocused(m_impl->subagentModelCombo)) {
        if (!snapSubModel.isEmpty() ||
            (m_impl->hasInitializedFromSnapshot && snapSubModel != m_impl->lastAppliedSubagentModel)) {
            const QSignalBlocker blocker(m_impl->subagentModelCombo);
            const int idx = m_impl->subagentModelCombo->findText(snapSubModel, Qt::MatchExactly);
            if (idx >= 0) {
                m_impl->subagentModelCombo->setCurrentIndex(idx);
            } else {
                m_impl->subagentModelCombo->setCurrentIndex(-1);
                m_impl->subagentModelCombo->setEditText(snapSubModel);
            }
        }
    }

    if (m_impl->slot2CriticCheck &&
        (!m_impl->hasInitializedFromSnapshot || snap.slot2CriticEnabled != m_impl->lastAppliedSlot2Critic)) {
        const QSignalBlocker blocker(m_impl->slot2CriticCheck);
        m_impl->slot2CriticCheck->setChecked(snap.slot2CriticEnabled);
    }

    m_impl->lastAppliedBaseUrl = snapUrl;
    m_impl->lastAppliedPrimaryModel = snapModel;
    m_impl->lastAppliedSubagentModel = snapSubModel;
    m_impl->lastAppliedSlot2Critic = snap.slot2CriticEnabled;
    m_impl->hasInitializedFromSnapshot = true;

    m_impl->updateSummaryRows(snap);

    const bool configured = snap.endpointConfigured || snap.telemetry.endpointConfigured || !snapUrl.isEmpty();
    if (m_impl->refreshStatusButton) {
        m_impl->refreshStatusButton->setEnabled(configured && !m_impl->testInFlight);
    }

    if (!m_impl->testInFlight) {
        if (!configured) {
            m_impl->updateConnectionBadge(QStringLiteral("NOT CONFIGURED"), BadgeTone::Neutral);
            m_impl->updateLatencyDisplay(-1);
            if (!snap.telemetry.engineError.trimmed().isEmpty()) {
                m_impl->setErrorText(snap.telemetry.engineError);
            } else {
                m_impl->setErrorText(QString());
            }
        } else if (snap.telemetry.engineOnline) {
            m_impl->updateConnectionBadge(QStringLiteral("ONLINE"), BadgeTone::Success);
            const int lat = (snap.telemetry.endpointLatencyMs >= 0)
                                ? snap.telemetry.endpointLatencyMs
                                : m_impl->lastMeasuredLatencyMs;
            m_impl->updateLatencyDisplay(lat);
            m_impl->setErrorText(QString());
        } else {
            m_impl->updateConnectionBadge(QStringLiteral("OFFLINE"), BadgeTone::Danger);
            m_impl->updateLatencyDisplay(-1);
            m_impl->setErrorText(snap.telemetry.engineError);
        }
    }
}

void EngineSettingsPane::onModelsFetched(const QVector<ModelDescriptor>& models, const QString& error) {
    if (!m_impl) {
        return;
    }

    if (m_impl->testInFlight) {
        m_impl->lastMeasuredLatencyMs = static_cast<int>(m_impl->testTimer.elapsed());
        m_impl->testInFlight = false;
    }

    if (m_impl->testConnectionButton) {
        m_impl->testConnectionButton->setEnabled(true);
        m_impl->testConnectionButton->setText(QStringLiteral("Test Connection & Fetch Models"));
    }

    const bool configured = m_impl->lastSnapshot.endpointConfigured ||
                            !m_impl->lastSnapshot.endpointBaseUrl.trimmed().isEmpty();
    if (m_impl->refreshStatusButton) {
        m_impl->refreshStatusButton->setEnabled(configured);
    }

    const QString trimmedError = error.trimmed();
    if (!trimmedError.isEmpty()) {
        m_impl->updateConnectionBadge(QStringLiteral("OFFLINE"), BadgeTone::Danger);
        m_impl->updateLatencyDisplay(-1);
        m_impl->setErrorText(trimmedError);
        m_impl->setFeedback(
            m_impl->testFeedbackLabel,
            QStringLiteral("Connection test failed. Verify the Base URL, server status, and API key."),
            "feedbackError");
        return;
    }

    m_impl->updateDiscoveredModelsUi(models, true);
    m_impl->updateConnectionBadge(QStringLiteral("ONLINE"), BadgeTone::Success);

    const int latencyMs = (m_impl->lastSnapshot.telemetry.endpointLatencyMs >= 0)
                              ? m_impl->lastSnapshot.telemetry.endpointLatencyMs
                              : m_impl->lastMeasuredLatencyMs;
    m_impl->updateLatencyDisplay(latencyMs);
    m_impl->setErrorText(QString());

    const int count = m_impl->primaryModelCombo ? m_impl->primaryModelCombo->count() : 0;
    if (count == 0) {
        m_impl->setFeedback(
            m_impl->testFeedbackLabel,
            QStringLiteral("Connected — GET /models returned 0 models. Enter a custom Model ID below."),
            "feedbackOk");
    } else if (count == 1) {
        m_impl->setFeedback(
            m_impl->testFeedbackLabel,
            QStringLiteral("Connected — discovered 1 model. Click Save Endpoint & Model to apply."),
            "feedbackOk");
    } else {
        m_impl->setFeedback(
            m_impl->testFeedbackLabel,
            QStringLiteral("Connected — discovered %1 models. Select a model and click Save Endpoint & Model.")
                .arg(count),
            "feedbackOk");
    }
}

void EngineSettingsPane::applyTheme() {
    if (!m_impl) {
        return;
    }

    const ThemeManager& tm = ThemeManager::instance();
    const UiPalette u = tm.ui();

    const QColor solidBorder    = compositeOver(u.subtleBorder, u.background);
    const QColor solidElevated  = compositeOver(u.elevatedSurface, u.background);
    const QColor solidSelected  = compositeOver(u.selectedFill, u.background);
    const QColor solidSecondary = compositeOver(u.textSecondary, u.background);
    const QColor solidMuted     = compositeOver(u.textMuted, u.background);
    const QColor dangerFill     = withAlpha(u.danger, 0.14);
    const QColor dangerBorder   = withAlpha(u.danger, 0.55);

    const QString qss = QStringLiteral(
        "QWidget#EngineSettingsPane {\n"
        "  background-color: %1;\n"
        "}\n"
        "QFrame[spellCard=\"true\"] {\n"
        "  background-color: %2;\n"
        "  border: 1px solid %3;\n"
        "  border-radius: 8px;\n"
        "}\n"
        "QLabel[spellRole=\"fieldLabel\"] {\n"
        "  color: %4;\n"
        "  font-size: 12px;\n"
        "  font-weight: 600;\n"
        "}\n"
        "QLabel[spellRole=\"hint\"] {\n"
        "  color: %5;\n"
        "  font-size: 11px;\n"
        "}\n"
        "QLabel[spellRole=\"preview\"] {\n"
        "  color: %4;\n"
        "  font-size: 11px;\n"
        "}\n"
        "QLabel[spellRole=\"previewError\"] {\n"
        "  color: %6;\n"
        "  font-size: 11px;\n"
        "}\n"
        "QLabel[spellRole=\"latency\"] {\n"
        "  color: %4;\n"
        "  font-size: 11.5px;\n"
        "  padding: 0px 4px;\n"
        "}\n"
        "QLabel[spellRole=\"errorBox\"] {\n"
        "  background-color: %7;\n"
        "  border: 1px solid %8;\n"
        "  border-radius: 6px;\n"
        "  color: %6;\n"
        "  padding: 7px 10px;\n"
        "  font-size: 11px;\n"
        "}\n"
        "QLabel[spellRole=\"feedbackOk\"] {\n"
        "  color: %9;\n"
        "  font-size: 11.5px;\n"
        "}\n"
        "QLabel[spellRole=\"feedbackInfo\"] {\n"
        "  color: %4;\n"
        "  font-size: 11.5px;\n"
        "}\n"
        "QLabel[spellRole=\"feedbackError\"] {\n"
        "  color: %6;\n"
        "  font-size: 11.5px;\n"
        "}\n"
        "QPushButton[preset=\"true\"] {\n"
        "  background-color: %2;\n"
        "  color: %4;\n"
        "  border: 1px solid %3;\n"
        "  border-radius: 6px;\n"
        "  padding: 4px 9px;\n"
        "  font-size: 11.5px;\n"
        "  font-weight: 600;\n"
        "}\n"
        "QPushButton[preset=\"true\"]:hover {\n"
        "  background-color: %10;\n"
        "  color: %11;\n"
        "  border-color: %12;\n"
        "}\n"
        "QPushButton[preset=\"true\"][presetActive=\"true\"] {\n"
        "  background-color: %13;\n"
        "  color: %11;\n"
        "  border-color: %12;\n"
        "}\n"
        "QComboBox[editableModel=\"true\"] QLineEdit {\n"
        "  border: none;\n"
        "  background-color: transparent;\n"
        "  padding: 0px;\n"
        "  margin: 0px;\n"
        "}\n"
    ).arg(qssColor(u.background),
          qssColor(u.surface),
          qssColor(solidBorder),
          qssColor(solidSecondary),
          qssColor(solidMuted),
          qssColor(u.danger),
          qssColor(dangerFill),
          qssColor(dangerBorder),
          qssColor(u.diffAdded),
          qssColor(solidElevated),
          qssColor(u.textPrimary),
          qssColor(u.selectedBorder),
          qssColor(solidSelected));

    setStyleSheet(qss);

    QFont monoInput = tm.editorFont();
    monoInput.setPointSizeF(11.5);
    QFont monoSmall = tm.editorFont();
    monoSmall.setPointSizeF(10.5);

    if (m_impl->baseUrlEdit) {
        m_impl->baseUrlEdit->setFont(monoInput);
    }
    if (m_impl->apiKeyEdit) {
        m_impl->apiKeyEdit->setFont(monoInput);
    }
    if (m_impl->primaryModelCombo) {
        m_impl->primaryModelCombo->setFont(monoInput);
        if (m_impl->primaryModelCombo->lineEdit()) {
            m_impl->primaryModelCombo->lineEdit()->setFont(monoInput);
        }
    }
    if (m_impl->subagentModelCombo) {
        m_impl->subagentModelCombo->setFont(monoInput);
        if (m_impl->subagentModelCombo->lineEdit()) {
            m_impl->subagentModelCombo->lineEdit()->setFont(monoInput);
        }
    }
    if (m_impl->normalizedPreviewLabel) {
        m_impl->normalizedPreviewLabel->setFont(monoSmall);
    }
    if (m_impl->latencyLabel) {
        m_impl->latencyLabel->setFont(monoSmall);
    }
    if (m_impl->errorLabel) {
        m_impl->errorLabel->setFont(monoSmall);
    }

    // Re-tint icons to match active monochrome palette
    if (m_impl->presetOpenAiButton) {
        m_impl->presetOpenAiButton->setIcon(icons::vectorIcon(QStringLiteral("globe"), u.textPrimary, 13));
    }
    if (m_impl->presetOpenRouterButton) {
        m_impl->presetOpenRouterButton->setIcon(icons::vectorIcon(QStringLiteral("globe"), u.textPrimary, 13));
    }
    if (m_impl->presetOllamaButton) {
        m_impl->presetOllamaButton->setIcon(icons::vectorIcon(QStringLiteral("server"), u.textPrimary, 13));
    }
    if (m_impl->presetLmStudioButton) {
        m_impl->presetLmStudioButton->setIcon(icons::vectorIcon(QStringLiteral("server"), u.textPrimary, 13));
    }
    if (m_impl->presetClearButton) {
        m_impl->presetClearButton->setIcon(icons::vectorIcon(QStringLiteral("close"), u.textSecondary, 13));
    }
    if (m_impl->testConnectionButton) {
        m_impl->testConnectionButton->setIcon(icons::vectorIcon(QStringLiteral("refresh"), u.textPrimary, 14));
    }
    if (m_impl->showHideKeyButton) {
        const bool visible = m_impl->showHideKeyButton->isChecked();
        m_impl->showHideKeyButton->setIcon(
            icons::vectorIcon(visible ? QStringLiteral("unlock") : QStringLiteral("lock"),
                              u.textPrimary, 14));
    }
    if (m_impl->saveKeyButton) {
        m_impl->saveKeyButton->setIcon(icons::vectorIcon(QStringLiteral("save"), u.textPrimary, 14));
    }
    if (m_impl->clearKeyButton) {
        m_impl->clearKeyButton->setIcon(icons::vectorIcon(QStringLiteral("trash"), u.danger, 14));
    }
    if (m_impl->saveEndpointButton) {
        m_impl->saveEndpointButton->setIcon(icons::vectorIcon(QStringLiteral("check"), u.textPrimary, 14));
    }

    for (SpellSectionHeader* h : m_impl->sectionHeaders) {
        if (h) {
            h->applyTheme();
        }
    }
    for (SpellHairline* line : m_impl->hairlines) {
        if (line) {
            line->applyTheme();
        }
    }
    for (SpellValueRow* row : m_impl->valueRows) {
        if (row) {
            row->applyTheme();
        }
    }
    if (m_impl->statusBadge) {
        m_impl->statusBadge->applyTheme();
    }
    if (m_impl->apiKeyStatusBadge) {
        m_impl->apiKeyStatusBadge->applyTheme();
    }
    if (m_impl->discoveredModelsBadge) {
        m_impl->discoveredModelsBadge->applyTheme();
    }
    if (m_impl->refreshStatusButton) {
        m_impl->refreshStatusButton->applyTheme();
    }
}

} // namespace spell
