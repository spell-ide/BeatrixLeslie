#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <initializer_list>
#include <optional>

namespace spell {

namespace detail {

inline QJsonValue firstValue(const QJsonObject& obj, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const auto it = obj.constFind(QLatin1StringView(key));
        if (it != obj.constEnd() && !it.value().isUndefined() && !it.value().isNull()) {
            return it.value();
        }
    }
    return QJsonValue(QJsonValue::Undefined);
}

inline QString getString(const QJsonObject& obj,
                         std::initializer_list<const char*> keys,
                         const QString& defaultVal = {}) {
    const QJsonValue v = firstValue(obj, keys);
    if (v.isString()) {
        return v.toString();
    }
    return defaultVal;
}

inline std::optional<bool> getOptionalBool(const QJsonObject& obj,
                                           std::initializer_list<const char*> keys) {
    const QJsonValue v = firstValue(obj, keys);
    if (v.isBool()) {
        return v.toBool();
    }
    if (v.isDouble()) {
        return v.toInteger() != 0;
    }
    if (v.isString()) {
        const QString s = v.toString().trimmed().toLower();
        if (s == QLatin1StringView("true") || s == QLatin1StringView("1") || s == QLatin1StringView("yes")) {
            return true;
        }
        if (s == QLatin1StringView("false") || s == QLatin1StringView("0") || s == QLatin1StringView("no")) {
            return false;
        }
    }
    return std::nullopt;
}

inline bool getBool(const QJsonObject& obj,
                    std::initializer_list<const char*> keys,
                    bool defaultVal = false) {
    return getOptionalBool(obj, keys).value_or(defaultVal);
}

inline int getInt(const QJsonObject& obj,
                  std::initializer_list<const char*> keys,
                  int defaultVal = 0) {
    const QJsonValue v = firstValue(obj, keys);
    if (v.isDouble()) {
        return static_cast<int>(v.toInteger());
    }
    if (v.isString()) {
        bool ok = false;
        const int parsed = v.toString().trimmed().toInt(&ok);
        if (ok) {
            return parsed;
        }
    }
    return defaultVal;
}

inline qint64 getInt64(const QJsonObject& obj,
                       std::initializer_list<const char*> keys,
                       qint64 defaultVal = 0) {
    const QJsonValue v = firstValue(obj, keys);
    if (v.isDouble()) {
        return v.toInteger();
    }
    if (v.isString()) {
        bool ok = false;
        const qint64 parsed = v.toString().trimmed().toLongLong(&ok);
        if (ok) {
            return parsed;
        }
    }
    return defaultVal;
}

inline quint64 getUInt64(const QJsonObject& obj,
                         std::initializer_list<const char*> keys,
                         quint64 defaultVal = 0) {
    const QJsonValue v = firstValue(obj, keys);
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (d < 0.0) {
            return 0;
        }
        const qint64 i = v.toInteger();
        if (i >= 0 && static_cast<double>(i) == d) {
            return static_cast<quint64>(i);
        }
        return static_cast<quint64>(d);
    }
    if (v.isString()) {
        bool ok = false;
        const quint64 parsed = v.toString().trimmed().toULongLong(&ok);
        if (ok) {
            return parsed;
        }
    }
    return defaultVal;
}

inline double getDouble(const QJsonObject& obj,
                        std::initializer_list<const char*> keys,
                        double defaultVal = 0.0) {
    const QJsonValue v = firstValue(obj, keys);
    if (v.isDouble()) {
        return v.toDouble();
    }
    if (v.isString()) {
        bool ok = false;
        const double parsed = v.toString().trimmed().toDouble(&ok);
        if (ok) {
            return parsed;
        }
    }
    return defaultVal;
}

inline QStringList getStringList(const QJsonObject& obj,
                                 std::initializer_list<const char*> keys) {
    QStringList out;
    const QJsonValue v = firstValue(obj, keys);
    if (!v.isArray()) {
        return out;
    }
    const QJsonArray arr = v.toArray();
    out.reserve(arr.size());
    for (const QJsonValue& item : arr) {
        if (item.isString()) {
            out.append(item.toString());
        }
    }
    return out;
}

inline QJsonObject getObject(const QJsonObject& obj,
                             std::initializer_list<const char*> keys) {
    const QJsonValue v = firstValue(obj, keys);
    return v.isObject() ? v.toObject() : QJsonObject{};
}

inline QJsonArray getArray(const QJsonObject& obj,
                           std::initializer_list<const char*> keys) {
    const QJsonValue v = firstValue(obj, keys);
    return v.isArray() ? v.toArray() : QJsonArray{};
}

inline QJsonArray toJsonArray(const QStringList& list) {
    QJsonArray arr;
    for (const QString& s : list) {
        arr.append(s);
    }
    return arr;
}

inline QString normalizeEventKind(const QString& rawKind, const QString& role) {
    const QString lower = rawKind.trimmed().toLower();
    if (lower == QLatin1StringView("user") || lower == QLatin1StringView("user_message") ||
        lower == QLatin1StringView("usermessage")) {
        return QStringLiteral("user");
    }
    if (lower == QLatin1StringView("assistant") || lower == QLatin1StringView("assistant_message") ||
        lower == QLatin1StringView("assistantmessage") || lower == QLatin1StringView("assistant_markdown")) {
        return QStringLiteral("assistant");
    }
    if (lower == QLatin1StringView("reasoning") || lower == QLatin1StringView("thinking")) {
        return QStringLiteral("reasoning");
    }
    if (lower == QLatin1StringView("tool_call") || lower == QLatin1StringView("toolcall") ||
        lower == QLatin1StringView("tool_call_card") || lower == QLatin1StringView("toolcallcard") ||
        lower == QLatin1StringView("tool")) {
        return QStringLiteral("tool_call");
    }
    if (lower == QLatin1StringView("tool_result") || lower == QLatin1StringView("toolresult")) {
        return QStringLiteral("tool_result");
    }
    if (lower == QLatin1StringView("diff_staged") || lower == QLatin1StringView("diffstaged") ||
        lower == QLatin1StringView("staged_diff") || lower == QLatin1StringView("stageddiff")) {
        return QStringLiteral("diff_staged");
    }
    if (lower == QLatin1StringView("delegation") || lower == QLatin1StringView("subagent_delegation")) {
        return QStringLiteral("delegation");
    }
    if (lower == QLatin1StringView("timer_alarm") || lower == QLatin1StringView("timeralarm") ||
        lower == QLatin1StringView("engine_timer_alarm") || lower == QLatin1StringView("enginetimeralarm") ||
        lower == QLatin1StringView("wakeup_alarm") || lower == QLatin1StringView("wakeup")) {
        return QStringLiteral("timer_alarm");
    }
    if (lower == QLatin1StringView("compaction_notice") || lower == QLatin1StringView("compactionnotice") ||
        lower == QLatin1StringView("engine_compaction_notice") ||
        lower == QLatin1StringView("enginecompactionnotice") || lower == QLatin1StringView("compaction")) {
        return QStringLiteral("compaction_notice");
    }
    if (lower == QLatin1StringView("error")) {
        return QStringLiteral("error");
    }
    if (lower == QLatin1StringView("system")) {
        return QStringLiteral("system");
    }
    const QString lowerRole = role.trimmed().toLower();
    if (lowerRole == QLatin1StringView("user")) {
        return QStringLiteral("user");
    }
    if (lowerRole == QLatin1StringView("system")) {
        return QStringLiteral("system");
    }
    return lower.isEmpty() ? QStringLiteral("assistant") : lower;
}

inline QString normalizeOnlineMode(const QString& rawMode) {
    const QString lower = rawMode.trimmed().toLower();
    if (lower == QLatin1StringView("curated") || lower == QLatin1StringView("curated_allowlist") ||
        lower == QLatin1StringView("curatedallowlist") || lower == QLatin1StringView("allowlist")) {
        return QStringLiteral("curated");
    }
    if (lower == QLatin1StringView("open_web") || lower == QLatin1StringView("openweb") ||
        lower == QLatin1StringView("open_web_ask_per_domain") ||
        lower == QLatin1StringView("openwebaskperdomain") || lower == QLatin1StringView("open") ||
        lower == QLatin1StringView("ask_per_domain")) {
        return QStringLiteral("open_web");
    }
    return QStringLiteral("offline");
}

inline QString normalizePermissionMode(const QString& rawMode) {
    const QString lower = rawMode.trimmed().toLower();
    if (lower == QLatin1StringView("ask") || lower == QLatin1StringView("confirm")) {
        return QStringLiteral("ask");
    }
    if (lower == QLatin1StringView("deny") || lower == QLatin1StringView("block")) {
        return QStringLiteral("deny");
    }
    return QStringLiteral("allow");
}

inline QString normalizeSubagentProtocol(const QString& rawProtocol) {
    const QString lower = rawProtocol.trimmed().toLower();
    if (lower == QLatin1StringView("native") || lower == QLatin1StringView("native_tool_calls") ||
        lower == QLatin1StringView("nativetoolcalls") || lower == QLatin1StringView("tool_calls")) {
        return QStringLiteral("native");
    }
    if (lower == QLatin1StringView("json") || lower == QLatin1StringView("json_action") ||
        lower == QLatin1StringView("jsonaction") || lower == QLatin1StringView("json_loop") ||
        lower == QLatin1StringView("json_action_loop")) {
        return QStringLiteral("json");
    }
    return QStringLiteral("auto");
}

} // namespace detail

struct ModelDescriptor {
    QString id;
    QString ownedBy;
    qint64 contextWindow = 0;

    bool operator==(const ModelDescriptor&) const = default;

    static ModelDescriptor fromJson(const QJsonObject& obj) {
        ModelDescriptor m;
        m.id = detail::getString(obj, {"id", "name", "model"}).trimmed();
        m.ownedBy = detail::getString(obj, {"owned_by", "ownedBy", "provider"}).trimmed();
        if (m.ownedBy.isEmpty()) {
            const QJsonObject details = detail::getObject(obj, {"details"});
            m.ownedBy = detail::getString(details, {"family"}).trimmed();
        }
        m.contextWindow = detail::getInt64(
            obj,
            {"context_window", "contextWindow", "max_model_len", "maxModelLen",
             "context_length", "contextLength", "max_context_length", "input_token_limit"},
            0);
        if (m.contextWindow <= 0) {
            const QJsonObject topProvider = detail::getObject(obj, {"top_provider", "topProvider"});
            m.contextWindow = detail::getInt64(topProvider, {"context_length", "contextLength"}, 0);
        }
        return m;
    }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), id);
        if (!ownedBy.isEmpty()) {
            obj.insert(QStringLiteral("owned_by"), ownedBy);
        }
        if (contextWindow > 0) {
            obj.insert(QStringLiteral("context_window"), contextWindow);
        }
        return obj;
    }
};

struct ProjectInfo {
    QString id;
    QString name;
    QString rootPath;
    bool isReadOnly = false;
    qint64 createdAt = 0;

    bool operator==(const ProjectInfo&) const = default;

    static ProjectInfo fromJson(const QJsonObject& obj) {
        ProjectInfo p;
        p.id = detail::getString(obj, {"id", "project_id", "projectId"});
        p.name = detail::getString(obj, {"name", "title"}, QStringLiteral("Project"));
        p.rootPath = detail::getString(obj, {"root_path", "rootPath", "path"});
        p.isReadOnly = detail::getBool(obj, {"is_read_only", "isReadOnly", "read_only", "readOnly"}, false);
        p.createdAt = detail::getInt64(obj, {"created_at", "createdAt"}, 0);
        return p;
    }

    QJsonObject toJson() const {
        return QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("root_path"), rootPath},
            {QStringLiteral("is_read_only"), isReadOnly},
            {QStringLiteral("created_at"), createdAt},
        };
    }
};

struct ThreadSummary {
    QString id;
    QString projectId;
    QString title;
    QString profile = QStringLiteral("write");
    qint64 updatedAt = 0;
    int messageCount = 0;
    QString linkedFolder;

    bool operator==(const ThreadSummary&) const = default;

    static ThreadSummary fromJson(const QJsonObject& obj) {
        ThreadSummary t;
        t.id = detail::getString(obj, {"id", "thread_id", "threadId"});
        t.projectId = detail::getString(obj, {"project_id", "projectId"});
        t.title = detail::getString(obj, {"title", "name"}, QStringLiteral("Conversation"));
        const QString rawProfile = detail::getString(obj, {"profile"}, QStringLiteral("write")).trimmed().toLower();
        t.profile = (rawProfile == QLatin1StringView("ask")) ? QStringLiteral("ask") : QStringLiteral("write");
        t.updatedAt = detail::getInt64(obj, {"updated_at", "updatedAt", "created_at", "createdAt"}, 0);
        t.messageCount = detail::getInt(obj, {"message_count", "messageCount"}, -1);
        if (t.messageCount < 0) {
            const QJsonArray evs = detail::getArray(obj, {"events", "ui_events", "uiEvents"});
            if (!evs.isEmpty()) {
                t.messageCount = static_cast<int>(evs.size());
            } else {
                const QJsonArray msgs = detail::getArray(obj, {"wire_messages", "wireMessages", "messages"});
                t.messageCount = static_cast<int>(msgs.size());
            }
        }
        t.linkedFolder = detail::getString(
            obj,
            {"linked_folder", "linkedFolder", "working_dir", "workingDir", "folder_path", "folderPath"}).trimmed();
        return t;
    }

    QJsonObject toJson() const {
        QJsonObject obj{
            {QStringLiteral("id"), id},
            {QStringLiteral("project_id"), projectId},
            {QStringLiteral("title"), title},
            {QStringLiteral("profile"), profile},
            {QStringLiteral("updated_at"), updatedAt},
            {QStringLiteral("message_count"), messageCount},
        };
        if (!linkedFolder.isEmpty()) {
            obj.insert(QStringLiteral("linked_folder"), linkedFolder);
        }
        return obj;
    }
};

struct UiChatEvent {
    QString id;
    QString kind; // "user" | "assistant" | "reasoning" | "tool_call" | "tool_result" | "diff_staged" | "delegation" | "timer_alarm" | "compaction_notice" | "error" | "system"
    QString text;
    QString reasoning;
    QString toolName;
    QString toolArgsJson;
    QString toolOutput;
    QString diffId;
    QString filePath;
    QString diffText;
    QString diffStatus;
    QString modelId;
    double tokPerSec = 0.0;
    qint64 promptTokens = 0;
    qint64 completionTokens = 0;
    qint64 latencyMs = 0;
    qint64 timestamp = 0;
    bool isStreaming = false;
    int turnIndex = -1;
    QJsonObject raw;

    bool operator==(const UiChatEvent&) const = default;

    static UiChatEvent fromJson(const QJsonObject& obj) {
        UiChatEvent ev;
        ev.raw = obj;
        ev.id = detail::getString(obj, {"id", "event_id", "eventId"});
        const QString role = detail::getString(obj, {"role"}, QStringLiteral("assistant"));
        const QString rawKind = detail::getString(obj, {"kind", "event_kind", "eventKind", "type"}, role);
        ev.kind = detail::normalizeEventKind(rawKind, role);
        ev.text = detail::getString(obj, {"text", "content", "message", "summary"});
        ev.reasoning = detail::getString(obj, {"reasoning", "reasoning_content", "reasoningContent"});

        const QJsonObject toolObj = detail::getObject(obj, {"tool_call", "toolCall"});
        if (!toolObj.isEmpty()) {
            ev.toolName = detail::getString(toolObj, {"name", "tool_name", "toolName"});
            const QJsonValue argsVal = detail::firstValue(
                toolObj, {"arguments_json", "argumentsJSON", "tool_args_json", "toolArgsJson", "arguments", "input"});
            if (argsVal.isString()) {
                ev.toolArgsJson = argsVal.toString();
            } else if (argsVal.isObject()) {
                ev.toolArgsJson = QString::fromUtf8(
                    QJsonDocument(argsVal.toObject()).toJson(QJsonDocument::Compact));
            }
            ev.toolOutput = detail::getString(toolObj, {"output", "result", "tool_output", "toolOutput"});
            ev.diffStatus = detail::getString(toolObj, {"status", "tool_status", "toolStatus"});
        }
        if (ev.toolName.isEmpty()) {
            ev.toolName = detail::getString(obj, {"tool_name", "toolName"});
        }
        if (ev.toolArgsJson.isEmpty()) {
            const QJsonValue argsVal = detail::firstValue(
                obj, {"tool_args_json", "toolArgsJson", "arguments_json", "argumentsJSON", "arguments"});
            if (argsVal.isString()) {
                ev.toolArgsJson = argsVal.toString();
            } else if (argsVal.isObject()) {
                ev.toolArgsJson = QString::fromUtf8(
                    QJsonDocument(argsVal.toObject()).toJson(QJsonDocument::Compact));
            }
        }
        if (ev.toolOutput.isEmpty()) {
            ev.toolOutput = detail::getString(obj, {"tool_output", "toolOutput", "output", "result"});
        }

        ev.diffId = detail::getString(obj, {"diff_id", "diffId", "staged_diff_id", "stagedDiffId"});
        ev.filePath = detail::getString(obj, {"file_path", "filePath", "path", "relative_path", "relativePath"});
        ev.diffText = detail::getString(obj, {"diff_text", "diffText", "unified_diff", "unifiedDiff", "diff", "patch"});
        const QString topDiffStatus = detail::getString(obj, {"diff_status", "diffStatus"});
        if (!topDiffStatus.isEmpty()) {
            ev.diffStatus = topDiffStatus;
        } else if (ev.diffStatus.isEmpty() && ev.kind == QLatin1StringView("diff_staged")) {
            ev.diffStatus = detail::getString(obj, {"status", "state"});
        }

        ev.modelId = detail::getString(obj, {"model_id", "modelId", "model"});
        ev.tokPerSec = detail::getDouble(obj, {"tok_per_sec", "tokPerSec", "decode_tok_s", "decodeTokS", "tok_s"}, 0.0);
        ev.promptTokens = detail::getInt64(obj, {"prompt_tokens", "promptTokens"}, 0);
        ev.completionTokens = detail::getInt64(obj, {"completion_tokens", "completionTokens"}, 0);
        ev.latencyMs = detail::getInt64(obj, {"latency_ms", "latencyMs", "elapsed_ms", "elapsedMs"}, 0);

        const QJsonObject advice = detail::getObject(obj, {"context_advice", "contextAdvice"});
        if (!advice.isEmpty()) {
            if (ev.tokPerSec <= 0.0) {
                ev.tokPerSec = detail::getDouble(advice, {"decode_tok_s", "decodeTokS"}, 0.0);
            }
            if (ev.promptTokens <= 0) {
                ev.promptTokens = detail::getInt64(advice, {"prompt_tokens", "promptTokens"}, 0);
            }
        }

        ev.timestamp = detail::getInt64(obj, {"timestamp", "created_at", "createdAt"}, 0);
        ev.isStreaming = detail::getBool(obj, {"is_streaming", "isStreaming", "streaming"}, false);
        ev.turnIndex = detail::getInt(obj, {"turn_index", "turnIndex"}, -1);
        return ev;
    }

    QJsonObject toJson() const {
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), id);
        obj.insert(QStringLiteral("kind"), kind);
        obj.insert(QStringLiteral("text"), text);
        if (!reasoning.isEmpty()) {
            obj.insert(QStringLiteral("reasoning"), reasoning);
        }
        if (!toolName.isEmpty()) {
            obj.insert(QStringLiteral("tool_name"), toolName);
        }
        if (!toolArgsJson.isEmpty()) {
            obj.insert(QStringLiteral("tool_args_json"), toolArgsJson);
        }
        if (!toolOutput.isEmpty()) {
            obj.insert(QStringLiteral("tool_output"), toolOutput);
        }
        if (!diffId.isEmpty()) {
            obj.insert(QStringLiteral("diff_id"), diffId);
        }
        if (!filePath.isEmpty()) {
            obj.insert(QStringLiteral("file_path"), filePath);
        }
        if (!diffText.isEmpty()) {
            obj.insert(QStringLiteral("diff_text"), diffText);
        }
        if (!diffStatus.isEmpty()) {
            obj.insert(QStringLiteral("diff_status"), diffStatus);
        }
        if (!modelId.isEmpty()) {
            obj.insert(QStringLiteral("model_id"), modelId);
        }
        if (tokPerSec > 0.0) {
            obj.insert(QStringLiteral("tok_per_sec"), tokPerSec);
        }
        if (promptTokens > 0) {
            obj.insert(QStringLiteral("prompt_tokens"), promptTokens);
        }
        if (completionTokens > 0) {
            obj.insert(QStringLiteral("completion_tokens"), completionTokens);
        }
        if (latencyMs > 0) {
            obj.insert(QStringLiteral("latency_ms"), latencyMs);
        }
        obj.insert(QStringLiteral("timestamp"), timestamp);
        obj.insert(QStringLiteral("is_streaming"), isStreaming);
        if (turnIndex >= 0) {
            obj.insert(QStringLiteral("turn_index"), turnIndex);
        }
        return obj;
    }
};

struct StagedDiff {
    QString id;
    QString threadId;
    QString filePath;
    QString unifiedDiff;
    QString status;
    int linesAdded = 0;
    int linesRemoved = 0;
    QString summary;

    bool operator==(const StagedDiff&) const = default;

    static StagedDiff fromJson(const QJsonObject& obj) {
        StagedDiff d;
        d.filePath = detail::getString(obj, {"file_path", "filePath", "path", "relative_path", "relativePath"});
        d.id = detail::getString(obj, {"id", "diff_id", "diffId"}, d.filePath);
        d.threadId = detail::getString(obj, {"thread_id", "threadId"});
        d.unifiedDiff = detail::getString(obj, {"unified_diff", "unifiedDiff", "diff", "patch"});
        const bool verified = detail::getBool(obj, {"is_verified", "isVerified", "verified"}, false);
        d.status = detail::getString(obj, {"status", "state"},
                                     verified ? QStringLiteral("verified") : QStringLiteral("staged"));
        if (d.status == QLatin1StringView("staged") && verified) {
            d.status = QStringLiteral("verified");
        }
        d.linesAdded = detail::getInt(
            obj, {"lines_added", "linesAdded", "added_lines", "addedLines", "insertions", "added"}, 0);
        d.linesRemoved = detail::getInt(
            obj, {"lines_removed", "linesRemoved", "removed_lines", "removedLines", "deletions", "removed"}, 0);
        d.summary = detail::getString(obj, {"summary", "description", "relative_path", "relativePath"});
        return d;
    }

    QJsonObject toJson() const {
        return QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("thread_id"), threadId},
            {QStringLiteral("file_path"), filePath},
            {QStringLiteral("unified_diff"), unifiedDiff},
            {QStringLiteral("status"), status},
            {QStringLiteral("lines_added"), linesAdded},
            {QStringLiteral("lines_removed"), linesRemoved},
            {QStringLiteral("summary"), summary},
        };
    }
};

struct AgentTimer {
    QString id;
    QString threadId;
    QString label;
    QString note;
    double durationS = 0.0;
    double remainingS = 0.0;
    QString status;
    bool delivered = false;
    QString watchPath;

    bool operator==(const AgentTimer&) const = default;

    static AgentTimer fromJson(const QJsonObject& obj) {
        AgentTimer t;
        t.id = detail::getString(obj, {"id", "timer_id", "timerId"});
        t.threadId = detail::getString(obj, {"thread_id", "threadId"});
        t.label = detail::getString(obj, {"label", "name"}, QStringLiteral("timer"));
        t.note = detail::getString(obj, {"note", "alarm_note", "alarmNote"});
        t.durationS = detail::getDouble(obj, {"duration_s", "durationS", "duration_secs", "durationSecs"}, 0.0);
        t.remainingS = detail::getDouble(
            obj, {"remaining_s", "remainingS", "remaining_secs", "remainingSecs"}, t.durationS);
        t.status = detail::getString(obj, {"status"}, QStringLiteral("armed"));
        t.delivered = detail::getBool(obj, {"delivered"}, false);

        const QJsonValue watchVal = detail::firstValue(
            obj, {"watch_path", "watchPath", "watch_target", "watchTarget", "watch_target_json", "watchTargetJSON"});
        if (watchVal.isString()) {
            t.watchPath = watchVal.toString();
        } else if (watchVal.isObject()) {
            const QJsonObject wObj = watchVal.toObject();
            t.watchPath = detail::getString(wObj, {"path", "file", "watch_path", "watchPath"});
            if (t.watchPath.isEmpty()) {
                t.watchPath = QString::fromUtf8(QJsonDocument(wObj).toJson(QJsonDocument::Compact));
            }
        }
        return t;
    }

    QJsonObject toJson() const {
        QJsonObject obj{
            {QStringLiteral("id"), id},
            {QStringLiteral("thread_id"), threadId},
            {QStringLiteral("label"), label},
            {QStringLiteral("note"), note},
            {QStringLiteral("duration_s"), durationS},
            {QStringLiteral("remaining_s"), remainingS},
            {QStringLiteral("status"), status},
            {QStringLiteral("delivered"), delivered},
        };
        if (!watchPath.isEmpty()) {
            obj.insert(QStringLiteral("watch_path"), watchPath);
        }
        return obj;
    }
};

struct RubricTag {
    QString id;
    QString name;
    QString description;
    QString directive;
    bool enabled = true;
    bool isBuiltIn = false;

    bool operator==(const RubricTag&) const = default;

    static RubricTag fromJson(const QJsonObject& obj) {
        RubricTag tag;
        tag.name = detail::getString(obj, {"name", "id"});
        tag.id = detail::getString(obj, {"id", "name"}, tag.name);
        tag.description = detail::getString(obj, {"description", "summary"});
        tag.directive = detail::getString(
            obj, {"directive", "rubric_prompt", "rubricPrompt", "instruction", "prompt"});
        tag.enabled = detail::getBool(obj, {"enabled", "is_enabled", "isEnabled"}, true);
        tag.isBuiltIn = detail::getBool(
            obj, {"is_built_in", "isBuiltIn", "is_builtin", "isBuiltin", "builtin"}, false);
        return tag;
    }

    QJsonObject toJson() const {
        return QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("description"), description},
            {QStringLiteral("directive"), directive},
            {QStringLiteral("enabled"), enabled},
            {QStringLiteral("is_built_in"), isBuiltIn},
        };
    }
};

struct SubagentSlotStatus {
    int slot = 0;
    QString state;
    QString modelId;
    QString lastReply;
    QStringList recentLines;
    int turnCount = 0;

    bool operator==(const SubagentSlotStatus&) const = default;

    static SubagentSlotStatus fromJson(const QJsonObject& obj) {
        SubagentSlotStatus s;
        s.slot = detail::getInt(obj, {"slot", "slot_id", "slotId", "id", "index"}, 0);
        s.state = detail::getString(obj, {"state", "status"}, QStringLiteral("idle"));
        s.modelId = detail::getString(obj, {"model_id", "modelId", "model"});
        s.lastReply = detail::getString(
            obj, {"last_reply", "lastReply", "reply", "output", "active_task", "activeTask"});
        s.recentLines = detail::getStringList(obj, {"recent_lines", "recentLines", "lines"});
        s.turnCount = detail::getInt(obj, {"turn_count", "turnCount", "turns", "line_count", "lineCount"}, 0);
        return s;
    }

    QJsonObject toJson() const {
        return QJsonObject{
            {QStringLiteral("slot"), slot},
            {QStringLiteral("state"), state},
            {QStringLiteral("model_id"), modelId},
            {QStringLiteral("last_reply"), lastReply},
            {QStringLiteral("recent_lines"), detail::toJsonArray(recentLines)},
            {QStringLiteral("turn_count"), turnCount},
        };
    }
};

struct ActiveSubagentInfo {
    QString id;
    QString role;
    QString label;
    QString modelId;
    QString status;
    qint64 startedAt = 0;

    bool operator==(const ActiveSubagentInfo&) const = default;

    static ActiveSubagentInfo fromJson(const QJsonObject& obj) {
        ActiveSubagentInfo info;
        info.id = detail::getString(obj, {"id", "subagent_id", "subagentId"});
        info.role = detail::getString(obj, {"role"});
        info.label = detail::getString(obj, {"label", "slot_label", "slotLabel", "title"}, info.role);
        info.modelId = detail::getString(obj, {"model_id", "modelId", "model"});
        info.status = detail::getString(obj, {"status", "state"}, QStringLiteral("running"));
        info.startedAt = detail::getInt64(
            obj, {"started_at", "startedAt", "started_at_unix_ms", "startedAtUnixMs"}, 0);
        return info;
    }

    QJsonObject toJson() const {
        return QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("role"), role},
            {QStringLiteral("label"), label},
            {QStringLiteral("model_id"), modelId},
            {QStringLiteral("status"), status},
            {QStringLiteral("started_at"), startedAt},
        };
    }
};

struct OnlineResearchSettings {
    QString mode = QStringLiteral("offline"); // "offline" | "curated" | "open_web"
    QStringList allowlist;
    QStringList denylist;
    QString searchProvider;
    QString searchEndpoint;
    int maxFetchesPerTurn = 6;
    int timeoutSecs = 20;
    bool hasGithubToken = false;
    bool hasHfToken = false;
    bool hasOpenAlexKey = false;
    bool hasSearchKey = false;

    bool operator==(const OnlineResearchSettings&) const = default;

    static OnlineResearchSettings fromJson(const QJsonObject& obj) {
        OnlineResearchSettings s;
        s.mode = detail::normalizeOnlineMode(
            detail::getString(obj, {"mode", "online_research_mode", "onlineResearchMode"}, QStringLiteral("offline")));
        s.allowlist = detail::getStringList(
            obj, {"allowlist", "custom_allowed_domains", "customAllowedDomains", "allowed_domains", "allowedDomains"});
        s.denylist = detail::getStringList(
            obj, {"denylist", "denied_domains", "deniedDomains", "blocked_domains", "blockedDomains"});
        s.searchProvider = detail::getString(obj, {"search_provider", "searchProvider"}, QStringLiteral("none"));
        s.searchEndpoint = detail::getString(obj, {"search_endpoint", "searchEndpoint", "endpoint"});
        s.maxFetchesPerTurn = detail::getInt(obj, {"max_fetches_per_turn", "maxFetchesPerTurn"}, 6);
        s.timeoutSecs = detail::getInt(obj, {"timeout_secs", "timeoutSecs", "timeout_s", "timeoutS"}, 20);
        s.hasGithubToken = detail::getBool(obj, {"has_github_token", "hasGithubToken", "hasGitHubToken"}, false);
        s.hasHfToken = detail::getBool(
            obj, {"has_hf_token", "hasHfToken", "has_huggingface_token", "hasHuggingFaceToken"}, false);
        s.hasOpenAlexKey = detail::getBool(
            obj, {"has_open_alex_key", "hasOpenAlexKey", "has_openalex_api_key", "hasOpenAlexAPIKey"}, false);
        s.hasSearchKey = detail::getBool(
            obj, {"has_search_key", "hasSearchKey", "has_search_api_key", "hasSearchAPIKey"}, false);
        return s;
    }

    QJsonObject toJson() const {
        return QJsonObject{
            {QStringLiteral("mode"), mode},
            {QStringLiteral("allowlist"), detail::toJsonArray(allowlist)},
            {QStringLiteral("denylist"), detail::toJsonArray(denylist)},
            {QStringLiteral("search_provider"), searchProvider},
            {QStringLiteral("search_endpoint"), searchEndpoint},
            {QStringLiteral("max_fetches_per_turn"), maxFetchesPerTurn},
            {QStringLiteral("timeout_secs"), timeoutSecs},
            {QStringLiteral("has_github_token"), hasGithubToken},
            {QStringLiteral("has_hf_token"), hasHfToken},
            {QStringLiteral("has_open_alex_key"), hasOpenAlexKey},
            {QStringLiteral("has_search_key"), hasSearchKey},
        };
    }
};

struct ToolPermissionEntry {
    QString toolName;
    QString category;
    QString mode = QStringLiteral("allow"); // "allow" | "ask" | "deny"
    QString description;

    bool operator==(const ToolPermissionEntry&) const = default;

    static ToolPermissionEntry fromJson(const QJsonObject& obj) {
        ToolPermissionEntry e;
        e.toolName = detail::getString(obj, {"tool_name", "toolName", "name", "tool"});
        e.category = detail::getString(obj, {"category", "group"});
        e.mode = detail::normalizePermissionMode(
            detail::getString(obj, {"mode", "policy"}, QStringLiteral("allow")));
        e.description = detail::getString(obj, {"description", "summary"});
        return e;
    }

    QJsonObject toJson() const {
        return QJsonObject{
            {QStringLiteral("tool_name"), toolName},
            {QStringLiteral("category"), category},
            {QStringLiteral("mode"), mode},
            {QStringLiteral("description"), description},
        };
    }
};

struct TerminalApprovalRequest {
    QString id;
    QString command;
    QString reason;
    QString threadId;
    qint64 createdAt = 0;

    bool operator==(const TerminalApprovalRequest&) const = default;

    static TerminalApprovalRequest fromJson(const QJsonObject& obj) {
        TerminalApprovalRequest r;
        r.id = detail::getString(obj, {"id", "approval_id", "approvalId", "call_id", "callId"});
        r.command = detail::getString(obj, {"command", "text", "input", "cmd_text", "cmdText"});
        if (r.command.isEmpty()) {
            const QJsonValue argsVal = detail::firstValue(obj, {"arguments", "arguments_json", "argumentsJSON"});
            if (argsVal.isObject()) {
                r.command = detail::getString(argsVal.toObject(), {"text", "command", "input"});
            } else if (argsVal.isString()) {
                const QJsonDocument doc = QJsonDocument::fromJson(argsVal.toString().toUtf8());
                if (doc.isObject()) {
                    r.command = detail::getString(doc.object(), {"text", "command", "input"});
                }
            }
        }
        r.reason = detail::getString(obj, {"reason", "summary", "description", "note"});
        r.threadId = detail::getString(obj, {"thread_id", "threadId"});
        r.createdAt = detail::getInt64(obj, {"created_at", "createdAt", "timestamp"}, 0);
        return r;
    }

    QJsonObject toJson() const {
        return QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("command"), command},
            {QStringLiteral("reason"), reason},
            {QStringLiteral("thread_id"), threadId},
            {QStringLiteral("created_at"), createdAt},
        };
    }
};

struct SystemInfo {
    QString os;
    QString osVersion;
    QString arch;
    QString cpuBrand;
    int cpuCores = 0;
    quint64 totalMemoryBytes = 0;
    quint64 availableMemoryBytes = 0;
    QString hostname;
    QString dataDir;
    QString logsDir;

    bool operator==(const SystemInfo&) const = default;

    static SystemInfo fromJson(const QJsonObject& obj) {
        SystemInfo info;
        info.os = detail::getString(obj, {"os", "os_name", "osName", "platform"});
        info.osVersion = detail::getString(obj, {"os_version", "osVersion", "kernel_version", "kernelVersion"});
        info.arch = detail::getString(obj, {"arch", "cpu_arch", "cpuArch", "architecture"});
        info.cpuBrand = detail::getString(obj, {"cpu_brand", "cpuBrand", "cpu_model", "cpuModel", "cpu"});
        info.cpuCores = detail::getInt(
            obj, {"cpu_cores", "cpuCores", "cpu_core_count", "cpuCoreCount", "logical_cores", "logicalCores"}, 0);
        info.totalMemoryBytes = detail::getUInt64(
            obj, {"total_memory_bytes", "totalMemoryBytes", "physical_memory_bytes", "physicalMemoryBytes"}, 0);
        info.availableMemoryBytes = detail::getUInt64(
            obj, {"available_memory_bytes", "availableMemoryBytes", "free_memory_bytes", "freeMemoryBytes"}, 0);
        info.hostname = detail::getString(obj, {"hostname", "host_name", "hostName"});
        info.dataDir = detail::getString(obj, {"data_dir", "dataDir"});
        info.logsDir = detail::getString(obj, {"logs_dir", "logsDir"});
        return info;
    }

    QJsonObject toJson() const {
        return QJsonObject{
            {QStringLiteral("os"), os},
            {QStringLiteral("os_version"), osVersion},
            {QStringLiteral("arch"), arch},
            {QStringLiteral("cpu_brand"), cpuBrand},
            {QStringLiteral("cpu_cores"), cpuCores},
            {QStringLiteral("total_memory_bytes"), static_cast<qint64>(totalMemoryBytes)},
            {QStringLiteral("available_memory_bytes"), static_cast<qint64>(availableMemoryBytes)},
            {QStringLiteral("hostname"), hostname},
            {QStringLiteral("data_dir"), dataDir},
            {QStringLiteral("logs_dir"), logsDir},
        };
    }
};

struct TelemetrySnapshot {
    bool endpointConfigured = false;
    bool engineOnline = false;
    QString engineError;
    QString modelId;
    QString effectiveSubagentModelId;
    int endpointLatencyMs = -1;
    double decodeTokS = 0.0;
    double decodeTokSEwma = 0.0;
    qint64 activePromptTokens = 0;
    quint64 totalPromptTokens = 0;
    quint64 totalCompletionTokens = 0;
    quint64 totalCachedTokens = 0;
    int compactionEpoch = 0;
    bool slot2CriticEnabled = false;

    bool operator==(const TelemetrySnapshot&) const = default;

    static TelemetrySnapshot fromJson(const QJsonObject& obj) {
        TelemetrySnapshot t;
        t.engineOnline = detail::getBool(obj, {"engine_online", "engineOnline", "online"}, false);
        t.engineError = detail::getString(obj, {"engine_error", "engineError", "error"});
        t.modelId = detail::getString(obj, {"model_id", "modelId", "model"});
        t.effectiveSubagentModelId = detail::getString(
            obj,
            {"effective_subagent_model_id", "effectiveSubagentModelId",
             "subagent_model_id", "subagentModelId",
             "coding_subagent_model_id", "codingSubagentModelId"});
        if (t.effectiveSubagentModelId.isEmpty()) {
            t.effectiveSubagentModelId = t.modelId;
        }
        const QString baseUrl = detail::getString(
            obj, {"endpoint_base_url", "endpointBaseUrl", "endpointBaseURL", "engine_base_url", "engineBaseUrl"});
        t.endpointConfigured = detail::getOptionalBool(obj, {"endpoint_configured", "endpointConfigured"})
                                   .value_or(t.engineOnline || !baseUrl.isEmpty() || !t.modelId.isEmpty());
        t.endpointLatencyMs = detail::getInt(
            obj, {"endpoint_latency_ms", "endpointLatencyMs", "latency_ms", "latencyMs"}, -1);
        t.decodeTokS = detail::getDouble(obj, {"decode_tok_s", "decodeTokS"}, 0.0);
        t.decodeTokSEwma = detail::getDouble(obj, {"decode_tok_s_ewma", "decodeTokSEwma"}, 0.0);
        t.activePromptTokens = detail::getInt64(
            obj,
            {"active_prompt_tokens", "activePromptTokens", "prompt_tokens", "promptTokens",
             "kv_cache_used_tokens", "kvCacheUsedTokens"},
            0);
        t.totalPromptTokens = detail::getUInt64(
            obj, {"total_prompt_tokens", "totalPromptTokens", "prompt_tokens", "promptTokens"}, 0);
        t.totalCompletionTokens = detail::getUInt64(
            obj, {"total_completion_tokens", "totalCompletionTokens", "completion_tokens", "completionTokens"}, 0);
        t.totalCachedTokens = detail::getUInt64(
            obj, {"total_cached_tokens", "totalCachedTokens", "cached_tokens", "cachedTokens"}, 0);
        t.compactionEpoch = detail::getInt(obj, {"compaction_epoch", "compactionEpoch"}, 0);
        t.slot2CriticEnabled = detail::getBool(obj, {"slot2_critic_enabled", "slot2CriticEnabled"}, false);
        return t;
    }

    QJsonObject toJson() const {
        return QJsonObject{
            {QStringLiteral("endpoint_configured"), endpointConfigured},
            {QStringLiteral("engine_online"), engineOnline},
            {QStringLiteral("engine_error"), engineError},
            {QStringLiteral("model_id"), modelId},
            {QStringLiteral("effective_subagent_model_id"), effectiveSubagentModelId},
            {QStringLiteral("endpoint_latency_ms"), endpointLatencyMs},
            {QStringLiteral("decode_tok_s"), decodeTokS},
            {QStringLiteral("decode_tok_s_ewma"), decodeTokSEwma},
            {QStringLiteral("active_prompt_tokens"), activePromptTokens},
            {QStringLiteral("total_prompt_tokens"), static_cast<qint64>(totalPromptTokens)},
            {QStringLiteral("total_completion_tokens"), static_cast<qint64>(totalCompletionTokens)},
            {QStringLiteral("total_cached_tokens"), static_cast<qint64>(totalCachedTokens)},
            {QStringLiteral("compaction_epoch"), compactionEpoch},
            {QStringLiteral("slot2_critic_enabled"), slot2CriticEnabled},
        };
    }
};

struct WorkspaceSnapshot {
    QVector<ProjectInfo> projects;
    QString selectedProjectId;
    QVector<ThreadSummary> threads;
    QString selectedThreadId;
    QVector<UiChatEvent> activeEvents;
    QVector<StagedDiff> pendingDiffs;
    QVector<AgentTimer> activeTimers;
    QVector<ModelDescriptor> availableModels;
    QVector<SubagentSlotStatus> subagentSlots;
    QVector<ActiveSubagentInfo> activeSubagents;
    QVector<RubricTag> tags;
    QStringList activeThreadTagIds;
    QVector<ToolPermissionEntry> toolPermissions;
    QVector<TerminalApprovalRequest> pendingTerminalApprovals;
    OnlineResearchSettings onlineResearch;
    TelemetrySnapshot telemetry;
    QString endpointBaseUrl;
    QString modelId;
    QString subagentModelId;
    QString effectiveSubagentModelId;
    bool endpointConfigured = false;
    bool stageWriteMode = false;
    QString subagentProtocol = QStringLiteral("auto"); // "auto" | "native" | "json"
    bool autonomousLoopEnabled = true;
    int maxAgentSteps = 12;
    bool slot2CriticEnabled = false;
    bool isAgentBusy = false;
    QJsonObject raw;

    bool operator==(const WorkspaceSnapshot&) const = default;

    static WorkspaceSnapshot fromJson(const QJsonObject& rootObj) {
        QJsonObject obj = rootObj;
        const QJsonObject nestedSnap = detail::getObject(rootObj, {"snapshot"});
        if (!nestedSnap.isEmpty()) {
            obj = nestedSnap;
        } else {
            const QJsonObject nestedData = detail::getObject(rootObj, {"data"});
            if (!nestedData.isEmpty() && nestedData.contains(QStringLiteral("projects"))) {
                obj = nestedData;
            }
        }

        // Helper checking `obj` first, then falling back to outer envelope `rootObj`.
        auto pickValue = [&](std::initializer_list<const char*> keys) -> QJsonValue {
            QJsonValue v = detail::firstValue(obj, keys);
            if (v.isUndefined() && &obj != &rootObj) {
                v = detail::firstValue(rootObj, keys);
            }
            return v;
        };
        auto pickString = [&](std::initializer_list<const char*> keys, const QString& def = {}) -> QString {
            const QJsonValue v = pickValue(keys);
            return v.isString() ? v.toString() : def;
        };
        auto pickOptionalBool = [&](std::initializer_list<const char*> keys) -> std::optional<bool> {
            if (auto b = detail::getOptionalBool(obj, keys)) {
                return b;
            }
            return detail::getOptionalBool(rootObj, keys);
        };
        auto pickArray = [&](std::initializer_list<const char*> keys) -> QJsonArray {
            const QJsonValue v = pickValue(keys);
            return v.isArray() ? v.toArray() : QJsonArray{};
        };
        auto pickObject = [&](std::initializer_list<const char*> keys) -> QJsonObject {
            const QJsonValue v = pickValue(keys);
            return v.isObject() ? v.toObject() : QJsonObject{};
        };

        WorkspaceSnapshot snap;
        snap.raw = obj;

        // Projects
        const QJsonArray projArr = pickArray({"projects"});
        snap.projects.reserve(projArr.size());
        for (const QJsonValue& item : projArr) {
            if (item.isObject()) {
                snap.projects.append(ProjectInfo::fromJson(item.toObject()));
            }
        }
        snap.selectedProjectId = pickString(
            {"selected_project_id", "selectedProjectId", "active_project_id", "activeProjectId"});
        if (snap.selectedProjectId.isEmpty() && !snap.projects.isEmpty()) {
            snap.selectedProjectId = snap.projects.first().id;
        }

        // Threads & Active Thread
        const QJsonArray threadArr = pickArray({"threads"});
        snap.threads.reserve(threadArr.size());
        for (const QJsonValue& item : threadArr) {
            if (item.isObject()) {
                snap.threads.append(ThreadSummary::fromJson(item.toObject()));
            }
        }

        QJsonObject activeThreadObj = pickObject(
            {"active_thread", "activeThread", "selected_thread", "selectedThread"});
        snap.selectedThreadId = pickString(
            {"selected_thread_id", "selectedThreadId", "active_thread_id", "activeThreadId"});
        if (snap.selectedThreadId.isEmpty() && !activeThreadObj.isEmpty()) {
            snap.selectedThreadId = detail::getString(activeThreadObj, {"id", "thread_id", "threadId"});
        }
        if (snap.selectedThreadId.isEmpty() && !snap.threads.isEmpty()) {
            snap.selectedThreadId = snap.threads.first().id;
        }
        if (activeThreadObj.isEmpty() && !snap.selectedThreadId.isEmpty()) {
            for (const QJsonValue& item : threadArr) {
                if (!item.isObject()) {
                    continue;
                }
                const QJsonObject tObj = item.toObject();
                if (detail::getString(tObj, {"id", "thread_id", "threadId"}) == snap.selectedThreadId) {
                    activeThreadObj = tObj;
                    break;
                }
            }
        }

        // Active chat events
        QJsonArray eventsArr = pickArray({"active_events", "activeEvents"});
        if (eventsArr.isEmpty() && !activeThreadObj.isEmpty()) {
            eventsArr = detail::getArray(activeThreadObj, {"events", "ui_events", "uiEvents"});
        }
        snap.activeEvents.reserve(eventsArr.size());
        for (const QJsonValue& item : eventsArr) {
            if (item.isObject()) {
                snap.activeEvents.append(UiChatEvent::fromJson(item.toObject()));
            }
        }

        // Active thread tags
        snap.activeThreadTagIds = detail::getStringList(obj, {"active_thread_tag_ids", "activeThreadTagIds"});
        if (snap.activeThreadTagIds.isEmpty() && &obj != &rootObj) {
            snap.activeThreadTagIds = detail::getStringList(rootObj, {"active_thread_tag_ids", "activeThreadTagIds"});
        }
        if (snap.activeThreadTagIds.isEmpty() && !activeThreadObj.isEmpty()) {
            snap.activeThreadTagIds = detail::getStringList(
                activeThreadObj, {"active_tags", "activeTags", "tags"});
        }

        // Pending diffs / staged mutations
        const QJsonArray diffsArr = pickArray(
            {"pending_diffs", "pendingDiffs", "staged_mutations", "stagedMutations", "staged_diffs", "stagedDiffs"});
        snap.pendingDiffs.reserve(diffsArr.size());
        for (const QJsonValue& item : diffsArr) {
            if (item.isObject()) {
                snap.pendingDiffs.append(StagedDiff::fromJson(item.toObject()));
            }
        }

        // Active timers
        const QJsonArray timersArr = pickArray({"active_timers", "activeTimers", "timers"});
        snap.activeTimers.reserve(timersArr.size());
        for (const QJsonValue& item : timersArr) {
            if (item.isObject()) {
                snap.activeTimers.append(AgentTimer::fromJson(item.toObject()));
            }
        }

        // Available models
        const QJsonArray modelsArr = pickArray({"available_models", "availableModels", "models"});
        snap.availableModels.reserve(modelsArr.size());
        for (const QJsonValue& item : modelsArr) {
            if (item.isObject()) {
                ModelDescriptor m = ModelDescriptor::fromJson(item.toObject());
                if (!m.id.isEmpty()) {
                    snap.availableModels.append(std::move(m));
                }
            } else if (item.isString()) {
                const QString id = item.toString().trimmed();
                if (!id.isEmpty()) {
                    snap.availableModels.append(ModelDescriptor{id, {}, 0});
                }
            }
        }

        // Subagent slots
        QJsonArray slotsArr = pickArray({"subagent_slots", "subagentSlots"});
        if (slotsArr.isEmpty()) {
            const QJsonObject poolObj = pickObject({"subagent_pool_status", "subagentPoolStatus"});
            if (!poolObj.isEmpty()) {
                slotsArr = detail::getArray(poolObj, {"subagents", "slots", "subagent_slots", "subagentSlots"});
            }
        }
        snap.subagentSlots.reserve(slotsArr.size());
        for (const QJsonValue& item : slotsArr) {
            if (item.isObject()) {
                snap.subagentSlots.append(SubagentSlotStatus::fromJson(item.toObject()));
            }
        }

        // Active subagents
        const QJsonArray activeSubArr = pickArray({"active_subagents", "activeSubagents"});
        snap.activeSubagents.reserve(activeSubArr.size());
        for (const QJsonValue& item : activeSubArr) {
            if (item.isObject()) {
                snap.activeSubagents.append(ActiveSubagentInfo::fromJson(item.toObject()));
            }
        }

        // Rubric tags
        const QJsonArray tagsArr = pickArray({"tags", "available_tags", "availableTags"});
        snap.tags.reserve(tagsArr.size());
        for (const QJsonValue& item : tagsArr) {
            if (item.isObject()) {
                snap.tags.append(RubricTag::fromJson(item.toObject()));
            }
        }

        // Tool permissions (supports both array-of-objects and map `{toolName: policy}`)
        const QJsonValue permVal = pickValue({"tool_permissions", "toolPermissions"});
        if (permVal.isArray()) {
            const QJsonArray permArr = permVal.toArray();
            snap.toolPermissions.reserve(permArr.size());
            for (const QJsonValue& item : permArr) {
                if (item.isObject()) {
                    snap.toolPermissions.append(ToolPermissionEntry::fromJson(item.toObject()));
                }
            }
        } else if (permVal.isObject()) {
            const QJsonObject permMap = permVal.toObject();
            snap.toolPermissions.reserve(permMap.size());
            for (auto it = permMap.constBegin(); it != permMap.constEnd(); ++it) {
                ToolPermissionEntry entry;
                entry.toolName = it.key();
                if (it.value().isString()) {
                    entry.mode = detail::normalizePermissionMode(it.value().toString());
                } else if (it.value().isObject()) {
                    entry = ToolPermissionEntry::fromJson(it.value().toObject());
                    if (entry.toolName.isEmpty()) {
                        entry.toolName = it.key();
                    }
                }
                snap.toolPermissions.append(std::move(entry));
            }
            std::sort(snap.toolPermissions.begin(), snap.toolPermissions.end(),
                      [](const ToolPermissionEntry& a, const ToolPermissionEntry& b) {
                          return a.toolName < b.toolName;
                      });
        }

        // Pending terminal approvals (also scans `pending_confirmations` for terminal calls)
        const QJsonArray termArr = pickArray({"pending_terminal_approvals", "pendingTerminalApprovals"});
        if (!termArr.isEmpty()) {
            snap.pendingTerminalApprovals.reserve(termArr.size());
            for (const QJsonValue& item : termArr) {
                if (item.isObject()) {
                    snap.pendingTerminalApprovals.append(TerminalApprovalRequest::fromJson(item.toObject()));
                }
            }
        } else {
            const QJsonArray confArr = pickArray({"pending_confirmations", "pendingConfirmations"});
            for (const QJsonValue& item : confArr) {
                if (!item.isObject()) {
                    continue;
                }
                const QJsonObject cObj = item.toObject();
                const QString tName = detail::getString(cObj, {"tool_name", "toolName", "name"}).toLower();
                if (tName.contains(QLatin1StringView("terminal")) || cObj.contains(QStringLiteral("command"))) {
                    snap.pendingTerminalApprovals.append(TerminalApprovalRequest::fromJson(cObj));
                }
            }
        }

        // Online research settings
        const QJsonObject onlineObj = pickObject({"online_research", "onlineResearch"});
        if (!onlineObj.isEmpty()) {
            snap.onlineResearch = OnlineResearchSettings::fromJson(onlineObj);
        }

        // Telemetry & top-level endpoint fields
        const QJsonObject telObj = pickObject({"telemetry"});
        snap.telemetry = TelemetrySnapshot::fromJson(telObj);
        if (snap.telemetry.activePromptTokens <= 0 && !activeThreadObj.isEmpty()) {
            snap.telemetry.activePromptTokens = detail::getInt64(
                activeThreadObj, {"prompt_tokens", "promptTokens"}, 0);
        }

        const QString telBaseUrl = detail::getString(
            telObj, {"endpoint_base_url", "endpointBaseUrl", "endpointBaseURL", "engine_base_url", "engineBaseUrl"});
        snap.endpointBaseUrl = pickString(
            {"endpoint_base_url", "endpointBaseUrl", "endpointBaseURL", "engine_base_url", "engineBaseUrl"},
            telBaseUrl);
        snap.modelId = pickString({"model_id", "modelId"}, snap.telemetry.modelId);
        snap.subagentModelId = pickString(
            {"subagent_model_id", "subagentModelId"},
            detail::getString(telObj, {"subagent_model_id", "subagentModelId"}));
        snap.effectiveSubagentModelId = pickString(
            {"effective_subagent_model_id", "effectiveSubagentModelId"});
        if (snap.effectiveSubagentModelId.isEmpty()) {
            if (!snap.telemetry.effectiveSubagentModelId.isEmpty()) {
                snap.effectiveSubagentModelId = snap.telemetry.effectiveSubagentModelId;
            } else if (!snap.subagentModelId.isEmpty()) {
                snap.effectiveSubagentModelId = snap.subagentModelId;
            } else {
                snap.effectiveSubagentModelId = snap.modelId;
            }
        }
        snap.endpointConfigured = pickOptionalBool({"endpoint_configured", "endpointConfigured"})
                                      .value_or(snap.telemetry.endpointConfigured || !snap.endpointBaseUrl.isEmpty());
        snap.slot2CriticEnabled = pickOptionalBool({"slot2_critic_enabled", "slot2CriticEnabled"})
                                      .value_or(snap.telemetry.slot2CriticEnabled);

        // Synchronize top-level endpoint fields back into telemetry
        if (snap.telemetry.modelId.isEmpty() && !snap.modelId.isEmpty()) {
            snap.telemetry.modelId = snap.modelId;
        }
        if (snap.telemetry.effectiveSubagentModelId.isEmpty() && !snap.effectiveSubagentModelId.isEmpty()) {
            snap.telemetry.effectiveSubagentModelId = snap.effectiveSubagentModelId;
        }
        if (snap.endpointConfigured) {
            snap.telemetry.endpointConfigured = true;
        }
        if (snap.slot2CriticEnabled) {
            snap.telemetry.slot2CriticEnabled = true;
        }

        // Execution & loop flags
        snap.stageWriteMode = pickOptionalBool({"stage_write_mode", "stageWriteMode"})
                                  .value_or(detail::getBool(telObj, {"stage_write_mode", "stageWriteMode"}, false));
        snap.subagentProtocol = detail::normalizeSubagentProtocol(
            pickString({"subagent_protocol", "subagentProtocol"}, QStringLiteral("auto")));
        snap.autonomousLoopEnabled = pickOptionalBool({"autonomous_loop_enabled", "autonomousLoopEnabled"})
                                         .value_or(true);
        snap.maxAgentSteps = detail::getInt(
            obj, {"max_agent_steps", "maxAgentSteps", "max_steps", "maxSteps"},
            detail::getInt(rootObj, {"max_agent_steps", "maxAgentSteps", "max_steps", "maxSteps"}, 12));
        snap.isAgentBusy = pickOptionalBool({"is_agent_busy", "isAgentBusy", "agent_busy", "agentBusy", "busy"})
                               .value_or(false);

        return snap;
    }

    QJsonObject toJson() const {
        QJsonArray projArr;
        for (const auto& p : projects) {
            projArr.append(p.toJson());
        }
        QJsonArray threadArr;
        for (const auto& t : threads) {
            threadArr.append(t.toJson());
        }
        QJsonArray eventsArr;
        for (const auto& e : activeEvents) {
            eventsArr.append(e.toJson());
        }
        QJsonArray diffsArr;
        for (const auto& d : pendingDiffs) {
            diffsArr.append(d.toJson());
        }
        QJsonArray timersArr;
        for (const auto& tm : activeTimers) {
            timersArr.append(tm.toJson());
        }
        QJsonArray modelsArr;
        for (const auto& m : availableModels) {
            modelsArr.append(m.toJson());
        }
        QJsonArray slotsArr;
        for (const auto& s : subagentSlots) {
            slotsArr.append(s.toJson());
        }
        QJsonArray activeSubArr;
        for (const auto& a : activeSubagents) {
            activeSubArr.append(a.toJson());
        }
        QJsonArray tagsArr;
        for (const auto& tg : tags) {
            tagsArr.append(tg.toJson());
        }
        QJsonArray permsArr;
        for (const auto& p : toolPermissions) {
            permsArr.append(p.toJson());
        }
        QJsonArray termArr;
        for (const auto& tr : pendingTerminalApprovals) {
            termArr.append(tr.toJson());
        }

        return QJsonObject{
            {QStringLiteral("projects"), projArr},
            {QStringLiteral("selected_project_id"), selectedProjectId},
            {QStringLiteral("threads"), threadArr},
            {QStringLiteral("selected_thread_id"), selectedThreadId},
            {QStringLiteral("active_events"), eventsArr},
            {QStringLiteral("pending_diffs"), diffsArr},
            {QStringLiteral("active_timers"), timersArr},
            {QStringLiteral("available_models"), modelsArr},
            {QStringLiteral("subagent_slots"), slotsArr},
            {QStringLiteral("active_subagents"), activeSubArr},
            {QStringLiteral("tags"), tagsArr},
            {QStringLiteral("active_thread_tag_ids"), detail::toJsonArray(activeThreadTagIds)},
            {QStringLiteral("tool_permissions"), permsArr},
            {QStringLiteral("pending_terminal_approvals"), termArr},
            {QStringLiteral("online_research"), onlineResearch.toJson()},
            {QStringLiteral("telemetry"), telemetry.toJson()},
            {QStringLiteral("endpoint_base_url"), endpointBaseUrl},
            {QStringLiteral("model_id"), modelId},
            {QStringLiteral("subagent_model_id"), subagentModelId},
            {QStringLiteral("effective_subagent_model_id"), effectiveSubagentModelId},
            {QStringLiteral("endpoint_configured"), endpointConfigured},
            {QStringLiteral("stage_write_mode"), stageWriteMode},
            {QStringLiteral("subagent_protocol"), subagentProtocol},
            {QStringLiteral("autonomous_loop_enabled"), autonomousLoopEnabled},
            {QStringLiteral("max_agent_steps"), maxAgentSteps},
            {QStringLiteral("slot2_critic_enabled"), slot2CriticEnabled},
            {QStringLiteral("is_agent_busy"), isAgentBusy},
        };
    }
};

} // namespace spell

Q_DECLARE_METATYPE(spell::ModelDescriptor)
Q_DECLARE_METATYPE(spell::ProjectInfo)
Q_DECLARE_METATYPE(spell::ThreadSummary)
Q_DECLARE_METATYPE(spell::UiChatEvent)
Q_DECLARE_METATYPE(spell::StagedDiff)
Q_DECLARE_METATYPE(spell::AgentTimer)
Q_DECLARE_METATYPE(spell::RubricTag)
Q_DECLARE_METATYPE(spell::SubagentSlotStatus)
Q_DECLARE_METATYPE(spell::ActiveSubagentInfo)
Q_DECLARE_METATYPE(spell::OnlineResearchSettings)
Q_DECLARE_METATYPE(spell::ToolPermissionEntry)
Q_DECLARE_METATYPE(spell::TerminalApprovalRequest)
Q_DECLARE_METATYPE(spell::SystemInfo)
Q_DECLARE_METATYPE(spell::TelemetrySnapshot)
Q_DECLARE_METATYPE(spell::WorkspaceSnapshot)
