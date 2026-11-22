#include "SpellCoreClient.h"

#include "spell_core.h"

#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace spell {

namespace {

constexpr int kPatchGenPollIntervalMs = 30;
constexpr int kLocalTimerPollIntervalMs = 1000;
constexpr const char* kKeyringService = "com.spell.SpellIDE.credentials";

struct SpellStringDeleter {
    void operator()(char* ptr) const noexcept {
        if (ptr != nullptr) {
            spell_free_string(ptr);
        }
    }
};

using SpellStringPtr = std::unique_ptr<char, SpellStringDeleter>;

void registerSpellMetaTypesOnce() {
    static std::once_flag once;
    std::call_once(once, []() {
        qRegisterMetaType<spell::ModelDescriptor>("spell::ModelDescriptor");
        qRegisterMetaType<QVector<spell::ModelDescriptor>>("QVector<spell::ModelDescriptor>");
        qRegisterMetaType<spell::ProjectInfo>("spell::ProjectInfo");
        qRegisterMetaType<spell::ThreadSummary>("spell::ThreadSummary");
        qRegisterMetaType<spell::UiChatEvent>("spell::UiChatEvent");
        qRegisterMetaType<spell::StagedDiff>("spell::StagedDiff");
        qRegisterMetaType<spell::AgentTimer>("spell::AgentTimer");
        qRegisterMetaType<spell::RubricTag>("spell::RubricTag");
        qRegisterMetaType<spell::SubagentSlotStatus>("spell::SubagentSlotStatus");
        qRegisterMetaType<spell::ActiveSubagentInfo>("spell::ActiveSubagentInfo");
        qRegisterMetaType<spell::OnlineResearchSettings>("spell::OnlineResearchSettings");
        qRegisterMetaType<spell::ToolPermissionEntry>("spell::ToolPermissionEntry");
        qRegisterMetaType<spell::TerminalApprovalRequest>("spell::TerminalApprovalRequest");
        qRegisterMetaType<spell::SystemInfo>("spell::SystemInfo");
        qRegisterMetaType<spell::TelemetrySnapshot>("spell::TelemetrySnapshot");
        qRegisterMetaType<spell::WorkspaceSnapshot>("spell::WorkspaceSnapshot");
    });
}

bool looksLikeSnapshotObject(const QJsonObject& obj) {
    if (obj.contains(QStringLiteral("projects")) ||
        obj.contains(QStringLiteral("threads")) ||
        obj.contains(QStringLiteral("telemetry")) ||
        obj.contains(QStringLiteral("endpoint_configured")) ||
        obj.contains(QStringLiteral("available_models"))) {
        return true;
    }
    const QJsonValue nestedSnap = obj.value(QStringLiteral("snapshot"));
    if (nestedSnap.isObject()) {
        return looksLikeSnapshotObject(nestedSnap.toObject());
    }
    const QJsonValue nestedData = obj.value(QStringLiteral("data"));
    if (nestedData.isObject() && nestedData.toObject().contains(QStringLiteral("projects"))) {
        return true;
    }
    return false;
}

QString extractResponseError(const QJsonObject& obj) {
    const QJsonValue okVal = obj.value(QStringLiteral("ok"));
    if (okVal.isBool() && !okVal.toBool()) {
        const QString err = obj.value(QStringLiteral("error")).toString().trimmed();
        return err.isEmpty() ? QStringLiteral("Unknown error from Spell core engine") : err;
    }
    if (!looksLikeSnapshotObject(obj)) {
        const QString err = obj.value(QStringLiteral("error")).toString().trimmed();
        if (!err.isEmpty()) {
            return err;
        }
    }
    return {};
}

bool isUnknownCommandError(const QString& error) {
    const QString lower = error.toLower();
    return lower.contains(QLatin1StringView("unknown session command")) ||
           lower.contains(QLatin1StringView("unknown command")) ||
           lower.contains(QLatin1StringView("unrecognized command")) ||
           lower.contains(QLatin1StringView("unsupported command"));
}

QString resolveCommandName(const QJsonObject& cmd) {
    const QJsonValue cmdVal = cmd.value(QStringLiteral("cmd"));
    if (cmdVal.isString() && !cmdVal.toString().trimmed().isEmpty()) {
        return cmdVal.toString().trimmed();
    }
    const QJsonValue actionVal = cmd.value(QStringLiteral("action"));
    if (actionVal.isString() && !actionVal.toString().trimmed().isEmpty()) {
        return actionVal.toString().trimmed();
    }
    const QJsonValue typeVal = cmd.value(QStringLiteral("type"));
    if (typeVal.isString() && !typeVal.toString().trimmed().isEmpty()) {
        return typeVal.toString().trimmed();
    }
    return {};
}

QJsonObject normalizeCommandPayload(const QJsonObject& cmd) {
    QJsonObject normalized = cmd;
    const QString cmdName = resolveCommandName(normalized);
    if (!cmdName.isEmpty()) {
        if (!normalized.contains(QStringLiteral("cmd")) ||
            !normalized.value(QStringLiteral("cmd")).isString() ||
            normalized.value(QStringLiteral("cmd")).toString().trimmed().isEmpty()) {
            normalized.insert(QStringLiteral("cmd"), cmdName);
        }
        // Only backfill "action" when absent so sub-actions like stage_write_action("commit")
        // or loop_control("resume") are never overwritten.
        if (!normalized.contains(QStringLiteral("action"))) {
            normalized.insert(QStringLiteral("action"), cmdName);
        }
    }
    return normalized;
}

bool isBusyCommand(const QJsonObject& cmd) {
    const QString name = resolveCommandName(cmd);
    if (name == QLatin1StringView("send_prompt") ||
        name == QLatin1StringView("edit_or_regenerate_turn") ||
        name == QLatin1StringView("regenerate_turn") ||
        name == QLatin1StringView("run_subagent_slot")) {
        return true;
    }
    if (name == QLatin1StringView("resolve_tool_confirmation") ||
        name == QLatin1StringView("approve_tool_call") ||
        name == QLatin1StringView("confirm_tool_call")) {
        return cmd.value(QStringLiteral("approved")).toBool(false);
    }
    return false;
}

QVector<ModelDescriptor> parseModelsFromResponse(const QJsonObject& obj) {
    QVector<ModelDescriptor> models;
    QJsonArray arr = detail::getArray(obj, {"models", "available_models", "availableModels"});
    if (arr.isEmpty()) {
        const QJsonObject nestedSnap = detail::getObject(obj, {"snapshot", "data"});
        if (!nestedSnap.isEmpty()) {
            arr = detail::getArray(nestedSnap, {"models", "available_models", "availableModels"});
        }
    }
    models.reserve(arr.size());
    for (const QJsonValue& item : arr) {
        if (item.isObject()) {
            ModelDescriptor m = ModelDescriptor::fromJson(item.toObject());
            if (!m.id.isEmpty()) {
                models.append(std::move(m));
            }
        } else if (item.isString()) {
            const QString id = item.toString().trimmed();
            if (!id.isEmpty()) {
                models.append(ModelDescriptor{id, {}, 0});
            }
        }
    }
    return models;
}

SystemInfo populateFallbackSystemInfo(SystemInfo info) {
    if (info.os.isEmpty()) {
        info.os = QSysInfo::prettyProductName();
        if (info.os.isEmpty()) {
            info.os = QSysInfo::productType();
        }
    }
    if (info.osVersion.isEmpty()) {
        info.osVersion = QSysInfo::productVersion();
        if (info.osVersion.isEmpty()) {
            info.osVersion = QSysInfo::kernelVersion();
        }
    }
    if (info.arch.isEmpty()) {
        info.arch = QSysInfo::currentCpuArchitecture();
    }
    if (info.cpuCores <= 0) {
        info.cpuCores = std::max(1, QThread::idealThreadCount());
    }
    if (info.cpuBrand.isEmpty()) {
        info.cpuBrand = QStringLiteral("%1 (%2 logical cores)")
                            .arg(info.arch.isEmpty() ? QStringLiteral("unknown") : info.arch)
                            .arg(info.cpuCores);
    }
    if (info.hostname.isEmpty()) {
        info.hostname = QSysInfo::machineHostName();
    }
    if (info.dataDir.isEmpty()) {
        const QString envDir = qEnvironmentVariable("SPELL_DATA_DIR").trimmed();
        if (!envDir.isEmpty()) {
            info.dataDir = QDir::cleanPath(envDir);
        } else {
            const QString genericData =
                QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation).trimmed();
            if (!genericData.isEmpty()) {
                info.dataDir = QDir::cleanPath(QDir(genericData).filePath(QStringLiteral("SpellIDE")));
            } else {
                info.dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            }
        }
    }
    if (info.logsDir.isEmpty() && !info.dataDir.isEmpty()) {
        info.logsDir = QDir(info.dataDir).filePath(QStringLiteral("logs"));
    }
    return info;
}

bool hasActivePendingTimer(const WorkspaceSnapshot& snap) {
    for (const AgentTimer& t : snap.activeTimers) {
        if (t.delivered) {
            continue;
        }
        const QString st = t.status.trimmed().toLower();
        if (st == QLatin1StringView("cancelled") ||
            st == QLatin1StringView("canceled") ||
            st == QLatin1StringView("fired") ||
            st == QLatin1StringView("delivered")) {
            continue;
        }
        return true;
    }
    return false;
}

QString terminalApprovalKey(const TerminalApprovalRequest& req) {
    if (!req.id.trimmed().isEmpty()) {
        return req.id.trimmed();
    }
    return req.command + QLatin1Char(':') + QString::number(req.createdAt);
}

struct SessionState {
    QByteArray dbPathUtf8;
    mutable QMutex sessionMutex;
    SpellCoreSession* session = nullptr;

    std::atomic<bool> shuttingDown{false};
    std::atomic<int> inFlightCount{0};
    std::atomic<int> busyOperationsInFlight{0};
    std::atomic<bool> effectiveBusy{false};
    std::atomic<std::uint64_t> lastObservedPatchGen{0};

    ~SessionState() {
        QMutexLocker lock(&sessionMutex);
        if (session != nullptr) {
            spell_session_free(session);
            session = nullptr;
        }
    }

    SpellCoreSession* ensureSessionLocked() {
        if (session == nullptr && !shuttingDown.load(std::memory_order_acquire)) {
            const char* rawPath = dbPathUtf8.isEmpty() ? nullptr : dbPathUtf8.constData();
            session = spell_session_new(rawPath);
        }
        return session;
    }

    QJsonObject dispatchLocked(const QJsonObject& normalizedCmd) {
        SpellCoreSession* handle = ensureSessionLocked();
        if (handle == nullptr) {
            return QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"), QStringLiteral("Failed to initialize Spell core session")},
            };
        }

        const QByteArray payload = QJsonDocument(normalizedCmd).toJson(QJsonDocument::Compact);
        SpellStringPtr rawResp(spell_session_dispatch_json(handle, payload.constData()));
        if (!rawResp) {
            return QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"), QStringLiteral("Spell core returned a null response pointer")},
            };
        }

        const QByteArray respBytes(rawResp.get());
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(respBytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            return QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"),
                 QStringLiteral("Failed to parse JSON response from Spell core: %1")
                     .arg(parseError.errorString())},
            };
        }
        return doc.object();
    }
};

struct DispatchOptions {
    bool emitErrorSignal = true;
    bool autoUpdateSnapshot = true;
    bool fallbackFetchSnapshotIfAckOnly = false;
    bool forceBusy = false;
    bool suppressUnchangedSnapshotEmit = false;
};

} // namespace

struct SpellCoreClient::Impl {
    SpellCoreClient* owner = nullptr;
    std::shared_ptr<SessionState> state;
    std::unique_ptr<QThread> workerThread;
    QObject* workerContext = nullptr;

    mutable QMutex snapshotMutex;
    WorkspaceSnapshot cachedSnapshot;
    bool hasInitialSnapshot = false;

    mutable QMutex fallbackSecretsMutex;
    QHash<QString, QString> fallbackSecrets;

    // UI-thread state
    QTimer* patchGenTimer = nullptr;
    QTimer* localTimerPoller = nullptr;
    bool lastEmittedBusy = false;
    bool patchGenRefreshPending = false;
    bool timerPollPending = false;
    QSet<QString> emittedTerminalApprovalKeys;

    // Local shadow for autonomous loop preferences when backend snapshots omit explicit fields
    std::optional<bool> localAutonomousLoopEnabled;
    std::optional<int> localMaxAgentSteps;

    explicit Impl(SpellCoreClient* client, const QString& dbPath)
        : owner(client)
        , state(std::make_shared<SessionState>())
        , workerThread(std::make_unique<QThread>()) {
        state->dbPathUtf8 = dbPath.trimmed().toUtf8();
        state->lastObservedPatchGen.store(spell_get_patch_gen(), std::memory_order_release);

        workerThread->setObjectName(QStringLiteral("SpellCoreWorker"));
        workerContext = new QObject();
        workerContext->moveToThread(workerThread.get());

        // Clean up the Rust session and worker context on the worker thread right before exit.
        QObject::connect(workerThread.get(), &QThread::finished, [ctx = workerContext, st = state]() {
            {
                QMutexLocker lock(&st->sessionMutex);
                if (st->session != nullptr) {
                    spell_session_free(st->session);
                    st->session = nullptr;
                }
            }
            delete ctx;
        });

        workerThread->start();

        // Warm up SpellCoreSession asynchronously on the worker thread so the GUI thread never blocks.
        QMetaObject::invokeMethod(
            workerContext,
            [st = state]() {
                if (st->shuttingDown.load(std::memory_order_acquire)) {
                    return;
                }
                QMutexLocker lock(&st->sessionMutex);
                (void)st->ensureSessionLocked();
            },
            Qt::QueuedConnection);

        patchGenTimer = new QTimer(owner);
        patchGenTimer->setInterval(kPatchGenPollIntervalMs);
        QObject::connect(patchGenTimer, &QTimer::timeout, owner, [this]() { onPatchGenTick(); });

        localTimerPoller = new QTimer(owner);
        localTimerPoller->setInterval(kLocalTimerPollIntervalMs);
        QObject::connect(localTimerPoller, &QTimer::timeout, owner, [this]() { onLocalTimerTick(); });
    }

    ~Impl() {
        if (patchGenTimer != nullptr) {
            patchGenTimer->stop();
        }
        if (localTimerPoller != nullptr) {
            localTimerPoller->stop();
        }

        state->shuttingDown.store(true, std::memory_order_release);
        if (state->inFlightCount.load(std::memory_order_acquire) > 0) {
            (void)spell_request_turn_cancel();
        }

        if (workerThread && workerContext != nullptr) {
            // Post quit() onto workerContext's event queue so it can never race with QThread::exec() startup.
            QMetaObject::invokeMethod(
                workerContext,
                [thr = workerThread.get()]() { thr->quit(); },
                Qt::QueuedConnection);
            workerThread->wait();
            workerContext = nullptr;
        }
    }

    void updateBusyOnUiThread() {
        bool snapshotBusy = false;
        {
            QMutexLocker lock(&snapshotMutex);
            snapshotBusy = cachedSnapshot.isAgentBusy;
        }
        const bool busy =
            (state->busyOperationsInFlight.load(std::memory_order_acquire) > 0) || snapshotBusy;
        state->effectiveBusy.store(busy, std::memory_order_release);
        if (busy != lastEmittedBusy) {
            lastEmittedBusy = busy;
            emit owner->busyChanged(busy);
        }
    }

    void applySnapshotOnUiThread(WorkspaceSnapshot snap, bool suppressIfUnchanged) {
        if (localAutonomousLoopEnabled.has_value() &&
            !snap.raw.contains(QStringLiteral("autonomous_loop_enabled")) &&
            !snap.raw.contains(QStringLiteral("autonomousLoopEnabled"))) {
            snap.autonomousLoopEnabled = *localAutonomousLoopEnabled;
        }
        if (localMaxAgentSteps.has_value() &&
            !snap.raw.contains(QStringLiteral("max_agent_steps")) &&
            !snap.raw.contains(QStringLiteral("maxAgentSteps")) &&
            !snap.raw.contains(QStringLiteral("max_steps")) &&
            !snap.raw.contains(QStringLiteral("maxSteps"))) {
            snap.maxAgentSteps = *localMaxAgentSteps;
        }
        if (state->busyOperationsInFlight.load(std::memory_order_acquire) > 0) {
            snap.isAgentBusy = true;
        }

        bool changed = true;
        {
            QMutexLocker lock(&snapshotMutex);
            if (hasInitialSnapshot && snap == cachedSnapshot) {
                changed = false;
            }
            cachedSnapshot = snap;
            hasInitialSnapshot = true;
        }

        // Track newly observed terminal approval requests and prune resolved ones.
        QSet<QString> currentKeys;
        QVector<TerminalApprovalRequest> newRequests;
        for (const TerminalApprovalRequest& req : snap.pendingTerminalApprovals) {
            const QString key = terminalApprovalKey(req);
            currentKeys.insert(key);
            if (!emittedTerminalApprovalKeys.contains(key)) {
                emittedTerminalApprovalKeys.insert(key);
                newRequests.append(req);
            }
        }
        emittedTerminalApprovalKeys.intersect(currentKeys);

        updateBusyOnUiThread();

        if (changed || !suppressIfUnchanged) {
            emit owner->snapshotUpdated(snap);
        }
        for (const TerminalApprovalRequest& req : newRequests) {
            emit owner->terminalCommandRequested(req);
        }
    }

    void dispatchInternal(const QJsonObject& rawCmd,
                          DispatchOptions opts,
                          std::function<void(QJsonObject)> callback = nullptr) {
        if (state->shuttingDown.load(std::memory_order_acquire) || workerContext == nullptr) {
            return;
        }

        const QJsonObject normalizedCmd = normalizeCommandPayload(rawCmd);
        const bool trackBusy = opts.forceBusy || isBusyCommand(normalizedCmd);

        if (trackBusy) {
            state->busyOperationsInFlight.fetch_add(1, std::memory_order_acq_rel);
            state->effectiveBusy.store(true, std::memory_order_release);
            QPointer<SpellCoreClient> self(owner);
            if (QThread::currentThread() == owner->thread()) {
                updateBusyOnUiThread();
            } else {
                QMetaObject::invokeMethod(
                    owner,
                    [self]() {
                        if (self && self->m_impl) {
                            self->m_impl->updateBusyOnUiThread();
                        }
                    },
                    Qt::QueuedConnection);
            }
        }

        state->inFlightCount.fetch_add(1, std::memory_order_acq_rel);

        SpellCoreClient* target = owner;
        auto st = state;

        QMetaObject::invokeMethod(
            workerContext,
            [st, target, normalizedCmd, opts, trackBusy, cb = std::move(callback)]() mutable {
                struct InFlightGuard {
                    std::shared_ptr<SessionState> s;
                    ~InFlightGuard() { s->inFlightCount.fetch_sub(1, std::memory_order_acq_rel); }
                } inFlightGuard{st};

                if (st->shuttingDown.load(std::memory_order_acquire)) {
                    if (trackBusy) {
                        st->busyOperationsInFlight.fetch_sub(1, std::memory_order_acq_rel);
                    }
                    return;
                }

                QJsonObject responseObj;
                QJsonObject fallbackSnapshotObj;
                {
                    QMutexLocker lock(&st->sessionMutex);
                    responseObj = st->dispatchLocked(normalizedCmd);

                    const QString err = extractResponseError(responseObj);
                    if (err.isEmpty() && opts.fallbackFetchSnapshotIfAckOnly &&
                        !looksLikeSnapshotObject(responseObj) &&
                        !st->shuttingDown.load(std::memory_order_acquire)) {
                        QJsonObject getSnapCmd;
                        getSnapCmd.insert(QStringLiteral("cmd"), QStringLiteral("get_snapshot"));
                        getSnapCmd.insert(QStringLiteral("action"), QStringLiteral("get_snapshot"));
                        fallbackSnapshotObj = st->dispatchLocked(getSnapCmd);
                    }

                    // Consume our own SPELL_PATCH_GEN bump so the 30 ms poller doesn't trigger a redundant refresh.
                    st->lastObservedPatchGen.store(spell_get_patch_gen(), std::memory_order_release);
                }

                if (st->shuttingDown.load(std::memory_order_acquire)) {
                    if (trackBusy) {
                        st->busyOperationsInFlight.fetch_sub(1, std::memory_order_acq_rel);
                    }
                    return;
                }

                QMetaObject::invokeMethod(
                    target,
                    [target, st, responseObj, fallbackSnapshotObj, opts, trackBusy,
                     uiCb = std::move(cb)]() mutable {
                        if (trackBusy) {
                            st->busyOperationsInFlight.fetch_sub(1, std::memory_order_acq_rel);
                        }
                        if (st->shuttingDown.load(std::memory_order_acquire) || !target->m_impl) {
                            return;
                        }

                        Impl* impl = target->m_impl.get();
                        const QString err = extractResponseError(responseObj);

                        if (opts.autoUpdateSnapshot) {
                            if (looksLikeSnapshotObject(responseObj)) {
                                impl->applySnapshotOnUiThread(
                                    WorkspaceSnapshot::fromJson(responseObj),
                                    opts.suppressUnchangedSnapshotEmit);
                            } else if (!fallbackSnapshotObj.isEmpty() &&
                                       looksLikeSnapshotObject(fallbackSnapshotObj)) {
                                impl->applySnapshotOnUiThread(
                                    WorkspaceSnapshot::fromJson(fallbackSnapshotObj),
                                    opts.suppressUnchangedSnapshotEmit);
                            } else {
                                impl->updateBusyOnUiThread();
                            }
                        } else {
                            impl->updateBusyOnUiThread();
                        }

                        if (!err.isEmpty() && opts.emitErrorSignal) {
                            emit target->errorOccurred(err);
                        }

                        if (uiCb) {
                            uiCb(responseObj);
                        }
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }

    void onPatchGenTick() {
        if (state->shuttingDown.load(std::memory_order_acquire)) {
            return;
        }

        const std::uint64_t currentGen = spell_get_patch_gen();
        const std::uint64_t lastGen = state->lastObservedPatchGen.load(std::memory_order_acquire);
        if (currentGen == lastGen) {
            return;
        }

        // If a streaming turn or command is actively executing on the worker thread,
        // read the lock-free cached snapshot so UI tokens stream at ~30 ms without waiting on sessionMutex.
        if (state->inFlightCount.load(std::memory_order_acquire) > 0) {
            state->lastObservedPatchGen.store(currentGen, std::memory_order_release);
            SpellStringPtr rawCached(spell_get_cached_snapshot_json());
            if (rawCached) {
                const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(rawCached.get()));
                if (doc.isObject() && looksLikeSnapshotObject(doc.object())) {
                    applySnapshotOnUiThread(WorkspaceSnapshot::fromJson(doc.object()), true);
                }
            }
            return;
        }

        // Worker is idle and spell_get_patch_gen() advanced: dispatch get_snapshot once on worker thread.
        if (patchGenRefreshPending) {
            return;
        }
        state->lastObservedPatchGen.store(currentGen, std::memory_order_release);
        patchGenRefreshPending = true;

        DispatchOptions opts;
        opts.emitErrorSignal = false;
        opts.autoUpdateSnapshot = true;
        opts.suppressUnchangedSnapshotEmit = false;

        QJsonObject cmd;
        cmd.insert(QStringLiteral("cmd"), QStringLiteral("get_snapshot"));
        QPointer<SpellCoreClient> self(owner);
        dispatchInternal(cmd, opts, [self](const QJsonObject&) {
            if (self && self->m_impl) {
                self->m_impl->patchGenRefreshPending = false;
            }
        });
    }

    void onLocalTimerTick() {
        if (state->shuttingDown.load(std::memory_order_acquire) || timerPollPending) {
            return;
        }
        if (state->busyOperationsInFlight.load(std::memory_order_acquire) > 0) {
            return;
        }

        WorkspaceSnapshot snap;
        {
            QMutexLocker lock(&snapshotMutex);
            snap = cachedSnapshot;
        }

        if (!hasActivePendingTimer(snap)) {
            return;
        }

        QStringList targetThreads;
        for (const AgentTimer& t : snap.activeTimers) {
            if (t.delivered) {
                continue;
            }
            const QString st = t.status.trimmed().toLower();
            if (st == QLatin1StringView("cancelled") ||
                st == QLatin1StringView("canceled") ||
                st == QLatin1StringView("fired") ||
                st == QLatin1StringView("delivered")) {
                continue;
            }
            const QString tid = t.threadId.trimmed().isEmpty()
                                    ? snap.selectedThreadId.trimmed()
                                    : t.threadId.trimmed();
            if (!tid.isEmpty() && !targetThreads.contains(tid)) {
                targetThreads.append(tid);
            }
        }
        if (targetThreads.isEmpty() && !snap.selectedThreadId.trimmed().isEmpty()) {
            targetThreads.append(snap.selectedThreadId.trimmed());
        }
        if (targetThreads.isEmpty()) {
            return;
        }

        timerPollPending = true;
        const auto remainingPolls = std::make_shared<int>(static_cast<int>(targetThreads.size()));
        QPointer<SpellCoreClient> self(owner);

        for (const QString& tid : targetThreads) {
            QJsonObject cmd;
            cmd.insert(QStringLiteral("cmd"), QStringLiteral("fire_due_timers"));
            cmd.insert(QStringLiteral("thread_id"), tid);
            cmd.insert(QStringLiteral("threadId"), tid);

            DispatchOptions opts;
            opts.emitErrorSignal = false;
            opts.autoUpdateSnapshot = true;
            opts.suppressUnchangedSnapshotEmit = true;

            dispatchInternal(cmd, opts, [self, remainingPolls](const QJsonObject&) {
                if (--(*remainingPolls) <= 0 && self && self->m_impl) {
                    self->m_impl->timerPollPending = false;
                }
            });
        }
    }
};

// =============================================================================
// Lifecycle
// =============================================================================

SpellCoreClient::SpellCoreClient(const QString& dbPath, QObject* parent)
    : QObject(parent) {
    registerSpellMetaTypesOnce();
    m_impl = std::make_unique<Impl>(this, dbPath);
}

SpellCoreClient::~SpellCoreClient() = default;

// =============================================================================
// Thread-safe synchronous accessors
// =============================================================================

WorkspaceSnapshot SpellCoreClient::currentSnapshot() const {
    QMutexLocker lock(&m_impl->snapshotMutex);
    return m_impl->cachedSnapshot;
}

bool SpellCoreClient::isBusy() const {
    return m_impl->state->effectiveBusy.load(std::memory_order_acquire);
}

QString SpellCoreClient::readFileUnrestricted(const QString& path,
                                                std::size_t startLine,
                                                std::size_t endLine) const {
    const QByteArray pathUtf8 = path.toUtf8();
    SpellStringPtr raw(spell_read_file_unrestricted(pathUtf8.constData(), startLine, endLine));
    if (!raw) {
        return {};
    }
    return QString::fromUtf8(raw.get());
}

// =============================================================================
// Low-level async JSON command dispatch
// =============================================================================

void SpellCoreClient::dispatchCommand(const QJsonObject& cmd,
                                        std::function<void(QJsonObject)> callback) {
    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts, std::move(callback));
}

// =============================================================================
// Workspace snapshot & endpoint configuration
// =============================================================================

void SpellCoreClient::refreshSnapshot() {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("get_snapshot"));
    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::configureEndpoint(const QString& baseUrl,
                                          const std::optional<QString>& apiKey,
                                          const QString& modelId,
                                          const QString& subagentModelId) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("configure_endpoint"));
    cmd.insert(QStringLiteral("base_url"), baseUrl);
    cmd.insert(QStringLiteral("baseUrl"), baseUrl);
    if (apiKey.has_value()) {
        cmd.insert(QStringLiteral("api_key"), *apiKey);
        cmd.insert(QStringLiteral("apiKey"), *apiKey);
    }
    cmd.insert(QStringLiteral("model_id"), modelId);
    cmd.insert(QStringLiteral("modelId"), modelId);
    cmd.insert(QStringLiteral("subagent_model_id"), subagentModelId);
    cmd.insert(QStringLiteral("subagentModelId"), subagentModelId);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    opts.fallbackFetchSnapshotIfAckOnly = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::fetchModels(const QString& overrideBaseUrl,
                                    const std::optional<QString>& overrideApiKey) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("fetch_models"));
    const QString trimmedUrl = overrideBaseUrl.trimmed();
    if (!trimmedUrl.isEmpty()) {
        cmd.insert(QStringLiteral("base_url"), trimmedUrl);
        cmd.insert(QStringLiteral("baseUrl"), trimmedUrl);
    }
    if (overrideApiKey.has_value()) {
        cmd.insert(QStringLiteral("api_key"), *overrideApiKey);
        cmd.insert(QStringLiteral("apiKey"), *overrideApiKey);
    }

    DispatchOptions opts;
    opts.emitErrorSignal = false;
    opts.autoUpdateSnapshot = true;

    QPointer<SpellCoreClient> self(this);
    m_impl->dispatchInternal(cmd, opts, [self](const QJsonObject& resp) {
        if (!self) {
            return;
        }
        const QString err = extractResponseError(resp);
        const QVector<ModelDescriptor> models = parseModelsFromResponse(resp);
        emit self->modelsFetched(models, err);
    });
}

void SpellCoreClient::refreshEndpointStatus() {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("refresh_engine_status"));
    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::setCriticEnabled(bool enabled) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("set_critic_enabled"));
    cmd.insert(QStringLiteral("enabled"), enabled);
    cmd.insert(QStringLiteral("critic_enabled"), enabled);
    cmd.insert(QStringLiteral("criticEnabled"), enabled);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

// =============================================================================
// Projects & threads
// =============================================================================

void SpellCoreClient::createProject(const QString& name,
                                      const QString& rootPath,
                                      bool readOnly) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("add_project"));
    cmd.insert(QStringLiteral("name"), name);
    cmd.insert(QStringLiteral("root_path"), rootPath);
    cmd.insert(QStringLiteral("rootPath"), rootPath);
    cmd.insert(QStringLiteral("is_read_only"), readOnly);
    cmd.insert(QStringLiteral("isReadOnly"), readOnly);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::removeProject(const QString& projectId) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("remove_project"));
    cmd.insert(QStringLiteral("project_id"), projectId);
    cmd.insert(QStringLiteral("projectId"), projectId);
    cmd.insert(QStringLiteral("id"), projectId);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    opts.fallbackFetchSnapshotIfAckOnly = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::selectProject(const QString& projectId) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("select_project"));
    cmd.insert(QStringLiteral("project_id"), projectId);
    cmd.insert(QStringLiteral("projectId"), projectId);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::renameProject(const QString& projectId, const QString& name) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("rename_project"));
    cmd.insert(QStringLiteral("project_id"), projectId);
    cmd.insert(QStringLiteral("projectId"), projectId);
    cmd.insert(QStringLiteral("name"), name);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::setProjectReadOnly(const QString& projectId, bool readOnly) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("set_project_read_only"));
    cmd.insert(QStringLiteral("project_id"), projectId);
    cmd.insert(QStringLiteral("projectId"), projectId);
    cmd.insert(QStringLiteral("is_read_only"), readOnly);
    cmd.insert(QStringLiteral("isReadOnly"), readOnly);
    cmd.insert(QStringLiteral("read_only"), readOnly);
    cmd.insert(QStringLiteral("readOnly"), readOnly);

    DispatchOptions opts;
    opts.emitErrorSignal = false;
    opts.autoUpdateSnapshot = true;

    QPointer<SpellCoreClient> self(this);
    m_impl->dispatchInternal(cmd, opts, [self, projectId, readOnly](const QJsonObject& resp) {
        if (!self) {
            return;
        }
        const QString err = extractResponseError(resp);
        if (err.isEmpty()) {
            return;
        }
        if (isUnknownCommandError(err)) {
            const WorkspaceSnapshot snap = self->currentSnapshot();
            bool found = false;
            bool alreadyMatches = false;
            for (const ProjectInfo& proj : snap.projects) {
                if (proj.id == projectId) {
                    found = true;
                    alreadyMatches = (proj.isReadOnly == readOnly);
                    break;
                }
            }
            if (found && alreadyMatches) {
                self->refreshSnapshot();
                return;
            }
            QJsonObject toggleCmd;
            toggleCmd.insert(QStringLiteral("cmd"), QStringLiteral("toggle_project_read_only"));
            toggleCmd.insert(QStringLiteral("project_id"), projectId);
            toggleCmd.insert(QStringLiteral("projectId"), projectId);

            DispatchOptions toggleOpts;
            toggleOpts.emitErrorSignal = true;
            toggleOpts.autoUpdateSnapshot = true;
            self->m_impl->dispatchInternal(toggleCmd, toggleOpts);
            return;
        }
        emit self->errorOccurred(err);
    });
}

void SpellCoreClient::createThread(const QString& projectId,
                                     const QString& title,
                                     const QString& profile) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("create_thread"));
    if (!projectId.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("project_id"), projectId.trimmed());
        cmd.insert(QStringLiteral("projectId"), projectId.trimmed());
    }
    if (!title.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("title"), title.trimmed());
    }
    if (!profile.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("profile"), profile.trimmed().toLower());
    }

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::selectThread(const QString& threadId) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("select_thread"));
    cmd.insert(QStringLiteral("thread_id"), threadId);
    cmd.insert(QStringLiteral("threadId"), threadId);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::deleteThread(const QString& threadId) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("delete_thread"));
    cmd.insert(QStringLiteral("thread_id"), threadId);
    cmd.insert(QStringLiteral("threadId"), threadId);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::renameThread(const QString& threadId, const QString& title) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("rename_thread"));
    cmd.insert(QStringLiteral("thread_id"), threadId);
    cmd.insert(QStringLiteral("threadId"), threadId);
    cmd.insert(QStringLiteral("title"), title);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::setThreadProfile(const QString& threadId, const QString& profile) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("set_thread_profile"));
    if (!threadId.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("thread_id"), threadId.trimmed());
        cmd.insert(QStringLiteral("threadId"), threadId.trimmed());
    }
    const QString normalized = (profile.trimmed().compare(QLatin1StringView("ask"), Qt::CaseInsensitive) == 0)
        ? QStringLiteral("ask")
        : QStringLiteral("write");
    cmd.insert(QStringLiteral("profile"), normalized);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

// =============================================================================
// Agent turns & staged diffs
// =============================================================================

void SpellCoreClient::sendPrompt(const QString& threadId,
                                   const QString& prompt,
                                   const QStringList& attachedFiles) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("send_prompt"));
    if (!threadId.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("thread_id"), threadId.trimmed());
        cmd.insert(QStringLiteral("threadId"), threadId.trimmed());
    }

    QString effectivePrompt = prompt;
    if (!attachedFiles.isEmpty()) {
        const QJsonArray filesArr = detail::toJsonArray(attachedFiles);
        cmd.insert(QStringLiteral("attached_files"), filesArr);
        cmd.insert(QStringLiteral("attachedFiles"), filesArr);

        QStringList unreferencedPaths;
        for (const QString& path : attachedFiles) {
            const QString trimmed = path.trimmed();
            if (!trimmed.isEmpty() && !effectivePrompt.contains(trimmed)) {
                unreferencedPaths.append(trimmed);
            }
        }
        if (!unreferencedPaths.isEmpty()) {
            if (!effectivePrompt.isEmpty() && !effectivePrompt.endsWith(QLatin1Char('\n'))) {
                effectivePrompt.append(QLatin1Char('\n'));
            }
            effectivePrompt.append(QStringLiteral("\n[Attached files]\n"));
            for (const QString& ref : unreferencedPaths) {
                effectivePrompt.append(QStringLiteral("- "));
                effectivePrompt.append(ref);
                effectivePrompt.append(QLatin1Char('\n'));
            }
        }
    }

    cmd.insert(QStringLiteral("prompt"), effectivePrompt);
    cmd.insert(QStringLiteral("text"), effectivePrompt);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    opts.forceBusy = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::editAndResendTurn(const QString& threadId,
                                          int turnIndex,
                                          const QString& newPrompt) {
    QString resolvedEventId;
    if (turnIndex >= 0) {
        const WorkspaceSnapshot snap = currentSnapshot();
        if (threadId.trimmed().isEmpty() || snap.selectedThreadId == threadId.trimmed()) {
            for (const UiChatEvent& ev : snap.activeEvents) {
                if (ev.kind == QLatin1StringView("user") && ev.turnIndex == turnIndex &&
                    !ev.id.isEmpty()) {
                    resolvedEventId = ev.id;
                    break;
                }
            }
            if (resolvedEventId.isEmpty() && turnIndex < snap.activeEvents.size() &&
                snap.activeEvents.at(turnIndex).kind == QLatin1StringView("user") &&
                !snap.activeEvents.at(turnIndex).id.isEmpty()) {
                resolvedEventId = snap.activeEvents.at(turnIndex).id;
            }
            if (resolvedEventId.isEmpty()) {
                int userOrdinal = 0;
                for (const UiChatEvent& ev : snap.activeEvents) {
                    if (ev.kind == QLatin1StringView("user")) {
                        if (userOrdinal == turnIndex && !ev.id.isEmpty()) {
                            resolvedEventId = ev.id;
                            break;
                        }
                        ++userOrdinal;
                    }
                }
            }
        }
    }

    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("edit_or_regenerate_turn"));
    if (!threadId.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("thread_id"), threadId.trimmed());
        cmd.insert(QStringLiteral("threadId"), threadId.trimmed());
    }
    if (turnIndex >= 0) {
        cmd.insert(QStringLiteral("turn_index"), turnIndex);
        cmd.insert(QStringLiteral("turnIndex"), turnIndex);
    }
    if (!resolvedEventId.isEmpty()) {
        cmd.insert(QStringLiteral("event_id"), resolvedEventId);
        cmd.insert(QStringLiteral("eventId"), resolvedEventId);
    }
    if (!newPrompt.isEmpty()) {
        cmd.insert(QStringLiteral("new_prompt"), newPrompt);
        cmd.insert(QStringLiteral("newPrompt"), newPrompt);
    }

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    opts.forceBusy = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::cancelActiveTurn() {
    // Immediate (< 1 ms) lock-free cancellation signal before queuing session command.
    (void)spell_request_turn_cancel();

    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("cancel_active_turn"));

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::approveStagedDiff(const QString& diffId) {
    QString resolvedPath = diffId.trimmed();
    const WorkspaceSnapshot snap = currentSnapshot();
    for (const StagedDiff& d : snap.pendingDiffs) {
        if ((d.id == diffId || d.filePath == diffId) && !d.filePath.trimmed().isEmpty()) {
            resolvedPath = d.filePath.trimmed();
            break;
        }
    }

    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("stage_write_action"));
    cmd.insert(QStringLiteral("action"), QStringLiteral("commit"));
    cmd.insert(QStringLiteral("stage_action"), QStringLiteral("commit"));
    if (!resolvedPath.isEmpty()) {
        cmd.insert(QStringLiteral("path"), resolvedPath);
    }
    if (!diffId.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("diff_id"), diffId.trimmed());
        cmd.insert(QStringLiteral("diffId"), diffId.trimmed());
    }

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::rejectStagedDiff(const QString& diffId) {
    QString resolvedPath = diffId.trimmed();
    const WorkspaceSnapshot snap = currentSnapshot();
    for (const StagedDiff& d : snap.pendingDiffs) {
        if ((d.id == diffId || d.filePath == diffId) && !d.filePath.trimmed().isEmpty()) {
            resolvedPath = d.filePath.trimmed();
            break;
        }
    }

    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("stage_write_action"));
    cmd.insert(QStringLiteral("action"), QStringLiteral("rollback"));
    cmd.insert(QStringLiteral("stage_action"), QStringLiteral("rollback"));
    if (!resolvedPath.isEmpty()) {
        cmd.insert(QStringLiteral("path"), resolvedPath);
    }
    if (!diffId.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("diff_id"), diffId.trimmed());
        cmd.insert(QStringLiteral("diffId"), diffId.trimmed());
    }

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::setStageWriteMode(bool enabled) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("set_stage_write_mode"));
    cmd.insert(QStringLiteral("enabled"), enabled);
    cmd.insert(QStringLiteral("stage_write_mode"), enabled);
    cmd.insert(QStringLiteral("stageWriteMode"), enabled);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::setSubagentProtocol(const QString& protocol) {
    const QString norm = detail::normalizeSubagentProtocol(protocol);
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("set_subagent_protocol"));
    cmd.insert(QStringLiteral("protocol"), norm);
    cmd.insert(QStringLiteral("subagent_protocol"), norm);
    cmd.insert(QStringLiteral("subagentProtocol"), norm);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::setAutonomousLoop(bool enabled, int maxSteps) {
    const int clampedSteps = std::max(1, maxSteps);
    m_impl->localAutonomousLoopEnabled = enabled;
    m_impl->localMaxAgentSteps = clampedSteps;

    const QString loopAction = enabled ? QStringLiteral("resume") : QStringLiteral("pause");
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("loop_control"));
    cmd.insert(QStringLiteral("action"), loopAction);
    cmd.insert(QStringLiteral("loop_action"), loopAction);
    cmd.insert(QStringLiteral("enabled"), enabled);
    cmd.insert(QStringLiteral("autonomous_loop_enabled"), enabled);
    cmd.insert(QStringLiteral("autonomousLoopEnabled"), enabled);
    cmd.insert(QStringLiteral("max_steps"), clampedSteps);
    cmd.insert(QStringLiteral("maxSteps"), clampedSteps);
    cmd.insert(QStringLiteral("max_agent_steps"), clampedSteps);
    cmd.insert(QStringLiteral("maxAgentSteps"), clampedSteps);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::requestLocalContextReset(const QString& threadId) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("request_hot_cache_clear"));
    if (!threadId.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("thread_id"), threadId.trimmed());
        cmd.insert(QStringLiteral("threadId"), threadId.trimmed());
    }

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

// =============================================================================
// Local SQLite timers
// =============================================================================

void SpellCoreClient::scheduleTimer(const QString& threadId,
                                      double durationS,
                                      const QString& label,
                                      const QString& note,
                                      const QString& watchPath) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("schedule_timer"));
    if (!threadId.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("thread_id"), threadId.trimmed());
        cmd.insert(QStringLiteral("threadId"), threadId.trimmed());
    }
    cmd.insert(QStringLiteral("duration_s"), durationS);
    cmd.insert(QStringLiteral("durationS"), durationS);
    cmd.insert(QStringLiteral("label"), label);
    cmd.insert(QStringLiteral("note"), note);
    if (!watchPath.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("watch_path"), watchPath.trimmed());
        cmd.insert(QStringLiteral("watchPath"), watchPath.trimmed());
    }

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::cancelTimer(const QString& timerId) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("cancel_timer"));
    cmd.insert(QStringLiteral("timer_id"), timerId);
    cmd.insert(QStringLiteral("timerId"), timerId);
    cmd.insert(QStringLiteral("id"), timerId);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::fireDueTimersNow(const QString& threadId) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("fire_due_timers"));
    if (!threadId.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("thread_id"), threadId.trimmed());
        cmd.insert(QStringLiteral("threadId"), threadId.trimmed());
    }

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

// =============================================================================
// Subagent slots
// =============================================================================

void SpellCoreClient::runSubagentSlot(int slot,
                                        const QString& mode,
                                        const QString& message,
                                        int maxTokens) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("run_subagent_slot"));
    cmd.insert(QStringLiteral("slot"), slot);
    cmd.insert(QStringLiteral("slot_id"), slot);
    cmd.insert(QStringLiteral("mode"), mode);
    cmd.insert(QStringLiteral("message"), message);
    cmd.insert(QStringLiteral("prompt"), message);
    cmd.insert(QStringLiteral("max_tokens"), maxTokens);
    cmd.insert(QStringLiteral("maxTokens"), maxTokens);

    DispatchOptions opts;
    opts.emitErrorSignal = false;
    opts.autoUpdateSnapshot = true;
    opts.forceBusy = true;

    QPointer<SpellCoreClient> self(this);
    m_impl->dispatchInternal(cmd, opts, [self, slot](const QJsonObject& resp) {
        if (!self) {
            return;
        }
        const QString err = extractResponseError(resp);
        QString reply = detail::getString(resp, {"reply", "content", "output", "message"});
        if (reply.isEmpty()) {
            const QJsonObject subResp = detail::getObject(resp, {"subagent_response", "subagentResponse"});
            if (!subResp.isEmpty()) {
                reply = detail::getString(subResp, {"content", "reply", "output", "message"});
            }
        }
        emit self->subagentSlotReply(slot, reply, err);
    });
}

void SpellCoreClient::terminateSubagentSlot(int slot) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("terminate_subagent_slot"));
    cmd.insert(QStringLiteral("slot"), slot);
    cmd.insert(QStringLiteral("slot_id"), slot);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::duplicateSubagentSlot(int srcSlot, int dstSlot) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("duplicate_subagent_slot"));
    cmd.insert(QStringLiteral("src_slot"), srcSlot);
    cmd.insert(QStringLiteral("srcSlot"), srcSlot);
    cmd.insert(QStringLiteral("source_slot"), srcSlot);
    cmd.insert(QStringLiteral("dst_slot"), dstSlot);
    cmd.insert(QStringLiteral("dstSlot"), dstSlot);
    cmd.insert(QStringLiteral("destination_slot"), dstSlot);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::readSubagentSlotLines(int slot, int limit) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("read_subagent_slot_lines"));
    cmd.insert(QStringLiteral("slot"), slot);
    cmd.insert(QStringLiteral("slot_id"), slot);
    cmd.insert(QStringLiteral("limit"), limit);
    cmd.insert(QStringLiteral("line_count"), limit);

    DispatchOptions opts;
    opts.emitErrorSignal = false;
    opts.autoUpdateSnapshot = false;

    QPointer<SpellCoreClient> self(this);
    m_impl->dispatchInternal(cmd, opts, [self, slot](const QJsonObject& resp) {
        if (!self) {
            return;
        }
        const QString err = extractResponseError(resp);
        QStringList lines = detail::getStringList(resp, {"lines", "recent_lines", "recentLines"});
        if (lines.isEmpty()) {
            const QString text = detail::getString(resp, {"output", "content"});
            if (!text.isEmpty()) {
                lines = text.split(QLatin1Char('\n'));
            }
        }
        emit self->subagentSlotLines(slot, lines);
        if (!err.isEmpty()) {
            emit self->errorOccurred(err);
        }
    });
}

// =============================================================================
// Rubric tags
// =============================================================================

void SpellCoreClient::createTag(const QString& name,
                                  const QString& description,
                                  const QString& directive) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("create_tag"));
    cmd.insert(QStringLiteral("name"), name);
    cmd.insert(QStringLiteral("category"), QStringLiteral("general"));
    cmd.insert(QStringLiteral("description"), description);
    cmd.insert(QStringLiteral("instruction"), directive);
    cmd.insert(QStringLiteral("directive"), directive);
    cmd.insert(QStringLiteral("rubric_prompt"), directive);
    cmd.insert(QStringLiteral("rubricPrompt"), directive);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::updateTag(const QString& id,
                                  const QString& name,
                                  const QString& description,
                                  const QString& directive,
                                  bool enabled) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("upsert_tag"));
    if (!id.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("id"), id.trimmed());
    }
    cmd.insert(QStringLiteral("name"), name);
    cmd.insert(QStringLiteral("category"), QStringLiteral("general"));
    cmd.insert(QStringLiteral("description"), description);
    cmd.insert(QStringLiteral("instruction"), directive);
    cmd.insert(QStringLiteral("directive"), directive);
    cmd.insert(QStringLiteral("rubric_prompt"), directive);
    cmd.insert(QStringLiteral("rubricPrompt"), directive);
    cmd.insert(QStringLiteral("enabled"), enabled);
    cmd.insert(QStringLiteral("is_enabled"), enabled);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::deleteTag(const QString& id) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("delete_tag"));
    cmd.insert(QStringLiteral("id"), id);
    cmd.insert(QStringLiteral("tag_id"), id);
    cmd.insert(QStringLiteral("tagId"), id);
    cmd.insert(QStringLiteral("name"), id);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    opts.fallbackFetchSnapshotIfAckOnly = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::setThreadTags(const QString& threadId, const QStringList& tagIds) {
    const QJsonArray arr = detail::toJsonArray(tagIds);
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("set_thread_tags"));
    if (!threadId.trimmed().isEmpty()) {
        cmd.insert(QStringLiteral("thread_id"), threadId.trimmed());
        cmd.insert(QStringLiteral("threadId"), threadId.trimmed());
    }
    cmd.insert(QStringLiteral("tag_ids"), arr);
    cmd.insert(QStringLiteral("tagIds"), arr);
    cmd.insert(QStringLiteral("tags"), arr);
    cmd.insert(QStringLiteral("active_tags"), arr);

    DispatchOptions opts;
    opts.emitErrorSignal = false;
    opts.autoUpdateSnapshot = true;

    QPointer<SpellCoreClient> self(this);
    m_impl->dispatchInternal(cmd, opts, [self, threadId, tagIds](const QJsonObject& resp) {
        if (!self) {
            return;
        }
        const QString err = extractResponseError(resp);
        if (err.isEmpty()) {
            return;
        }
        if (isUnknownCommandError(err)) {
            const WorkspaceSnapshot snap = self->currentSnapshot();
            QSet<QString> currentSet;
            for (const QString& existing : snap.activeThreadTagIds) {
                const QString trimmed = existing.trimmed();
                if (!trimmed.isEmpty()) {
                    currentSet.insert(trimmed);
                }
            }
            QSet<QString> desiredSet;
            for (const QString& desired : tagIds) {
                const QString trimmed = desired.trimmed();
                if (!trimmed.isEmpty()) {
                    desiredSet.insert(trimmed);
                }
            }

            QStringList toToggle;
            for (const QString& t : desiredSet) {
                if (!currentSet.contains(t)) {
                    toToggle.append(t);
                }
            }
            for (const QString& t : currentSet) {
                if (!desiredSet.contains(t)) {
                    toToggle.append(t);
                }
            }

            if (toToggle.isEmpty()) {
                self->refreshSnapshot();
                return;
            }

            const QString effTid = threadId.trimmed().isEmpty()
                                       ? snap.selectedThreadId.trimmed()
                                       : threadId.trimmed();
            for (const QString& tagName : toToggle) {
                QJsonObject toggleCmd;
                toggleCmd.insert(QStringLiteral("cmd"), QStringLiteral("toggle_tag"));
                if (!effTid.isEmpty()) {
                    toggleCmd.insert(QStringLiteral("thread_id"), effTid);
                    toggleCmd.insert(QStringLiteral("threadId"), effTid);
                }
                toggleCmd.insert(QStringLiteral("tag_name"), tagName);
                toggleCmd.insert(QStringLiteral("tagName"), tagName);
                toggleCmd.insert(QStringLiteral("name"), tagName);

                DispatchOptions toggleOpts;
                toggleOpts.emitErrorSignal = true;
                toggleOpts.autoUpdateSnapshot = true;
                self->m_impl->dispatchInternal(toggleCmd, toggleOpts);
            }
            return;
        }
        emit self->errorOccurred(err);
    });
}

// =============================================================================
// Online research, permissions & terminal bridge
// =============================================================================

void SpellCoreClient::setOnlineResearchSettings(const OnlineResearchSettings& settings) {
    QJsonObject cmd = settings.toJson();
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("configure_online_research"));
    cmd.insert(QStringLiteral("action"), QStringLiteral("configure_online_research"));

    const QJsonArray allowArr = detail::toJsonArray(settings.allowlist);
    const QJsonArray denyArr = detail::toJsonArray(settings.denylist);
    cmd.insert(QStringLiteral("custom_allowed_domains"), allowArr);
    cmd.insert(QStringLiteral("customAllowedDomains"), allowArr);
    cmd.insert(QStringLiteral("denied_domains"), denyArr);
    cmd.insert(QStringLiteral("deniedDomains"), denyArr);
    cmd.insert(QStringLiteral("searchProvider"), settings.searchProvider);
    cmd.insert(QStringLiteral("searchEndpoint"), settings.searchEndpoint);
    cmd.insert(QStringLiteral("maxFetchesPerTurn"), settings.maxFetchesPerTurn);
    cmd.insert(QStringLiteral("timeoutSecs"), settings.timeoutSecs);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    opts.fallbackFetchSnapshotIfAckOnly = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::airGapNow() {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("air_gap_now"));
    cmd.insert(QStringLiteral("action"), QStringLiteral("air_gap_now"));

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    opts.fallbackFetchSnapshotIfAckOnly = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::setToolPermission(const QString& toolName, const QString& mode) {
    const QString normMode = detail::normalizePermissionMode(mode);
    const QString rustPolicy = (normMode == QLatin1StringView("ask"))
                                   ? QStringLiteral("confirm")
                                   : normMode;

    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("set_tool_permission"));
    cmd.insert(QStringLiteral("tool_name"), toolName);
    cmd.insert(QStringLiteral("toolName"), toolName);
    cmd.insert(QStringLiteral("policy"), rustPolicy);
    cmd.insert(QStringLiteral("mode"), normMode);

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    m_impl->dispatchInternal(cmd, opts);
}

void SpellCoreClient::respondTerminalApproval(const QString& approvalId,
                                                bool approved,
                                                const QString& terminalOutput) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("resolve_tool_confirmation"));
    cmd.insert(QStringLiteral("call_id"), approvalId);
    cmd.insert(QStringLiteral("callId"), approvalId);
    cmd.insert(QStringLiteral("approval_id"), approvalId);
    cmd.insert(QStringLiteral("approvalId"), approvalId);
    cmd.insert(QStringLiteral("approved"), approved);
    cmd.insert(QStringLiteral("decision"),
               approved ? QStringLiteral("allow_once") : QStringLiteral("deny"));
    if (!terminalOutput.isEmpty()) {
        cmd.insert(QStringLiteral("terminal_output"), terminalOutput);
        cmd.insert(QStringLiteral("terminalOutput"), terminalOutput);
        cmd.insert(QStringLiteral("output"), terminalOutput);
    }

    DispatchOptions opts;
    opts.emitErrorSignal = true;
    opts.autoUpdateSnapshot = true;
    opts.forceBusy = approved;
    m_impl->dispatchInternal(cmd, opts);
}

// =============================================================================
// OS keyring secrets, system info & PDF extraction
// =============================================================================

void SpellCoreClient::getSecret(const QString& account,
                                  std::function<void(QString)> callback) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("get_secret"));
    cmd.insert(QStringLiteral("service"), QLatin1StringView(kKeyringService));
    cmd.insert(QStringLiteral("account"), account);

    DispatchOptions opts;
    opts.emitErrorSignal = false;
    opts.autoUpdateSnapshot = false;

    QPointer<SpellCoreClient> self(this);
    m_impl->dispatchInternal(cmd, opts,
                             [self, account, cb = std::move(callback)](const QJsonObject& resp) {
                                 if (!cb) {
                                     return;
                                 }
                                 const QString err = extractResponseError(resp);
                                 if (err.isEmpty()) {
                                     const QString val = detail::getString(
                                         resp, {"value", "secret", "data"});
                                     cb(val);
                                     return;
                                 }
                                 if (isUnknownCommandError(err) && self && self->m_impl) {
                                     QMutexLocker lock(&self->m_impl->fallbackSecretsMutex);
                                     cb(self->m_impl->fallbackSecrets.value(account));
                                     return;
                                 }
                                 cb(QString());
                             });
}

void SpellCoreClient::setSecret(const QString& account,
                                  const QString& value,
                                  std::function<void(bool)> callback) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("set_secret"));
    cmd.insert(QStringLiteral("service"), QLatin1StringView(kKeyringService));
    cmd.insert(QStringLiteral("account"), account);
    cmd.insert(QStringLiteral("value"), value);
    cmd.insert(QStringLiteral("secret"), value);

    DispatchOptions opts;
    opts.emitErrorSignal = false;
    opts.autoUpdateSnapshot = false;

    QPointer<SpellCoreClient> self(this);
    m_impl->dispatchInternal(
        cmd, opts, [self, account, value, cb = std::move(callback)](const QJsonObject& resp) {
            const QString err = extractResponseError(resp);
            if (err.isEmpty()) {
                if (cb) {
                    cb(true);
                }
                return;
            }
            if (isUnknownCommandError(err) && self && self->m_impl) {
                // Memory-only fallback when backend keyring command is unavailable; never writes plaintext to disk.
                QMutexLocker lock(&self->m_impl->fallbackSecretsMutex);
                self->m_impl->fallbackSecrets.insert(account, value);
                if (cb) {
                    cb(true);
                }
                return;
            }
            if (self) {
                emit self->errorOccurred(err);
            }
            if (cb) {
                cb(false);
            }
        });
}

void SpellCoreClient::deleteSecret(const QString& account,
                                     std::function<void(bool)> callback) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("delete_secret"));
    cmd.insert(QStringLiteral("service"), QLatin1StringView(kKeyringService));
    cmd.insert(QStringLiteral("account"), account);

    DispatchOptions opts;
    opts.emitErrorSignal = false;
    opts.autoUpdateSnapshot = false;

    QPointer<SpellCoreClient> self(this);
    m_impl->dispatchInternal(cmd, opts,
                             [self, account, cb = std::move(callback)](const QJsonObject& resp) {
                                 const QString err = extractResponseError(resp);
                                 if (err.isEmpty()) {
                                     if (self && self->m_impl) {
                                         QMutexLocker lock(&self->m_impl->fallbackSecretsMutex);
                                         self->m_impl->fallbackSecrets.remove(account);
                                     }
                                     if (cb) {
                                         cb(true);
                                     }
                                     return;
                                 }
                                 if (isUnknownCommandError(err) && self && self->m_impl) {
                                     QMutexLocker lock(&self->m_impl->fallbackSecretsMutex);
                                     self->m_impl->fallbackSecrets.remove(account);
                                     if (cb) {
                                         cb(true);
                                     }
                                     return;
                                 }
                                 if (self) {
                                     emit self->errorOccurred(err);
                                 }
                                 if (cb) {
                                     cb(false);
                                 }
                             });
}

void SpellCoreClient::fetchSystemInfo(std::function<void(SystemInfo)> callback) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("get_system_info"));

    DispatchOptions opts;
    opts.emitErrorSignal = false;
    opts.autoUpdateSnapshot = false;

    m_impl->dispatchInternal(cmd, opts, [cb = std::move(callback)](const QJsonObject& resp) {
        if (!cb) {
            return;
        }
        SystemInfo info;
        const QString err = extractResponseError(resp);
        if (err.isEmpty()) {
            const QJsonObject nested = detail::getObject(resp, {"system_info", "systemInfo", "info"});
            info = SystemInfo::fromJson(nested.isEmpty() ? resp : nested);
        }
        cb(populateFallbackSystemInfo(std::move(info)));
    });
}

void SpellCoreClient::extractPdfText(const QString& path,
                                       std::function<void(QString, QString)> callback) {
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("extract_pdf_text"));
    cmd.insert(QStringLiteral("path"), path);
    cmd.insert(QStringLiteral("file_path"), path);
    cmd.insert(QStringLiteral("filePath"), path);

    DispatchOptions opts;
    opts.emitErrorSignal = false;
    opts.autoUpdateSnapshot = false;

    m_impl->dispatchInternal(cmd, opts, [cb = std::move(callback)](const QJsonObject& resp) {
        if (!cb) {
            return;
        }
        const QString err = extractResponseError(resp);
        if (!err.isEmpty()) {
            cb(QString(), err);
            return;
        }
        const QString text = detail::getString(
            resp, {"text", "extracted_text", "extractedText", "content"});
        cb(text, QString());
    });
}

// =============================================================================
// Background pollers (zero network calls when idle)
// =============================================================================

void SpellCoreClient::startBackgroundPollers() {
    if (QThread::currentThread() != thread()) {
        QPointer<SpellCoreClient> self(this);
        QMetaObject::invokeMethod(
            this,
            [self]() {
                if (self) {
                    self->startBackgroundPollers();
                }
            },
            Qt::QueuedConnection);
        return;
    }

    if (m_impl->patchGenTimer != nullptr && !m_impl->patchGenTimer->isActive()) {
        m_impl->patchGenTimer->start(kPatchGenPollIntervalMs);
    }
    if (m_impl->localTimerPoller != nullptr && !m_impl->localTimerPoller->isActive()) {
        m_impl->localTimerPoller->start(kLocalTimerPollIntervalMs);
    }
}

void SpellCoreClient::stopBackgroundPollers() {
    if (QThread::currentThread() != thread()) {
        QPointer<SpellCoreClient> self(this);
        QMetaObject::invokeMethod(
            this,
            [self]() {
                if (self) {
                    self->stopBackgroundPollers();
                }
            },
            Qt::QueuedConnection);
        return;
    }

    if (m_impl->patchGenTimer != nullptr && m_impl->patchGenTimer->isActive()) {
        m_impl->patchGenTimer->stop();
    }
    if (m_impl->localTimerPoller != nullptr && m_impl->localTimerPoller->isActive()) {
        m_impl->localTimerPoller->stop();
    }
}

} // namespace spell
