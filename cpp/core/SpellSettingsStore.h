#pragma once

#include "SpellModels.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace spell {

class SpellCoreClient;

/**
 * Persistent preferences, secret delegation, structured logging, and diagnostics
 * bundle export for Spell IDE (`com.spell.SpellIDE`).
 *
 * Non-secret UI and endpoint preferences are stored via `QSettings("Spell", "SpellIDE")`.
 * Secret credentials are NEVER written to `QSettings` or plaintext files; they are
 * delegated exclusively to `SpellCoreClient::getSecret` / `setSecret` / `deleteSecret`
 * with an in-memory cache and environment-variable fallback (`SPELL_API_KEY`,
 * `OPENAI_API_KEY`) for the primary endpoint API key.
 */
class SpellSettingsStore : public QObject {
    Q_OBJECT

public:
    // Canonical OS keyring account names (service: com.spell.SpellIDE.credentials)
    static constexpr const char* kAccountEndpointApiKey = "endpoint.api_key";
    static constexpr const char* kAccountGithubToken = "online.github.token";
    static constexpr const char* kAccountHuggingFaceToken = "online.huggingface.token";
    static constexpr const char* kAccountOpenAlexApiKey = "online.openalex.apiKey";
    static constexpr const char* kAccountSearchApiKey = "online.search.apiKey";

    explicit SpellSettingsStore(SpellCoreClient* client = nullptr, QObject* parent = nullptr);
    ~SpellSettingsStore() override;

    SpellSettingsStore(const SpellSettingsStore&) = delete;
    SpellSettingsStore& operator=(const SpellSettingsStore&) = delete;
    SpellSettingsStore(SpellSettingsStore&&) = delete;
    SpellSettingsStore& operator=(SpellSettingsStore&&) = delete;

    /** Returns the active singleton instance if constructed, or creates one lazily. */
    static SpellSettingsStore* instance();

    /** Attaches or updates the `SpellCoreClient` used for OS keyring secret operations. */
    void attachClient(SpellCoreClient* client);

    // -------------------------------------------------------------------------
    // Non-secret preferences (`QSettings("Spell", "SpellIDE")`)
    // -------------------------------------------------------------------------

    QString endpointBaseUrl() const;
    void setEndpointBaseUrl(const QString& url);

    QString modelId() const;
    void setModelId(const QString& modelId);

    QString subagentModelId() const;
    void setSubagentModelId(const QString& modelId);

    /** Batch-updates endpoint URL, primary model, and subagent model, emitting `endpointConfigChanged()` once. */
    void setEndpointConfig(const QString& baseUrl,
                           const QString& modelId,
                           const QString& subagentModelId);

    bool criticEnabled() const;
    void setCriticEnabled(bool enabled);

    int leftPaneWidth() const;
    void setLeftPaneWidth(int width);

    int rightPaneWidth() const;
    void setRightPaneWidth(int width);

    bool leftPaneCollapsed() const;
    void setLeftPaneCollapsed(bool collapsed);

    bool rightPaneCollapsed() const;
    void setRightPaneCollapsed(bool collapsed);

    int dockedTerminalHeight() const;
    void setDockedTerminalHeight(int height);

    int composerHeight() const;
    void setComposerHeight(int height);

    bool terminalDocked() const;
    void setTerminalDocked(bool docked);

    QByteArray windowGeometry() const;
    void setWindowGeometry(const QByteArray& geometry);

    QStringList openFiles() const;
    void setOpenFiles(const QStringList& files);

    // -------------------------------------------------------------------------
    // Per-conversation linked folders (`conversations/linkedFolders/<threadId>`)
    // -------------------------------------------------------------------------

    /** Returns the persisted linked folder override for `threadId`, or empty string if none. */
    QString conversationLinkedFolder(const QString& threadId) const;

    /**
     * Persists a per-conversation linked folder override for `threadId`.
     * Passing an empty string removes the override and emits
     * `conversationLinkedFolderChanged(threadId, cleanedPath)` when changed.
     */
    void setConversationLinkedFolder(const QString& threadId, const QString& folderPath);

    /** Returns all persisted `threadId -> linkedFolder` overrides. */
    QHash<QString, QString> allConversationLinkedFolders() const;

    /** Flushes pending `QSettings` writes to disk. */
    void sync();

    // -------------------------------------------------------------------------
    // Secret credentials (in-memory cache + SpellCoreClient keyring; NEVER in QSettings)
    // -------------------------------------------------------------------------

    /** Preloads all known credential accounts from `SpellCoreClient` into memory. */
    void loadAllSecrets(std::function<void()> onLoaded = nullptr);

    /** Returns the in-memory cached secret for `account` (or env fallback for `endpoint.api_key`). */
    QString cachedSecret(const QString& account) const;

    /** Returns true if a non-empty secret is cached (or available via env fallback) for `account`. */
    bool hasSecret(const QString& account) const;

    /** Fetches `account` via `SpellCoreClient::getSecret`, refreshes the in-memory cache, and invokes `callback`. */
    void fetchSecret(const QString& account, std::function<void(QString)> callback);

    /** Saves `value` for `account` via `SpellCoreClient::setSecret` and updates the in-memory cache. */
    void saveSecret(const QString& account,
                    const QString& value,
                    std::function<void(bool)> callback = nullptr);

    /** Deletes `account` via `SpellCoreClient::deleteSecret` and evicts it from the in-memory cache. */
    void removeSecret(const QString& account, std::function<void(bool)> callback = nullptr);

    /** Convenience accessor for `endpoint.api_key` with `SPELL_API_KEY` / `OPENAI_API_KEY` env fallback. */
    QString cachedEndpointApiKey() const;
    bool hasEndpointApiKey() const;
    void setEndpointApiKey(const QString& apiKey, std::function<void(bool)> callback = nullptr);
    void clearEndpointApiKey(std::function<void(bool)> callback = nullptr);

    // -------------------------------------------------------------------------
    // Logging & diagnostics bundle export
    // -------------------------------------------------------------------------

    /**
     * Returns the log file path under the SpellIDE data directory:
     * `<GenericDataLocation>/SpellIDE/logs/spell.log` (or `$SPELL_DATA_DIR/logs/spell.log`).
     */
    QString logFilePath() const;

    /** Appends a timestamped log line (`[ISO8601] [LEVEL] msg`) to `spell.log` (thread-safe). */
    void appendLog(const QString& level, const QString& msg) const;

    /**
     * Exports a redacted diagnostics bundle containing `diagnostics_summary.json`
     * and a copy of `spell.log` into a timestamped folder under `targetDir`
     * (defaulting to `<GenericDataLocation>/SpellIDE/diagnostics/`).
     * Never writes API keys or secret tokens. Returns the created bundle directory path,
     * or an empty string on failure.
     */
    QString exportDiagnosticsBundle(const WorkspaceSnapshot& snap,
                                    const SystemInfo& sysInfo,
                                    const QString& targetDir = QString()) const;

signals:
    void endpointConfigChanged();
    void apiKeyChanged();
    void secretChanged(const QString& account);
    void conversationLinkedFolderChanged(const QString& threadId, const QString& folderPath);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
