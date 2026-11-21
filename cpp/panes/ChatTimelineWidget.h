#pragma once

#include "core/SpellModels.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <memory>
#include <optional>

class QEvent;
class QResizeEvent;

namespace spell {

/**
 * Per-file outcome entry parsed from an autonomous coding subagent delegation payload.
 */
struct DelegationFileSummary {
    QString path;
    int added = 0;
    int removed = 0;
    bool criticPassed = true;
    int bounces = 0;

    bool operator==(const DelegationFileSummary&) const = default;

    static DelegationFileSummary fromJson(const QJsonObject& obj) {
        DelegationFileSummary f;
        f.path = detail::getString(obj, {"path", "file_path", "filePath", "relative_path", "relativePath"});
        f.added = detail::getInt(obj, {"added", "added_lines", "addedLines", "insertions"}, 0);
        f.removed = detail::getInt(obj, {"removed", "removed_lines", "removedLines", "deletions"}, 0);
        f.criticPassed = detail::getBool(obj, {"critic_passed", "criticPassed", "passed", "verified"}, true);
        f.bounces = detail::getInt(obj, {"bounces", "critic_bounces", "criticBounces", "bounce_count"}, 0);
        return f;
    }
};

/**
 * Structured metadata emitted by `spell_core` inside subagent delegation events via
 * `<!-- spell_delegation_meta: {...} -->` comments or structured `delegation_meta` JSON.
 */
struct DelegationMeta {
    static constexpr const char* kCommentPrefix = "<!-- spell_delegation_meta:";
    static constexpr const char* kAltCommentPrefix = "<!-- beatrix_delegation_meta:";
    static constexpr const char* kCommentSuffix = "-->";

    QString undoId;
    QString protocolUsed;
    bool fallbackTriggered = false;
    int steps = 0;
    int maxSteps = 0;
    QString status; // "committed" | "escalated" | ...
    QString escalationReason;
    QString summary;
    QStringList proposalIds;
    QVector<DelegationFileSummary> files;

    bool operator==(const DelegationMeta&) const = default;

    [[nodiscard]] bool isCommitted() const {
        return status.trimmed().compare(QStringLiteral("committed"), Qt::CaseInsensitive) == 0;
    }

    [[nodiscard]] bool isEscalated() const {
        return status.trimmed().compare(QStringLiteral("escalated"), Qt::CaseInsensitive) == 0;
    }

    [[nodiscard]] int totalAdded() const {
        int sum = 0;
        for (const auto& f : files) {
            sum += f.added;
        }
        return sum;
    }

    [[nodiscard]] int totalRemoved() const {
        int sum = 0;
        for (const auto& f : files) {
            sum += f.removed;
        }
        return sum;
    }

    static DelegationMeta fromJson(const QJsonObject& obj) {
        DelegationMeta meta;
        meta.undoId = detail::getString(obj, {"undo_id", "undoId"});
        meta.protocolUsed = detail::getString(obj, {"protocol_used", "protocolUsed", "protocol"},
                                              QStringLiteral("native"));
        meta.fallbackTriggered = detail::getBool(
            obj, {"fallback_triggered", "fallbackTriggered", "fallback"}, false);
        meta.steps = detail::getInt(obj, {"steps", "step_count", "stepCount"}, 0);
        meta.maxSteps = detail::getInt(obj, {"max_steps", "maxSteps"}, 0);
        meta.status = detail::getString(obj, {"status", "state"});
        meta.escalationReason = detail::getString(obj, {"escalation_reason", "escalationReason"});
        meta.summary = detail::getString(obj, {"summary", "message"});
        meta.proposalIds = detail::getStringList(obj, {"proposal_ids", "proposalIds"});

        const QJsonArray filesArr = detail::getArray(obj, {"files"});
        meta.files.reserve(filesArr.size());
        for (const QJsonValue& item : filesArr) {
            if (item.isObject()) {
                meta.files.append(DelegationFileSummary::fromJson(item.toObject()));
            }
        }
        return meta;
    }

    /**
     * Parses `<!-- spell_delegation_meta: {...} -->` (or legacy fallback)
     * embedded in tool output or delegation event text.
     */
    static std::optional<DelegationMeta> parseFromText(const QString& text) {
        const auto extractWithPrefix = [&text](QLatin1StringView prefix) -> std::optional<DelegationMeta> {
            const qsizetype start = text.lastIndexOf(prefix);
            if (start < 0) {
                return std::nullopt;
            }
            const qsizetype jsonStart = start + prefix.size();
            const qsizetype endRel = text.indexOf(QLatin1StringView(kCommentSuffix), jsonStart);
            if (endRel < 0) {
                return std::nullopt;
            }
            const QString jsonSlice = text.mid(jsonStart, endRel - jsonStart).trimmed();
            if (jsonSlice.isEmpty()) {
                return std::nullopt;
            }
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(jsonSlice.toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                return std::nullopt;
            }
            return fromJson(doc.object());
        };

        if (auto parsed = extractWithPrefix(QLatin1StringView(kCommentPrefix))) {
            return parsed;
        }
        return extractWithPrefix(QLatin1StringView(kAltCommentPrefix));
    }

    /**
     * Extracts `DelegationMeta` from a `UiChatEvent`, checking structured JSON fields
     * on `event.raw` first and then falling back to HTML comment metadata in
     * `event.toolOutput` and `event.text`.
     */
    static std::optional<DelegationMeta> parseFromEvent(const UiChatEvent& event) {
        QJsonObject metaObj = detail::getObject(event.raw, {"delegation_meta", "delegationMeta"});
        if (metaObj.isEmpty()) {
            const QJsonObject toolObj = detail::getObject(event.raw, {"tool_call", "toolCall"});
            if (!toolObj.isEmpty()) {
                metaObj = detail::getObject(toolObj, {"delegation_meta", "delegationMeta"});
            }
        }
        if (!metaObj.isEmpty()) {
            return fromJson(metaObj);
        }
        if (!event.toolOutput.isEmpty()) {
            if (auto parsed = parseFromText(event.toolOutput)) {
                return parsed;
            }
        }
        if (!event.text.isEmpty()) {
            return parseFromText(event.text);
        }
        return std::nullopt;
    }

    /** Removes embedded delegation metadata HTML comments from user-visible text. */
    static QString stripMetadataComment(const QString& text) {
        QString out = text;
        const auto stripPrefix = [&out](QLatin1StringView prefix) {
            while (true) {
                const qsizetype start = out.indexOf(prefix);
                if (start < 0) {
                    break;
                }
                const qsizetype end = out.indexOf(QLatin1StringView(kCommentSuffix), start + prefix.size());
                if (end < 0) {
                    out.remove(start, out.size() - start);
                    break;
                }
                out.remove(start, (end + 3) - start);
            }
        };
        stripPrefix(QLatin1StringView(kCommentPrefix));
        stripPrefix(QLatin1StringView(kAltCommentPrefix));
        return out.trimmed();
    }
};

/**
 * Scrollable conversation timeline (`QScrollArea`) for Spell IDE.
 *
 * Renders every `UiChatEvent` kind cleanly in the monochrome Spell IDE visual system:
 * - `user`: right-aligned monochrome bubble with inline Edit & Resend editor.
 * - `assistant` / `reasoning`: left-aligned `SpellMarkdownView` with clickable file
 *   citations, optional collapsible Reasoning/Thinking drawer, and Turn Stats footer
 *   (`tok/s`, prompt/completion tokens, latency, model ID).
 * - `tool_call` / `tool_result`: collapsible monospaced card showing tool name,
 *   arguments/output, and clickable file links.
 * - `diff_staged`: embedded `SpellDiffWidget` with Approve / Reject controls.
 * - `delegation`: structured subagent delegation card parsing `DelegationMeta`
 *   (`<!-- spell_delegation_meta: ... -->`) when present.
 * - `timer_alarm`, `compaction_notice`, `error`, `system`: monochrome/danger banners.
 *
 * Supports smooth auto-scrolling during streaming (automatically paused when the user
 * scrolls up) and flicker-free incremental tail updates when only the trailing streaming
 * message grows.
 */
class ChatTimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit ChatTimelineWidget(QWidget* parent = nullptr);
    ~ChatTimelineWidget() override;

    ChatTimelineWidget(const ChatTimelineWidget&) = delete;
    ChatTimelineWidget& operator=(const ChatTimelineWidget&) = delete;
    ChatTimelineWidget(ChatTimelineWidget&&) = delete;
    ChatTimelineWidget& operator=(ChatTimelineWidget&&) = delete;

    /**
     * Synchronizes the timeline with `events`, pending `diffs`, and workspace `projectRoots`
     * (used to resolve relative file citations).
     *
     * Performs an in-place update of the streaming tail widget whenever only the last
     * streaming event grew, or appends new events incrementally, avoiding full rebuild
     * flicker during token streaming.
     */
    void updateEvents(const QVector<UiChatEvent>& events,
                      const QVector<StagedDiff>& diffs = {},
                      const QStringList& projectRoots = {});

    /** Stores empty-state context metadata (empty state renders only the centered witch hat logo). */
    void setEmptyStateContext(bool hasProject, bool endpointConfigured);

    /** Removes all rendered event widgets and resets scroll tracking state. */
    void clear();

    /**
     * Scrolls the timeline to the bottom. When `force` is false, respects user scroll-up
     * pause so reading earlier messages is never interrupted during streaming.
     */
    void scrollToBottom(bool force = false);

    /** Returns true when auto-scroll is currently paused because the user scrolled upward. */
    [[nodiscard]] bool isAutoScrollPaused() const;

    /** Returns the number of events currently represented in the timeline. */
    [[nodiscard]] int eventCount() const;

    /** Re-applies the active Spell IDE monochrome palette and code syntax theme. */
    void applyTheme();

signals:
    void fileCitationClicked(const QString& filePath, int line);
    void approveDiffRequested(const QString& diffId);
    void rejectDiffRequested(const QString& diffId);
    void editAndResendRequested(int turnIndex, const QString& newPrompt);
    void configureEndpointRequested();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
