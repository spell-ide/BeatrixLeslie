#include "SpellSettingsStore.h"

#include "SpellCoreClient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGlobalStatic>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QRecursiveMutex>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <utility>

namespace spell {

namespace {

constexpr QLatin1StringView kSettingsOrg("Spell");
constexpr QLatin1StringView kSettingsApp("SpellIDE");

constexpr QLatin1StringView kCredentialsService("com.spell.SpellIDE.credentials");

constexpr QLatin1StringView kKeyEndpointBaseUrl("spell.endpoint.baseURL");
constexpr QLatin1StringView kKeyModelId("spell.endpoint.modelId");
constexpr QLatin1StringView kKeySubagentModelId("spell.endpoint.subagentModelId");
constexpr QLatin1StringView kKeyCriticEnabled("spell.endpoint.criticEnabled");

constexpr QLatin1StringView kKeyLeftPaneWidth("spell.layout.leftPaneWidth");
constexpr QLatin1StringView kKeyRightPaneWidth("spell.layout.rightPaneWidth");
constexpr QLatin1StringView kKeyLeftPaneCollapsed("spell.layout.leftPaneCollapsed");
constexpr QLatin1StringView kKeyRightPaneCollapsed("spell.layout.rightPaneCollapsed");
constexpr QLatin1StringView kKeyDockedTerminalHeight("spell.terminal.dockedHeight");
constexpr QLatin1StringView kKeyComposerHeight("spell.layout.composerHeight");
constexpr QLatin1StringView kKeyTerminalDocked("spell.terminal.docked");

constexpr QLatin1StringView kKeyWindowGeometry("spell.window.geometry");
constexpr QLatin1StringView kKeyOpenFiles("spell.editor.openFiles");
constexpr QLatin1StringView kGroupConversationLinkedFolders("conversations/linkedFolders");

constexpr int kDefaultLeftPaneWidth = 260;
constexpr int kDefaultRightPaneWidth = 420;
constexpr bool kDefaultLeftPaneCollapsed = false;
constexpr bool kDefaultRightPaneCollapsed = false;
constexpr int kDefaultDockedTerminalHeight = 280;
constexpr int kDefaultComposerHeight = 210;
constexpr bool kDefaultTerminalDocked = true;

constexpr qint64 kMaxLogFileBytes = 10LL * 1024LL * 1024LL; // 10 MiB rotation threshold

struct InstanceRegistry {
    QRecursiveMutex mutex;
    SpellSettingsStore* active = nullptr;
    SpellSettingsStore* lazy = nullptr;
    QVector<SpellSettingsStore*> live;
};

Q_GLOBAL_STATIC(InstanceRegistry, s_instanceRegistry)
Q_GLOBAL_STATIC(QMutex, s_logMutex)

QString normalizeBaseUrl(const QString& raw) {
    QString trimmed = raw.trimmed();
    while (trimmed.endsWith(QLatin1Char('/'))) {
        trimmed.chop(1);
    }
    return trimmed;
}

QString cleanFolderPath(const QString& rawPath) {
    const QString trimmed = rawPath.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    return QDir::cleanPath(trimmed);
}

struct SettingsGroupGuard {
    QSettings& settings;
    explicit SettingsGroupGuard(QSettings& s, QLatin1StringView prefix) : settings(s) {
        settings.beginGroup(prefix);
    }
    ~SettingsGroupGuard() {
        settings.endGroup();
    }
    SettingsGroupGuard(const SettingsGroupGuard&) = delete;
    SettingsGroupGuard& operator=(const SettingsGroupGuard&) = delete;
};

bool isEndpointApiKeyAccount(const QString& account) {
    return account == QLatin1StringView(SpellSettingsStore::kAccountEndpointApiKey);
}

QString envFallbackEndpointApiKey() {
    const QString spellKey = qEnvironmentVariable("SPELL_API_KEY").trimmed();
    if (!spellKey.isEmpty()) {
        return spellKey;
    }
    const QString openaiKey = qEnvironmentVariable("OPENAI_API_KEY").trimmed();
    if (!openaiKey.isEmpty()) {
        return openaiKey;
    }
    return QString();
}

QStringList knownSecretAccounts() {
    return {
        QString::fromLatin1(SpellSettingsStore::kAccountEndpointApiKey),
        QString::fromLatin1(SpellSettingsStore::kAccountGithubToken),
        QString::fromLatin1(SpellSettingsStore::kAccountHuggingFaceToken),
        QString::fromLatin1(SpellSettingsStore::kAccountOpenAlexApiKey),
        QString::fromLatin1(SpellSettingsStore::kAccountSearchApiKey),
    };
}

QString resolveAppLocalDataDir() {
    const QString envDir = qEnvironmentVariable("SPELL_DATA_DIR").trimmed();
    if (!envDir.isEmpty()) {
        return QDir::cleanPath(envDir);
    }
    const QString genericData =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation).trimmed();
    if (!genericData.isEmpty()) {
        return QDir::cleanPath(QDir(genericData).filePath(QStringLiteral("SpellIDE")));
    }
    const QString appLocal =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).trimmed();
    if (!appLocal.isEmpty()) {
        return QDir::cleanPath(appLocal);
    }
    return QDir::cleanPath(QDir::temp().filePath(QStringLiteral("SpellIDE")));
}

QString redactSensitiveText(QString text, const QStringList& knownSecrets) {
    if (text.isEmpty()) {
        return text;
    }

    // Replace any known secret values longest-first so substrings cannot leave tails behind.
    QStringList sortedSecrets = knownSecrets;
    std::sort(sortedSecrets.begin(), sortedSecrets.end(),
              [](const QString& a, const QString& b) { return a.size() > b.size(); });
    for (const QString& secret : sortedSecrets) {
        if (secret.size() >= 4) {
            text.replace(secret, QStringLiteral("[REDACTED]"));
        }
    }

    static const QRegularExpression kBearerRegex(
        QStringLiteral(R"((Bearer\s+)[A-Za-z0-9._\-~+/=]{6,})"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(kBearerRegex, QStringLiteral(R"(\1[REDACTED])"));

    static const QRegularExpression kTokenPrefixRegex(
        QStringLiteral(R"(\b(?:sk-[A-Za-z0-9_\-]{8,}|gh[pousr]_[A-Za-z0-9_]{8,}|github_pat_[A-Za-z0-9_]{8,}|hf_[A-Za-z0-9]{8,})\b)"));
    text.replace(kTokenPrefixRegex, QStringLiteral("[REDACTED]"));

    return text;
}

QString redactUrlCredentials(const QString& rawUrl, const QStringList& knownSecrets) {
    const QString trimmed = rawUrl.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }

    QUrl url(trimmed);
    if (url.isValid() && !url.scheme().isEmpty() && (!url.userInfo().isEmpty() || url.hasQuery())) {
        url.setUserInfo(QString());
        if (url.hasQuery()) {
            QUrlQuery query(url);
            const auto items = query.queryItems(QUrl::FullyDecoded);
            QUrlQuery sanitized;
            for (const auto& pair : items) {
                const QString lowerKey = pair.first.toLower();
                const bool sensitiveKey =
                    lowerKey.contains(QLatin1StringView("key")) ||
                    lowerKey.contains(QLatin1StringView("token")) ||
                    lowerKey.contains(QLatin1StringView("secret")) ||
                    lowerKey.contains(QLatin1StringView("pass")) ||
                    lowerKey.contains(QLatin1StringView("auth"));
                sanitized.addQueryItem(pair.first,
                                       sensitiveKey ? QStringLiteral("REDACTED") : pair.second);
            }
            url.setQuery(sanitized);
        }
        return redactSensitiveText(url.toString(), knownSecrets);
    }

    return redactSensitiveText(trimmed, knownSecrets);
}

} // namespace

struct SpellSettingsStore::Impl {
    mutable QMutex settingsMutex;
    mutable QSettings settings;

    mutable QMutex cacheMutex;
    QPointer<SpellCoreClient> client;
    QHash<QString, QString> secretCache;
    QHash<QString, quint64> secretSeq;

    explicit Impl(SpellCoreClient* coreClient)
        : settings(QSettings::defaultFormat(), QSettings::UserScope, QString(kSettingsOrg), QString(kSettingsApp))
        , client(coreClient) {}

    QStringList collectKnownSecretsForRedaction() const {
        QStringList secrets;
        {
            QMutexLocker locker(&cacheMutex);
            secrets.reserve(secretCache.size() + 2);
            for (auto it = secretCache.constBegin(); it != secretCache.constEnd(); ++it) {
                const QString val = it.value().trimmed();
                if (!val.isEmpty()) {
                    secrets.append(val);
                }
            }
        }
        const QString spellEnv = qEnvironmentVariable("SPELL_API_KEY").trimmed();
        if (!spellEnv.isEmpty()) {
            secrets.append(spellEnv);
        }
        const QString openaiEnv = qEnvironmentVariable("OPENAI_API_KEY").trimmed();
        if (!openaiEnv.isEmpty()) {
            secrets.append(openaiEnv);
        }
        return secrets;
    }
};

SpellSettingsStore::SpellSettingsStore(SpellCoreClient* client, QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>(client)) {
    if (!s_instanceRegistry.isDestroyed()) {
        auto* reg = s_instanceRegistry();
        QMutexLocker locker(&reg->mutex);
        reg->live.append(this);
        if (reg->active == nullptr || reg->active == reg->lazy) {
            reg->active = this;
        }
    }
}

SpellSettingsStore::~SpellSettingsStore() {
    if (!s_instanceRegistry.isDestroyed()) {
        auto* reg = s_instanceRegistry();
        QMutexLocker locker(&reg->mutex);
        reg->live.removeAll(this);
        if (reg->lazy == this) {
            reg->lazy = nullptr;
        }
        if (reg->active == this) {
            reg->active = nullptr;
            for (SpellSettingsStore* inst : reg->live) {
                if (inst != reg->lazy) {
                    reg->active = inst;
                    break;
                }
            }
            if (reg->active == nullptr) {
                reg->active = reg->lazy;
            }
        }
    }
    sync();
}

SpellSettingsStore* SpellSettingsStore::instance() {
    if (s_instanceRegistry.isDestroyed()) {
        return nullptr;
    }
    auto* reg = s_instanceRegistry();
    if (reg == nullptr) {
        return nullptr;
    }
    QMutexLocker locker(&reg->mutex);
    if (reg->active != nullptr) {
        return reg->active;
    }
    QObject* owner = nullptr;
    if (QCoreApplication* app = QCoreApplication::instance();
        app != nullptr && QThread::currentThread() == app->thread()) {
        owner = app;
    }
    auto* created = new SpellSettingsStore(nullptr, owner);
    reg->lazy = created;
    reg->active = created;
    return reg->active;
}

void SpellSettingsStore::attachClient(SpellCoreClient* client) {
    QMutexLocker locker(&m_impl->cacheMutex);
    m_impl->client = client;
}

// -----------------------------------------------------------------------------
// Non-secret preferences (`QSettings("Spell", "SpellIDE")`)
// -----------------------------------------------------------------------------

QString SpellSettingsStore::endpointBaseUrl() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    return normalizeBaseUrl(m_impl->settings.value(kKeyEndpointBaseUrl, QString()).toString());
}

void SpellSettingsStore::setEndpointBaseUrl(const QString& url) {
    const QString normalized = normalizeBaseUrl(url);
    bool changed = false;
    {
        QMutexLocker locker(&m_impl->settingsMutex);
        const QString previous =
            normalizeBaseUrl(m_impl->settings.value(kKeyEndpointBaseUrl, QString()).toString());
        if (previous != normalized) {
            if (normalized.isEmpty()) {
                m_impl->settings.remove(kKeyEndpointBaseUrl);
            } else {
                m_impl->settings.setValue(kKeyEndpointBaseUrl, normalized);
            }
            changed = true;
        }
    }
    if (changed) {
        emit endpointConfigChanged();
    }
}

QString SpellSettingsStore::modelId() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    return m_impl->settings.value(kKeyModelId, QString()).toString().trimmed();
}

void SpellSettingsStore::setModelId(const QString& modelId) {
    const QString trimmed = modelId.trimmed();
    bool changed = false;
    {
        QMutexLocker locker(&m_impl->settingsMutex);
        const QString previous = m_impl->settings.value(kKeyModelId, QString()).toString().trimmed();
        if (previous != trimmed) {
            if (trimmed.isEmpty()) {
                m_impl->settings.remove(kKeyModelId);
            } else {
                m_impl->settings.setValue(kKeyModelId, trimmed);
            }
            changed = true;
        }
    }
    if (changed) {
        emit endpointConfigChanged();
    }
}

QString SpellSettingsStore::subagentModelId() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    return m_impl->settings.value(kKeySubagentModelId, QString()).toString().trimmed();
}

void SpellSettingsStore::setSubagentModelId(const QString& modelId) {
    const QString trimmed = modelId.trimmed();
    bool changed = false;
    {
        QMutexLocker locker(&m_impl->settingsMutex);
        const QString previous =
            m_impl->settings.value(kKeySubagentModelId, QString()).toString().trimmed();
        if (previous != trimmed) {
            if (trimmed.isEmpty()) {
                m_impl->settings.remove(kKeySubagentModelId);
            } else {
                m_impl->settings.setValue(kKeySubagentModelId, trimmed);
            }
            changed = true;
        }
    }
    if (changed) {
        emit endpointConfigChanged();
    }
}

void SpellSettingsStore::setEndpointConfig(const QString& baseUrl,
                                           const QString& modelId,
                                           const QString& subagentModelId) {
    const QString normalizedUrl = normalizeBaseUrl(baseUrl);
    const QString trimmedModel = modelId.trimmed();
    const QString trimmedSubagent = subagentModelId.trimmed();

    bool changed = false;
    {
        QMutexLocker locker(&m_impl->settingsMutex);
        const QString prevUrl =
            normalizeBaseUrl(m_impl->settings.value(kKeyEndpointBaseUrl, QString()).toString());
        const QString prevModel =
            m_impl->settings.value(kKeyModelId, QString()).toString().trimmed();
        const QString prevSubagent =
            m_impl->settings.value(kKeySubagentModelId, QString()).toString().trimmed();

        if (prevUrl != normalizedUrl) {
            if (normalizedUrl.isEmpty()) {
                m_impl->settings.remove(kKeyEndpointBaseUrl);
            } else {
                m_impl->settings.setValue(kKeyEndpointBaseUrl, normalizedUrl);
            }
            changed = true;
        }
        if (prevModel != trimmedModel) {
            if (trimmedModel.isEmpty()) {
                m_impl->settings.remove(kKeyModelId);
            } else {
                m_impl->settings.setValue(kKeyModelId, trimmedModel);
            }
            changed = true;
        }
        if (prevSubagent != trimmedSubagent) {
            if (trimmedSubagent.isEmpty()) {
                m_impl->settings.remove(kKeySubagentModelId);
            } else {
                m_impl->settings.setValue(kKeySubagentModelId, trimmedSubagent);
            }
            changed = true;
        }
    }
    if (changed) {
        emit endpointConfigChanged();
    }
}

bool SpellSettingsStore::criticEnabled() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    return m_impl->settings.value(kKeyCriticEnabled, false).toBool();
}

void SpellSettingsStore::setCriticEnabled(bool enabled) {
    bool changed = false;
    {
        QMutexLocker locker(&m_impl->settingsMutex);
        const bool previous = m_impl->settings.value(kKeyCriticEnabled, false).toBool();
        if (previous != enabled) {
            m_impl->settings.setValue(kKeyCriticEnabled, enabled);
            changed = true;
        }
    }
    if (changed) {
        emit endpointConfigChanged();
    }
}

int SpellSettingsStore::leftPaneWidth() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    bool ok = false;
    const int value = m_impl->settings.value(kKeyLeftPaneWidth, kDefaultLeftPaneWidth).toInt(&ok);
    return (ok && value > 0) ? value : kDefaultLeftPaneWidth;
}

void SpellSettingsStore::setLeftPaneWidth(int width) {
    QMutexLocker locker(&m_impl->settingsMutex);
    if (width <= 0) {
        m_impl->settings.remove(kKeyLeftPaneWidth);
    } else {
        m_impl->settings.setValue(kKeyLeftPaneWidth, width);
    }
}

int SpellSettingsStore::rightPaneWidth() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    bool ok = false;
    const int value = m_impl->settings.value(kKeyRightPaneWidth, kDefaultRightPaneWidth).toInt(&ok);
    return (ok && value > 0) ? value : kDefaultRightPaneWidth;
}

void SpellSettingsStore::setRightPaneWidth(int width) {
    QMutexLocker locker(&m_impl->settingsMutex);
    if (width <= 0) {
        m_impl->settings.remove(kKeyRightPaneWidth);
    } else {
        m_impl->settings.setValue(kKeyRightPaneWidth, width);
    }
}

bool SpellSettingsStore::leftPaneCollapsed() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    return m_impl->settings.value(kKeyLeftPaneCollapsed, kDefaultLeftPaneCollapsed).toBool();
}

void SpellSettingsStore::setLeftPaneCollapsed(bool collapsed) {
    QMutexLocker locker(&m_impl->settingsMutex);
    m_impl->settings.setValue(kKeyLeftPaneCollapsed, collapsed);
}

bool SpellSettingsStore::rightPaneCollapsed() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    return m_impl->settings.value(kKeyRightPaneCollapsed, kDefaultRightPaneCollapsed).toBool();
}

void SpellSettingsStore::setRightPaneCollapsed(bool collapsed) {
    QMutexLocker locker(&m_impl->settingsMutex);
    m_impl->settings.setValue(kKeyRightPaneCollapsed, collapsed);
}

int SpellSettingsStore::dockedTerminalHeight() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    bool ok = false;
    const int value =
        m_impl->settings.value(kKeyDockedTerminalHeight, kDefaultDockedTerminalHeight).toInt(&ok);
    return (ok && value > 0) ? value : kDefaultDockedTerminalHeight;
}

void SpellSettingsStore::setDockedTerminalHeight(int height) {
    QMutexLocker locker(&m_impl->settingsMutex);
    if (height <= 0) {
        m_impl->settings.remove(kKeyDockedTerminalHeight);
    } else {
        m_impl->settings.setValue(kKeyDockedTerminalHeight, height);
    }
}

int SpellSettingsStore::composerHeight() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    bool ok = false;
    const int value = m_impl->settings.value(kKeyComposerHeight, kDefaultComposerHeight).toInt(&ok);
    return (ok && value > 0) ? value : kDefaultComposerHeight;
}

void SpellSettingsStore::setComposerHeight(int height) {
    QMutexLocker locker(&m_impl->settingsMutex);
    if (height <= 0) {
        m_impl->settings.remove(kKeyComposerHeight);
    } else {
        m_impl->settings.setValue(kKeyComposerHeight, height);
    }
}

bool SpellSettingsStore::terminalDocked() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    return m_impl->settings.value(kKeyTerminalDocked, kDefaultTerminalDocked).toBool();
}

void SpellSettingsStore::setTerminalDocked(bool docked) {
    QMutexLocker locker(&m_impl->settingsMutex);
    m_impl->settings.setValue(kKeyTerminalDocked, docked);
}

QByteArray SpellSettingsStore::windowGeometry() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    return m_impl->settings.value(kKeyWindowGeometry).toByteArray();
}

void SpellSettingsStore::setWindowGeometry(const QByteArray& geometry) {
    QMutexLocker locker(&m_impl->settingsMutex);
    if (geometry.isEmpty()) {
        m_impl->settings.remove(kKeyWindowGeometry);
    } else {
        m_impl->settings.setValue(kKeyWindowGeometry, geometry);
    }
}

QStringList SpellSettingsStore::openFiles() const {
    QMutexLocker locker(&m_impl->settingsMutex);
    const QStringList raw = m_impl->settings.value(kKeyOpenFiles).toStringList();
    QStringList result;
    result.reserve(raw.size());
    for (const QString& entry : raw) {
        const QString trimmed = entry.trimmed();
        if (!trimmed.isEmpty()) {
            result.append(trimmed);
        }
    }
    return result;
}

void SpellSettingsStore::setOpenFiles(const QStringList& files) {
    QStringList cleaned;
    cleaned.reserve(files.size());
    QSet<QString> seen;
    for (const QString& entry : files) {
        const QString trimmed = entry.trimmed();
        if (!trimmed.isEmpty() && !seen.contains(trimmed)) {
            seen.insert(trimmed);
            cleaned.append(trimmed);
        }
    }

    QMutexLocker locker(&m_impl->settingsMutex);
    if (cleaned.isEmpty()) {
        m_impl->settings.remove(kKeyOpenFiles);
    } else {
        m_impl->settings.setValue(kKeyOpenFiles, cleaned);
    }
}

QString SpellSettingsStore::conversationLinkedFolder(const QString& threadId) const {
    const QString tid = threadId.trimmed();
    if (tid.isEmpty()) {
        return QString();
    }
    QMutexLocker locker(&m_impl->settingsMutex);
    SettingsGroupGuard group(m_impl->settings, kGroupConversationLinkedFolders);
    return cleanFolderPath(m_impl->settings.value(tid, QString()).toString());
}

void SpellSettingsStore::setConversationLinkedFolder(const QString& threadId,
                                                     const QString& folderPath) {
    const QString tid = threadId.trimmed();
    if (tid.isEmpty()) {
        return;
    }

    const QString cleanedPath = cleanFolderPath(folderPath);
    bool changed = false;
    {
        QMutexLocker locker(&m_impl->settingsMutex);
        bool modified = false;
        {
            SettingsGroupGuard group(m_impl->settings, kGroupConversationLinkedFolders);
            const bool existed = m_impl->settings.contains(tid);
            const QString rawPrevious = m_impl->settings.value(tid, QString()).toString();
            const QString previous = cleanFolderPath(rawPrevious);

            if (cleanedPath.isEmpty()) {
                if (existed) {
                    m_impl->settings.remove(tid);
                    modified = true;
                }
                if (!previous.isEmpty()) {
                    changed = true;
                }
            } else {
                if (!existed || rawPrevious != cleanedPath) {
                    m_impl->settings.setValue(tid, cleanedPath);
                    modified = true;
                }
                if (previous != cleanedPath) {
                    changed = true;
                }
            }
        }
        if (modified) {
            m_impl->settings.sync();
        }
    }
    if (changed) {
        emit conversationLinkedFolderChanged(tid, cleanedPath);
    }
}

QHash<QString, QString> SpellSettingsStore::allConversationLinkedFolders() const {
    QHash<QString, QString> result;
    QMutexLocker locker(&m_impl->settingsMutex);
    SettingsGroupGuard group(m_impl->settings, kGroupConversationLinkedFolders);
    const QStringList keys = m_impl->settings.allKeys();
    result.reserve(keys.size());
    for (const QString& key : keys) {
        const QString tid = key.trimmed();
        const QString cleaned = cleanFolderPath(m_impl->settings.value(key, QString()).toString());
        if (!tid.isEmpty() && !cleaned.isEmpty()) {
            result.insert(tid, cleaned);
        }
    }
    return result;
}

void SpellSettingsStore::sync() {
    QMutexLocker locker(&m_impl->settingsMutex);
    m_impl->settings.sync();
}

// -----------------------------------------------------------------------------
// Secret credentials (in-memory cache + SpellCoreClient keyring; NEVER in QSettings)
// -----------------------------------------------------------------------------

void SpellSettingsStore::loadAllSecrets(std::function<void()> onLoaded) {
    QPointer<SpellCoreClient> client;
    {
        QMutexLocker locker(&m_impl->cacheMutex);
        client = m_impl->client;
    }

    if (!client) {
        if (onLoaded) {
            onLoaded();
        }
        return;
    }

    const QStringList accounts = knownSecretAccounts();
    if (accounts.isEmpty()) {
        if (onLoaded) {
            onLoaded();
        }
        return;
    }

    auto remaining = std::make_shared<std::atomic<int>>(static_cast<int>(accounts.size()));
    auto completion = std::make_shared<std::function<void()>>(std::move(onLoaded));

    for (const QString& account : accounts) {
        fetchSecret(account, [remaining, completion](QString) {
            if (remaining->fetch_sub(1) == 1 && *completion) {
                (*completion)();
            }
        });
    }
}

QString SpellSettingsStore::cachedSecret(const QString& account) const {
    const QString key = account.trimmed();
    if (key.isEmpty()) {
        return QString();
    }

    {
        QMutexLocker locker(&m_impl->cacheMutex);
        const QString cached = m_impl->secretCache.value(key);
        if (!cached.isEmpty()) {
            return cached;
        }
    }

    if (isEndpointApiKeyAccount(key)) {
        return envFallbackEndpointApiKey();
    }
    return QString();
}

bool SpellSettingsStore::hasSecret(const QString& account) const {
    return !cachedSecret(account).isEmpty();
}

void SpellSettingsStore::fetchSecret(const QString& account, std::function<void(QString)> callback) {
    const QString key = account.trimmed();
    if (key.isEmpty()) {
        if (callback) {
            callback(QString());
        }
        return;
    }

    QPointer<SpellCoreClient> client;
    quint64 startSeq = 0;
    {
        QMutexLocker locker(&m_impl->cacheMutex);
        client = m_impl->client;
        startSeq = m_impl->secretSeq.value(key, 0);
    }

    if (!client) {
        if (callback) {
            callback(cachedSecret(key));
        }
        return;
    }

    QPointer<SpellSettingsStore> weakSelf(this);
    client->getSecret(key, [weakSelf, key, startSeq, cb = std::move(callback)](QString fetched) mutable {
        const QString trimmed = fetched.trimmed();
        QString effective = trimmed;
        bool changed = false;

        if (weakSelf) {
            bool stale = false;
            {
                QMutexLocker locker(&weakSelf->m_impl->cacheMutex);
                if (weakSelf->m_impl->secretSeq.value(key, 0) != startSeq) {
                    stale = true;
                } else {
                    const QString previous = weakSelf->m_impl->secretCache.value(key);
                    if (trimmed.isEmpty()) {
                        if (weakSelf->m_impl->secretCache.remove(key) > 0) {
                            changed = true;
                        }
                    } else if (previous != trimmed) {
                        weakSelf->m_impl->secretCache.insert(key, trimmed);
                        changed = true;
                    }
                }
            }

            if (stale) {
                effective = weakSelf->cachedSecret(key);
            } else {
                if (effective.isEmpty() && isEndpointApiKeyAccount(key)) {
                    effective = envFallbackEndpointApiKey();
                }
                if (changed) {
                    emit weakSelf->secretChanged(key);
                    if (isEndpointApiKeyAccount(key)) {
                        emit weakSelf->apiKeyChanged();
                    }
                }
            }
        } else if (effective.isEmpty() && isEndpointApiKeyAccount(key)) {
            effective = envFallbackEndpointApiKey();
        }

        if (cb) {
            cb(effective);
        }
    });
}

void SpellSettingsStore::saveSecret(const QString& account,
                                    const QString& value,
                                    std::function<void(bool)> callback) {
    const QString key = account.trimmed();
    if (key.isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        removeSecret(key, std::move(callback));
        return;
    }

    QPointer<SpellCoreClient> client;
    bool hadPrevious = false;
    QString previousValue;
    bool changed = false;
    quint64 opSeq = 0;

    {
        QMutexLocker locker(&m_impl->cacheMutex);
        client = m_impl->client;
        const auto it = m_impl->secretCache.constFind(key);
        if (it != m_impl->secretCache.constEnd()) {
            hadPrevious = true;
            previousValue = it.value();
        }
        if (!hadPrevious || previousValue != trimmed) {
            m_impl->secretCache.insert(key, trimmed);
            changed = true;
        }
        opSeq = ++m_impl->secretSeq[key];
    }

    if (changed) {
        emit secretChanged(key);
        if (isEndpointApiKeyAccount(key)) {
            emit apiKeyChanged();
        }
    }

    if (!client) {
        if (callback) {
            callback(true);
        }
        return;
    }

    QPointer<SpellSettingsStore> weakSelf(this);
    client->setSecret(
        key, trimmed,
        [weakSelf, key, hadPrevious, previousValue, opSeq, cb = std::move(callback)](bool ok) mutable {
            if (!ok && weakSelf) {
                bool rolledBack = false;
                {
                    QMutexLocker locker(&weakSelf->m_impl->cacheMutex);
                    if (weakSelf->m_impl->secretSeq.value(key, 0) == opSeq) {
                        if (hadPrevious) {
                            weakSelf->m_impl->secretCache.insert(key, previousValue);
                        } else {
                            weakSelf->m_impl->secretCache.remove(key);
                        }
                        rolledBack = true;
                    }
                }
                if (rolledBack) {
                    emit weakSelf->secretChanged(key);
                    if (isEndpointApiKeyAccount(key)) {
                        emit weakSelf->apiKeyChanged();
                    }
                }
            }
            if (cb) {
                cb(ok);
            }
        });
}

void SpellSettingsStore::removeSecret(const QString& account, std::function<void(bool)> callback) {
    const QString key = account.trimmed();
    if (key.isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    QPointer<SpellCoreClient> client;
    bool hadPrevious = false;
    QString previousValue;
    quint64 opSeq = 0;

    {
        QMutexLocker locker(&m_impl->cacheMutex);
        client = m_impl->client;
        const auto it = m_impl->secretCache.constFind(key);
        if (it != m_impl->secretCache.constEnd()) {
            hadPrevious = true;
            previousValue = it.value();
            m_impl->secretCache.remove(key);
        }
        opSeq = ++m_impl->secretSeq[key];
    }

    if (hadPrevious) {
        emit secretChanged(key);
        if (isEndpointApiKeyAccount(key)) {
            emit apiKeyChanged();
        }
    }

    if (!client) {
        if (callback) {
            callback(true);
        }
        return;
    }

    QPointer<SpellSettingsStore> weakSelf(this);
    client->deleteSecret(
        key,
        [weakSelf, key, hadPrevious, previousValue, opSeq, cb = std::move(callback)](bool ok) mutable {
            if (!ok && weakSelf && hadPrevious) {
                bool restored = false;
                {
                    QMutexLocker locker(&weakSelf->m_impl->cacheMutex);
                    if (weakSelf->m_impl->secretSeq.value(key, 0) == opSeq) {
                        weakSelf->m_impl->secretCache.insert(key, previousValue);
                        restored = true;
                    }
                }
                if (restored) {
                    emit weakSelf->secretChanged(key);
                    if (isEndpointApiKeyAccount(key)) {
                        emit weakSelf->apiKeyChanged();
                    }
                }
            }
            if (cb) {
                cb(ok);
            }
        });
}

QString SpellSettingsStore::cachedEndpointApiKey() const {
    return cachedSecret(QString::fromLatin1(kAccountEndpointApiKey));
}

bool SpellSettingsStore::hasEndpointApiKey() const {
    return hasSecret(QString::fromLatin1(kAccountEndpointApiKey));
}

void SpellSettingsStore::setEndpointApiKey(const QString& apiKey, std::function<void(bool)> callback) {
    saveSecret(QString::fromLatin1(kAccountEndpointApiKey), apiKey, std::move(callback));
}

void SpellSettingsStore::clearEndpointApiKey(std::function<void(bool)> callback) {
    removeSecret(QString::fromLatin1(kAccountEndpointApiKey), std::move(callback));
}

// -----------------------------------------------------------------------------
// Logging & diagnostics bundle export
// -----------------------------------------------------------------------------

QString SpellSettingsStore::logFilePath() const {
    return QDir::cleanPath(QDir(resolveAppLocalDataDir()).filePath(QStringLiteral("logs/spell.log")));
}

void SpellSettingsStore::appendLog(const QString& level, const QString& msg) const {
    const QString path = logFilePath();
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QString normalizedLevel =
        level.trimmed().isEmpty() ? QStringLiteral("INFO") : level.trimmed().toUpper();

    QString sanitizedMsg = redactSensitiveText(msg, m_impl->collectKnownSecretsForRedaction());
    sanitizedMsg.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    sanitizedMsg.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    while (sanitizedMsg.endsWith(QLatin1Char('\n'))) {
        sanitizedMsg.chop(1);
    }

    const QString line = QStringLiteral("[%1] [%2] %3\n")
                             .arg(timestamp, normalizedLevel, sanitizedMsg);
    const QByteArray payload = line.toUtf8();

    QMutex* logMutex = s_logMutex.isDestroyed() ? nullptr : s_logMutex();
    QMutexLocker locker(logMutex);

    const QFileInfo fi(path);
    if (!QDir().mkpath(fi.absolutePath())) {
        return;
    }

    if (fi.exists() && fi.size() >= kMaxLogFileBytes) {
        const QString rotatedPath = path + QStringLiteral(".1");
        QFile::remove(rotatedPath);
        QFile::rename(path, rotatedPath);
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return;
    }
    file.write(payload);
    file.flush();
}

QString SpellSettingsStore::exportDiagnosticsBundle(const WorkspaceSnapshot& snap,
                                                    const SystemInfo& sysInfo,
                                                    const QString& targetDir) const {
    const QString baseRoot =
        targetDir.trimmed().isEmpty()
            ? QDir(resolveAppLocalDataDir()).filePath(QStringLiteral("diagnostics"))
            : QDir::cleanPath(targetDir.trimmed());

    QDir rootDir(baseRoot);
    if (!rootDir.mkpath(QStringLiteral("."))) {
        return QString();
    }

    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    const QString stamp = nowUtc.toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString bundlePath = rootDir.filePath(QStringLiteral("BeatrixLeslieIDE-Diagnostics-%1").arg(stamp));
    if (QFileInfo::exists(bundlePath)) {
        const QString stampMs = nowUtc.toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
        bundlePath = rootDir.filePath(QStringLiteral("BeatrixLeslieIDE-Diagnostics-%1").arg(stampMs));
        int suffix = 1;
        while (QFileInfo::exists(bundlePath) && suffix < 1000) {
            bundlePath = rootDir.filePath(
                QStringLiteral("BeatrixLeslieIDE-Diagnostics-%1-%2").arg(stampMs).arg(suffix));
            ++suffix;
        }
    }

    if (!QDir().mkpath(bundlePath)) {
        return QString();
    }

    const QStringList knownSecrets = m_impl->collectKnownSecretsForRedaction();
    const QString currentLogPath = logFilePath();

    const QString effectiveBaseUrl =
        !snap.endpointBaseUrl.trimmed().isEmpty() ? snap.endpointBaseUrl.trimmed() : endpointBaseUrl();
    const QString effectiveModelId =
        !snap.modelId.trimmed().isEmpty() ? snap.modelId.trimmed() : modelId();
    const QString effectiveSubagentModelId =
        !snap.subagentModelId.trimmed().isEmpty() ? snap.subagentModelId.trimmed() : subagentModelId();
    const bool effectiveConfigured = snap.endpointConfigured || !effectiveBaseUrl.isEmpty();

    const QString appVersion = QCoreApplication::applicationVersion().trimmed().isEmpty()
                                   ? QStringLiteral("1.0.0")
                                   : QCoreApplication::applicationVersion().trimmed();

    const QJsonObject endpointObj{
        {QStringLiteral("base_url"), redactUrlCredentials(effectiveBaseUrl, knownSecrets)},
        {QStringLiteral("configured"), effectiveConfigured},
        {QStringLiteral("online"), snap.telemetry.engineOnline},
        {QStringLiteral("model_id"), effectiveModelId},
        {QStringLiteral("subagent_model_id"), effectiveSubagentModelId},
        {QStringLiteral("effective_subagent_model_id"),
         !snap.effectiveSubagentModelId.isEmpty() ? snap.effectiveSubagentModelId
                                                  : (!effectiveSubagentModelId.isEmpty()
                                                         ? effectiveSubagentModelId
                                                         : effectiveModelId)},
        {QStringLiteral("critic_enabled"),
         snap.endpointConfigured ? snap.criticEnabled : criticEnabled()},
        {QStringLiteral("latency_ms"), snap.telemetry.endpointLatencyMs},
        {QStringLiteral("available_models_count"), static_cast<int>(snap.availableModels.size())},
        {QStringLiteral("has_api_key"), hasEndpointApiKey()},
        {QStringLiteral("last_error"),
         redactSensitiveText(snap.telemetry.engineError, knownSecrets)},
    };

    const QJsonObject credentialsObj{
        {QStringLiteral("service"), QString(kCredentialsService)},
        {QStringLiteral("redacted"), true},
        {QStringLiteral("endpoint_api_key_configured"), hasEndpointApiKey()},
        {QStringLiteral("github_token_configured"),
         hasSecret(QString::fromLatin1(kAccountGithubToken)) || snap.onlineResearch.hasGithubToken},
        {QStringLiteral("huggingface_token_configured"),
         hasSecret(QString::fromLatin1(kAccountHuggingFaceToken)) || snap.onlineResearch.hasHfToken},
        {QStringLiteral("openalex_api_key_configured"),
         hasSecret(QString::fromLatin1(kAccountOpenAlexApiKey)) || snap.onlineResearch.hasOpenAlexKey},
        {QStringLiteral("search_api_key_configured"),
         hasSecret(QString::fromLatin1(kAccountSearchApiKey)) || snap.onlineResearch.hasSearchKey},
    };

    const QJsonObject telemetryObj{
        {QStringLiteral("endpoint_configured"), snap.telemetry.endpointConfigured},
        {QStringLiteral("engine_online"), snap.telemetry.engineOnline},
        {QStringLiteral("engine_error"),
         redactSensitiveText(snap.telemetry.engineError, knownSecrets)},
        {QStringLiteral("model_id"), snap.telemetry.modelId},
        {QStringLiteral("effective_subagent_model_id"), snap.telemetry.effectiveSubagentModelId},
        {QStringLiteral("endpoint_latency_ms"), snap.telemetry.endpointLatencyMs},
        {QStringLiteral("decode_tok_s"), snap.telemetry.decodeTokS},
        {QStringLiteral("decode_tok_s_ewma"), snap.telemetry.decodeTokSEwma},
        {QStringLiteral("active_prompt_tokens"), snap.telemetry.activePromptTokens},
        {QStringLiteral("total_prompt_tokens"), static_cast<qint64>(snap.telemetry.totalPromptTokens)},
        {QStringLiteral("total_completion_tokens"),
         static_cast<qint64>(snap.telemetry.totalCompletionTokens)},
        {QStringLiteral("total_cached_tokens"), static_cast<qint64>(snap.telemetry.totalCachedTokens)},
        {QStringLiteral("compaction_epoch"), snap.telemetry.compactionEpoch},
        {QStringLiteral("critic_enabled"), snap.telemetry.criticEnabled},
    };

    const QJsonObject workspaceObj{
        {QStringLiteral("project_count"), static_cast<int>(snap.projects.size())},
        {QStringLiteral("thread_count"), static_cast<int>(snap.threads.size())},
        {QStringLiteral("active_events_count"), static_cast<int>(snap.activeEvents.size())},
        {QStringLiteral("pending_diffs_count"), static_cast<int>(snap.pendingDiffs.size())},
        {QStringLiteral("active_timers_count"), static_cast<int>(snap.activeTimers.size())},
        {QStringLiteral("active_subagents_count"), static_cast<int>(snap.activeSubagents.size())},
        {QStringLiteral("subagent_slots_count"), static_cast<int>(snap.subagentSlots.size())},
        {QStringLiteral("tags_count"), static_cast<int>(snap.tags.size())},
        {QStringLiteral("stage_write_mode"), snap.stageWriteMode},
        {QStringLiteral("subagent_protocol"), snap.subagentProtocol},
        {QStringLiteral("autonomous_loop_enabled"), snap.autonomousLoopEnabled},
        {QStringLiteral("max_agent_steps"), snap.maxAgentSteps},
        {QStringLiteral("critic_enabled"), snap.criticEnabled},
        {QStringLiteral("is_agent_busy"), snap.isAgentBusy},
        {QStringLiteral("online_research_mode"), snap.onlineResearch.mode},
        {QStringLiteral("online_research_search_provider"), snap.onlineResearch.searchProvider},
        {QStringLiteral("online_research_search_endpoint"),
         redactUrlCredentials(snap.onlineResearch.searchEndpoint, knownSecrets)},
        {QStringLiteral("online_research_max_fetches_per_turn"),
         snap.onlineResearch.maxFetchesPerTurn},
        {QStringLiteral("online_research_timeout_secs"), snap.onlineResearch.timeoutSecs},
    };

    const QJsonObject preferencesObj{
        {QStringLiteral("critic_enabled"), criticEnabled()},
        {QStringLiteral("left_pane_width"), leftPaneWidth()},
        {QStringLiteral("right_pane_width"), rightPaneWidth()},
        {QStringLiteral("left_pane_collapsed"), leftPaneCollapsed()},
        {QStringLiteral("right_pane_collapsed"), rightPaneCollapsed()},
        {QStringLiteral("docked_terminal_height"), dockedTerminalHeight()},
        {QStringLiteral("composer_height"), composerHeight()},
        {QStringLiteral("terminal_docked"), terminalDocked()},
        {QStringLiteral("open_files_count"), static_cast<int>(openFiles().size())},
        {QStringLiteral("conversation_linked_folders_count"),
         static_cast<int>(allConversationLinkedFolders().size())},
    };

    const QJsonObject systemObj{
        {QStringLiteral("os"),
         !sysInfo.os.trimmed().isEmpty() ? sysInfo.os.trimmed() : QSysInfo::prettyProductName()},
        {QStringLiteral("os_version"),
         !sysInfo.osVersion.trimmed().isEmpty() ? sysInfo.osVersion.trimmed()
                                                : QSysInfo::productVersion()},
        {QStringLiteral("arch"),
         !sysInfo.arch.trimmed().isEmpty() ? sysInfo.arch.trimmed()
                                           : QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("cpu_brand"), sysInfo.cpuBrand.trimmed()},
        {QStringLiteral("cpu_cores"),
         sysInfo.cpuCores > 0 ? sysInfo.cpuCores : QThread::idealThreadCount()},
        {QStringLiteral("total_memory_bytes"), static_cast<qint64>(sysInfo.totalMemoryBytes)},
        {QStringLiteral("available_memory_bytes"), static_cast<qint64>(sysInfo.availableMemoryBytes)},
        {QStringLiteral("hostname"),
         !sysInfo.hostname.trimmed().isEmpty() ? sysInfo.hostname.trimmed()
                                               : QSysInfo::machineHostName()},
        {QStringLiteral("data_dir"),
         !sysInfo.dataDir.trimmed().isEmpty() ? sysInfo.dataDir.trimmed() : resolveAppLocalDataDir()},
        {QStringLiteral("logs_dir"),
         !sysInfo.logsDir.trimmed().isEmpty() ? sysInfo.logsDir.trimmed()
                                              : QFileInfo(currentLogPath).absolutePath()},
        {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
    };

    const QJsonObject summaryObj{
        {QStringLiteral("app"), QStringLiteral("BeatrixLeslie IDE")},
        {QStringLiteral("bundle_id"), QStringLiteral("com.spell.SpellIDE")},
        {QStringLiteral("version"), appVersion},
        {QStringLiteral("timestamp_utc"), nowUtc.toString(Qt::ISODateWithMs)},
        {QStringLiteral("log_file_path"), currentLogPath},
        {QStringLiteral("endpoint"), endpointObj},
        {QStringLiteral("credentials"), credentialsObj},
        {QStringLiteral("telemetry"), telemetryObj},
        {QStringLiteral("workspace"), workspaceObj},
        {QStringLiteral("preferences"), preferencesObj},
        {QStringLiteral("system"), systemObj},
    };

    const QDir bundleDir(bundlePath);
    const QString summaryPath = bundleDir.filePath(QStringLiteral("diagnostics_summary.json"));
    {
        QSaveFile summaryFile(summaryPath);
        if (!summaryFile.open(QIODevice::WriteOnly)) {
            return QString();
        }
        const QByteArray jsonBytes = QJsonDocument(summaryObj).toJson(QJsonDocument::Indented);
        if (summaryFile.write(jsonBytes) != jsonBytes.size() || !summaryFile.commit()) {
            return QString();
        }
    }

    const QString exportedLogPath = bundleDir.filePath(QStringLiteral("spell.log"));
    {
        QMutex* logMutex = s_logMutex.isDestroyed() ? nullptr : s_logMutex();
        QMutexLocker locker(logMutex);
        QSaveFile logOut(exportedLogPath);
        if (!logOut.open(QIODevice::WriteOnly)) {
            return QString();
        }

        QFile logIn(currentLogPath);
        if (logIn.exists() && logIn.open(QIODevice::ReadOnly)) {
            const QString rawLog = QString::fromUtf8(logIn.readAll());
            const QByteArray redactedLog = redactSensitiveText(rawLog, knownSecrets).toUtf8();
            if (logOut.write(redactedLog) != redactedLog.size() || !logOut.commit()) {
                return QString();
            }
        } else {
            const QByteArray placeholder =
                QByteArrayLiteral("[BeatrixLeslie IDE] No spell.log entries recorded yet.\n");
            if (logOut.write(placeholder) != placeholder.size() || !logOut.commit()) {
                return QString();
            }
        }
    }

    return QDir::cleanPath(bundlePath);
}

} // namespace spell
