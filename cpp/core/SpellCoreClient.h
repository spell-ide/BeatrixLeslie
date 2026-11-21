#pragma once

#include "SpellModels.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

namespace spell {

/**
 * Asynchronous, thread-safe Qt 6 client for `spell_core` (`spell_core.h`).
 *
 * All blocking FFI calls (`spell_session_dispatch_json`) execute on a dedicated worker
 * `QThread` owned by the PIMPL implementation (`Impl`) so the Qt GUI thread is never
 * blocked. Streaming updates use lock-free `spell_get_patch_gen()` +
 * `spell_get_cached_snapshot_json()` polling (30 ms), and active turn cancellation uses
 * lock-free `spell_request_turn_cancel()` before dispatching `cancel_active_turn`.
 */
class SpellCoreClient : public QObject {
    Q_OBJECT

public:
    explicit SpellCoreClient(const QString& dbPath = QString(), QObject* parent = nullptr);
    ~SpellCoreClient() override;

    SpellCoreClient(const SpellCoreClient&) = delete;
    SpellCoreClient& operator=(const SpellCoreClient&) = delete;
    SpellCoreClient(SpellCoreClient&&) = delete;
    SpellCoreClient& operator=(SpellCoreClient&&) = delete;

    // -------------------------------------------------------------------------
    // Thread-safe synchronous accessors
    // -------------------------------------------------------------------------

    WorkspaceSnapshot currentSnapshot() const;
    bool isBusy() const;

    /**
     * Reads a file via `spell_read_file_unrestricted` with zero line truncation.
     * Pass `startLine = 0, endLine = 0` to read the entire file.
     */
    QString readFileUnrestricted(const QString& path,
                                 std::size_t startLine = 0,
                                 std::size_t endLine = 0) const;

    // -------------------------------------------------------------------------
    // Low-level async JSON command dispatch
    // -------------------------------------------------------------------------

    /**
     * Dispatches a JSON command to `spell_session_dispatch_json` on the worker thread.
     * `callback` (if non-null) is always invoked on this object's thread (UI thread).
     */
    void dispatchCommand(const QJsonObject& cmd,
                         std::function<void(QJsonObject)> callback = nullptr);

    // -------------------------------------------------------------------------
    // Workspace snapshot & endpoint configuration
    // -------------------------------------------------------------------------

    /** Dispatches `get_snapshot`. */
    void refreshSnapshot();

    /** Dispatches `configure_endpoint` (`base_url`, optional `api_key`, `model_id`, `subagent_model_id`). */
    void configureEndpoint(const QString& baseUrl,
                           const std::optional<QString>& apiKey,
                           const QString& modelId,
                           const QString& subagentModelId);

    /** Dispatches `fetch_models` and emits `modelsFetched`. */
    void fetchModels(const QString& overrideBaseUrl = QString(),
                     const std::optional<QString>& overrideApiKey = std::nullopt);

    /** Dispatches `refresh_engine_status`. */
    void refreshEndpointStatus();

    /** Dispatches `set_critic_enabled`. */
    void setCriticEnabled(bool enabled);

    // -------------------------------------------------------------------------
    // Projects & threads
    // -------------------------------------------------------------------------

    /** Dispatches `add_project`. */
    void createProject(const QString& name, const QString& rootPath, bool readOnly = false);

    /** Dispatches `remove_project`. */
    void removeProject(const QString& projectId);

    /** Dispatches `select_project`. */
    void selectProject(const QString& projectId);

    /** Dispatches `rename_project`. */
    void renameProject(const QString& projectId, const QString& name);

    /** Dispatches `set_project_read_only` / `toggle_project_read_only`. */
    void setProjectReadOnly(const QString& projectId, bool readOnly);

    /** Dispatches `create_thread`. */
    void createThread(const QString& projectId,
                      const QString& title = QString(),
                      const QString& profile = QString());

    /** Dispatches `select_thread`. */
    void selectThread(const QString& threadId);

    /** Dispatches `delete_thread`. */
    void deleteThread(const QString& threadId);

    /** Dispatches `rename_thread`. */
    void renameThread(const QString& threadId, const QString& title);

    /** Dispatches `set_thread_profile` (`"write"` or `"ask"`). */
    void setThreadProfile(const QString& threadId, const QString& profile);

    // -------------------------------------------------------------------------
    // Agent turns & staged diffs
    // -------------------------------------------------------------------------

    /** Dispatches `send_prompt` with optional attached file paths. */
    void sendPrompt(const QString& threadId,
                    const QString& prompt,
                    const QStringList& attachedFiles = {});

    /** Dispatches `edit_or_regenerate_turn`. */
    void editAndResendTurn(const QString& threadId, int turnIndex, const QString& newPrompt);

    /** Signals lock-free `spell_request_turn_cancel()` and dispatches `cancel_active_turn`. */
    void cancelActiveTurn();

    /** Dispatches `stage_write_action` (`commit`) for `diffId` / path. */
    void approveStagedDiff(const QString& diffId);

    /** Dispatches `stage_write_action` (`rollback`) for `diffId` / path. */
    void rejectStagedDiff(const QString& diffId);

    /** Dispatches `set_stage_write_mode`. */
    void setStageWriteMode(bool enabled);

    /** Dispatches `set_subagent_protocol` (`"auto" | "native" | "json"`). */
    void setSubagentProtocol(const QString& protocol);

    /** Dispatches `loop_control` / autonomous loop configuration. */
    void setAutonomousLoop(bool enabled, int maxSteps);

    /** Dispatches `request_hot_cache_clear` for a local context epoch reset. */
    void requestLocalContextReset(const QString& threadId);

    // -------------------------------------------------------------------------
    // Local SQLite timers
    // -------------------------------------------------------------------------

    /** Dispatches `schedule_timer`. */
    void scheduleTimer(const QString& threadId,
                       double durationS,
                       const QString& label,
                       const QString& note,
                       const QString& watchPath = QString());

    /** Dispatches `cancel_timer`. */
    void cancelTimer(const QString& timerId);

    /** Dispatches `fire_due_timers`. */
    void fireDueTimersNow(const QString& threadId = QString());

    // -------------------------------------------------------------------------
    // Subagent slots
    // -------------------------------------------------------------------------

    /** Dispatches `run_subagent_slot` and emits `subagentSlotReply`. */
    void runSubagentSlot(int slot,
                         const QString& mode,
                         const QString& message,
                         int maxTokens = 1024);

    /** Dispatches `terminate_subagent_slot`. */
    void terminateSubagentSlot(int slot);

    /** Dispatches `duplicate_subagent_slot`. */
    void duplicateSubagentSlot(int srcSlot, int dstSlot);

    /** Dispatches `read_subagent_slot_lines` and emits `subagentSlotLines`. */
    void readSubagentSlotLines(int slot, int limit = 200);

    // -------------------------------------------------------------------------
    // Rubric tags
    // -------------------------------------------------------------------------

    /** Dispatches `create_tag`. */
    void createTag(const QString& name, const QString& description, const QString& directive);

    /** Dispatches `update_tag` / `upsert_tag`. */
    void updateTag(const QString& id,
                   const QString& name,
                   const QString& description,
                   const QString& directive,
                   bool enabled);

    /** Dispatches `delete_tag`. */
    void deleteTag(const QString& id);

    /** Dispatches `set_thread_tags` / `toggle_tag`. */
    void setThreadTags(const QString& threadId, const QStringList& tagIds);

    // -------------------------------------------------------------------------
    // Online research, permissions & terminal bridge
    // -------------------------------------------------------------------------

    /** Dispatches `configure_online_research`. */
    void setOnlineResearchSettings(const OnlineResearchSettings& settings);

    /** Dispatches `air_gap_now` (immediate kill-switch to offline mode). */
    void airGapNow();

    /** Dispatches `set_tool_permission`. */
    void setToolPermission(const QString& toolName, const QString& mode);

    /** Dispatches `resolve_tool_confirmation` for a pending terminal approval request. */
    void respondTerminalApproval(const QString& approvalId,
                                 bool approved,
                                 const QString& terminalOutput = QString());

    // -------------------------------------------------------------------------
    // OS keyring secrets, system info & PDF extraction
    // -------------------------------------------------------------------------

    /** Dispatches `get_secret` for `account` (`com.spell.SpellIDE.credentials`). */
    void getSecret(const QString& account, std::function<void(QString)> callback);

    /** Dispatches `set_secret` for `account`. */
    void setSecret(const QString& account,
                   const QString& value,
                   std::function<void(bool)> callback = nullptr);

    /** Dispatches `delete_secret` for `account`. */
    void deleteSecret(const QString& account, std::function<void(bool)> callback = nullptr);

    /** Dispatches `get_system_info` and invokes `callback` with parsed `SystemInfo`. */
    void fetchSystemInfo(std::function<void(SystemInfo)> callback);

    /**
     * Dispatches `extract_pdf_text` for `path`.
     * `callback` receives `(extractedText, errorMessage)`.
     */
    void extractPdfText(const QString& path, std::function<void(QString, QString)> callback);

    // -------------------------------------------------------------------------
    // Background pollers (zero network calls when idle)
    // -------------------------------------------------------------------------

    /**
     * Starts the 30 ms lock-free `spell_get_patch_gen()` streaming poller and the
     * 1000 ms local SQLite timer countdown poller. Performs zero network I/O when idle.
     */
    void startBackgroundPollers();

    /** Stops both background pollers. */
    void stopBackgroundPollers();

signals:
    void snapshotUpdated(const spell::WorkspaceSnapshot& snapshot);
    void modelsFetched(const QVector<spell::ModelDescriptor>& models, const QString& error);
    void subagentSlotReply(int slot, const QString& reply, const QString& error);
    void subagentSlotLines(int slot, const QStringList& lines);
    void errorOccurred(const QString& message);
    void busyChanged(bool busy);
    void terminalCommandRequested(const spell::TerminalApprovalRequest& req);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
