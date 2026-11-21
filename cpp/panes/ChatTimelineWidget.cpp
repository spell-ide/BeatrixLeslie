#include "panes/ChatTimelineWidget.h"

#include "core/SpellCitationLinkifier.h"
#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"
#include "widgets/SpellDiffWidget.h"
#include "widgets/SpellMarkdownView.h"

#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace spell {

namespace {

constexpr int kBottomThresholdPx = 36;
constexpr int kMaxToolBodyHeightPx = 260;
constexpr int kMinToolBodyHeightPx = 38;
constexpr int kMinUserEditHeightPx = 64;
constexpr int kMaxUserEditHeightPx = 220;

QFont makeUiFont(double ptSize, QFont::Weight weight = QFont::Normal) {
    QFont f;
    f.setPointSizeF(ptSize);
    f.setWeight(weight);
    return f;
}

QFont makeMonoFont(double ptSize, QFont::Weight weight = QFont::Normal) {
    QFont f(ThemeManager::instance().editorFontFamily());
    f.setStyleHint(QFont::Monospace);
    f.setPointSizeF(ptSize);
    f.setWeight(weight);
    return f;
}

QString resolveWorkspacePath(const QString& rawPath, const QStringList& projectRoots) {
    const QString trimmed = rawPath.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    const QFileInfo direct(trimmed);
    if (direct.isAbsolute()) {
        return QDir::cleanPath(trimmed);
    }
    for (const QString& root : projectRoots) {
        if (root.trimmed().isEmpty()) {
            continue;
        }
        const QString candidate = QDir(root).filePath(trimmed);
        if (QFileInfo::exists(candidate)) {
            return QDir::cleanPath(candidate);
        }
    }
    if (!projectRoots.isEmpty() && !projectRoots.first().trimmed().isEmpty()) {
        return QDir::cleanPath(QDir(projectRoots.first()).filePath(trimmed));
    }
    return trimmed;
}

QString elideDisplayPath(const QString& path, int maxChars = 56) {
    if (path.size() <= maxChars || maxChars < 12) {
        return path;
    }
    const int head = maxChars / 3;
    const int tail = maxChars - head - 1;
    return path.left(head) + QStringLiteral("…") + path.right(tail);
}

QString formatTimestamp(qint64 rawTimestamp) {
    if (rawTimestamp <= 0) {
        return {};
    }
    const QDateTime dt = (rawTimestamp > 10'000'000'000LL)
                             ? QDateTime::fromMSecsSinceEpoch(rawTimestamp)
                             : QDateTime::fromSecsSinceEpoch(rawTimestamp);
    if (!dt.isValid()) {
        return {};
    }
    if (dt.date() == QDate::currentDate()) {
        return dt.toString(QStringLiteral("HH:mm"));
    }
    return dt.toString(QStringLiteral("MMM d, HH:mm"));
}

QString formatLatency(qint64 latencyMs) {
    if (latencyMs <= 0) {
        return {};
    }
    if (latencyMs < 1000) {
        return QStringLiteral("%1 ms").arg(latencyMs);
    }
    if (latencyMs < 60'000) {
        return QStringLiteral("%1 s").arg(static_cast<double>(latencyMs) / 1000.0, 0, 'f', 2);
    }
    const qint64 mins = latencyMs / 60'000;
    const qint64 secs = (latencyMs % 60'000) / 1000;
    return QStringLiteral("%1m %2s").arg(mins).arg(secs);
}

QString normalizeCardKind(const UiChatEvent& event) {
    return detail::normalizeEventKind(event.kind, {});
}

bool eventsShareIdentity(const UiChatEvent& a, const UiChatEvent& b) {
    if (normalizeCardKind(a) != normalizeCardKind(b)) {
        return false;
    }
    if (!a.id.isEmpty() || !b.id.isEmpty()) {
        return a.id == b.id;
    }
    return true;
}

QString iconNameForTool(const QString& toolName) {
    const QString lower = toolName.trimmed().toLower();
    if (lower == QLatin1StringView("delegate_coding_subagent") ||
        lower.contains(QLatin1StringView("subagent"))) {
        return QStringLiteral("cpu");
    }
    if (lower == QLatin1StringView("fetch") ||
        lower.contains(QLatin1StringView("web")) ||
        lower.contains(QLatin1StringView("http"))) {
        return QStringLiteral("globe");
    }
    if (lower.contains(QLatin1StringView("terminal")) ||
        lower.contains(QLatin1StringView("shell")) ||
        lower.contains(QLatin1StringView("exec"))) {
        return QStringLiteral("terminal");
    }
    if (lower.contains(QLatin1StringView("search")) ||
        lower.contains(QLatin1StringView("grep")) ||
        lower.contains(QLatin1StringView("find"))) {
        return QStringLiteral("search");
    }
    if (lower.contains(QLatin1StringView("file")) ||
        lower.contains(QLatin1StringView("read")) ||
        lower.contains(QLatin1StringView("write")) ||
        lower.contains(QLatin1StringView("edit"))) {
        return QStringLiteral("file");
    }
    return QStringLiteral("code");
}

struct ExtractedToolMeta {
    QString filePath;
    int line = 0;
    bool isMcp = false;
    QString status;
    QString errorKind;
};

ExtractedToolMeta extractToolMeta(const UiChatEvent& ev) {
    ExtractedToolMeta out;
    out.filePath = ev.filePath.trimmed();
    out.status = ev.diffStatus.trimmed();

    const QJsonObject toolObj = detail::getObject(ev.raw, {"tool_call", "toolCall"});
    out.isMcp = detail::getBool(toolObj, {"is_mcp", "isMCP", "mcp"}, false) ||
                detail::getBool(ev.raw, {"is_mcp", "isMCP", "mcp"}, false);
    out.errorKind = detail::getString(toolObj, {"error_kind", "errorKind"});
    if (out.errorKind.isEmpty()) {
        out.errorKind = detail::getString(ev.raw, {"error_kind", "errorKind"});
    }
    if (out.status.isEmpty()) {
        out.status = detail::getString(toolObj, {"status", "tool_status", "toolStatus"});
    }
    if (out.status.isEmpty()) {
        out.status = detail::getString(ev.raw, {"status", "tool_status", "toolStatus"});
    }

    if (!ev.toolArgsJson.trimmed().isEmpty()) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(ev.toolArgsJson.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject args = doc.object();
            if (out.filePath.isEmpty()) {
                out.filePath = detail::getString(
                    args, {"path", "file_path", "filePath", "target_file", "targetFile", "filename"}).trimmed();
            }
            out.line = detail::getInt(args, {"start_line", "startLine", "line"}, 0);
        }
    }
    return out;
}

QString prettyJsonString(const QString& rawJson) {
    const QString trimmed = rawJson.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
    if (err.error == QJsonParseError::NoError && (doc.isObject() || doc.isArray())) {
        return QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed();
    }
    return trimmed;
}

void detachAndDeleteWidget(QLayout* layout, QWidget* widget) {
    if (!widget) {
        return;
    }
    widget->hide();
    if (layout) {
        layout->removeWidget(widget);
    }
    widget->setParent(nullptr);
    widget->deleteLater();
}

void updateTextBrowserHeight(QTextBrowser* browser, int minPx, int maxPx) {
    if (!browser) {
        return;
    }
    QTextDocument* doc = browser->document();
    if (!doc) {
        return;
    }
    const int availWidth = std::max(120, browser->viewport()->width());
    doc->setTextWidth(static_cast<qreal>(availWidth));
    const qreal docHeight = doc->size().height();
    const int frameExtra = browser->frameWidth() * 2 + 8;
    const int target = std::clamp(static_cast<int>(std::ceil(docHeight)) + frameExtra, minPx, maxPx);
    if (browser->minimumHeight() != target || browser->maximumHeight() != target) {
        browser->setFixedHeight(target);
    }
}

void openHrefOrCitation(const QUrl& url,
                        const std::function<void(const QString&, int)>& onCitation) {
    QString path;
    int line = 0;
    if (parseSpellFileUrl(url, &path, &line)) {
        if (onCitation) {
            onCitation(path, line);
        }
        return;
    }
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1StringView("http") || scheme == QLatin1StringView("https")) {
        QDesktopServices::openUrl(url);
    }
}

StagedDiff resolveStagedDiffForEvent(const UiChatEvent& event, const QVector<StagedDiff>& diffs) {
    StagedDiff result;
    result.id = !event.diffId.isEmpty() ? event.diffId : event.id;
    result.filePath = event.filePath;
    result.unifiedDiff = event.diffText;
    result.status = !event.diffStatus.isEmpty() ? event.diffStatus : QStringLiteral("staged");
    result.summary = event.text;

    for (const StagedDiff& live : diffs) {
        const bool idMatch = !result.id.isEmpty() && !live.id.isEmpty() && live.id == result.id;
        const bool pathMatch = !result.filePath.isEmpty() && !live.filePath.isEmpty() &&
                               QDir::cleanPath(live.filePath) == QDir::cleanPath(result.filePath);
        if (idMatch || pathMatch) {
            if (!live.id.isEmpty()) {
                result.id = live.id;
            }
            if (!live.threadId.isEmpty()) {
                result.threadId = live.threadId;
            }
            if (!live.filePath.isEmpty()) {
                result.filePath = live.filePath;
            }
            if (!live.unifiedDiff.isEmpty()) {
                result.unifiedDiff = live.unifiedDiff;
            }
            if (!live.status.isEmpty()) {
                result.status = live.status;
            }
            if (live.linesAdded > 0 || live.linesRemoved > 0) {
                result.linesAdded = live.linesAdded;
                result.linesRemoved = live.linesRemoved;
            }
            if (!live.summary.isEmpty()) {
                result.summary = live.summary;
            }
            break;
        }
    }

    if (result.linesAdded == 0 && result.linesRemoved == 0 && !result.unifiedDiff.isEmpty()) {
        SpellDiffWidget::computeLineCounts(result.unifiedDiff, &result.linesAdded, &result.linesRemoved);
    }
    return result;
}

// ============================================================================
// Shared Delegation Strip (used by DelegationEventCard and ToolEventCard)
// ============================================================================

struct DelegationPanel {
    struct FileRowWidgets {
        QWidget* row = nullptr;
        QPushButton* pathButton = nullptr;
        QLabel* addedLabel = nullptr;
        QLabel* removedLabel = nullptr;
        SpellBadge* criticBadge = nullptr;
        QString rawPath;
    };

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    QFrame* frame = nullptr;
    QLabel* titleIconLabel = nullptr;
    QLabel* titleLabel = nullptr;
    SpellBadge* statusBadge = nullptr;
    SpellBadge* protocolBadge = nullptr;
    SpellBadge* fallbackBadge = nullptr;
    QLabel* totalAddedLabel = nullptr;
    QLabel* totalRemovedLabel = nullptr;
    QLabel* summaryLabel = nullptr;
    QWidget* escalationRow = nullptr;
    QLabel* escalationIconLabel = nullptr;
    QLabel* escalationLabel = nullptr;
    SpellHairline* filesDivider = nullptr;
    QWidget* filesContainer = nullptr;
    QVBoxLayout* filesLayout = nullptr;
    QVector<FileRowWidgets> fileRows;

    bool standalone = false;
    bool hasRendered = false;
    DelegationMeta lastMeta;
    QString lastSummary;
    QStringList projectRoots;
    std::function<void(const QString&, int)> onOpenFile;

    DelegationPanel(QWidget* parent,
                    bool isStandalone,
                    std::function<void(const QString&, int)> openFileCb)
        : standalone(isStandalone)
        , onOpenFile(std::move(openFileCb)) {
        frame = new QFrame(parent);
        frame->setObjectName(QStringLiteral("spellDelegationStrip"));

        auto* rootLayout = new QVBoxLayout(frame);
        rootLayout->setContentsMargins(10, 8, 10, 8);
        rootLayout->setSpacing(6);

        auto* headerRow = new QHBoxLayout();
        headerRow->setContentsMargins(0, 0, 0, 0);
        headerRow->setSpacing(6);

        if (standalone) {
            titleIconLabel = new QLabel(frame);
            titleIconLabel->setFixedSize(14, 14);
            headerRow->addWidget(titleIconLabel);

            titleLabel = new QLabel(QStringLiteral("Subagent Delegation"), frame);
            titleLabel->setFont(makeUiFont(11.5, QFont::DemiBold));
            headerRow->addWidget(titleLabel);
        }

        statusBadge = new SpellBadge(QStringLiteral("COMPLETED"), BadgeTone::Info, frame);
        headerRow->addWidget(statusBadge);

        protocolBadge = new SpellBadge(QStringLiteral("NATIVE · 0 steps"), BadgeTone::Info, frame);
        headerRow->addWidget(protocolBadge);

        fallbackBadge = new SpellBadge(QStringLiteral("JSON FALLBACK"), BadgeTone::Warning, frame);
        fallbackBadge->hide();
        headerRow->addWidget(fallbackBadge);

        headerRow->addStretch(1);

        totalAddedLabel = new QLabel(frame);
        totalAddedLabel->setFont(makeMonoFont(10.0, QFont::DemiBold));
        totalAddedLabel->hide();
        headerRow->addWidget(totalAddedLabel);

        totalRemovedLabel = new QLabel(frame);
        totalRemovedLabel->setFont(makeMonoFont(10.0, QFont::DemiBold));
        totalRemovedLabel->hide();
        headerRow->addWidget(totalRemovedLabel);

        rootLayout->addLayout(headerRow);

        summaryLabel = new QLabel(frame);
        summaryLabel->setFont(makeUiFont(11.0));
        summaryLabel->setWordWrap(true);
        summaryLabel->setTextFormat(Qt::PlainText);
        summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        summaryLabel->hide();
        rootLayout->addWidget(summaryLabel);

        escalationRow = new QWidget(frame);
        auto* escLayout = new QHBoxLayout(escalationRow);
        escLayout->setContentsMargins(0, 0, 0, 0);
        escLayout->setSpacing(6);

        escalationIconLabel = new QLabel(escalationRow);
        escalationIconLabel->setFixedSize(12, 12);
        escLayout->addWidget(escalationIconLabel, 0, Qt::AlignTop);

        escalationLabel = new QLabel(escalationRow);
        escalationLabel->setFont(makeUiFont(10.5, QFont::Medium));
        escalationLabel->setWordWrap(true);
        escalationLabel->setTextFormat(Qt::PlainText);
        escalationLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        escLayout->addWidget(escalationLabel, 1);

        escalationRow->hide();
        rootLayout->addWidget(escalationRow);

        filesDivider = new SpellHairline(Qt::Horizontal, frame);
        filesDivider->hide();
        rootLayout->addWidget(filesDivider);

        filesContainer = new QWidget(frame);
        filesLayout = new QVBoxLayout(filesContainer);
        filesLayout->setContentsMargins(0, 0, 0, 0);
        filesLayout->setSpacing(4);
        filesContainer->hide();
        rootLayout->addWidget(filesContainer);

        applyTheme();
    }

    ~DelegationPanel() {
        *alive = false;
    }

    void update(const DelegationMeta& meta,
                const QString& fallbackSummary,
                const QStringList& roots) {
        projectRoots = roots;
        const QString effectiveSummary = !meta.summary.trimmed().isEmpty()
                                             ? meta.summary.trimmed()
                                             : fallbackSummary.trimmed();

        if (hasRendered && meta == lastMeta && effectiveSummary == lastSummary) {
            return;
        }
        const bool filesChanged = !hasRendered || (meta.files != lastMeta.files);
        lastMeta = meta;
        lastSummary = effectiveSummary;
        hasRendered = true;

        if (meta.isCommitted()) {
            statusBadge->setText(QStringLiteral("COMMITTED"));
            statusBadge->setTone(BadgeTone::Success);
        } else if (meta.isEscalated()) {
            statusBadge->setText(QStringLiteral("ESCALATED"));
            statusBadge->setTone(BadgeTone::Warning);
        } else {
            const QString st = meta.status.trimmed();
            statusBadge->setText(st.isEmpty() ? QStringLiteral("COMPLETED") : st.toUpper());
            statusBadge->setTone(BadgeTone::Info);
        }

        const QString proto = meta.protocolUsed.trimmed().isEmpty()
                                  ? QStringLiteral("NATIVE")
                                  : meta.protocolUsed.trimmed().toUpper();
        QString stepText;
        if (meta.maxSteps > 0) {
            stepText = QStringLiteral("%1 · %2/%3 %4")
                           .arg(proto)
                           .arg(meta.steps)
                           .arg(meta.maxSteps)
                           .arg(meta.steps == 1 ? QStringLiteral("step") : QStringLiteral("steps"));
        } else {
            stepText = QStringLiteral("%1 · %2 %3")
                           .arg(proto)
                           .arg(meta.steps)
                           .arg(meta.steps == 1 ? QStringLiteral("step") : QStringLiteral("steps"));
        }
        protocolBadge->setText(stepText);
        fallbackBadge->setVisible(meta.fallbackTriggered);

        if (!meta.files.isEmpty()) {
            totalAddedLabel->setText(QStringLiteral("+%1").arg(meta.totalAdded()));
            totalRemovedLabel->setText(QStringLiteral("−%1").arg(meta.totalRemoved()));
            totalAddedLabel->show();
            totalRemovedLabel->show();
        } else {
            totalAddedLabel->hide();
            totalRemovedLabel->hide();
        }

        if (!effectiveSummary.isEmpty()) {
            summaryLabel->setText(effectiveSummary);
            summaryLabel->show();
        } else {
            summaryLabel->hide();
        }

        const QString reason = meta.escalationReason.trimmed();
        if (!reason.isEmpty()) {
            QString readable = reason;
            readable.replace(QLatin1Char('_'), QLatin1Char(' '));
            escalationLabel->setText(
                QStringLiteral("Escalated to parent (%1) — no files auto-committed").arg(readable));
            escalationRow->show();
        } else {
            escalationRow->hide();
        }

        if (filesChanged) {
            rebuildFileRows();
        }
        applyTheme();
    }

    void rebuildFileRows() {
        for (const FileRowWidgets& fr : fileRows) {
            detachAndDeleteWidget(filesLayout, fr.row);
        }
        fileRows.clear();

        if (lastMeta.files.isEmpty()) {
            filesDivider->hide();
            filesContainer->hide();
            return;
        }

        filesDivider->show();
        filesContainer->show();

        for (const DelegationFileSummary& file : lastMeta.files) {
            FileRowWidgets fr;
            fr.rawPath = file.path;
            fr.row = new QWidget(filesContainer);
            auto* rowLayout = new QHBoxLayout(fr.row);
            rowLayout->setContentsMargins(0, 1, 0, 1);
            rowLayout->setSpacing(6);

            fr.pathButton = new QPushButton(elideDisplayPath(file.path, 52), fr.row);
            fr.pathButton->setFlat(true);
            fr.pathButton->setCursor(Qt::PointingHandCursor);
            fr.pathButton->setFont(makeMonoFont(10.5, QFont::Medium));
            fr.pathButton->setToolTip(QStringLiteral("Open %1").arg(file.path));
            fr.pathButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            const QString rawPathCopy = file.path;
            QObject::connect(fr.pathButton, &QPushButton::clicked, fr.pathButton,
                             [alive = alive, this, rawPathCopy]() {
                                 if (!*alive || !onOpenFile) {
                                     return;
                                 }
                                 onOpenFile(resolveWorkspacePath(rawPathCopy, projectRoots), 0);
                             });
            rowLayout->addWidget(fr.pathButton, 0, Qt::AlignLeft);

            rowLayout->addStretch(1);

            fr.addedLabel = new QLabel(QStringLiteral("+%1").arg(file.added), fr.row);
            fr.addedLabel->setFont(makeMonoFont(10.0, QFont::DemiBold));
            rowLayout->addWidget(fr.addedLabel);

            fr.removedLabel = new QLabel(QStringLiteral("−%1").arg(file.removed), fr.row);
            fr.removedLabel->setFont(makeMonoFont(10.0, QFont::DemiBold));
            rowLayout->addWidget(fr.removedLabel);

            const QString criticText = file.criticPassed
                                           ? QStringLiteral("Critic PASS")
                                           : QStringLiteral("Critic REJECT (%1)").arg(file.bounces);
            fr.criticBadge = new SpellBadge(
                criticText, file.criticPassed ? BadgeTone::Success : BadgeTone::Warning, fr.row);
            rowLayout->addWidget(fr.criticBadge);

            filesLayout->addWidget(fr.row);
            fileRows.append(fr);
        }
    }

    void applyTheme() {
        const UiPalette u = ThemeManager::instance().ui();
        const QColor bg = compositeOver(u.elevatedSurface, u.background);
        const QColor border = compositeOver(u.subtleBorder, u.background);

        frame->setStyleSheet(QStringLiteral(
            "QFrame#spellDelegationStrip {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 7px;"
            "}")
                                 .arg(qssColor(bg), qssColor(border)));

        if (titleIconLabel) {
            titleIconLabel->setPixmap(icons::vectorIcon(QStringLiteral("cpu"), u.textSecondary, 13).pixmap(13, 13));
            titleIconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        }
        if (titleLabel) {
            titleLabel->setStyleSheet(QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.textPrimary)));
        }
        statusBadge->applyTheme();
        protocolBadge->applyTheme();
        fallbackBadge->applyTheme();
        filesDivider->applyTheme();

        totalAddedLabel->setStyleSheet(QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.diffAdded)));
        totalRemovedLabel->setStyleSheet(QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.diffRemoved)));
        summaryLabel->setStyleSheet(QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.textSecondary)));

        escalationRow->setStyleSheet(QStringLiteral("background: transparent;"));
        escalationIconLabel->setPixmap(
            icons::vectorIcon(QStringLiteral("warning"), u.warning, 12).pixmap(12, 12));
        escalationIconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        escalationLabel->setStyleSheet(QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.textPrimary)));

        filesContainer->setStyleSheet(QStringLiteral("background: transparent;"));
        const QString fileLinkStyle = QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  border: none;"
            "  padding: 0px;"
            "  color: %1;"
            "  text-decoration: underline;"
            "  text-align: left;"
            "}"
            "QPushButton:hover {"
            "  color: %2;"
            "}")
                                          .arg(qssColor(u.textSecondary), qssColor(u.textPrimary));

        for (const FileRowWidgets& fr : fileRows) {
            fr.row->setStyleSheet(QStringLiteral("background: transparent;"));
            fr.pathButton->setStyleSheet(fileLinkStyle);
            fr.addedLabel->setStyleSheet(
                QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.diffAdded)));
            fr.removedLabel->setStyleSheet(
                QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.diffRemoved)));
            fr.criticBadge->applyTheme();
        }
    }
};

// ============================================================================
// Base EventCard Interface
// ============================================================================

struct EventCard {
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    QPointer<QWidget> root;
    QString kind;
    UiChatEvent lastEvent;
    bool initialized = false;

    virtual ~EventCard() {
        *alive = false;
    }

    virtual void update(const UiChatEvent& event,
                        const QVector<StagedDiff>& diffs,
                        const QStringList& projectRoots,
                        int userTurnOrdinal,
                        bool rootsChanged) = 0;
    virtual void applyTheme() = 0;
    virtual void onViewportResized(int /*viewportWidth*/) {}
};

// ============================================================================
// 1. User Event Card (Monochrome Bubble + Inline Edit & Resend)
// ============================================================================

struct UserEventCard : EventCard {
    QPointer<QObject> filterObj;
    QFrame* bubble = nullptr;
    QLabel* textLabel = nullptr;

    QWidget* editContainer = nullptr;
    QPointer<QPlainTextEdit> editor;
    QPushButton* cancelBtn = nullptr;
    QPushButton* saveBtn = nullptr;

    QWidget* actionsRow = nullptr;
    QPushButton* editBtn = nullptr;
    QPushButton* resendBtn = nullptr;

    bool isEditing = false;
    bool isHovered = false;
    int effectiveTurnIndex = 0;
    int lastViewportWidth = 520;

    std::function<void(int, const QString&)> onEditAndResend;
    std::function<void(UserEventCard*)> onDestroyed;

    UserEventCard(QWidget* parent,
                  QObject* eventFilterObj,
                  std::function<void(int, const QString&)> editCb,
                  std::function<void(UserEventCard*)> destroyCb)
        : filterObj(eventFilterObj)
        , onEditAndResend(std::move(editCb))
        , onDestroyed(std::move(destroyCb)) {
        kind = QStringLiteral("user");
        root = new QWidget(parent);
        root->setStyleSheet(QStringLiteral("background: transparent;"));
        root->setAttribute(Qt::WA_Hover, true);
        if (filterObj) {
            root->installEventFilter(filterObj);
        }

        auto* outerRow = new QHBoxLayout(root);
        outerRow->setContentsMargins(36, 0, 0, 0);
        outerRow->setSpacing(0);
        outerRow->addStretch(1);

        auto* rightCol = new QVBoxLayout();
        rightCol->setContentsMargins(0, 0, 0, 0);
        rightCol->setSpacing(4);

        // View-mode monochrome bubble
        bubble = new QFrame(root);
        bubble->setObjectName(QStringLiteral("spellUserBubble"));
        auto* bubbleLayout = new QVBoxLayout(bubble);
        bubbleLayout->setContentsMargins(13, 9, 13, 9);
        bubbleLayout->setSpacing(0);

        textLabel = new QLabel(bubble);
        textLabel->setFont(makeUiFont(12.5));
        textLabel->setWordWrap(true);
        textLabel->setTextFormat(Qt::PlainText);
        textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        textLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        bubbleLayout->addWidget(textLabel);
        rightCol->addWidget(bubble, 0, Qt::AlignRight);

        // Inline edit container
        editContainer = new QWidget(root);
        editContainer->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* editLayout = new QVBoxLayout(editContainer);
        editLayout->setContentsMargins(0, 0, 0, 0);
        editLayout->setSpacing(6);

        editor = new QPlainTextEdit(editContainer);
        editor->setFont(makeUiFont(12.5));
        editor->setPlaceholderText(QStringLiteral("Edit prompt… (Ctrl+Enter to resend)"));
        editor->setTabChangesFocus(false);
        editor->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        if (filterObj) {
            editor->installEventFilter(filterObj);
        }
        editLayout->addWidget(editor);

        auto* editBtnRow = new QHBoxLayout();
        editBtnRow->setContentsMargins(0, 0, 0, 0);
        editBtnRow->setSpacing(6);
        editBtnRow->addStretch(1);

        cancelBtn = new QPushButton(QStringLiteral("Cancel"), editContainer);
        cancelBtn->setCursor(Qt::PointingHandCursor);
        cancelBtn->setFont(makeUiFont(11.0, QFont::Medium));
        editBtnRow->addWidget(cancelBtn);

        saveBtn = new QPushButton(QStringLiteral("Save & Resend"), editContainer);
        saveBtn->setProperty("primary", true);
        saveBtn->setCursor(Qt::PointingHandCursor);
        saveBtn->setFont(makeUiFont(11.0, QFont::DemiBold));
        editBtnRow->addWidget(saveBtn);

        editLayout->addLayout(editBtnRow);
        editContainer->hide();
        rightCol->addWidget(editContainer, 0, Qt::AlignRight);

        // Action row (Edit / Resend)
        actionsRow = new QWidget(root);
        actionsRow->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* actionsLayout = new QHBoxLayout(actionsRow);
        actionsLayout->setContentsMargins(0, 0, 2, 0);
        actionsLayout->setSpacing(8);
        actionsLayout->addStretch(1);

        editBtn = new QPushButton(QStringLiteral("Edit"), actionsRow);
        editBtn->setFlat(true);
        editBtn->setCursor(Qt::PointingHandCursor);
        editBtn->setFont(makeUiFont(10.0, QFont::Medium));
        editBtn->setToolTip(QStringLiteral("Edit prompt and resend from this turn"));
        actionsLayout->addWidget(editBtn);

        resendBtn = new QPushButton(QStringLiteral("Resend"), actionsRow);
        resendBtn->setFlat(true);
        resendBtn->setCursor(Qt::PointingHandCursor);
        resendBtn->setFont(makeUiFont(10.0, QFont::Medium));
        resendBtn->setToolTip(QStringLiteral("Regenerate response from this prompt"));
        actionsLayout->addWidget(resendBtn);

        rightCol->addWidget(actionsRow, 0, Qt::AlignRight);
        outerRow->addLayout(rightCol, 0);

        QObject::connect(editBtn, &QPushButton::clicked, editBtn, [alive = alive, this]() {
            if (*alive) {
                setEditing(!isEditing);
            }
        });
        QObject::connect(cancelBtn, &QPushButton::clicked, cancelBtn, [alive = alive, this]() {
            if (*alive) {
                setEditing(false);
            }
        });
        QObject::connect(saveBtn, &QPushButton::clicked, saveBtn, [alive = alive, this]() {
            if (*alive) {
                submitEdit();
            }
        });
        QObject::connect(resendBtn, &QPushButton::clicked, resendBtn, [alive = alive, this]() {
            if (!*alive) {
                return;
            }
            const QString prompt = lastEvent.text.trimmed();
            if (!prompt.isEmpty() && onEditAndResend) {
                onEditAndResend(effectiveTurnIndex, prompt);
            }
        });
        QObject::connect(editor, &QPlainTextEdit::textChanged, editor, [alive = alive, this]() {
            if (!*alive) {
                return;
            }
            adjustEditorHeight();
            saveBtn->setEnabled(!editor->toPlainText().trimmed().isEmpty());
        });

        applyTheme();
    }

    ~UserEventCard() override {
        *alive = false;
        if (filterObj) {
            if (root) {
                root->removeEventFilter(filterObj);
            }
            if (editor) {
                editor->removeEventFilter(filterObj);
            }
        }
        if (onDestroyed) {
            onDestroyed(this);
        }
    }

    void updateResponsiveWidths(int viewportWidth) {
        if (viewportWidth > 0) {
            lastViewportWidth = viewportWidth;
        }
        const int avail = std::max(180, lastViewportWidth - 84);
        bubble->setMaximumWidth(avail);
        const int editWidth = std::clamp(avail, 180, 580);
        editContainer->setMinimumWidth(editWidth);
        editContainer->setMaximumWidth(editWidth);
    }

    void adjustEditorHeight() {
        if (!editor) {
            return;
        }
        const QFontMetrics fm(editor->font());
        const int lineCount = std::max(2, editor->document()->blockCount());
        const int desired = lineCount * fm.lineSpacing() + 24;
        editor->setFixedHeight(std::clamp(desired, kMinUserEditHeightPx, kMaxUserEditHeightPx));
    }

    void setEditing(bool editing) {
        if (isEditing == editing) {
            return;
        }
        isEditing = editing;
        if (isEditing) {
            updateResponsiveWidths(lastViewportWidth);
            editor->setPlainText(lastEvent.text);
            adjustEditorHeight();
            saveBtn->setEnabled(!lastEvent.text.trimmed().isEmpty());
            bubble->hide();
            editContainer->show();
            editBtn->setText(QStringLiteral("Close"));
            editor->setFocus();
            editor->moveCursor(QTextCursor::End);
        } else {
            editContainer->hide();
            bubble->show();
            editBtn->setText(QStringLiteral("Edit"));
        }
        updateActionStyle();
    }

    void submitEdit() {
        if (!editor) {
            return;
        }
        const QString trimmed = editor->toPlainText().trimmed();
        if (trimmed.isEmpty()) {
            return;
        }
        setEditing(false);
        if (onEditAndResend) {
            onEditAndResend(effectiveTurnIndex, trimmed);
        }
    }

    void setHovered(bool hovered) {
        if (isHovered == hovered) {
            return;
        }
        isHovered = hovered;
        updateActionStyle();
    }

    void updateActionStyle() {
        const UiPalette u = ThemeManager::instance().ui();
        const QColor fg = (isHovered || isEditing) ? u.textSecondary : withAlpha(u.textMuted, 0.72);
        const QString style = QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  border: none;"
            "  padding: 2px 4px;"
            "  color: %1;"
            "}"
            "QPushButton:hover {"
            "  color: %2;"
            "}")
                                  .arg(qssColor(fg), qssColor(u.textPrimary));
        editBtn->setStyleSheet(style);
        resendBtn->setStyleSheet(style);
        editBtn->setIcon(icons::vectorIcon(QStringLiteral("edit"), fg, 11));
        editBtn->setIconSize(QSize(11, 11));
        resendBtn->setIcon(icons::vectorIcon(QStringLiteral("repeat"), fg, 11));
        resendBtn->setIconSize(QSize(11, 11));
    }

    void update(const UiChatEvent& event,
                const QVector<StagedDiff>& /*diffs*/,
                const QStringList& /*projectRoots*/,
                int userTurnOrdinal,
                bool /*rootsChanged*/) override {
        effectiveTurnIndex = (event.turnIndex >= 0) ? event.turnIndex : userTurnOrdinal;
        if (initialized && event.text == lastEvent.text) {
            lastEvent = event;
            return;
        }
        lastEvent = event;
        initialized = true;
        textLabel->setText(event.text);
    }

    void onViewportResized(int viewportWidth) override {
        updateResponsiveWidths(viewportWidth);
    }

    void applyTheme() override {
        const UiPalette u = ThemeManager::instance().ui();
        const QColor fill = compositeOver(u.selectedFill, u.background);
        const QColor border = compositeOver(u.subtleBorder, u.background);
        const QColor focusBorder = compositeOver(u.selectedBorder, u.background);

        bubble->setStyleSheet(QStringLiteral(
            "QFrame#spellUserBubble {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 11px;"
            "}")
                                  .arg(qssColor(fill), qssColor(border)));
        textLabel->setStyleSheet(
            QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.textPrimary)));

        editor->setStyleSheet(QStringLiteral(
            "QPlainTextEdit {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 8px;"
            "  padding: 6px 9px;"
            "}")
                                  .arg(qssColor(u.surface), qssColor(u.textPrimary), qssColor(focusBorder)));

        saveBtn->setIcon(icons::vectorIcon(QStringLiteral("refresh"), u.textPrimary, 12));
        saveBtn->setIconSize(QSize(12, 12));
        updateActionStyle();
    }
};

// ============================================================================
// 2. Tool Call / Tool Result Event Card (Collapsible Monospaced Card)
// ============================================================================

struct ToolEventCard : EventCard {
    QPointer<QObject> filterObj;
    QFrame* cardFrame = nullptr;
    QPointer<QWidget> headerWidget;
    QToolButton* toggleBtn = nullptr;
    QLabel* iconLabel = nullptr;
    QLabel* nameLabel = nullptr;
    SpellBadge* resultKindBadge = nullptr;
    QPushButton* fileLinkBtn = nullptr;
    SpellBadge* mcpBadge = nullptr;
    SpellBadge* errorKindBadge = nullptr;
    SpellBadge* statusBadge = nullptr;
    QLabel* okCheckLabel = nullptr;

    std::unique_ptr<DelegationPanel> delegationPanel;

    QFrame* bodyFrame = nullptr;
    QTextBrowser* bodyBrowser = nullptr;

    bool expanded = false;
    bool bodyDirty = true;
    QString currentFilePath;
    int currentFileLine = 0;
    QStringList currentProjectRoots;
    ExtractedToolMeta lastMeta;
    QString lastStyledStatus;
    QString lastStyledToolName;

    std::function<void(const QString&, int)> onOpenFile;
    std::function<void(ToolEventCard*)> onDestroyed;

    ToolEventCard(QWidget* parent,
                  QObject* eventFilterObj,
                  const QString& cardKind,
                  std::function<void(const QString&, int)> openFileCb,
                  std::function<void(ToolEventCard*)> destroyCb)
        : filterObj(eventFilterObj)
        , onOpenFile(std::move(openFileCb))
        , onDestroyed(std::move(destroyCb)) {
        kind = cardKind;
        cardFrame = new QFrame(parent);
        cardFrame->setObjectName(QStringLiteral("spellToolCard"));
        root = cardFrame;

        auto* rootLayout = new QVBoxLayout(cardFrame);
        rootLayout->setContentsMargins(10, 7, 10, 7);
        rootLayout->setSpacing(6);

        headerWidget = new QWidget(cardFrame);
        headerWidget->setCursor(Qt::PointingHandCursor);
        headerWidget->setStyleSheet(QStringLiteral("background: transparent;"));
        if (filterObj) {
            headerWidget->installEventFilter(filterObj);
        }

        auto* headerLayout = new QHBoxLayout(headerWidget);
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->setSpacing(6);

        toggleBtn = new QToolButton(headerWidget);
        toggleBtn->setAutoRaise(true);
        toggleBtn->setFixedSize(16, 16);
        toggleBtn->setCursor(Qt::PointingHandCursor);
        toggleBtn->setToolTip(QStringLiteral("Expand / collapse tool details"));
        headerLayout->addWidget(toggleBtn);

        iconLabel = new QLabel(headerWidget);
        iconLabel->setFixedSize(13, 13);
        headerLayout->addWidget(iconLabel);

        nameLabel = new QLabel(headerWidget);
        nameLabel->setFont(makeMonoFont(11.0, QFont::DemiBold));
        headerLayout->addWidget(nameLabel);

        resultKindBadge = new SpellBadge(QStringLiteral("RESULT"), BadgeTone::Neutral, headerWidget);
        resultKindBadge->hide();
        headerLayout->addWidget(resultKindBadge);

        fileLinkBtn = new QPushButton(headerWidget);
        fileLinkBtn->setFlat(true);
        fileLinkBtn->setCursor(Qt::PointingHandCursor);
        fileLinkBtn->setFont(makeMonoFont(10.5));
        fileLinkBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        fileLinkBtn->hide();
        headerLayout->addWidget(fileLinkBtn);

        mcpBadge = new SpellBadge(QStringLiteral("MCP"), BadgeTone::Neutral, headerWidget);
        mcpBadge->hide();
        headerLayout->addWidget(mcpBadge);

        headerLayout->addStretch(1);

        errorKindBadge = new SpellBadge(QString(), BadgeTone::Danger, headerWidget);
        errorKindBadge->hide();
        headerLayout->addWidget(errorKindBadge);

        statusBadge = new SpellBadge(QString(), BadgeTone::Neutral, headerWidget);
        statusBadge->hide();
        headerLayout->addWidget(statusBadge);

        okCheckLabel = new QLabel(headerWidget);
        okCheckLabel->setFixedSize(13, 13);
        okCheckLabel->setToolTip(QStringLiteral("Completed"));
        okCheckLabel->hide();
        headerLayout->addWidget(okCheckLabel);

        rootLayout->addWidget(headerWidget);

        // Collapsible monospaced body
        bodyFrame = new QFrame(cardFrame);
        bodyFrame->setObjectName(QStringLiteral("spellToolBody"));
        auto* bodyLayout = new QVBoxLayout(bodyFrame);
        bodyLayout->setContentsMargins(8, 7, 8, 7);
        bodyLayout->setSpacing(0);

        bodyBrowser = new QTextBrowser(bodyFrame);
        bodyBrowser->setFrameShape(QFrame::NoFrame);
        bodyBrowser->setOpenLinks(false);
        bodyBrowser->setOpenExternalLinks(false);
        bodyBrowser->setLineWrapMode(QTextEdit::WidgetWidth);
        bodyBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        bodyBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        bodyBrowser->setTextInteractionFlags(Qt::TextBrowserInteraction);
        bodyBrowser->setFont(makeMonoFont(10.5));
        bodyLayout->addWidget(bodyBrowser);

        bodyFrame->hide();
        rootLayout->addWidget(bodyFrame);

        QObject::connect(toggleBtn, &QToolButton::clicked, toggleBtn, [alive = alive, this]() {
            if (*alive) {
                toggleExpanded();
            }
        });
        QObject::connect(fileLinkBtn, &QPushButton::clicked, fileLinkBtn, [alive = alive, this]() {
            if (*alive && !currentFilePath.isEmpty() && onOpenFile) {
                onOpenFile(resolveWorkspacePath(currentFilePath, currentProjectRoots), currentFileLine);
            }
        });
        QObject::connect(bodyBrowser, &QTextBrowser::anchorClicked, bodyBrowser,
                         [alive = alive, this](const QUrl& url) {
                             if (*alive) {
                                 openHrefOrCitation(url, onOpenFile);
                             }
                         });

        applyTheme();
    }

    ~ToolEventCard() override {
        *alive = false;
        if (filterObj && headerWidget) {
            headerWidget->removeEventFilter(filterObj);
        }
        if (onDestroyed) {
            onDestroyed(this);
        }
    }

    void toggleExpanded() {
        expanded = !expanded;
        bodyFrame->setVisible(expanded);
        refreshChevron();
        if (expanded && bodyDirty) {
            renderBody();
        }
        if (expanded) {
            updateTextBrowserHeight(bodyBrowser, kMinToolBodyHeightPx, kMaxToolBodyHeightPx);
        }
    }

    void refreshChevron() {
        const UiPalette u = ThemeManager::instance().ui();
        const QString chevron = expanded ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right");
        toggleBtn->setIcon(icons::vectorIcon(chevron, u.textMuted, 11));
        toggleBtn->setIconSize(QSize(11, 11));
    }

    void refreshCardChrome() {
        const UiPalette u = ThemeManager::instance().ui();
        const QString lowerStatus = lastMeta.status.toLower();
        const bool isError = (lowerStatus == QLatin1StringView("denied") ||
                              lowerStatus == QLatin1StringView("cancelled") ||
                              lowerStatus == QLatin1StringView("failed") ||
                              lowerStatus == QLatin1StringView("error"));
        const bool isWaiting = (lowerStatus == QLatin1StringView("awaiting_confirmation"));

        const QColor borderColor = isError
                                       ? withAlpha(u.danger, 0.55)
                                       : (isWaiting ? compositeOver(u.selectedBorder, u.background)
                                                    : compositeOver(u.subtleBorder, u.background));
        const QColor cardBg = compositeOver(withAlpha(u.elevatedSurface, 0.45), u.background);

        cardFrame->setStyleSheet(QStringLiteral(
            "QFrame#spellToolCard {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 7px;"
            "}")
                                     .arg(qssColor(cardBg), qssColor(borderColor)));

        const QString toolIcon = iconNameForTool(lastEvent.toolName);
        iconLabel->setPixmap(icons::vectorIcon(toolIcon, u.textSecondary, 12).pixmap(12, 12));
        lastStyledStatus = lowerStatus;
        lastStyledToolName = lastEvent.toolName;
    }

    void renderBody() {
        bodyDirty = false;
        const UiPalette u = ThemeManager::instance().ui();
        const CodePalette c = ThemeManager::instance().code();
        const QString monoFamily = ThemeManager::instance().editorFontFamily().toHtmlEscaped();

        QString html;
        html.reserve(1024);
        html += QStringLiteral(
            "<style>"
            "a { color: %1; text-decoration: underline; }"
            "pre { margin: 0; white-space: pre-wrap; word-wrap: break-word; "
            "font-family: '%2', monospace; font-size: 10.5pt; }"
            ".section { color: %3; font-size: 9pt; font-weight: 600; "
            "letter-spacing: 0.4px; margin-bottom: 3px; }"
            "hr { border: none; border-top: 1px solid %4; margin: 8px 0; }"
            "</style>")
                    .arg(qssColor(u.textPrimary), monoFamily, qssColor(u.textMuted), qssColor(u.divider));

        bool wroteSection = false;
        const QString prettyArgs = prettyJsonString(lastEvent.toolArgsJson);
        if (!prettyArgs.isEmpty() && prettyArgs != QLatin1StringView("{}")) {
            html += QStringLiteral("<div class=\"section\">ARGUMENTS</div>");
            html += QStringLiteral("<pre style=\"color:%1;\">%2</pre>")
                        .arg(qssColor(c.plain),
                             SpellCodeBlockWidget::highlightToHtml(prettyArgs, QStringLiteral("json"), c));
            wroteSection = true;
        }

        const QString rawOutput = !lastEvent.toolOutput.isEmpty() ? lastEvent.toolOutput : lastEvent.text;
        const QString cleanOutput = DelegationMeta::stripMetadataComment(rawOutput);
        if (!cleanOutput.isEmpty()) {
            if (wroteSection) {
                html += QStringLiteral("<hr/>");
            }
            html += QStringLiteral("<div class=\"section\">OUTPUT</div>");
            const QString escaped = cleanOutput.toHtmlEscaped();
            const QString linkified = linkifyHtml(escaped, currentProjectRoots);
            html += QStringLiteral("<pre style=\"color:%1;\">%2</pre>")
                        .arg(qssColor(c.plain), linkified);
            wroteSection = true;
        }

        if (!wroteSection) {
            html += QStringLiteral("<pre style=\"color:%1;\">(no tool payload)</pre>")
                        .arg(qssColor(u.textMuted));
        }

        bodyBrowser->setHtml(html);
        updateTextBrowserHeight(bodyBrowser, kMinToolBodyHeightPx, kMaxToolBodyHeightPx);
    }

    void update(const UiChatEvent& event,
                const QVector<StagedDiff>& /*diffs*/,
                const QStringList& projectRoots,
                int /*userTurnOrdinal*/,
                bool rootsChanged) override {
        currentProjectRoots = projectRoots;
        if (initialized && event == lastEvent && !rootsChanged) {
            return;
        }

        const bool payloadChanged = !initialized ||
                                    event.toolArgsJson != lastEvent.toolArgsJson ||
                                    event.toolOutput != lastEvent.toolOutput ||
                                    event.text != lastEvent.text ||
                                    rootsChanged;
        lastEvent = event;
        initialized = true;

        const QString displayName = !event.toolName.trimmed().isEmpty()
                                        ? event.toolName.trimmed()
                                        : (kind == QLatin1StringView("tool_result")
                                               ? QStringLiteral("tool_result")
                                               : QStringLiteral("tool_call"));
        nameLabel->setText(displayName);
        resultKindBadge->setVisible(kind == QLatin1StringView("tool_result") &&
                                    !event.toolName.trimmed().isEmpty());

        lastMeta = extractToolMeta(event);
        currentFilePath = lastMeta.filePath;
        currentFileLine = lastMeta.line;

        if (!currentFilePath.isEmpty()) {
            fileLinkBtn->setText(elideDisplayPath(currentFilePath, 46));
            fileLinkBtn->setToolTip(QStringLiteral("Open %1 in editor").arg(currentFilePath));
            fileLinkBtn->show();
        } else {
            fileLinkBtn->hide();
        }

        mcpBadge->setVisible(lastMeta.isMcp);

        if (!lastMeta.errorKind.isEmpty()) {
            QString kindLabel = lastMeta.errorKind;
            kindLabel.replace(QLatin1Char('_'), QLatin1Char(' '));
            errorKindBadge->setText(kindLabel.toUpper());
            errorKindBadge->show();
        } else {
            errorKindBadge->hide();
        }

        const QString lowerStatus = lastMeta.status.toLower();
        if (lowerStatus == QLatin1StringView("denied") ||
            lowerStatus == QLatin1StringView("cancelled") ||
            lowerStatus == QLatin1StringView("failed") ||
            lowerStatus == QLatin1StringView("error")) {
            statusBadge->setText(lowerStatus.toUpper());
            statusBadge->setTone(BadgeTone::Danger);
            statusBadge->setShowDot(false);
            statusBadge->show();
            okCheckLabel->hide();
        } else if (lowerStatus == QLatin1StringView("awaiting_confirmation")) {
            statusBadge->setText(QStringLiteral("AWAITING"));
            statusBadge->setTone(BadgeTone::Warning);
            statusBadge->setShowDot(true);
            statusBadge->show();
            okCheckLabel->hide();
        } else if (event.isStreaming ||
                   lowerStatus == QLatin1StringView("running") ||
                   lowerStatus == QLatin1StringView("pending")) {
            statusBadge->setText(QStringLiteral("RUNNING"));
            statusBadge->setTone(BadgeTone::Info);
            statusBadge->setShowDot(true);
            statusBadge->show();
            okCheckLabel->hide();
        } else if (lowerStatus == QLatin1StringView("completed") ||
                   lowerStatus == QLatin1StringView("ok") ||
                   lowerStatus == QLatin1StringView("approved")) {
            statusBadge->hide();
            okCheckLabel->show();
        } else {
            statusBadge->hide();
            okCheckLabel->hide();
        }

        if (lowerStatus != lastStyledStatus || event.toolName != lastStyledToolName) {
            refreshCardChrome();
        }

        // Optional structured delegation metadata inside tool output
        const auto parsedDelegation = DelegationMeta::parseFromEvent(event);
        if (parsedDelegation.has_value()) {
            if (!delegationPanel) {
                delegationPanel = std::make_unique<DelegationPanel>(cardFrame, false, onOpenFile);
                auto* layout = static_cast<QVBoxLayout*>(cardFrame->layout());
                layout->insertWidget(1, delegationPanel->frame);
            }
            const QString fallbackText = DelegationMeta::stripMetadataComment(
                !event.toolOutput.isEmpty() ? event.toolOutput : event.text);
            delegationPanel->update(*parsedDelegation, fallbackText, projectRoots);
            delegationPanel->frame->show();
        } else if (delegationPanel) {
            delegationPanel->frame->hide();
        }

        if (payloadChanged) {
            bodyDirty = true;
            if (expanded) {
                renderBody();
            }
        }
    }

    void onViewportResized(int /*viewportWidth*/) override {
        if (expanded && bodyBrowser) {
            updateTextBrowserHeight(bodyBrowser, kMinToolBodyHeightPx, kMaxToolBodyHeightPx);
        }
    }

    void applyTheme() override {
        const UiPalette u = ThemeManager::instance().ui();
        const CodePalette c = ThemeManager::instance().code();

        refreshCardChrome();
        refreshChevron();

        toggleBtn->setStyleSheet(QStringLiteral(
            "QToolButton { background: transparent; border: none; padding: 0px; }"));
        iconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        nameLabel->setStyleSheet(
            QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.textPrimary)));

        fileLinkBtn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  border: none;"
            "  padding: 0px;"
            "  color: %1;"
            "  text-decoration: underline;"
            "  text-align: left;"
            "}"
            "QPushButton:hover {"
            "  color: %2;"
            "}")
                                       .arg(qssColor(u.textSecondary), qssColor(u.textPrimary)));

        resultKindBadge->applyTheme();
        mcpBadge->applyTheme();
        errorKindBadge->applyTheme();
        statusBadge->applyTheme();

        okCheckLabel->setPixmap(icons::vectorIcon(QStringLiteral("check"), u.textMuted, 12).pixmap(12, 12));
        okCheckLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));

        const QColor codeBg = c.hasCustomBackground ? c.background : compositeOver(u.elevatedSurface, u.background);
        const QColor bodyBorder = compositeOver(u.divider, u.background);
        bodyFrame->setStyleSheet(QStringLiteral(
            "QFrame#spellToolBody {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 6px;"
            "}")
                                     .arg(qssColor(codeBg), qssColor(bodyBorder)));
        bodyBrowser->setStyleSheet(QStringLiteral(
            "QTextBrowser {"
            "  background-color: transparent;"
            "  color: %1;"
            "  border: none;"
            "  padding: 0px;"
            "}")
                                       .arg(qssColor(c.plain)));
        bodyBrowser->setFont(makeMonoFont(10.5));

        if (delegationPanel) {
            delegationPanel->applyTheme();
        }

        if (expanded) {
            renderBody();
        } else {
            bodyDirty = true;
        }
    }
};

// ============================================================================
// 3. Assistant / Reasoning Event Card (Markdown + Collapsible Reasoning + TurnStats)
// ============================================================================

struct AssistantEventCard : EventCard {
    // Collapsible Reasoning drawer
    QWidget* reasoningBox = nullptr;
    QPushButton* reasoningToggleBtn = nullptr;
    QFrame* reasoningBodyFrame = nullptr;
    QLabel* reasoningLabel = nullptr;
    bool reasoningExpanded = false;
    bool userToggledReasoning = false;
    QString lastReasoningText;

    // Optional truncation notice
    QFrame* truncationBanner = nullptr;
    QLabel* truncationIcon = nullptr;
    QLabel* truncationLabel = nullptr;

    // Optional embedded tool card (when an assistant event also carries tool_call data)
    std::unique_ptr<ToolEventCard> embeddedToolCard;

    // Markdown body
    SpellMarkdownView* markdownView = nullptr;
    QString lastMarkdownText;

    // Live streaming indicator
    QWidget* streamingRow = nullptr;
    QLabel* streamingHatLabel = nullptr;
    QLabel* streamingLabel = nullptr;

    // Completed TurnStats footer
    QWidget* statsRow = nullptr;
    QLabel* statsHatLabel = nullptr;
    QFrame* statsPill = nullptr;
    QLabel* statsLabel = nullptr;

    QPointer<QObject> eventFilterObj;
    std::function<void(const QString&, int)> onOpenFile;
    std::function<void(ToolEventCard*)> onRegisterToolHeader;
    std::function<void(ToolEventCard*)> onUnregisterToolHeader;

    AssistantEventCard(QWidget* parent,
                       QObject* filterObj,
                       const QString& cardKind,
                       std::function<void(const QString&, int)> openFileCb,
                       std::function<void(ToolEventCard*)> registerToolCb,
                       std::function<void(ToolEventCard*)> unregisterToolCb)
        : eventFilterObj(filterObj)
        , onOpenFile(std::move(openFileCb))
        , onRegisterToolHeader(std::move(registerToolCb))
        , onUnregisterToolHeader(std::move(unregisterToolCb)) {
        kind = cardKind;
        root = new QWidget(parent);
        root->setStyleSheet(QStringLiteral("background: transparent;"));

        auto* layout = new QVBoxLayout(root);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(7);

        // Collapsible Reasoning Drawer
        reasoningBox = new QWidget(root);
        reasoningBox->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* reasoningLayout = new QVBoxLayout(reasoningBox);
        reasoningLayout->setContentsMargins(0, 0, 0, 0);
        reasoningLayout->setSpacing(5);

        reasoningToggleBtn = new QPushButton(QStringLiteral("Thinking"), reasoningBox);
        reasoningToggleBtn->setFlat(true);
        reasoningToggleBtn->setCursor(Qt::PointingHandCursor);
        reasoningToggleBtn->setFont(makeUiFont(10.5, QFont::Medium));
        reasoningToggleBtn->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        reasoningLayout->addWidget(reasoningToggleBtn, 0, Qt::AlignLeft);

        reasoningBodyFrame = new QFrame(reasoningBox);
        reasoningBodyFrame->setObjectName(QStringLiteral("spellReasoningBody"));
        auto* rbLayout = new QVBoxLayout(reasoningBodyFrame);
        rbLayout->setContentsMargins(12, 2, 4, 2);
        rbLayout->setSpacing(0);

        reasoningLabel = new QLabel(reasoningBodyFrame);
        reasoningLabel->setFont(makeUiFont(11.5));
        reasoningLabel->setWordWrap(true);
        reasoningLabel->setTextFormat(Qt::PlainText);
        reasoningLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        rbLayout->addWidget(reasoningLabel);

        reasoningBodyFrame->hide();
        reasoningLayout->addWidget(reasoningBodyFrame);

        reasoningBox->hide();
        layout->addWidget(reasoningBox);

        // Optional truncation warning banner
        truncationBanner = new QFrame(root);
        truncationBanner->setObjectName(QStringLiteral("spellTruncationBanner"));
        auto* truncLayout = new QHBoxLayout(truncationBanner);
        truncLayout->setContentsMargins(10, 6, 10, 6);
        truncLayout->setSpacing(7);

        truncationIcon = new QLabel(truncationBanner);
        truncationIcon->setFixedSize(13, 13);
        truncLayout->addWidget(truncationIcon);

        truncationLabel = new QLabel(
            QStringLiteral("Reasoning truncated at max_tokens before final answer — increase max_tokens to complete response."),
            truncationBanner);
        truncationLabel->setFont(makeMonoFont(10.5, QFont::Medium));
        truncationLabel->setWordWrap(true);
        truncationLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        truncLayout->addWidget(truncationLabel, 1);

        truncationBanner->hide();
        layout->addWidget(truncationBanner);

        // Markdown Body
        markdownView = new SpellMarkdownView(root);
        markdownView->hide();
        QObject::connect(markdownView, &SpellMarkdownView::fileCitationClicked, markdownView,
                         [alive = alive, this](const QString& path, int line) {
                             if (*alive && onOpenFile) {
                                 onOpenFile(path, line);
                             }
                         });
        layout->addWidget(markdownView);

        // Live streaming indicator row
        streamingRow = new QWidget(root);
        streamingRow->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* streamLayout = new QHBoxLayout(streamingRow);
        streamLayout->setContentsMargins(0, 2, 0, 2);
        streamLayout->setSpacing(7);

        streamingHatLabel = new QLabel(streamingRow);
        streamingHatLabel->setFixedSize(15, 15);
        streamLayout->addWidget(streamingHatLabel);

        streamingLabel = new QLabel(streamingRow);
        streamingLabel->setFont(makeMonoFont(9.5));
        streamLayout->addWidget(streamingLabel);
        streamLayout->addStretch(1);

        streamingRow->hide();
        layout->addWidget(streamingRow);

        // TurnStats footer
        statsRow = new QWidget(root);
        statsRow->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* statsRowLayout = new QHBoxLayout(statsRow);
        statsRowLayout->setContentsMargins(0, 3, 0, 2);
        statsRowLayout->setSpacing(8);

        statsHatLabel = new QLabel(statsRow);
        statsHatLabel->setFixedSize(16, 16);
        statsRowLayout->addWidget(statsHatLabel);

        statsPill = new QFrame(statsRow);
        statsPill->setObjectName(QStringLiteral("spellTurnStatsPill"));
        auto* pillLayout = new QHBoxLayout(statsPill);
        pillLayout->setContentsMargins(9, 3, 9, 3);
        pillLayout->setSpacing(0);

        statsLabel = new QLabel(statsPill);
        statsLabel->setFont(makeMonoFont(9.5));
        statsLabel->setTextFormat(Qt::PlainText);
        statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        pillLayout->addWidget(statsLabel);

        statsRowLayout->addWidget(statsPill);
        statsRowLayout->addStretch(1);

        statsRow->hide();
        layout->addWidget(statsRow);

        QObject::connect(reasoningToggleBtn, &QPushButton::clicked, reasoningToggleBtn,
                         [alive = alive, this]() {
                             if (!*alive) {
                                 return;
                             }
                             userToggledReasoning = true;
                             reasoningExpanded = !reasoningExpanded;
                             reasoningBodyFrame->setVisible(reasoningExpanded);
                             refreshReasoningHeader();
                         });

        applyTheme();
    }

    ~AssistantEventCard() override {
        *alive = false;
        embeddedToolCard.reset();
    }

    void refreshReasoningHeader() {
        const UiPalette u = ThemeManager::instance().ui();
        const QString chevron = reasoningExpanded ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right");
        reasoningToggleBtn->setIcon(icons::vectorIcon(chevron, u.textMuted, 10));
        reasoningToggleBtn->setIconSize(QSize(10, 10));

        const bool activelyThinking =
            lastEvent.isStreaming &&
            (kind == QLatin1StringView("reasoning") || lastEvent.text.trimmed().isEmpty());
        if (activelyThinking) {
            reasoningToggleBtn->setText(QStringLiteral("Thinking…"));
        } else if (!lastReasoningText.isEmpty()) {
            reasoningToggleBtn->setText(
                QStringLiteral("Thinking (%1 chars)").arg(lastReasoningText.size()));
        } else {
            reasoningToggleBtn->setText(QStringLiteral("Thinking"));
        }
    }

    QString buildTurnStatsText(const UiChatEvent& ev) const {
        QStringList parts;

        if (ev.tokPerSec > 0.0) {
            parts.append(QStringLiteral("%1 tok/s").arg(ev.tokPerSec, 0, 'f', 1));
        }

        if (ev.promptTokens > 0 && ev.completionTokens > 0) {
            parts.append(QStringLiteral("%1 in / %2 out tok")
                             .arg(ev.promptTokens)
                             .arg(ev.completionTokens));
        } else if (ev.completionTokens > 0) {
            parts.append(QStringLiteral("%1 tok").arg(ev.completionTokens));
        } else if (ev.promptTokens > 0) {
            parts.append(QStringLiteral("%1 in tok").arg(ev.promptTokens));
        }

        if (ev.latencyMs > 0) {
            parts.append(formatLatency(ev.latencyMs));
        }

        if (!ev.modelId.trimmed().isEmpty()) {
            parts.append(ev.modelId.trimmed());
        }

        const QString timeStr = formatTimestamp(ev.timestamp);
        if (!timeStr.isEmpty()) {
            parts.append(timeStr);
        }

        return parts.join(QStringLiteral("  ·  "));
    }

    void update(const UiChatEvent& event,
                const QVector<StagedDiff>& diffs,
                const QStringList& projectRoots,
                int userTurnOrdinal,
                bool rootsChanged) override {
        if (initialized && event == lastEvent && !rootsChanged) {
            return;
        }
        lastEvent = event;
        initialized = true;

        const bool isPureReasoning = (kind == QLatin1StringView("reasoning"));
        const QString reasoningText = isPureReasoning
                                          ? (!event.reasoning.isEmpty() ? event.reasoning : event.text)
                                          : event.reasoning;
        const QString bodyText = isPureReasoning ? QString() : event.text;

        // 1. Reasoning drawer
        if (!reasoningText.trimmed().isEmpty()) {
            if (reasoningText != lastReasoningText) {
                lastReasoningText = reasoningText;
                reasoningLabel->setText(reasoningText);
            }
            if (!userToggledReasoning) {
                const bool shouldAutoExpand = event.isStreaming && bodyText.trimmed().isEmpty();
                reasoningExpanded = shouldAutoExpand;
            }
            reasoningBodyFrame->setVisible(reasoningExpanded);
            reasoningBox->show();
            refreshReasoningHeader();
        } else {
            lastReasoningText.clear();
            reasoningBox->hide();
        }

        // 2. Optional truncation notice
        const bool truncated = detail::getBool(
            event.raw, {"reasoning_truncated", "reasoningTruncated"}, false);
        truncationBanner->setVisible(truncated);

        // 3. Optional inline tool call embedded on assistant event
        if (!isPureReasoning && !event.toolName.trimmed().isEmpty()) {
            if (!embeddedToolCard) {
                embeddedToolCard = std::make_unique<ToolEventCard>(
                    root, eventFilterObj, QStringLiteral("tool_call"), onOpenFile, onUnregisterToolHeader);
                if (onRegisterToolHeader) {
                    onRegisterToolHeader(embeddedToolCard.get());
                }
                auto* layout = static_cast<QVBoxLayout*>(root->layout());
                const int mdIndex = layout->indexOf(markdownView);
                layout->insertWidget(std::max(0, mdIndex), embeddedToolCard->root);
            }
            embeddedToolCard->update(event, diffs, projectRoots, userTurnOrdinal, rootsChanged);
            embeddedToolCard->root->show();
        } else if (embeddedToolCard) {
            embeddedToolCard->root->hide();
        }

        // 4. Markdown body (in-place incremental update via SpellMarkdownView!)
        if (!bodyText.isEmpty()) {
            if (bodyText != lastMarkdownText || rootsChanged || !markdownView->isVisible()) {
                lastMarkdownText = bodyText;
                markdownView->setMarkdown(bodyText, projectRoots);
            }
            markdownView->show();
        } else {
            if (!lastMarkdownText.isEmpty()) {
                lastMarkdownText.clear();
                markdownView->clear();
            }
            markdownView->hide();
        }

        // 5. Streaming status vs completed TurnStats footer
        if (event.isStreaming) {
            QStringList liveParts;
            liveParts.append(QStringLiteral("Streaming…"));
            if (event.tokPerSec > 0.0) {
                liveParts.append(QStringLiteral("%1 tok/s").arg(event.tokPerSec, 0, 'f', 1));
            }
            if (event.completionTokens > 0) {
                liveParts.append(QStringLiteral("%1 tok").arg(event.completionTokens));
            }
            if (!event.modelId.trimmed().isEmpty()) {
                liveParts.append(event.modelId.trimmed());
            }
            streamingLabel->setText(liveParts.join(QStringLiteral("  ·  ")));
            streamingRow->show();
            statsRow->hide();
        } else {
            streamingRow->hide();
            const QString statsText = buildTurnStatsText(event);
            if (!statsText.isEmpty()) {
                statsLabel->setText(statsText);
                statsPill->setToolTip(
                    !event.modelId.trimmed().isEmpty()
                        ? QStringLiteral("Model: %1").arg(event.modelId.trimmed())
                        : QStringLiteral("Turn statistics"));
                statsRow->show();
            } else {
                statsRow->hide();
            }
        }
    }

    void onViewportResized(int viewportWidth) override {
        if (embeddedToolCard) {
            embeddedToolCard->onViewportResized(viewportWidth);
        }
    }

    void applyTheme() override {
        const UiPalette u = ThemeManager::instance().ui();
        const QColor border = compositeOver(u.subtleBorder, u.background);
        const QColor divider = compositeOver(u.divider, u.background);

        reasoningToggleBtn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: transparent;"
            "  border: none;"
            "  padding: 2px 0px;"
            "  color: %1;"
            "  text-align: left;"
            "}"
            "QPushButton:hover {"
            "  color: %2;"
            "}")
                                              .arg(qssColor(u.textMuted), qssColor(u.textPrimary)));
        refreshReasoningHeader();

        reasoningBodyFrame->setStyleSheet(QStringLiteral(
            "QFrame#spellReasoningBody {"
            "  background: transparent;"
            "  border: none;"
            "  border-left: 2px solid %1;"
            "}")
                                              .arg(qssColor(divider)));
        reasoningLabel->setStyleSheet(
            QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.textSecondary)));

        const QColor warnFill = compositeOver(u.elevatedSurface, u.background);
        const QColor warnBorder = compositeOver(u.selectedBorder, u.background);
        truncationBanner->setStyleSheet(QStringLiteral(
            "QFrame#spellTruncationBanner {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 7px;"
            "}")
                                            .arg(qssColor(warnFill), qssColor(warnBorder)));
        truncationIcon->setPixmap(icons::vectorIcon(QStringLiteral("warning"), u.warning, 12).pixmap(12, 12));
        truncationIcon->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        truncationLabel->setStyleSheet(
            QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.textPrimary)));

        markdownView->applyTheme();
        if (embeddedToolCard) {
            embeddedToolCard->applyTheme();
        }

        streamingHatLabel->setPixmap(icons::hatMarkPixmap(14, u.textMuted));
        streamingHatLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        streamingLabel->setStyleSheet(
            QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.textMuted)));

        statsHatLabel->setPixmap(icons::hatMarkPixmap(15, u.textSecondary));
        statsHatLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        statsPill->setStyleSheet(QStringLiteral(
            "QFrame#spellTurnStatsPill {"
            "  background: transparent;"
            "  border: 1px solid %1;"
            "  border-radius: 5px;"
            "}")
                                     .arg(qssColor(border)));
        statsLabel->setStyleSheet(
            QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.textSecondary)));
    }
};

// ============================================================================
// 4. Diff Staged Event Card (Embeds SpellDiffWidget)
// ============================================================================

struct DiffStagedEventCard : EventCard {
    SpellDiffWidget* diffWidget = nullptr;
    StagedDiff lastResolvedDiff;
    QStringList currentProjectRoots;

    std::function<void(const QString&)> onApprove;
    std::function<void(const QString&)> onReject;
    std::function<void(const QString&, int)> onOpenFile;

    DiffStagedEventCard(QWidget* parent,
                        std::function<void(const QString&)> approveCb,
                        std::function<void(const QString&)> rejectCb,
                        std::function<void(const QString&, int)> openFileCb)
        : onApprove(std::move(approveCb))
        , onReject(std::move(rejectCb))
        , onOpenFile(std::move(openFileCb)) {
        kind = QStringLiteral("diff_staged");
        diffWidget = new SpellDiffWidget(parent);
        diffWidget->setShowActions(true);
        root = diffWidget;

        QObject::connect(diffWidget, &SpellDiffWidget::approveClicked, diffWidget,
                         [alive = alive, this](const QString& diffId) {
                             if (*alive && onApprove) {
                                 onApprove(diffId);
                             }
                         });
        QObject::connect(diffWidget, &SpellDiffWidget::rejectClicked, diffWidget,
                         [alive = alive, this](const QString& diffId) {
                             if (*alive && onReject) {
                                 onReject(diffId);
                             }
                         });
        QObject::connect(diffWidget, &SpellDiffWidget::openFileRequested, diffWidget,
                         [alive = alive, this](const QString& path) {
                             if (*alive && onOpenFile) {
                                 onOpenFile(resolveWorkspacePath(path, currentProjectRoots), 0);
                             }
                         });
    }

    void update(const UiChatEvent& event,
                const QVector<StagedDiff>& diffs,
                const QStringList& projectRoots,
                int /*userTurnOrdinal*/,
                bool /*rootsChanged*/) override {
        currentProjectRoots = projectRoots;
        const StagedDiff resolved = resolveStagedDiffForEvent(event, diffs);
        if (initialized && resolved == lastResolvedDiff) {
            lastEvent = event;
            return;
        }
        lastEvent = event;
        lastResolvedDiff = resolved;
        initialized = true;
        diffWidget->setDiff(resolved);
    }

    void applyTheme() override {
        if (diffWidget) {
            diffWidget->applyTheme();
        }
    }
};

// ============================================================================
// 5. Delegation Event Card (Structured DelegationMeta + Markdown Fallback)
// ============================================================================

struct DelegationEventCard : EventCard {
    std::unique_ptr<DelegationPanel> panel;

    QFrame* fallbackFrame = nullptr;
    QLabel* fallbackIconLabel = nullptr;
    QLabel* fallbackTitleLabel = nullptr;
    SpellBadge* fallbackStatusBadge = nullptr;
    SpellMarkdownView* fallbackMarkdown = nullptr;
    QString lastFallbackText;

    std::function<void(const QString&, int)> onOpenFile;

    DelegationEventCard(QWidget* parent, std::function<void(const QString&, int)> openFileCb)
        : onOpenFile(std::move(openFileCb)) {
        kind = QStringLiteral("delegation");
        root = new QWidget(parent);
        root->setStyleSheet(QStringLiteral("background: transparent;"));

        auto* layout = new QVBoxLayout(root);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        panel = std::make_unique<DelegationPanel>(root, true, onOpenFile);
        panel->frame->hide();
        layout->addWidget(panel->frame);

        fallbackFrame = new QFrame(root);
        fallbackFrame->setObjectName(QStringLiteral("spellDelegationFallback"));
        auto* fbLayout = new QVBoxLayout(fallbackFrame);
        fbLayout->setContentsMargins(10, 8, 10, 8);
        fbLayout->setSpacing(6);

        auto* headerRow = new QHBoxLayout();
        headerRow->setContentsMargins(0, 0, 0, 0);
        headerRow->setSpacing(6);

        fallbackIconLabel = new QLabel(fallbackFrame);
        fallbackIconLabel->setFixedSize(14, 14);
        headerRow->addWidget(fallbackIconLabel);

        fallbackTitleLabel = new QLabel(QStringLiteral("Subagent Delegation"), fallbackFrame);
        fallbackTitleLabel->setFont(makeUiFont(11.5, QFont::DemiBold));
        headerRow->addWidget(fallbackTitleLabel);

        fallbackStatusBadge = new SpellBadge(QStringLiteral("DELEGATION"), BadgeTone::Info, fallbackFrame);
        headerRow->addWidget(fallbackStatusBadge);
        headerRow->addStretch(1);

        fbLayout->addLayout(headerRow);

        fallbackMarkdown = new SpellMarkdownView(fallbackFrame);
        QObject::connect(fallbackMarkdown, &SpellMarkdownView::fileCitationClicked, fallbackMarkdown,
                         [alive = alive, this](const QString& path, int line) {
                             if (*alive && onOpenFile) {
                                 onOpenFile(path, line);
                             }
                         });
        fbLayout->addWidget(fallbackMarkdown);

        fallbackFrame->hide();
        layout->addWidget(fallbackFrame);

        applyTheme();
    }

    void update(const UiChatEvent& event,
                const QVector<StagedDiff>& /*diffs*/,
                const QStringList& projectRoots,
                int /*userTurnOrdinal*/,
                bool rootsChanged) override {
        if (initialized && event == lastEvent && !rootsChanged) {
            return;
        }
        lastEvent = event;
        initialized = true;

        const QString rawBody = !event.text.isEmpty() ? event.text : event.toolOutput;
        const QString cleanBody = DelegationMeta::stripMetadataComment(rawBody);
        const auto meta = DelegationMeta::parseFromEvent(event);

        if (meta.has_value()) {
            panel->update(*meta, cleanBody, projectRoots);
            panel->frame->show();
            fallbackFrame->hide();
        } else {
            panel->frame->hide();
            fallbackStatusBadge->setText(event.isStreaming ? QStringLiteral("RUNNING")
                                                           : QStringLiteral("DELEGATION"));
            fallbackStatusBadge->setShowDot(event.isStreaming);
            if (cleanBody != lastFallbackText || rootsChanged) {
                lastFallbackText = cleanBody;
                if (!cleanBody.isEmpty()) {
                    fallbackMarkdown->setMarkdown(cleanBody, projectRoots);
                    fallbackMarkdown->show();
                } else {
                    fallbackMarkdown->clear();
                    fallbackMarkdown->hide();
                }
            }
            fallbackFrame->show();
        }
    }

    void applyTheme() override {
        const UiPalette u = ThemeManager::instance().ui();
        const QColor bg = compositeOver(u.elevatedSurface, u.background);
        const QColor border = compositeOver(u.subtleBorder, u.background);

        if (panel) {
            panel->applyTheme();
        }
        fallbackFrame->setStyleSheet(QStringLiteral(
            "QFrame#spellDelegationFallback {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 7px;"
            "}")
                                         .arg(qssColor(bg), qssColor(border)));
        fallbackIconLabel->setPixmap(icons::vectorIcon(QStringLiteral("cpu"), u.textSecondary, 13).pixmap(13, 13));
        fallbackIconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        fallbackTitleLabel->setStyleSheet(
            QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.textPrimary)));
        fallbackStatusBadge->applyTheme();
        fallbackMarkdown->applyTheme();
    }
};

// ============================================================================
// 6. Banner Event Card (timer_alarm, compaction_notice, error, system)
// ============================================================================

struct BannerEventCard : EventCard {
    QFrame* frame = nullptr;
    QLabel* iconLabel = nullptr;
    SpellBadge* badge = nullptr;
    QLabel* timeLabel = nullptr;
    QLabel* bodyLabel = nullptr;
    QPushButton* actionBtn = nullptr;

    QStringList currentProjectRoots;
    std::function<void(const QString&, int)> onOpenFile;
    std::function<void()> onConfigureEndpoint;

    BannerEventCard(QWidget* parent,
                    const QString& cardKind,
                    std::function<void(const QString&, int)> openFileCb,
                    std::function<void()> configureCb)
        : onOpenFile(std::move(openFileCb))
        , onConfigureEndpoint(std::move(configureCb)) {
        kind = cardKind;
        frame = new QFrame(parent);
        frame->setObjectName(QStringLiteral("spellBannerCard"));
        root = frame;

        auto* layout = new QVBoxLayout(frame);
        layout->setContentsMargins(11, 8, 11, 8);
        layout->setSpacing(5);

        auto* topRow = new QHBoxLayout();
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(6);

        iconLabel = new QLabel(frame);
        iconLabel->setFixedSize(13, 13);
        topRow->addWidget(iconLabel);

        badge = new SpellBadge(QString(), BadgeTone::Neutral, frame);
        topRow->addWidget(badge);

        topRow->addStretch(1);

        timeLabel = new QLabel(frame);
        timeLabel->setFont(makeMonoFont(9.5));
        timeLabel->hide();
        topRow->addWidget(timeLabel);

        actionBtn = new QPushButton(QStringLiteral("Configure Endpoint…"), frame);
        actionBtn->setCursor(Qt::PointingHandCursor);
        actionBtn->setFont(makeUiFont(10.0, QFont::DemiBold));
        actionBtn->hide();
        QObject::connect(actionBtn, &QPushButton::clicked, actionBtn, [alive = alive, this]() {
            if (*alive && onConfigureEndpoint) {
                onConfigureEndpoint();
            }
        });
        topRow->addWidget(actionBtn);

        layout->addLayout(topRow);

        bodyLabel = new QLabel(frame);
        bodyLabel->setFont(makeUiFont(11.5));
        bodyLabel->setWordWrap(true);
        bodyLabel->setTextFormat(Qt::RichText);
        bodyLabel->setOpenExternalLinks(false);
        bodyLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
        QObject::connect(bodyLabel, &QLabel::linkActivated, bodyLabel,
                         [alive = alive, this](const QString& href) {
                             if (*alive) {
                                 openHrefOrCitation(QUrl(href), onOpenFile);
                             }
                         });
        layout->addWidget(bodyLabel);

        configureBadgeForKind();
        applyTheme();
    }

    void configureBadgeForKind() {
        if (kind == QLatin1StringView("timer_alarm")) {
            badge->setText(QStringLiteral("TIMER ALARM"));
            badge->setTone(BadgeTone::Info);
            badge->setShowDot(true);
        } else if (kind == QLatin1StringView("compaction_notice")) {
            badge->setText(QStringLiteral("CONTEXT COMPACTED"));
            badge->setTone(BadgeTone::Neutral);
            badge->setShowDot(false);
        } else if (kind == QLatin1StringView("error")) {
            badge->setText(QStringLiteral("ERROR"));
            badge->setTone(BadgeTone::Danger);
            badge->setShowDot(true);
        } else {
            badge->setText(QStringLiteral("SYSTEM"));
            badge->setTone(BadgeTone::Neutral);
            badge->setShowDot(false);
        }
    }

    void renderBodyHtml() {
        const UiPalette u = ThemeManager::instance().ui();
        QString text = lastEvent.text.trimmed();
        if (text.isEmpty()) {
            if (kind == QLatin1StringView("compaction_notice")) {
                text = QStringLiteral("Earlier messages were compacted to preserve context window headroom.");
            } else if (kind == QLatin1StringView("timer_alarm")) {
                text = QStringLiteral("Scheduled timer fired.");
            } else if (kind == QLatin1StringView("error")) {
                text = QStringLiteral("An unexpected error occurred.");
            }
        }
        QString escaped = text.toHtmlEscaped();
        escaped.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
        const QString linkified = linkifyHtml(escaped, currentProjectRoots);
        const QString textColor = (kind == QLatin1StringView("compaction_notice"))
                                      ? qssColor(u.textSecondary)
                                      : qssColor(u.textPrimary);
        bodyLabel->setText(QStringLiteral(
            "<span style=\"color:%1;\">%2</span>")
                               .arg(textColor, linkified));
    }

    void update(const UiChatEvent& event,
                const QVector<StagedDiff>& /*diffs*/,
                const QStringList& projectRoots,
                int /*userTurnOrdinal*/,
                bool rootsChanged) override {
        currentProjectRoots = projectRoots;
        if (initialized && event == lastEvent && !rootsChanged) {
            return;
        }
        lastEvent = event;
        initialized = true;

        const QString ts = formatTimestamp(event.timestamp);
        if (!ts.isEmpty()) {
            timeLabel->setText(ts);
            timeLabel->show();
        } else {
            timeLabel->hide();
        }

        const QString lowerText = event.text.toLower();
        const bool isEndpointError = (kind == QLatin1StringView("error")) &&
                                     (lowerText.contains(QLatin1StringView("endpoint")) ||
                                      lowerText.contains(QLatin1StringView("api key")) ||
                                      lowerText.contains(QLatin1StringView("model")) ||
                                      lowerText.contains(QLatin1StringView("unauthorized")) ||
                                      lowerText.contains(QLatin1StringView("connection refused")));
        actionBtn->setVisible(isEndpointError);

        renderBodyHtml();
    }

    void applyTheme() override {
        const UiPalette u = ThemeManager::instance().ui();
        const bool isError = (kind == QLatin1StringView("error"));
        const bool isCompact = (kind == QLatin1StringView("compaction_notice"));
        const bool isTimer = (kind == QLatin1StringView("timer_alarm"));

        QColor bg = compositeOver(u.elevatedSurface, u.background);
        QColor border = compositeOver(u.subtleBorder, u.background);
        QColor iconTint = u.textSecondary;
        QString iconName = QStringLiteral("info");

        if (isError) {
            bg = compositeOver(withAlpha(u.danger, 0.12), u.background);
            border = withAlpha(u.danger, 0.55);
            iconTint = u.danger;
            iconName = QStringLiteral("warning");
        } else if (isTimer) {
            iconName = QStringLiteral("bell");
            iconTint = u.textPrimary;
            border = compositeOver(u.selectedBorder, u.background);
        } else if (isCompact) {
            bg = compositeOver(withAlpha(u.elevatedSurface, 0.45), u.background);
            border = compositeOver(u.divider, u.background);
            iconName = QStringLiteral("sparkles");
            iconTint = u.textMuted;
        }

        frame->setStyleSheet(QStringLiteral(
            "QFrame#spellBannerCard {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 8px;"
            "}")
                                 .arg(qssColor(bg), qssColor(border)));

        iconLabel->setPixmap(icons::vectorIcon(iconName, iconTint, 12).pixmap(12, 12));
        iconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        badge->applyTheme();
        timeLabel->setStyleSheet(
            QStringLiteral("background: transparent; color: %1;").arg(qssColor(u.textMuted)));
        bodyLabel->setStyleSheet(QStringLiteral("background: transparent;"));
        renderBodyHtml();
    }
};

} // namespace

// ============================================================================
// ChatTimelineWidget::Impl
// ============================================================================

struct ChatTimelineWidget::Impl {
    ChatTimelineWidget* q = nullptr;

    QStackedWidget* stack = nullptr;

    // Empty state page (strictly ONLY the centered monochrome witch hat logo — zero text/buttons)
    QWidget* emptyPage = nullptr;
    QLabel* emptyHatLabel = nullptr;
    bool hasProject = false;
    bool endpointConfigured = true;

    // Timeline scroll area page
    QScrollArea* scrollArea = nullptr;
    QWidget* contentWidget = nullptr;
    QVBoxLayout* contentLayout = nullptr;

    // Trailing unreferenced staged diffs section (only visible when diffs exist outside events)
    QWidget* extraDiffsContainer = nullptr;
    QVBoxLayout* extraDiffsLayout = nullptr;
    SpellSectionHeader* extraDiffsHeader = nullptr;
    struct ExtraDiffEntry {
        QString id;
        StagedDiff diff;
        SpellDiffWidget* widget = nullptr;
    };
    QVector<ExtraDiffEntry> extraDiffEntries;

    // Floating jump-to-bottom button
    QPushButton* jumpToBottomBtn = nullptr;

    // Event-filter lookup maps (MUST be declared before `cards` so they outlive card destructors)
    QHash<QObject*, UserEventCard*> userHoverMap;
    QHash<QObject*, UserEventCard*> userEditorMap;
    QHash<QObject*, ToolEventCard*> toolHeaderMap;

    // Tracked state for incremental updates
    QVector<UiChatEvent> events;
    QVector<StagedDiff> stagedDiffs;
    QStringList projectRoots;
    std::vector<std::unique_ptr<EventCard>> cards;

    // Auto-scroll state
    bool autoScrollPaused = false;
    bool programmaticScroll = false;
    bool scrollScheduled = false;
    int lastScrollMax = 0;

    explicit Impl(ChatTimelineWidget* owner)
        : q(owner) {}

    ~Impl() {
        cards.clear();
        userHoverMap.clear();
        userEditorMap.clear();
        toolHeaderMap.clear();
    }

    void setupUi() {
        q->setObjectName(QStringLiteral("spellChatTimeline"));
        q->setAttribute(Qt::WA_StyledBackground, true);

        auto* rootLayout = new QVBoxLayout(q);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        stack = new QStackedWidget(q);
        stack->setObjectName(QStringLiteral("spellChatTimelineStack"));
        stack->setAttribute(Qt::WA_StyledBackground, true);
        rootLayout->addWidget(stack);

        // --- Page 0: Empty State (strictly ONLY the centered Witch Hat logo, zero text) ---
        emptyPage = new QWidget(stack);
        emptyPage->setObjectName(QStringLiteral("spellEmptyChatPage"));
        emptyPage->setAttribute(Qt::WA_StyledBackground, true);
        auto* emptyOuter = new QVBoxLayout(emptyPage);
        emptyOuter->setContentsMargins(0, 0, 0, 0);
        emptyOuter->setSpacing(0);
        emptyOuter->addStretch(1);

        emptyHatLabel = new QLabel(emptyPage);
        emptyHatLabel->setObjectName(QStringLiteral("spellEmptyChatHatLogo"));
        emptyHatLabel->setAlignment(Qt::AlignCenter);
        emptyHatLabel->setFixedSize(48, 48);
        emptyOuter->addWidget(emptyHatLabel, 0, Qt::AlignHCenter);

        emptyOuter->addStretch(1);
        stack->addWidget(emptyPage);

        // --- Page 1: Scrollable Timeline ---
        scrollArea = new QScrollArea(stack);
        scrollArea->setObjectName(QStringLiteral("spellChatTimelineScroll"));
        scrollArea->setWidgetResizable(true);
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scrollArea->viewport()->installEventFilter(q);

        contentWidget = new QWidget(scrollArea);
        contentWidget->setObjectName(QStringLiteral("spellChatTimelineContent"));
        contentWidget->setAttribute(Qt::WA_StyledBackground, true);
        contentLayout = new QVBoxLayout(contentWidget);
        contentLayout->setContentsMargins(16, 14, 16, 20);
        contentLayout->setSpacing(12);

        // Extra staged diffs container (sits after event cards, before bottom stretch)
        extraDiffsContainer = new QWidget(contentWidget);
        extraDiffsLayout = new QVBoxLayout(extraDiffsContainer);
        extraDiffsLayout->setContentsMargins(0, 6, 0, 0);
        extraDiffsLayout->setSpacing(8);

        extraDiffsHeader = new SpellSectionHeader(
            QStringLiteral("STAGED DIFFS"), QStringLiteral("code"), BadgeTone::Accent, extraDiffsContainer);
        extraDiffsLayout->addWidget(extraDiffsHeader);
        extraDiffsContainer->hide();

        contentLayout->addWidget(extraDiffsContainer);
        contentLayout->addStretch(1);

        scrollArea->setWidget(contentWidget);
        stack->addWidget(scrollArea);

        // Floating Jump-to-Bottom Pill
        jumpToBottomBtn = new QPushButton(QStringLiteral("Latest"), q);
        jumpToBottomBtn->setObjectName(QStringLiteral("spellJumpToBottom"));
        jumpToBottomBtn->setCursor(Qt::PointingHandCursor);
        jumpToBottomBtn->setFont(makeUiFont(10.5, QFont::DemiBold));
        jumpToBottomBtn->setToolTip(QStringLiteral("Scroll to latest message"));
        jumpToBottomBtn->hide();
        QObject::connect(jumpToBottomBtn, &QPushButton::clicked, q, [this]() {
            q->scrollToBottom(true);
        });

        // Scrollbar tracking for non-intrusive auto-scroll pause
        QScrollBar* vbar = scrollArea->verticalScrollBar();
        QObject::connect(vbar, &QScrollBar::valueChanged, q, [this, vbar](int value) {
            if (programmaticScroll) {
                return;
            }
            const int maxVal = vbar->maximum();
            if (maxVal <= 0 || value >= maxVal - kBottomThresholdPx) {
                autoScrollPaused = false;
            } else {
                autoScrollPaused = true;
            }
            updateJumpButtonVisibility();
        });

        QObject::connect(vbar, &QScrollBar::rangeChanged, q, [this](int /*minVal*/, int maxVal) {
            const int prevMax = lastScrollMax;
            lastScrollMax = maxVal;
            if (!autoScrollPaused && maxVal > 0 && maxVal >= prevMax) {
                snapScrollbarToBottom();
            }
            updateJumpButtonVisibility();
        });

        QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged, q,
                         [this]() { q->applyTheme(); });

        stack->setCurrentWidget(emptyPage);
    }

    int viewportWidth() const {
        if (scrollArea && scrollArea->viewport() && scrollArea->viewport()->width() > 0) {
            return scrollArea->viewport()->width();
        }
        return std::max(320, q->width());
    }

    void snapScrollbarToBottom() {
        if (!scrollArea) {
            return;
        }
        QScrollBar* vbar = scrollArea->verticalScrollBar();
        if (!vbar) {
            return;
        }
        programmaticScroll = true;
        vbar->setValue(vbar->maximum());
        programmaticScroll = false;
        lastScrollMax = vbar->maximum();
    }

    void scheduleScrollToBottom() {
        if (scrollScheduled) {
            return;
        }
        scrollScheduled = true;
        QTimer::singleShot(0, q, [this]() {
            scrollScheduled = false;
            if (!autoScrollPaused) {
                snapScrollbarToBottom();
            }
            updateJumpButtonVisibility();
        });
    }

    void positionJumpButton() {
        if (!jumpToBottomBtn) {
            return;
        }
        jumpToBottomBtn->adjustSize();
        const int w = std::max(78, jumpToBottomBtn->sizeHint().width());
        const int h = std::max(26, jumpToBottomBtn->sizeHint().height());
        jumpToBottomBtn->resize(w, h);
        const int x = std::max(12, q->width() - w - 18);
        const int y = std::max(12, q->height() - h - 14);
        jumpToBottomBtn->move(x, y);
        jumpToBottomBtn->raise();
    }

    void updateJumpButtonVisibility() {
        if (!jumpToBottomBtn || !scrollArea) {
            return;
        }
        const bool hasScrollableTimeline =
            (stack->currentWidget() == scrollArea) &&
            (scrollArea->verticalScrollBar()->maximum() > kBottomThresholdPx);
        const bool shouldShow = autoScrollPaused && hasScrollableTimeline;
        if (jumpToBottomBtn->isVisible() != shouldShow) {
            jumpToBottomBtn->setVisible(shouldShow);
        }
        if (shouldShow) {
            positionJumpButton();
        }
    }

    std::unique_ptr<EventCard> createCardForEvent(const UiChatEvent& event) {
        const QString normalizedKind = normalizeCardKind(event);
        auto openFileCb = [this](const QString& path, int line) {
            emit q->fileCitationClicked(path, line);
        };
        auto approveCb = [this](const QString& diffId) {
            emit q->approveDiffRequested(diffId);
        };
        auto rejectCb = [this](const QString& diffId) {
            emit q->rejectDiffRequested(diffId);
        };
        auto editCb = [this](int turnIndex, const QString& newPrompt) {
            emit q->editAndResendRequested(turnIndex, newPrompt);
        };
        auto configureCb = [this]() {
            emit q->configureEndpointRequested();
        };
        auto registerToolCb = [this](ToolEventCard* tc) {
            if (tc && tc->headerWidget) {
                toolHeaderMap.insert(tc->headerWidget, tc);
            }
        };
        auto unregisterToolCb = [this](ToolEventCard* tc) {
            if (tc && tc->headerWidget) {
                toolHeaderMap.remove(tc->headerWidget);
            }
        };

        if (normalizedKind == QLatin1StringView("user")) {
            auto card = std::make_unique<UserEventCard>(
                contentWidget,
                q,
                editCb,
                [this](UserEventCard* uc) {
                    if (uc) {
                        if (uc->root) {
                            userHoverMap.remove(uc->root);
                        }
                        if (uc->editor) {
                            userEditorMap.remove(uc->editor);
                        }
                    }
                });
            card->onViewportResized(viewportWidth());
            userHoverMap.insert(card->root, card.get());
            userEditorMap.insert(card->editor, card.get());
            return card;
        }

        if (normalizedKind == QLatin1StringView("tool_call") ||
            normalizedKind == QLatin1StringView("tool_result")) {
            auto card = std::make_unique<ToolEventCard>(
                contentWidget, q, normalizedKind, openFileCb, unregisterToolCb);
            registerToolCb(card.get());
            return card;
        }

        if (normalizedKind == QLatin1StringView("diff_staged")) {
            return std::make_unique<DiffStagedEventCard>(
                contentWidget, approveCb, rejectCb, openFileCb);
        }

        if (normalizedKind == QLatin1StringView("delegation")) {
            return std::make_unique<DelegationEventCard>(contentWidget, openFileCb);
        }

        if (normalizedKind == QLatin1StringView("timer_alarm") ||
            normalizedKind == QLatin1StringView("compaction_notice") ||
            normalizedKind == QLatin1StringView("error") ||
            normalizedKind == QLatin1StringView("system")) {
            return std::make_unique<BannerEventCard>(
                contentWidget, normalizedKind, openFileCb, configureCb);
        }

        return std::make_unique<AssistantEventCard>(
            contentWidget, q, normalizedKind, openFileCb, registerToolCb, unregisterToolCb);
    }

    bool syncExtraDiffs(const QVector<UiChatEvent>& newEvents, const QVector<StagedDiff>& allDiffs) {
        QSet<QString> representedIds;
        QSet<QString> representedPaths;
        for (const UiChatEvent& ev : newEvents) {
            if (normalizeCardKind(ev) == QLatin1StringView("diff_staged")) {
                if (!ev.diffId.isEmpty()) {
                    representedIds.insert(ev.diffId);
                } else if (!ev.id.isEmpty()) {
                    representedIds.insert(ev.id);
                }
                if (!ev.filePath.isEmpty()) {
                    representedPaths.insert(QDir::cleanPath(ev.filePath));
                }
            }
        }

        QVector<StagedDiff> unreferenced;
        for (const StagedDiff& diff : allDiffs) {
            const bool byId = !diff.id.isEmpty() && representedIds.contains(diff.id);
            const bool byPath = !diff.filePath.isEmpty() &&
                                representedPaths.contains(QDir::cleanPath(diff.filePath));
            if (!byId && !byPath) {
                unreferenced.append(diff);
            }
        }

        const qsizetype prevCount = extraDiffEntries.size();

        // Remove entries that no longer exist
        QSet<QString> nextIds;
        for (const StagedDiff& d : unreferenced) {
            nextIds.insert(!d.id.isEmpty() ? d.id : d.filePath);
        }
        for (qsizetype i = extraDiffEntries.size() - 1; i >= 0; --i) {
            if (!nextIds.contains(extraDiffEntries[i].id)) {
                detachAndDeleteWidget(extraDiffsLayout, extraDiffEntries[i].widget);
                extraDiffEntries.removeAt(i);
            }
        }

        // Update existing or append new extra diffs
        for (const StagedDiff& d : unreferenced) {
            const QString key = !d.id.isEmpty() ? d.id : d.filePath;
            bool found = false;
            for (ExtraDiffEntry& entry : extraDiffEntries) {
                if (entry.id == key) {
                    if (entry.diff != d) {
                        entry.diff = d;
                        entry.widget->setDiff(d);
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                auto* widget = new SpellDiffWidget(d, true, extraDiffsContainer);
                QObject::connect(widget, &SpellDiffWidget::approveClicked, q,
                                 &ChatTimelineWidget::approveDiffRequested);
                QObject::connect(widget, &SpellDiffWidget::rejectClicked, q,
                                 &ChatTimelineWidget::rejectDiffRequested);
                QObject::connect(widget, &SpellDiffWidget::openFileRequested, q,
                                 [this](const QString& path) {
                                     emit q->fileCitationClicked(
                                         resolveWorkspacePath(path, projectRoots), 0);
                                 });
                extraDiffsLayout->addWidget(widget);
                extraDiffEntries.append(ExtraDiffEntry{key, d, widget});
            }
        }

        extraDiffsContainer->setVisible(!extraDiffEntries.isEmpty());
        return extraDiffEntries.size() > prevCount;
    }

    void removeCardAt(std::size_t index) {
        if (index >= cards.size()) {
            return;
        }
        QWidget* widget = cards[index]->root;
        detachAndDeleteWidget(contentLayout, widget);
        cards.erase(cards.begin() + static_cast<std::ptrdiff_t>(index));
    }
};

// ============================================================================
// ChatTimelineWidget Public API
// ============================================================================

ChatTimelineWidget::ChatTimelineWidget(QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>(this)) {
    m_impl->setupUi();
    applyTheme();
}

ChatTimelineWidget::~ChatTimelineWidget() = default;

void ChatTimelineWidget::updateEvents(const QVector<UiChatEvent>& events,
                                      const QVector<StagedDiff>& diffs,
                                      const QStringList& projectRoots) {
    if (events == m_impl->events &&
        diffs == m_impl->stagedDiffs &&
        projectRoots == m_impl->projectRoots) {
        return;
    }

    const bool rootsChanged = (projectRoots != m_impl->projectRoots);
    const bool diffsChanged = (diffs != m_impl->stagedDiffs);
    const bool wasEmpty = m_impl->events.isEmpty() && m_impl->extraDiffEntries.isEmpty();
    const qsizetype oldCount = m_impl->events.size();
    const qsizetype newCount = events.size();

    qsizetype commonPrefix = 0;
    const qsizetype minCount = std::min(oldCount, newCount);
    while (commonPrefix < minCount &&
           eventsShareIdentity(m_impl->events[commonPrefix], events[commonPrefix])) {
        ++commonPrefix;
    }

    const bool isThreadSwitch = (commonPrefix == 0 && oldCount > 0 && newCount > 0);
    const bool bulkMutation = ((oldCount - commonPrefix) + (newCount - commonPrefix)) > 2;

    if (bulkMutation) {
        m_impl->contentWidget->setUpdatesEnabled(false);
    }

    // 1. Remove obsolete tail cards after commonPrefix
    while (static_cast<qsizetype>(m_impl->cards.size()) > commonPrefix) {
        m_impl->removeCardAt(m_impl->cards.size() - 1);
    }

    // 2. Update existing cards in-place (preserving streaming tail & inline edit state!)
    bool tailChanged = false;
    int userTurnOrdinal = 0;
    for (qsizetype i = 0; i < commonPrefix; ++i) {
        const UiChatEvent& ev = events[i];
        const bool isUser = (normalizeCardKind(ev) == QLatin1StringView("user"));
        const bool isDiffCard = (normalizeCardKind(ev) == QLatin1StringView("diff_staged"));
        const bool eventChanged = (ev != m_impl->events[i]);

        if (eventChanged || rootsChanged || (isDiffCard && diffsChanged)) {
            m_impl->cards[static_cast<std::size_t>(i)]->update(
                ev, diffs, projectRoots, userTurnOrdinal, rootsChanged);
            if (i == newCount - 1 && eventChanged) {
                tailChanged = true;
            }
        }
        if (isUser) {
            ++userTurnOrdinal;
        }
    }

    // 3. Append newly added event cards before extraDiffsContainer & bottom stretch
    for (qsizetype i = commonPrefix; i < newCount; ++i) {
        const UiChatEvent& ev = events[i];
        const bool isUser = (normalizeCardKind(ev) == QLatin1StringView("user"));

        auto card = m_impl->createCardForEvent(ev);
        card->update(ev, diffs, projectRoots, userTurnOrdinal, true);
        m_impl->contentLayout->insertWidget(static_cast<int>(i), card->root);
        m_impl->cards.push_back(std::move(card));

        if (isUser) {
            ++userTurnOrdinal;
        }
    }

    m_impl->events = events;
    m_impl->stagedDiffs = diffs;
    m_impl->projectRoots = projectRoots;

    // 4. Sync any standalone staged diffs not already represented by inline diff_staged events
    const bool extraDiffsGrew = m_impl->syncExtraDiffs(events, diffs);

    if (bulkMutation) {
        m_impl->contentWidget->setUpdatesEnabled(true);
    }

    const bool nowEmpty = m_impl->events.isEmpty() && m_impl->extraDiffEntries.isEmpty();
    if (nowEmpty) {
        m_impl->stack->setCurrentWidget(m_impl->emptyPage);
        m_impl->autoScrollPaused = false;
        m_impl->updateJumpButtonVisibility();
        return;
    }

    m_impl->stack->setCurrentWidget(m_impl->scrollArea);

    if (wasEmpty || isThreadSwitch) {
        scrollToBottom(true);
    } else if (newCount > oldCount || tailChanged || extraDiffsGrew) {
        scrollToBottom(false);
    }
    m_impl->updateJumpButtonVisibility();
}

void ChatTimelineWidget::setEmptyStateContext(bool hasProject, bool endpointConfigured) {
    m_impl->hasProject = hasProject;
    m_impl->endpointConfigured = endpointConfigured;
}

void ChatTimelineWidget::clear() {
    m_impl->contentWidget->setUpdatesEnabled(false);

    while (!m_impl->cards.empty()) {
        m_impl->removeCardAt(m_impl->cards.size() - 1);
    }
    for (Impl::ExtraDiffEntry& entry : m_impl->extraDiffEntries) {
        detachAndDeleteWidget(m_impl->extraDiffsLayout, entry.widget);
    }
    m_impl->extraDiffEntries.clear();
    m_impl->extraDiffsContainer->hide();

    m_impl->events.clear();
    m_impl->stagedDiffs.clear();
    m_impl->autoScrollPaused = false;
    m_impl->lastScrollMax = 0;
    m_impl->scrollScheduled = false;

    m_impl->contentWidget->setUpdatesEnabled(true);
    m_impl->stack->setCurrentWidget(m_impl->emptyPage);
    m_impl->updateJumpButtonVisibility();
}

void ChatTimelineWidget::scrollToBottom(bool force) {
    if (force) {
        m_impl->autoScrollPaused = false;
    }
    if (m_impl->autoScrollPaused) {
        m_impl->updateJumpButtonVisibility();
        return;
    }
    m_impl->snapScrollbarToBottom();
    m_impl->scheduleScrollToBottom();
    m_impl->updateJumpButtonVisibility();
}

bool ChatTimelineWidget::isAutoScrollPaused() const {
    return m_impl->autoScrollPaused;
}

int ChatTimelineWidget::eventCount() const {
    return static_cast<int>(m_impl->events.size());
}

void ChatTimelineWidget::applyTheme() {
    const UiPalette u = ThemeManager::instance().ui();
    const QColor bg = u.background;
    const QColor border = compositeOver(u.subtleBorder, u.background);
    const QColor elevated = compositeOver(u.elevatedSurface, u.background);
    const QColor selected = compositeOver(u.selectedFill, u.background);

    setStyleSheet(QStringLiteral("QWidget#spellChatTimeline { background-color: %1; }").arg(qssColor(bg)));
    m_impl->stack->setStyleSheet(
        QStringLiteral("QStackedWidget#spellChatTimelineStack { background-color: %1; }").arg(qssColor(bg)));
    m_impl->emptyPage->setStyleSheet(
        QStringLiteral("QWidget#spellEmptyChatPage { background-color: %1; }").arg(qssColor(bg)));
    m_impl->scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea#spellChatTimelineScroll { background-color: %1; border: none; }").arg(qssColor(bg)));
    m_impl->scrollArea->viewport()->setStyleSheet(
        QStringLiteral("background-color: %1;").arg(qssColor(bg)));
    m_impl->contentWidget->setStyleSheet(
        QStringLiteral("QWidget#spellChatTimelineContent { background-color: %1; }").arg(qssColor(bg)));

    // Empty state styling: ONLY the crisp centered monochrome Witch Hat logo
    m_impl->emptyHatLabel->setPixmap(icons::hatMarkPixmap(48, u.textPrimary));
    m_impl->emptyHatLabel->setStyleSheet(
        QStringLiteral("QLabel#spellEmptyChatHatLogo { background: transparent; border: none; }"));

    // Floating jump-to-bottom pill
    m_impl->jumpToBottomBtn->setIcon(icons::vectorIcon(QStringLiteral("chevron-down"), u.textPrimary, 12));
    m_impl->jumpToBottomBtn->setIconSize(QSize(12, 12));
    m_impl->jumpToBottomBtn->setStyleSheet(QStringLiteral(
        "QPushButton#spellJumpToBottom {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 13px;"
        "  padding: 4px 11px;"
        "}"
        "QPushButton#spellJumpToBottom:hover {"
        "  background-color: %4;"
        "  border-color: %5;"
        "}")
                                               .arg(qssColor(elevated),
                                                    qssColor(u.textPrimary),
                                                    qssColor(border),
                                                    qssColor(selected),
                                                    qssColor(u.selectedBorder)));

    // Extra staged diffs section
    m_impl->extraDiffsContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    m_impl->extraDiffsHeader->applyTheme();
    for (Impl::ExtraDiffEntry& entry : m_impl->extraDiffEntries) {
        if (entry.widget) {
            entry.widget->applyTheme();
        }
    }

    // All active event cards
    for (const auto& card : m_impl->cards) {
        if (card) {
            card->applyTheme();
        }
    }
}

void ChatTimelineWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    m_impl->positionJumpButton();
    const int vw = m_impl->viewportWidth();
    for (const auto& card : m_impl->cards) {
        if (card) {
            card->onViewportResized(vw);
        }
    }
}

bool ChatTimelineWidget::eventFilter(QObject* watched, QEvent* event) {
    if (!watched || !event) {
        return QWidget::eventFilter(watched, event);
    }

    // 1. User card hover brightening for Edit / Resend links
    if (auto it = m_impl->userHoverMap.constFind(watched); it != m_impl->userHoverMap.constEnd()) {
        if (event->type() == QEvent::HoverEnter || event->type() == QEvent::Enter) {
            it.value()->setHovered(true);
        } else if (event->type() == QEvent::HoverLeave || event->type() == QEvent::Leave) {
            it.value()->setHovered(false);
        }
    }

    // 2. Inline prompt editor shortcuts: Ctrl/Cmd+Enter submits, Escape cancels
    if (auto it = m_impl->userEditorMap.constFind(watched); it != m_impl->userEditorMap.constEnd()) {
        if (event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Escape) {
                it.value()->setEditing(false);
                return true;
            }
            if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) &&
                (ke->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
                it.value()->submitEdit();
                return true;
            }
        }
    }

    // 3. Click anywhere on a tool card header (outside child buttons) to expand/collapse
    if (auto it = m_impl->toolHeaderMap.constFind(watched); it != m_impl->toolHeaderMap.constEnd()) {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                ToolEventCard* tc = it.value();
                if (tc && tc->headerWidget) {
                    QWidget* hit = tc->headerWidget->childAt(me->position().toPoint());
                    if (hit != tc->toggleBtn && hit != tc->fileLinkBtn) {
                        tc->toggleExpanded();
                        return true;
                    }
                }
            }
        }
    }

    // 4. Viewport resize updates wrapped tool browser heights & responsive user bubbles
    if (watched == m_impl->scrollArea->viewport() && event->type() == QEvent::Resize) {
        const int vw = m_impl->viewportWidth();
        for (const auto& card : m_impl->cards) {
            if (card) {
                card->onViewportResized(vw);
            }
        }
        m_impl->updateJumpButtonVisibility();
    }

    return QWidget::eventFilter(watched, event);
}

} // namespace spell
